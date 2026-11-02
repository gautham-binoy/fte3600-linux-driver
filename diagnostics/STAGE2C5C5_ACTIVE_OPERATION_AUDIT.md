# Stage 2C.5C.5 — Active Operation Accounting and Drain Redundancy Audit

## Executive Summary

This document audits the `active_ops` counter and condition-variable drain barrier in `tools/fw9369-mock/src/fw9369_transport_hw.c`.

The audit reveals a profound architectural insight: because `hw_transfer()` holds `ctx->lock` across the entire SPI transfer (including the blocking `ioctl_fn` syscall), **`ctx->active_ops` is mathematically guaranteed to be 0 whenever `ctx->lock` is released**. Consequently, the condition variable wait in `fw9369_hw_transport_destroy()` is completely redundant under the current serialized locking model.

---

## 1. Trace of Active Operation Lifecycle

### Exact Execution Sequence in `hw_transfer()`
In [src/fw9369_transport_hw.c:240-449](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L240-L449):

1. **Step 1: Mutex Acquisition (Line 246)**
   ```c
   pthread_mutex_lock(&ctx->lock);
   ```
2. **Step 2: Teardown Gate Check (Line 248)**
   ```c
   if (ctx->is_shutting_down) {
       pthread_mutex_unlock(&ctx->lock);
       return -ESHUTDOWN;
   }
   ```
   *(If shutting down, exits before registration. `active_ops` remains unchanged).*
3. **Step 3: Descriptor and Length Checks (Lines 253, 259)**
   *(If fd < 0 or len > max, exits before registration. `active_ops` remains unchanged).*
4. **Step 4: Active Operation Registration (Line 264)**
   ```c
   ctx->active_ops++;
   ```
5. **Step 5: Protocol Allowlist Enforcement (Line 271-354)**
   If allowlist validation fails (`!packet_valid`):
   ```c
   ctx->active_ops--;
   pthread_cond_broadcast(&ctx->cond);
   pthread_mutex_unlock(&ctx->lock);
   return -EPERM;
   ```
   *(Increment is immediately decremented; lock released; returns `-EPERM`).*
6. **Step 6: In-Flight State Transition (Lines 373-377)**
   Recorded while holding `ctx->lock`.
7. **Step 7: SPI ioctl System Call (Line 393)**
   ```c
   int ret = ctx->ops.ioctl_fn(ctx->fd, SPI_IOC_MESSAGE(1), &tr);
   ```
   **CRITICAL OBSERVATION:** `ctx->lock` is **HELD** throughout the entire blocking execution of `ioctl_fn()`.
8. **Step 8: Post-Transfer State and Telemetry Updates (Lines 408-441)**
   Updated while holding `ctx->lock`.
9. **Step 9: Active Operation Decrement & Signal (Lines 443-444)**
   ```c
   ctx->active_ops--;
   pthread_cond_broadcast(&ctx->cond);
   ```
10. **Step 10: Mutex Release (Line 445)**
    ```c
    pthread_mutex_unlock(&ctx->lock);
    ```

---

## 2. Mathematical Proof of Drain Barrier Redundancy

Let $T_{xfer}$ be an invocation of `hw_transfer()` and $T_{dest}$ be an invocation of `fw9369_hw_transport_destroy()`.

In `fw9369_hw_transport_destroy()`:
```c
pthread_mutex_lock(&ctx->lock);
ctx->is_shutting_down = true;
while (ctx->active_ops > 0) {
    pthread_cond_wait(&ctx->cond, &ctx->lock);
}
```

1. **Mutual Exclusion:**
   - Both $T_{xfer}$ and $T_{dest}$ must acquire `ctx->lock`.
   - If $T_{xfer}$ is active (executing `ioctl_fn`), $T_{xfer}$ holds `ctx->lock`.
   - When $T_{dest}$ calls `pthread_mutex_lock(&ctx->lock)`, $T_{dest}$ **blocks immediately on the mutex**.
   - $T_{dest}$ **cannot** read `ctx->active_ops` or evaluate `while (ctx->active_ops > 0)` while $T_{xfer}$ is running.
2. **State upon Mutex Release:**
   - In $T_{xfer}$, line 443 decrements `ctx->active_ops--` **strictly prior to** line 445 `pthread_mutex_unlock(&ctx->lock)`.
   - At the exact moment $T_{xfer}$ releases `ctx->lock`, `ctx->active_ops` is `0`.
3. **Evaluation in Destructor:**
   - When $T_{dest}$ unblocks and acquires `ctx->lock`, `ctx->active_ops` is ALREADY `0`.
   - The loop condition `while (ctx->active_ops > 0)` evaluates to `false` on the very first check.
   - `pthread_cond_wait(&ctx->cond, &ctx->lock)` is **NEVER CALLED**.

### Architectural Verdict
Under the current design where the mutex spans the entire `ioctl` transaction:
- The mutex ALREADY provides 100% serialization of transfers and destruction.
- `ctx->active_ops`, `ctx->cond`, and `pthread_cond_broadcast()` provide zero additional protection. They represent dead synchronization machinery.

---

## 3. Increment/Decrement Integrity Audit

Every path through `hw_transfer()` was audited for counter leaks or underflow:

| Path | `active_ops++` Executed? | `active_ops--` Executed? | Balance | Status |
| :--- | :--- | :--- | :--- | :--- |
| `is_shutting_down == true` (L248) | No | No | 0 | Clean |
| `fd < 0` (L253) | No | No | 0 | Clean |
| `len > max_len` (L259) | No | No | 0 | Clean |
| Allowlist rejection (`!packet_valid`, L356) | Yes (L264) | Yes (L356) | 0 | Clean |
| ioctl failure (`ret < 0`, L396) | Yes (L264) | Yes (L443) | 0 | Clean |
| Short transfer (`ret < len`, L398) | Yes (L264) | Yes (L443) | 0 | Clean |
| Protocol overflow (`ret > len`, L401) | Yes (L264) | Yes (L443) | 0 | Clean |
| Exact success (`status == 0`, L404) | Yes (L264) | Yes (L443) | 0 | Clean |

### Deterministic Test Verification
Test Suite 14, Tests 6 & 7 verified every error path:
- `6a`: Initial `active_ops == 0`.
- `6b`: Post-transfer `active_ops == 0`.
- `7a` (Allowlist error): `active_ops == 0`.
- `7b` (ioctl failure): `active_ops == 0`.
- `7c` (Short transfer): `active_ops == 0`.
- `7d` (Protocol overflow): `active_ops == 0`.
- `7e` (Shutdown rejection): `active_ops == 0`.
- Result: **Zero counter leaks or underflow under any execution path.**
