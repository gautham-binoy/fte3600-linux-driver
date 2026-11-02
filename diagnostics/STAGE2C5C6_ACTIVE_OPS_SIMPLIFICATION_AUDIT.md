# Stage 2C.5C.6 — Active Operations & Condition Variable Teardown Simplification Audit

**Target Silicon:** FocalTech FT9362 / FW9369  
**Component:** `tools/fw9369-mock/src/fw9369_transport_hw.c`  
**Evaluation Mode:** Software-Only Concurrency Formal Proof & Static Audit

---

## 1. Background: Why `active_ops` and `cond` Were Introduced

In Stage 2C.5C.4, an `active_ops` counter, a `pthread_cond_t cond`, `pthread_cond_wait()`, and `pthread_cond_broadcast()` were added to `struct hw_context`. The stated intention was to prevent `fw9369_hw_transport_destroy()` from freeing context memory while an operation was in flight by waiting for `active_ops == 0`.

However, the Stage 2C.5C.5 independent concurrency audit revealed that this mechanism introduced substantial synchronization complexity and latent hazards:
1. Every transfer was forced to increment and decrement `active_ops` and invoke `pthread_cond_broadcast(&ctx->cond)` upon exit.
2. In-flight operations held `ctx->lock` anyway throughout the entire duration of the SPI transfer.
3. If `destroy()` waited on `ctx->cond`, it had to release `ctx->lock`, allowing new transfers to potentially slip in unless blocked by `is_shutting_down`.

---

## 2. Formal Proof of Redundancy

We examine whether `active_ops` and `ctx->cond` can ever execute non-trivial work in the transport architecture:

### 2.1 Critical Section Analysis in `hw_transfer()`
In `hw_transfer(ctx, tx, rx, len)`:
1. `pthread_mutex_lock(&ctx->lock);` is called at the very entrance of the function.
2. If `ctx->is_shutting_down` is true, the mutex is unlocked, and `-ESHUTDOWN` is returned.
3. Allowlist validation and framing checks are performed while holding `ctx->lock`.
4. In-flight lock state transitions are updated while holding `ctx->lock`.
5. `ops->ioctl_fn(ctx->fd, SPI_IOC_MESSAGE(1), &tr)` is invoked while holding `ctx->lock`.
6. Post-transfer status and telemetry logging are recorded while holding `ctx->lock`.
7. `pthread_mutex_unlock(&ctx->lock);` is called at the very end of the function.
8. The function returns scalar stack integer `status`.

### 2.2 Critical Section Analysis in `fw9369_hw_transport_destroy()`
In `fw9369_hw_transport_destroy(t)`:
```c
struct hw_context *ctx = t->context;
pthread_mutex_lock(&ctx->lock);
ctx->is_shutting_down = true;
hw_close(ctx);
pthread_mutex_unlock(&ctx->lock);
pthread_mutex_destroy(&ctx->lock);
free(ctx);
free(t);
```

### 2.3 Mutual Exclusion Invariant
- **Theorem:** At the exact moment `pthread_mutex_lock(&ctx->lock)` succeeds inside `fw9369_hw_transport_destroy()`, the number of concurrent threads inside `hw_transfer()` is strictly zero ($N = 0$).
- **Proof:**
  1. By POSIX mutex invariants, `ctx->lock` permits at most one thread inside its critical section.
  2. Because `hw_transfer()` holds `ctx->lock` continuously from function entrance to function exit, no thread can be executing `hw_transfer()` unless it holds `ctx->lock`.
  3. If a thread is executing `hw_transfer()`, `fw9369_hw_transport_destroy()` blocks in `pthread_mutex_lock(&ctx->lock)` until that transfer releases the lock.
  4. Once `destroy()` acquires `ctx->lock`, no thread holds `ctx->lock` other than `destroy()`.
  5. Any thread attempting to enter `hw_transfer()` subsequently will block on `ctx->lock`. When `destroy()` sets `is_shutting_down = true`, closes the file descriptor, and unlocks the mutex, any waiting thread that acquires the lock immediately inspects `ctx->is_shutting_down == true`, releases the lock, and exits with `-ESHUTDOWN`.
  6. Therefore, `active_ops` was ALWAYS equal to 0 whenever `destroy()` held the lock.
  7. Consequently, the condition variable loop:
     ```c
     while (ctx->active_ops > 0) pthread_cond_wait(&ctx->cond, &ctx->lock);
     ```
     was provably dead code that never waited.

---

## 3. Removal & Code Simplification

The following redundant members and operations were completely removed:
1. `size_t active_ops;` from `struct hw_context`.
2. `pthread_cond_t cond;` from `struct hw_context`.
3. `pthread_cond_init(&ctx->cond, NULL)` from `fw9369_hw_transport_create_with_ops()`.
4. `pthread_cond_destroy(&ctx->cond)` from `fw9369_hw_transport_destroy()`.
5. `ctx->active_ops++` and `ctx->active_ops--` from `hw_transfer()`.
6. `pthread_cond_broadcast(&ctx->cond)` from `hw_transfer()`.
7. `fw9369_hw_get_active_ops_for_test()` from `fw9369_transport_hw.h` and `fw9369_transport_hw.c`.

### 3.1 Benefits
- **Zero Missed Wakeups:** No condition variables means zero risk of missed wakeups, lost signals, or spurious wakeups.
- **Lower Latency:** High-frequency SPI transfers avoid condition variable signaling overhead on every transaction.
- **Simpler Mental Model:** Lifetime and serialization rely entirely on the POSIX mutex and explicit ownership boundaries.
