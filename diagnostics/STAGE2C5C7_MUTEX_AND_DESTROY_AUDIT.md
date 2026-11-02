# Stage 2C.5C.7 — Mutex and Teardown Semantics Audit

**Target Silicon:** FocalTech FT9362 / FW9369  
**Component:** `tools/fw9369-mock/src/fw9369_transport_hw.c`  
**Audit Scope:** Locking Invariants, Error Paths, Mutex Lifecycle & Teardown Cases  
**Audit Mode:** Software-Only Static Analysis & Code Verification

---

## 1. Mutex Acquisition and Release Invariants

All functions interacting with `ctx->lock` in `tools/fw9369-mock/src/fw9369_transport_hw.c` were audited:

| Function | Lock Acquisition | Lock Release Points | Guaranteed Release On Every Path |
|---|---|---|---|
| `hw_transfer()` | Line 233 (`pthread_mutex_lock(&ctx->lock)`) | Line 238 (shutdown exit), Line 352 (allowlist error), Line 437 (normal / ioctl error exit) | **YES.** Every branch out of the function unlocks `ctx->lock` before returning. |
| `fw9369_hw_transport_destroy()` | Line 567 (`pthread_mutex_lock(&ctx->lock)`) | Line 570 (`pthread_mutex_unlock(&ctx->lock)`) | **YES.** Unlocked immediately before `pthread_mutex_destroy()`. |
| `fw9369_hw_get_tx_count()` | Line 583 | Line 585 | **YES.** |
| `fw9369_hw_get_last_tx()` | Line 595 | Line 598 (empty log), Line 610 (undersized), Line 615 (success) | **YES.** |
| `fw9369_hw_get_fd()` | Line 624 | Line 626 | **YES.** |
| `fw9369_hw_is_protected_unlocked()` | Line 635 | Line 637 | **YES.** |
| `fw9369_hw_get_lock_state()` | Line 646 | Line 648 | **YES.** |
| `fw9369_hw_set_lock_state_for_test()` | Line 657 | Line 659 | **YES.** |
| `fw9369_hw_is_shutting_down_for_test()` | Line 675 | Line 677 | **YES.** |
| `fw9369_hw_set_shutting_down_for_test()` | Line 686 | Line 688 | **YES.** |

### 1.1 Recursion Audit
The transport mutex is configured with `PTHREAD_MUTEX_NORMAL`.
- **Finding:** No production transport path calls another function that acquires `ctx->lock`.
- **Test Helpers:** Test inspection hooks inside mock `fake_ioctl` exclusively use `fw9369_hw_get_lock_state_unlocked_for_test()` and `fw9369_hw_set_shutting_down_unlocked_for_test()`.
- **Verdict:** Zero recursive locking occurs across production or test code.

---

## 2. Destruction Semantics & Teardown Cases

To prevent concurrency misunderstandings, we formally evaluate the five fundamental concurrency interleavings during transport teardown:

```
[Case A] Transfer holds mutex ---------> Destructor blocks on mutex until transfer completes.
[Case B] Waiter queued on mutex --------> Destructor vs. Waiter race; destroying mutex with waiters is UB.
[Case C] Stale pointer before lock ------> Context freed before entry; Use-After-Free (UAF).
[Case D] Operation after shutdown ------> Pre-ioctl check returns -ESHUTDOWN cleanly.
[Case E] Hanging in-flight ioctl -------> Destructor blocked indefinitely awaiting kernel return.
```

### Case A: A transfer already owns the mutex when destruction begins
- **Interleaving:** Thread 1 is inside `hw_transfer()` executing `ioctl`. Thread 2 calls `fw9369_hw_transport_destroy(t)`.
- **Behavior:** Thread 2 blocks in `pthread_mutex_lock(&ctx->lock)`. Thread 1 completes the transfer, updates state, and unlocks `ctx->lock`. Thread 1 returns scalar status without touching `ctx` again. Thread 2 acquires `ctx->lock`, sets `is_shutting_down = true`, closes the file descriptor, unlocks, destroys the mutex, and frees context memory.
- **Support Status:** **FULLY SUPPORTED INTERNALLY BY THE TRANSPORT.**

