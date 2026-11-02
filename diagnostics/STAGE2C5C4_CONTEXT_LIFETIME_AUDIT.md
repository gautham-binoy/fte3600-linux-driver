# Stage 2C.5C.4 — Context Lifetime and Teardown Audit

## Executive Summary

This document provides a technical audit of context lifetime, teardown mechanics, and caller responsibilities for the FW9369 Linux SPI transport backend ([`src/fw9369_transport_hw.c`](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c)).

The audit addresses the critical lifecycle question identified in Stage 2C.5C.3: *What happens if destruction is requested while an operation is actively executing in the kernel?*

---

## 1. Teardown Synchronization Implementation

In [src/fw9369_transport_hw.c:573-594](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L573-L594):

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

### The Drain Barrier Workflow
1. **Acquire Mutex:** The thread requesting destruction acquires `ctx->lock`. If another thread is currently in `hw_transfer()`, `destroy()` waits until that transfer either yields or reaches a condition wait.
2. **Assert Shutdown Gate:** `ctx->is_shutting_down = true` is asserted under the mutex.
3. **Drain Active Operations:** If an operation is currently executing inside `ioctl_fn()`, `ctx->active_ops` is greater than 0. The destructor calls `pthread_cond_wait(&ctx->cond, &ctx->lock)`, atomically releasing the mutex and waiting for the active transfer to finish.
4. **Active Operation Completion:** When the active transfer finishes, it decrements `ctx->active_ops--`, broadcasts `pthread_cond_broadcast(&ctx->cond)`, and releases the mutex.
5. **Teardown Execution:** The destructor thread re-acquires the mutex, verifies `active_ops == 0`, closes the underlying file descriptor via `hw_close(ctx)`, releases the mutex, destroys synchronization primitives, and frees both `ctx` and `t`.

---

## 2. Post-Shutdown Operation Rejection

In [src/fw9369_transport_hw.c:246-249](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L246-L249):

```c
pthread_mutex_lock(&ctx->lock);

if (ctx->is_shutting_down) {
    pthread_mutex_unlock(&ctx->lock);
    return -ESHUTDOWN;
}
```

If another thread attempts to invoke `hw_transfer()` while destruction is pending or in progress:
- The thread acquires `ctx->lock`.
- It observes `ctx->is_shutting_down == true`.
- It immediately releases the mutex and returns `-ESHUTDOWN` without touching the hardware file descriptor or submitting `ioctl`.

---

## 3. Epistemic Limitation: Why a Mutex Cannot Prevent Pointer Dereference Races

A common misconception in concurrent C programming is that adding a mutex to an object protects against use-after-free if another thread destroys the object.

**The Reality:**
A mutex residing inside `ctx` (`ctx->lock`) can only protect callers who have *already dereferenced* `t->context` and are waiting on the mutex.
If Thread 1 calls `fw9369_hw_transport_destroy(t)` and completes `free(t)` and `free(ctx)`:
If Thread 2 is about to execute `t->transfer(t->context, ...)`:
At the exact moment Thread 2 dereferences `t->context`, the memory has already been returned to the heap allocator.
**No mutex inside `ctx` can prevent Thread 2 from dereferencing an invalid pointer.**

---

## 4. Formal Caller Lifecycle Contract

To ensure absolute memory safety, the FW9369 transport enforces the standard POSIX single-owner lifecycle contract:

> [!IMPORTANT]
> ### The Single-Owner Transport Contract
> 1. **Single Owning Thread:** The transport instance (`struct fw9369_transport *`) is owned exclusively by the initializing thread (e.g. the session orchestrator).
> 2. **Worker Thread Join Mandatory:** Before calling `fw9369_hw_transport_destroy(t)`, the owner thread **MUST stop and join all worker threads** that hold a reference to `t`.
> 3. **Prohibition of Asynchronous Free:** Asynchronous or premature invocation of `fw9369_hw_transport_destroy()` while independent threads hold unprotected raw pointers to `t` is a caller contract violation.
> 4. **Transport-Level Protection:** If threads are properly joined, the transport's internal condition-variable drain guarantees that no lingering in-flight system call can race with descriptor closing or memory deallocation.
