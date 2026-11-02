# Stage 2C.5C.5 — Deterministic Concurrency Test Report

## Executive Summary

This document reports on the design, initial deadlock diagnosis, correction, and final results of the deterministic offline concurrency test suite (**Test Suite 14**) implemented in [`tests/test_runner.c`](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/tests/test_runner.c).

The test suite expanded from 199 to **219 total passing tests (100.0%)**, verifying all 10 concurrency requirements deterministically without arbitrary sleep delays or race conditions.

---

## 1. Deadlock Incident in Test 4: Root Cause and Resolution

### The Initial Deadlock
During the initial run of Test Suite 14, execution hung indefinitely in Test 4 (`a getter racing with a transport operation`).

### Root-Cause Diagnosis
1. **Barrier Structure:** The test fixture initialized a 2-party barrier (`pthread_barrier_init(&s14_barrier_a, NULL, 2)`).
2. **First Barrier Trip:** Both Thread 1 (transfer worker) and Thread 2 (getter worker) called `pthread_barrier_wait(&s14_barrier_a)`. Both unblocked.
3. **Circular Wait:**
   - Thread 1 called `t->transfer()`, acquired `ctx->lock`, and entered `test_fake_ioctl()`.
   - In `test_fake_ioctl`, the hook `s14_ioctl_hook_getter_race` called `pthread_barrier_wait(&s14_barrier_a)` **a second time**. Thread 1 blocked waiting for a second thread on the barrier while holding `ctx->lock`.
   - Thread 2 called `fw9369_hw_get_lock_state()`, which called `pthread_mutex_lock(&ctx->lock)`. Thread 2 blocked on `ctx->lock`.
   - Thread 2 could never reach the barrier a second time.
   - Main thread blocked in `pthread_join(th1, &r1)`.
   - Result: 100% deterministic deadlock in the test harness fixture (not in production transport code).

### The Corrected Synchronization Design
Test 4 was redesigned with an independent test-control synchronization harness:
1. **Dedicated Synchronization Primitives:**
   Created `struct test4_ctrl` with an independent `pthread_mutex_t lock` and `pthread_cond_t cond`, completely isolated from `ctx->lock`.
2. **Hook Handshake:**
   The fake ioctl hook signals `transfer_in_hook = true` and condition-waits for `release_hook = true`.
3. **Getter Launch & Verification:**
   The getter thread is launched only after `transfer_in_hook` is confirmed. The getter thread signals `getter_started = true`, then calls `fw9369_hw_get_lock_state()`.
4. **Ordering Invariant:**
   The controller verifies `!g_t4.getter_done && !g_t4.getter_finished_early` while the transfer is still holding `ctx->lock`, proving that the getter cannot complete while the transfer is active.
5. **Release & Completion:**
   The controller sets `release_hook = true` and wakes the hook. The transfer completes and unlocks `ctx->lock`. The getter acquires `ctx->lock`, reads `ASSUMED_UNLOCKED`, and finishes. Both threads join cleanly.
6. **Failure Containment:**
   All condition waits use bounded deadlines (`pthread_cond_timedwait` with 5-second limits). Any timeout records the failure stage, safely releases the hook, joins created threads, and reports assertion failure without hanging.

---

## 2. Comprehensive Test Suite 14 Results

```text
--- Test Suite 14: Stage 2C.5C.5 Deterministic Concurrency, Mutex & Context-Lifetime Audit ---
  [PASS] 1. Two concurrent operations serialized cleanly with strict mutual exclusion
  [PASS] 2. Active transfer completed and drained safely before destruction proceeded
  [PASS] 3. Waiting thread rejected with -ESHUTDOWN upon acquiring lock after shutdown
  [PASS] Test 4 test-control primitives initialized successfully
  [PASS] Test 4 transfer worker thread created
  [PASS] Test 4 transfer thread safely entered fake ioctl hook holding ctx->lock
  [PASS] Test 4 getter worker thread created
  [PASS] Test 4 ordering verified: getter remained blocked while transfer owned ctx->lock
  [PASS] 4. State getter serialized cleanly with transfer and returned valid state snapshot
  [PASS] 5. Multiple concurrent threads cleanly rejected with -ESHUTDOWN after shutdown
  [PASS] 6a. Initial active_ops is 0
  [PASS] 6b. Active_ops decrements cleanly to 0 upon transfer exit
  [PASS] 7a. Allowlist error decrements active_ops back to 0
  [PASS] 7b. ioctl -EIO error decrements active_ops back to 0
  [PASS] 7c. Short transfer error decrements active_ops back to 0
  [PASS] 7d. Protocol length error decrements active_ops back to 0
  [PASS] 7e. Shutdown rejection does not corrupt active_ops (remains 0)
  [PASS] 8. Repeated post-shutdown transfer calls consistently return -ESHUTDOWN
  [PASS] 9. Owner-managed thread joining prior to destruction executed without leak or race
  [PASS] 10. Operation rejected with -ESHUTDOWN immediately without invoking ioctl
```

### Cumulative Results
- Total Tests: **219 / 219 Passed (100.0%)**
- Execution Duration: **< 1.0 second**
- Skipped / Timed-Out Tests: **0**
