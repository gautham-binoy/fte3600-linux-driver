# Stage 2C.5C.4 — Concurrency and Multi-Threading Design

## Executive Summary

This document describes the concurrency model implemented in the FW9369 Linux SPI hardware transport (`fw9369_transport_hw.c`).

The Stage 2C.5C.3 audit established that the transport previously lacked all synchronization mechanisms, exposing callers to data races on `ctx->lock_state`, corrupted telemetry logs, interleaved SPI commands, and use-after-free during context destruction.

Stage 2C.5C.4 resolves these hazards by integrating POSIX thread synchronization (`<pthread.h>`), serializing all operations per context, protecting all internal state under a recursive mutex, and implementing a drain barrier for teardown.

---

## 1. Concurrency Architecture

### Internal Synchronization Primitives
In [src/fw9369_transport_hw.c:37-41](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L37-L41):

```c
struct hw_context {
    struct fw9369_hw_config config;
    struct fw9369_hw_ops    ops;
    int                     fd;
    enum fw9369_lock_state  lock_state; /* Tracks 0x9A protected register software knowledge */
    struct hw_log_entry     log[HW_MAX_LOG_PACKETS];
    size_t                  log_count;
    pthread_mutex_t         lock;             /* Serializes operations, state, and telemetry */
    pthread_cond_t          cond;             /* Drains in-flight operations before teardown */
    int                     active_ops;       /* Active transfer call counter */
    bool                    is_shutting_down; /* Teardown gate flag */
};
```

### Mutex Characteristics & Initialization
1. **Recursive Mutex (`PTHREAD_MUTEX_RECURSIVE`):**
   Initialized in [src/fw9369_transport_hw.c:531-544](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L531-L544):
   - External callers executing on different threads are strictly serialized: Thread B blocks until Thread A releases `ctx->lock`.
   - Inspection functions or re-entrant callbacks on the *same thread* (e.g. state inspection from within an `ioctl` hook or signal handler) can acquire the mutex without self-deadlock.
2. **Condition Variable (`pthread_cond_t`):**
   Initialized in [src/fw9369_transport_hw.c:547](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L547):
   - Allows `fw9369_hw_transport_destroy()` to block until all active `hw_transfer()` invocations have completed and drained.

---

## 2. Hazard Resolution & Proof of Protection

### Hazard 1: Concurrent `hw_transfer()` Invocations
- **Mechanism:** In [src/fw9369_transport_hw.c:244](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L244), `pthread_mutex_lock(&ctx->lock)` is acquired at the very start of `hw_transfer()`.
- **Proof:** Only one thread can execute `ioctl_fn()`, modify `ctx->lock_state`, or write `ctx->log` at any time.

### Hazard 2: Overlapping Protected Trim Writes and Lock Changes
- **Mechanism:** Both allowlist verification and `ioctl` execution occur under `ctx->lock`.
- **Proof:** If Thread 1 is shifting an unlock command, Thread 2's protected trim write cannot enter allowlist validation until Thread 1 finishes and releases the mutex.

### Hazard 3: Telemetry Log Race Condition
- **Mechanism:** In [src/fw9369_transport_hw.c:407-417](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L407-L417), the update to `ctx->log[ctx->log_count++]` is guarded by `ctx->lock`.
- **Proof:** Zero lost updates or out-of-order array indexing across threads.

### Hazard 4: State Inspection Atomicity
- **Mechanism:** All state getter functions ([`fw9369_hw_get_lock_state()`](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L653), [`fw9369_hw_is_protected_unlocked()`](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L644), [`fw9369_hw_get_tx_count()`](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L617)) acquire `ctx->lock` before reading.
- **Proof:** Callers receive atomic snapshots; compiler register caching and CPU store-buffer tearing are eliminated.

---

## 3. Multi-Threaded Test Verification

In `tests/test_runner.c` (Test Suite 13, Tests 13 & 14):
- Four concurrent worker threads were launched via `pthread_create()`.
- Each thread executed 25 SPI byte transfers (100 transfers total) simultaneously on a shared `struct fw9369_transport`.
- All four threads joined cleanly (`pthread_join()`), and `fw9369_hw_get_tx_count(t)` was verified to equal exactly `pre_count + 100`.
- All 100 transfers were logged without memory corruption or data loss.

---

## 4. Build System & Dependency Integration

In `tools/fw9369-mock/Makefile`:
- `CFLAGS` updated to include `-pthread`:
  `CFLAGS ?= -Wall -Wextra -Werror -pedantic -std=c11 -O2 -pthread`
- `LDFLAGS` updated to include `-pthread`:
  `LDFLAGS ?= -pthread`
- Targets `fw9369-tool`, `fw9369-tests`, and `fw9369-hw-tool` link with `-pthread`.
