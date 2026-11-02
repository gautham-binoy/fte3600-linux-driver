# Stage 2C.5C.5 — Mutex and Condition Variable Audit

## Executive Summary

This document presents an independent, adversarial audit of the synchronization primitives implemented in the FW9369 Linux SPI hardware transport backend ([`src/fw9369_transport_hw.c`](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c)), focusing on the combination of `PTHREAD_MUTEX_RECURSIVE` with `pthread_cond_wait()`, attribute lifecycle, call graph recursion, and return code handling under POSIX.1-2008 / IEEE Std 1003.1.

---

## 1. POSIX Semantics: Recursive Mutexes with Condition Variables

### The Standard Specification
Under POSIX (IEEE Std 1003.1, `pthread_cond_wait` specification):
> *"If the mutex type is PTHREAD_MUTEX_RECURSIVE and the mutex had been locked multiple times by the calling thread, it is unspecified whether the mutex is unlocked or whether `pthread_cond_wait()` returns [EINVAL]."*

In Linux glibc (NPTL):
- If `pthread_cond_wait(&cond, &mutex)` is invoked when the recursive mutex has a recursion count $> 1$, glibc immediately returns `EINVAL` without blocking and **without releasing the mutex**.
- This creates an immediate deadlock or broken invariant: the condition wait fails to release the lock, preventing other threads from making progress to satisfy the predicate.

### Audit of Recursion Count in `fw9369_transport_hw.c`
In [src/fw9369_transport_hw.c:580-597](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L580-L597):
```c
void fw9369_hw_transport_destroy(struct fw9369_transport *t)
{
    if (t) {
        if (t->context) {
            struct hw_context *ctx = t->context;
            pthread_mutex_lock(&ctx->lock);
            ctx->is_shutting_down = true;
            while (ctx->active_ops > 0) {
                pthread_cond_wait(&ctx->cond, &ctx->lock);
            }
            hw_close(ctx);
            pthread_mutex_unlock(&ctx->lock);
            pthread_mutex_destroy(&ctx->lock);
            pthread_cond_destroy(&ctx->cond);
            free(ctx);
        }
        free(t);
    }
}
```

1. **Internal Invariant Verification:**
   - `struct hw_context` is opaque to callers outside `fw9369_transport_hw.c`. External application code cannot directly lock `ctx->lock`.
   - `destroy()` locks `ctx->lock` exactly once at line 584.
   - Between line 584 and line 587, no helper function is called.
   - Therefore, at line 587, the recursion count of `ctx->lock` is guaranteed to be exactly `1`.
2. **Release Semantics:**
   - Because the recursion count is exactly 1, `pthread_cond_wait()` in glibc unlocks `ctx->lock` atomically and places the thread on the condition wait queue.
   - However, combining recursive mutexes with condition variables remains an architectural code smell in concurrent systems because any future refactoring that adds a nested call prior to the wait will trigger `EINVAL` in glibc.

---

## 2. Call Graph Inspection & Recursion Necessity

An exhaustive audit of every function in [`src/fw9369_transport_hw.c`](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c) was conducted to determine whether recursion is ever exercised in production:

| Function | Mutex Acquisition | Internal Calls Made | Recursion Exercised? |
| :--- | :--- | :--- | :--- |
| `hw_transfer()` (L240) | `pthread_mutex_lock(&ctx->lock)` | `fw9369_is_command_allowed`, `fw9369_is_register_allowed`, `decode_register_addr`, `ctx->ops.ioctl_fn`, `memcpy`, `memset` | **NO** (None of these acquire locks) |
| `fw9369_hw_transport_destroy()` (L580) | `pthread_mutex_lock(&ctx->lock)` | `hw_close(ctx)` (does not lock) | **NO** |
| `fw9369_hw_get_tx_count()` (L601) | `pthread_mutex_lock(&ctx->lock)` | None | **NO** |
| `fw9369_hw_get_last_tx()` (L612) | `pthread_mutex_lock(&ctx->lock)` | None | **NO** |
| `fw9369_hw_get_fd()` (L629) | `pthread_mutex_lock(&ctx->lock)` | None | **NO** |
| `fw9369_hw_is_protected_unlocked()` (L640) | `pthread_mutex_lock(&ctx->lock)` | None | **NO** |
| `fw9369_hw_get_lock_state()` (L651) | `pthread_mutex_lock(&ctx->lock)` | None | **NO** |
| `fw9369_hw_set_lock_state_for_test()` (L662) | `pthread_mutex_lock(&ctx->lock)` | None | **NO** |

