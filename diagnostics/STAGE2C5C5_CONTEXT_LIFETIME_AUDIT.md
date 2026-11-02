# Stage 2C.5C.5 — Context Lifetime, Stale Pointers, and Ownership Contract Audit

## Executive Summary

This document conducts an adversarial audit of context lifetime, teardown mechanics, and caller contracts in the FW9369 Linux SPI transport backend ([`src/fw9369_transport_hw.c`](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c)).

The audit rigorously exposes the limitations of C pointers: **no internal synchronization primitive (mutex or condition variable) can prevent a Use-After-Free (UAF) if an unjoined worker thread holds a stale pointer to an object being freed by another thread**. A mandatory caller-managed single-owner lifecycle contract is required to guarantee memory safety.

---

## 1. Audit of Critical Interaction Scenarios

| Scenario | Execution Sequence | Outcome & Safety Assessment |
| :--- | :--- | :--- |
| **1. Transfer active, destruction requested** | Thread A is in `hw_transfer()` holding `ctx->lock`. Thread B calls `destroy(t)` and blocks on `ctx->lock`. Thread A finishes, logs, decrements, unlocks. Thread B unblocks, marks shutdown, closes fd, frees `ctx` and `t`. | **Safe** IF Thread A makes no subsequent accesses to `t` or `ctx`. |
| **2. Thread about to enter transfer while destruction begins** | Thread B calls `destroy(t)`, acquires `ctx->lock`, closes fd, destroys mutex, frees `ctx` and `t`. Thread A has pointer `t` and invokes `t->transfer(t->context, ...)`. | **CRITICAL UAF / CRASH:** Thread A dereferences freed memory at `t->transfer` and `ctx->lock`. Internal mutex cannot prevent this because Thread A had not yet acquired `ctx->lock`. |
| **3. Two threads request destruction** | Thread B1 calls `destroy(t)`. Thread B2 calls `destroy(t)`. | **FATAL DOUBLE FREE:** Thread B1 destroys mutex and frees `ctx`/`t`. Thread B2 attempts `pthread_mutex_lock` on freed memory or frees already freed heap blocks. |
| **4. Thread calls getter during destruction** | Thread A calls `fw9369_hw_get_lock_state(t)` while Thread B executes `destroy(t)`. | **CRITICAL UAF:** Getter dereferences `t->context` and attempts to lock `ctx->lock` on memory that is concurrently being or has been freed. |
| **5. Caller retains context pointer after destruction** | Transport is destroyed via `destroy(t)`; caller retains pointer variable `t`. | **DANGLING POINTER / UAF:** Any subsequent call using `t` invokes undefined behavior. |
| **6. Owning application fails to join worker before destruction** | Application launches worker threads accessing `t`, then calls `destroy(t)` without `pthread_join()`. | **UNSYNCHRONIZED CONCURRENCY / UAF:** Uncontrolled race between heap deallocation and worker dereferencing. |

---

## 2. Epistemic Limitation: Why Internal Barriers Cannot Prevent UAF

It is a common design fallacy to believe that adding a condition-variable "drain barrier" to a struct makes its destruction thread-safe.

### The Pointer Dereference Race
Consider a caller invoking:
```c
t->transfer(t->context, tx, rx, len);
```
In the C abstract machine (and machine code):
1. Register $R_1 \leftarrow \text{load}(t + \text{offset}(\text{transfer}))$
2. Register $R_2 \leftarrow \text{load}(t + \text{offset}(\text{context}))$
3. Branch to address in $R_1$, passing $R_2$ as the first argument (`context`).
4. Inside `hw_transfer()`: `pthread_mutex_lock(&ctx->lock)`.

If another thread executes `free(ctx)` and `free(t)` between Step 1 and Step 4:
- The pointer in $R_2$ points to reclaimed or reallocated heap memory.
- `pthread_mutex_lock()` operates on reclaimed memory.
- **The internal mutex cannot execute before the pointer is loaded.**
- Therefore, no internal mechanism inside `ctx` can ever guard against concurrent calls to `destroy()`.

---

## 3. The Mandatory Single-Owner Lifecycle Contract

Because the C runtime cannot protect stale pointers across threads without external reference counting or garbage collection, the transport API enforces a strict, explicit ownership contract:

> [!IMPORTANT]
> ### The Single-Owner Transport Contract
> 1. **Exclusive Session Owner:** A `struct fw9369_transport *` instance is owned exclusively by a single orchestrator thread.
> 2. **Worker Join Requirement:** Before calling `fw9369_hw_transport_destroy(t)`, the owner thread **MUST stop and join all worker threads** that hold or access `t`.
> 3. **Pointer Invalidation:** After `destroy(t)` returns, the pointer `t` must be set to `NULL` by the caller.
> 4. **No Multi-Threaded Destructors:** `destroy()` must NEVER be invoked concurrently from multiple threads or asynchronously from signal handlers.

### Application Call-Site Verification
In [`tools/fw9369-mock/src/fw9369_main_hw.c`](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_main_hw.c):
- Execution is strictly single-threaded.
- No background pthreads are spawned.
- The lifecycle contract is fully adhered to by the application entry point.
- Offline unit tests in `tests/test_runner.c` (Test Suite 14, Test 9) explicitly test and enforce this joining contract prior to teardown.