### Case B: A thread is waiting to acquire the mutex when destruction begins
- **Interleaving:** Thread 1 holds `ctx->lock`. Thread 3 calls `hw_transfer()` and blocks on `ctx->lock`. Thread 2 calls `fw9369_hw_transport_destroy()` and also blocks on `ctx->lock`.
- **Hazard:** If Thread 2 acquires the lock when Thread 1 releases it, Thread 2 destroys the mutex (`pthread_mutex_destroy`) and frees the context memory. Thread 3 then wakes up and attempts to access or acquire a destroyed mutex on deallocated heap memory. Under POSIX (IEEE Std 1003.1), destroying a mutex upon which other threads are blocked results in **Undefined Behavior**.
- **Support Status:** **REQUIRES CALLER COORDINATION.** Callers must not initiate destruction while other threads are actively queuing for operations.

### Case C: A thread has a context pointer but has not yet acquired the mutex
- **Interleaving:** Thread 4 holds a pointer to `t` or `ctx`. Thread 2 calls `fw9369_hw_transport_destroy(t)`, which deallocates `t` and `ctx`. Thread 4 subsequently executes `t->transfer(t->context, ...)`.
- **Hazard:** Thread 4 dereferences `t->transfer` or passes freed `t->context` before entering `hw_transfer()`. This produces an immediate **Use-After-Free (UAF)**.
- **Architectural Reality:** In standard C, no internal lock inside an object can protect a pointer to that object from concurrent destruction prior to function entry.
- **Support Status:** **REQUIRES CALLER COORDINATION.** Callers must enforce the Single-Owner Transport Contract.

### Case D: A thread attempts a new operation after destruction starts
- **Interleaving:** `destroy()` has acquired `ctx->lock` and asserted `ctx->is_shutting_down = true`. Thread 5 enters `hw_transfer()`.
- **Behavior:** If Thread 5 enters while the context is still allocated, it blocks on `ctx->lock`, wakes after `destroy()` sets shutdown, immediately detects `ctx->is_shutting_down == true`, and returns `-ESHUTDOWN` without issuing any SPI ioctl.
- **Support Status:** **SUPPORTED WHILE ALLOCATED; REQUIRES CALLER COORDINATION BEFORE DEALLOCATION.**

### Case E: An ioctl never returns
- **Interleaving:** Thread 1 calls `ioctl(SPI_IOC_MESSAGE)` and the hardware/bus or kernel driver hangs indefinitely in uninterruptible sleep (`TASK_UNINTERRUPTIBLE`). Thread 2 calls `destroy()`.
- **Behavior:** Thread 2 blocks indefinitely in `pthread_mutex_lock(&ctx->lock)`.
- **Hazard Mitigation:** Userspace cannot safely abort an in-flight kernel SPI transfer without kernel-level driver support. Forcible userspace termination (`pthread_cancel`, closing `fd` from another thread) causes kernel descriptor races and corrupted hardware state.
- **Support Status:** **BOUNDED BY KERNEL/HARDWARE DRIVER COMPLETION ONLY.** (See `STAGE2C5C7_SHUTDOWN_LIMITATIONS.md`).

---

## 3. Mandatory Single-Owner Transport Contract

Because Cases B, C, and D involve memory lifecycle boundaries inherent to C, the driver contract is formally documented:

> [!IMPORTANT]
> **Single-Owner Lifecycle Contract:**
> 1. Transport handles (`struct fw9369_transport *`) have single-owner lifetime semantics.
> 2. The owning thread that created the transport MUST stop dispatching new work to worker threads.
> 3. The owning thread MUST synchronize and join all worker threads (`pthread_join()`) before calling `fw9369_hw_transport_destroy()`.
> 4. Destroying a transport while other threads hold active pointers to it is strictly prohibited.

**Audit Status:** **PASS (Semantics, invariants, and boundaries formally verified)**.