### Origin of `PTHREAD_MUTEX_RECURSIVE`
The recursive attribute was introduced in Stage 2C.5C.4 solely to accommodate unit test hooks in `tests/test_runner.c` (`test_in_flight_ioctl_hook`), where a mock callback executed during `ioctl_fn` re-entered `hw_transfer()` and `fw9369_hw_get_lock_state()` on the same thread.

In real Linux production execution:
- `ctx->ops.ioctl_fn` is `default_ioctl` $\rightarrow$ `ioctl(fd, SPI_IOC_MESSAGE(1), &tr)`.
- The Linux kernel SPI subsystem will never invoke userspace callbacks or re-enter transport functions on the calling thread.
- **Verdict on Recursion:** `PTHREAD_MUTEX_RECURSIVE` is completely unnecessary for production operations. A standard mutex (`PTHREAD_MUTEX_DEFAULT`) represents the simplest, most robust design.

---

## 3. Mutex Attribute Lifecycle and Return Code Audit

### Attribute Lifecycle
In [src/fw9369_transport_hw.c:531-548](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L531-L548):
- `pthread_mutexattr_init(&attr)` is initialized and checked.
- `pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE)` is set.
- `pthread_mutex_init(&ctx->lock, &attr)` is called and checked.
- `pthread_mutexattr_destroy(&attr)` is cleanly called in both normal and error branches.
- Primitives are destroyed with `pthread_mutex_destroy(&ctx->lock)` and `pthread_cond_destroy(&ctx->cond)` in `destroy()`.

### Unchecked Return Codes Audit
The audit identified several unchecked return values across the codebase:
1. `pthread_mutexattr_settype(&attr, ...)` return code is not checked at line 539.
2. In `hw_transfer()`: return values of `pthread_mutex_lock(&ctx->lock)` (line 246), `pthread_mutex_unlock(&ctx->lock)` (lines 249, 254, 260, 358, 445), and `pthread_cond_broadcast(&ctx->cond)` (lines 357, 444) are discarded.
3. In `destroy()`: return values of `pthread_mutex_lock`, `pthread_cond_wait`, `pthread_mutex_unlock`, `pthread_mutex_destroy`, and `pthread_cond_destroy` are discarded.
4. In all getters: `pthread_mutex_lock` and `unlock` return values are discarded.

Under POSIX, for a valid initialized mutex, `pthread_mutex_lock` can return `EINVAL` only if the mutex is corrupted or destroyed. While standard in C userspace for performance, critical lifecycle routines should defensively validate return codes during teardown.

---

## 4. Potential Deadlock Analysis in Condition Wait

In `fw9369_hw_transport_destroy()`:
```c
while (ctx->active_ops > 0) {
    pthread_cond_wait(&ctx->cond, &ctx->lock);
}
```
If an active `ioctl()` call is executing in the Linux kernel on the SPI controller, and the host controller freezes (e.g. infinite wait for hardware SPI FIFO TX ready flag without kernel timeout):
- `ctx->active_ops` remains `1`.
- `destroy()` waiting on `pthread_cond_wait()` will block indefinitely.
- The condition wait lacks a wall-clock timeout (`pthread_cond_timedwait`).
- Therefore, a kernel hang during an active transfer translates directly to an unrecoverable userspace hang in `destroy()`.
