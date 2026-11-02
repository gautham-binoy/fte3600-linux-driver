# Stage 2C.5C.6 — Mutex Design, Re-entrancy, and Locking Invariants Audit

**Target Silicon:** FocalTech FT9362 / FW9369  
**Component:** `tools/fw9369-mock/src/fw9369_transport_hw.c`  
**Evaluation Mode:** Software-Only Concurrency Analysis & Unit Testing

---

## 1. Analysis of `PTHREAD_MUTEX_RECURSIVE` vs `PTHREAD_MUTEX_NORMAL`

In Stage 2C.5C.4, `struct hw_context` configured its mutex as `PTHREAD_MUTEX_RECURSIVE`.

### 1.1 Why Re-entrancy Was Originally Introduced
An audit of all code paths revealed that **zero production operational transport paths** ever re-acquire `ctx->lock`. Re-entrancy was introduced solely to accommodate unit test hooks:
- During offline unit tests, mock `fake_ioctl` hooks were executed while `hw_transfer()` held `ctx->lock`.
- Some test fixtures invoked `fw9369_hw_get_lock_state(t)` or `fw9369_hw_set_shutting_down_for_test(t)` from inside the hook to inspect or mutate intermediate context state.
- Because those helpers acquired `ctx->lock`, a normal mutex would have deadlocked immediately unless recursive attributes were enabled.

### 1.2 Hazards of Recursive Mutexes in Device Drivers
1. **Masked Concurrency Bugs:** A recursive mutex allows accidental double-locking bugs to go completely unnoticed during development.
2. **Lock-Ordering Inversions:** Recursive mutexes mask subtle lock inversion cycles across multiple synchronization primitives.
3. **Mismatches in Unlock Semantics:** If an error path unlocks the mutex once while recursion depth is > 1, the lock remains held, starving other threads indefinitely.
4. **Performance Overhead:** Recursive mutexes maintain an ownership thread ID and recursion counter on every acquire/release.

---

## 2. Refactoring to `PTHREAD_MUTEX_NORMAL`

The mutex attribute in `fw9369_hw_transport_create_with_ops()` was changed to:
```c
pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
```

### 2.1 Refactoring Test Inspection Helpers
To eliminate the need for recursive locking in test hooks, explicit unlocked internal inspection variants were created for test harness usage:
- `fw9369_hw_get_lock_state_unlocked_for_test(t)`: Reads `ctx->lock_state` directly without attempting to re-lock `ctx->lock`. Safe because the calling test hook is executing synchronously inside `fake_ioctl`, where `ctx->lock` is already held by the current thread.
- `fw9369_hw_set_shutting_down_unlocked_for_test(t, down)`: Mutates `ctx->is_shutting_down` directly from inside the hook without attempting to re-lock `ctx->lock`.

---

## 3. Error-Path Lock Release Verification

Under `PTHREAD_MUTEX_NORMAL`, every single error exit in `hw_transfer()` must cleanly release `ctx->lock` exactly once. If any path fails to unlock, subsequent transfers on the same context will deadlock immediately.

In Suite 14 Test 7, all five discrete error exit paths were verified:
1. **Allowlist Error (`-EPERM`):** Rejects invalid opcodes, cleanly unlocks `ctx->lock`, and permits immediate subsequent transfer.
2. **ioctl Hardware Failure (`-EIO`):** Handles mock ioctl error, cleanly unlocks `ctx->lock`, and permits immediate recovery transfer.
3. **Short Transfer Error (`-EMSGSIZE`):** Handles incomplete SPI payload shift, cleanly unlocks `ctx->lock`, and permits immediate recovery transfer.
4. **Protocol Overflow Error (`-EPROTO`):** Handles invalid returned length, cleanly unlocks `ctx->lock`, and permits immediate recovery transfer.
5. **Shutdown Rejection (`-ESHUTDOWN`):** Rejects transfer while shutting down, cleanly unlocks `ctx->lock`, and preserves shutdown state.

All five error paths passed verification without deadlocks or resource leaks under standard, AddressSanitizer, and ThreadSanitizer builds.
