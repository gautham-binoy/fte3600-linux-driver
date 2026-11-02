# Stage 2C.5C.3 — Concurrency and Multi-Threading Audit

## Executive Summary

This audit evaluates the thread safety, re-entrancy properties, and concurrency boundaries of the FW9369 Linux hardware SPI transport (`fw9369_transport_hw.c`).

**Primary Finding (Verified from source code):**
The FW9369 transport implementation is strictly **NON-THREAD-SAFE** and **NON-REENTRANT**. The codebase contains zero synchronization primitives, mutexes, spinlocks, or atomic operations. The architecture strictly assumes a single-threaded calling environment, but does not provide defensive code to detect or reject concurrent access.

---

## 1. Source-Code Audit for Concurrency Primitives

### Context Structure Inspection
In [fw9369_transport_hw.c:29-36](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L29-L36):

```c
struct hw_context {
    struct fw9369_hw_config config;
    struct fw9369_hw_ops    ops;
    int                     fd;
    enum fw9369_lock_state  lock_state; /* Tracks 0x9A protected register software knowledge */
    struct hw_log_entry     log[HW_MAX_LOG_PACKETS];
    size_t                  log_count;
};
```

### Absence of Synchronization Mechanisms
- **No Mutexes:** `pthread_mutex_t` does not appear anywhere in `fw9369_transport_hw.c`, `fw9369_transport_hw.h`, or `fw9369_state.c`.
- **No Atomics:** `<stdatomic.h>` is not included; `atomic_int` or atomic compare-and-swap operations are not used.
- **No Volatile Qualifiers:** `lock_state` and `fd` are standard plain C types subject to compiler register caching and CPU store-buffer reordering.
- **No Re-entrancy Flags:** The transport contains no busy flag or recursive call detector.

---

## 2. Race Condition Vulnerability Scenarios

### Scenario 1: Concurrent Calls to `hw_transfer()`
- **Vulnerability:** If Thread 1 and Thread 2 both call `t->transfer(t->context, ...)`:
  - Both threads issue `ioctl(ctx->fd, SPI_IOC_MESSAGE(1), &tr)`.
  - While the Linux kernel `spidev` driver serializes transfers internally via `spi_device` master lock, userspace state tracking is completely unprotected.
  - Thread 1 could set `ctx->lock_state = UNLOCK_COMMAND_SENT`.
  - Thread 2 could immediately overwrite `ctx->lock_state = LOCK_COMMAND_SENT`.
  - The outcomes of the two ioctls will interleave unpredictably, resulting in a completely corrupted knowledge state.

### Scenario 2: Concurrent Protected Write during In-Flight Unlock
- **Vulnerability:** Thread 1 calls unlock (`0x9A = 0x5A`) and blocks inside `ioctl_fn()`.
- If Thread 2 calls `hw_transfer()` with a protected trim write (`0x00C0`):
  - At line 296, Thread 2 reads `ctx->lock_state`. Because Thread 1 set it to `UNLOCK_COMMAND_SENT`, Thread 2's write is rejected with `-EPERM`.
  - However, because the read of `ctx->lock_state` is non-atomic and unsynchronized, if Thread 1's ioctl completes and writes `ASSUMED_UNLOCKED` without a memory barrier, Thread 2 may observe stale memory or a data race (undefined behavior under the C11 memory model).

### Scenario 3: Concurrent Destruction and Transfer (Use-After-Free)
- **Vulnerability:** Thread 1 is blocked in `ioctl_fn()`. Thread 2 calls `fw9369_hw_transport_destroy(t)`.
- In [fw9369_transport_hw.c:519-529](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L519-L529):
  ```c
  void fw9369_hw_transport_destroy(struct fw9369_transport *t)
  {
      if (t) {
          if (t->context) {
              struct hw_context *ctx = t->context;
              hw_close(ctx);
              free(ctx);
          }
          free(t);
      }
  }
  ```
  - Thread 2 closes the file descriptor and calls `free(ctx)` and `free(t)`.
  - When Thread 1 wakes up from `ioctl_fn()`, it attempts to write `ctx->lock_state` and access `ctx->log`.
  - **Result:** Severe Use-After-Free (UAF) leading to segmentation fault or heap corruption.

### Scenario 4: Telemetry Logging Data Race
- In [fw9369_transport_hw.c:405-415](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L405-L415):
  ```c
  if (ctx->config.enable_logging && ctx->log_count < HW_MAX_LOG_PACKETS) {
      struct hw_log_entry *entry = &ctx->log[ctx->log_count++];
      /* memcpy ... */
  }
  ```
  - `ctx->log_count++` is a read-modify-write operation on non-atomic storage.
  - Concurrent invocations will cause lost updates, out-of-bounds array indices, and overlapping buffer writes.

---

## 3. Explicit Architectural Boundary

### Verified Classification
- **Verified from source code:** The FW9369 transport code contains no concurrency support.
- **Verified from architecture:** The userspace tooling (`fw9369-mock`, `fw9369-tool`, `fw9369-hw-tool`) is designed and implemented exclusively for **single-threaded sequential execution**.

### Architectural Rule
> [!WARNING]
> The FW9369 hardware transport context (`struct fw9369_transport`) MUST NOT be shared across multiple threads or accessed from POSIX signal handlers. All SPI transactions, calibration phases, and state inspections must be invoked sequentially from a single dedicated thread of execution. If multi-threaded access is ever contemplated in future kernel or daemon architectures, a dedicated mutex wrapping all transport methods and lifetime teardown is strictly mandatory.
