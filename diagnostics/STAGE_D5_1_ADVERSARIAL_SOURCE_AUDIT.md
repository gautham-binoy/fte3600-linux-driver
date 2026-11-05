# STAGE D5.1: ADVERSARIAL SOURCE AND DIFF AUDIT

## Document Information
- **Project:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`) Native Linux Fingerprint Driver
- **Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`, CS0)
- **Host Controller:** Intel LPSS SPI2 (`pxa2xx-spi.2`) on Linux `7.2.8-9-omarchy` (x86_64 SMP)
- **Author:** Adversarial Kernel Auditor & Security Reviewer (Antigravity Agent)
- **Status:** COMPLETED ADVERSARIAL AUDIT
- **Safety Boundary:** STRICTLY READ-ONLY SOURCE AUDIT (Zero Hardware Operations, Zero Builds/Loads)

---

## 1. Executive Summary

This report delivers an independent, adversarial audit of every source code modification introduced in Stage D5 across `kernel/fte3600.h`, `kernel/fte3600.c`, and `kernel/fte3600-test.c`.

The audit was conducted strictly against the actual Git diff (`git diff origin/master`). The findings reveal that while the core kobject device lifetime architecture remains sound, significant discrepancies exist between the documented claims of Stage D5 and the actual code in the repository:

1. **Dead Synchronization Code:** The event sequence counter (`event_sequence`) and notification helper (`fte3600_notify_event()`) have **zero callers** and **zero readers** in `kernel/fte3600.c`. No kernel thread or userspace waiter evaluates `event_sequence`.
2. **UAPI State Leakage:** Internal state `FTE3600_STATE_DEVICE_REMOVAL` (13) is assigned in `fte3600_remove()` and directly exposed to userspace via `FTE3600_IOC_GET_STATE` (`snap.state = (u32)data->state`), violating the documented invariant that extended states (9..13) are strictly internal.
3. **Mock Test Desynchronization:** The userspace test suite passed `test_scenario_5_get_state_after_removal` only because the test harness `sim_driver_remove()` was not updated to mirror `kernel/fte3600.c:689`. The test asserted `snap.state == UNINITIALIZED` (0), while the actual driver returns `13`.
4. **Unsynchronized Read in Poll:** `fte3600_cdev_poll()` inspects `data->state` without acquiring `data->lock` and without `READ_ONCE()` or atomic semantics, while all state writers hold `data->lock`.
5. **Unreachable States:** The conditions polled by `fte3600_cdev_poll()` (`TOUCH_DETECTED` and `FRAME_READY`) are never assigned anywhere in the driver, rendering poll permanently unable to report readiness.

---

## 2. Complete Git Diff Inspection by Function and Declaration

### 2.1 Modifications to `kernel/fte3600.h`

#### Modification 1: Extended State Enum Values (`enum fte3600_state`)
- **Diff:**
  ```diff
  +	FTE3600_STATE_WAITING_FOR_TOUCH     = 9,
  +	FTE3600_STATE_TOUCH_DETECTED        = 10,
  +	FTE3600_STATE_FRAME_READY           = 11,
  +	FTE3600_STATE_SUSPENDED             = 12,
  +	FTE3600_STATE_DEVICE_REMOVAL        = 13,
  ```
- **Previous Behavior:** The enum defined states `0` through `8`.
- **New Behavior:** Defines extended states `9` through `13` inside the public header `fte3600.h`.
- **Locking Requirements:** N/A (compile-time definition).
- **Hardware Communication:** None.
- **UAPI Compatibility Impact:** **DEFECT.** Although documented as "internal to driver operation and not exposed via public v1 UAPI snapshots", the enum is defined inside the public UAPI header `fte3600.h` without `#ifdef __KERNEL__` isolation. Furthermore, state `13` is directly returned to userspace via `FTE3600_IOC_GET_STATE`.

#### Modification 2: Legacy State Compatibility Alias
- **Diff:**
  ```diff
  +/* Compatibility alias: INITIALIZED represents the ready/standby hardware state */
  +#define FTE3600_STATE_INITIALIZED    FTE3600_STATE_READY
  ```
- **Previous Behavior:** Only `#define FTE3600_STATE_RESET_COMPLETE FTE3600_STATE_RESET_SEQUENCE_ISSUED` existed.
- **New Behavior:** Defines `FTE3600_STATE_INITIALIZED` as an alias for `FTE3600_STATE_READY` (value 4).
- **Locking Requirements:** N/A (preprocessor define).
- **Hardware Communication:** None.
- **UAPI Compatibility Impact:** Safe. Allows legacy clients expecting `INITIALIZED` to compile cleanly against the canonical `READY` state.

#### Modification 3: Compile-Time Static Assertions
- **Diff:**
  ```diff
  +#ifdef __KERNEL__
  +#include <linux/build_bug.h>
  +static_assert(sizeof(struct fte3600_state_snapshot) == 32, "struct fte3600_state_snapshot must be 32 bytes");
  +static_assert(sizeof(struct fte3600_init_params) == 32, "struct fte3600_init_params must be 32 bytes");
  +static_assert(sizeof(struct fte3600_frame_buffer) == 32, "struct fte3600_frame_buffer must be 32 bytes");
  +#endif
  ```
- **Previous Behavior:** No static assertions in header.
- **New Behavior:** Enforces 32-byte struct layout at kernel compile time.
- **Locking Requirements:** N/A.
- **Hardware Communication:** None.
- **UAPI Compatibility Impact:** Safe and beneficial. Enforces invariant without modifying layout.

---

### 2.2 Modifications to `kernel/fte3600.c`

#### Modification 1: Declaration of `event_sequence` in `struct fte3600_data`
- **Diff:**
  ```diff
  @@ -38,6 +38,7 @@ struct fte3600_data {
   	struct device dev;             /* Embedded device providing kobject refcounting */
   	struct mutex lock;             /* Serializes reset, transport, and shutdown */
   	wait_queue_head_t event_wq;
  +	atomic64_t event_sequence;     /* Monotonic sequence counter to prevent torn reads and lost wakeups */
   	enum fte3600_state state;
   	atomic_t is_open;              /* Driver-owned single-client exclusive lease */
   	atomic_t offline;              /* Set to 1 on unbind/shutdown */
  ```
- **Previous Behavior:** `struct fte3600_data` had `event_wq` but no sequence counter.
- **New Behavior:** Adds `atomic64_t event_sequence` field.
- **Locking Requirements:** Manipulated via `atomic64_*` APIs.
- **Hardware Communication:** None.
- **UAPI Compatibility Impact:** None (internal driver struct).

#### Modification 2: Notification Helper `fte3600_notify_event()`
- **Diff:**
  ```diff
  +static inline void fte3600_notify_event(struct fte3600_data *data)
  +{
  +	atomic64_inc(&data->event_sequence);
  +	smp_mb__after_atomic();
  +	wake_up_interruptible(&data->event_wq);
  +}
  ```
- **Previous Behavior:** Helper did not exist.
- **New Behavior:** Increments `event_sequence`, executes full memory barrier, wakes `event_wq`.
- **Locking Requirements:** None specified; safe in process context.
- **Hardware Communication:** None.
- **UAPI Compatibility Impact:** None.
- **Adversarial Finding:** **ZERO CALLERS.** In the entire kernel module, this function is never called.

#### Modification 3: Poll Handler Update (`fte3600_cdev_poll()`)
- **Diff:**
  ```diff
   static __poll_t fte3600_cdev_poll(struct file *filp, struct poll_table_struct *wait)
   {
   	struct fte3600_data *data = filp->private_data;
  +	__poll_t mask = 0;
   
   	if (atomic_read(&data->offline))
   		return EPOLLERR | EPOLLHUP;
  @@ -537,7 +555,15 @@ static __poll_t fte3600_cdev_poll(struct file *filp, struct poll_table_struct *w
   	if (atomic_read(&data->offline))
   		return EPOLLERR | EPOLLHUP;
   
  -	return 0;
  +	/*
  +	 * Readiness check for future touch and capture operational phases:
  +	 * When data->state is TOUCH_DETECTED or FRAME_READY, data/event is available.
  +	 */
  +	if (data->state == FTE3600_STATE_TOUCH_DETECTED ||
  +	    data->state == FTE3600_STATE_FRAME_READY)
  +		mask |= EPOLLIN | EPOLLRDNORM;
  +
  +	return mask;
   }
  ```
- **Previous Behavior:** Checked `offline`, called `poll_wait()`, returned `0`.
- **New Behavior:** Checks `offline`, calls `poll_wait()`, re-checks `offline`, inspects `data->state` for `TOUCH_DETECTED` or `FRAME_READY`, and sets `EPOLLIN | EPOLLRDNORM`.
- **Locking Requirements:** Does NOT acquire `data->lock`. Reads `data->state` bare.
- **Hardware Communication:** None.
- **UAPI Compatibility Impact:** Changes return mask of `poll()` from 0 to potential read readiness.
- **Adversarial Finding:**
  1. `data->state` is read without synchronization (data race with state writers).
  2. The checked states are never assigned anywhere in the driver, so readiness is never reported.

#### Modification 4: Probe Initialization (`fte3600_probe()`)
- **Diff:**
  ```diff
  @@ -571,6 +597,7 @@ static int fte3600_probe(struct spi_device *spi)
   
   	mutex_init(&data->lock);
   	init_waitqueue_head(&data->event_wq);
  +	atomic64_set(&data->event_sequence, 0);
   	atomic_set(&data->is_open, 0);
   	atomic_set(&data->offline, 0);
   	data->state = FTE3600_STATE_UNINITIALIZED;
  ```
- **Previous Behavior:** Initialized mutex, waitqueue, atomic flags, and state.
- **New Behavior:** Additionally initializes `atomic64_set(&data->event_sequence, 0)`.
- **Locking Requirements:** Single-threaded probe context.
- **Hardware Communication:** None.
- **UAPI Compatibility Impact:** None.

#### Modification 5: Removal Teardown State Update (`fte3600_remove()`)
- **Diff:**
  ```diff
  @@ -659,6 +686,7 @@ static void fte3600_remove(struct spi_device *spi)
   	 * Mark offline and nullify pointers to devres-managed resources before remove() returns.
   	 */
   	atomic_set(&data->offline, 1);
  +	data->state = FTE3600_STATE_DEVICE_REMOVAL;
   	data->reset_gpiod = NULL;
   	data->spi = NULL;
  ```
- **Previous Behavior:** Marked `offline = 1` and nullified hardware pointers under `data->lock`. `data->state` was left untouched.
- **New Behavior:** Overwrites `data->state = FTE3600_STATE_DEVICE_REMOVAL` (13) under `data->lock`.
- **Locking Requirements:** Called with `mutex_lock(&data->lock)` held.
- **Hardware Communication:** None.
- **UAPI Compatibility Impact:** **DEFECT.** Causes subsequent `FTE3600_IOC_GET_STATE` calls to leak state value `13` to userspace.

---

### 2.3 Modifications to `kernel/fte3600-test.c`

#### Modification 1: Include `<stddef.h>`
- **Diff:**
  ```diff
  @@ -22,6 +22,7 @@
   #include <stdbool.h>
   #include <stdint.h>
   #include <stdio.h>
  +#include <stddef.h>
  ```
- **Impact:** Required for `offsetof()` macro. Harmless.

#### Modification 2: Introduction of Test Suite 7
- **Added Functions:**
  1. `test_d5_state_alias_and_extended_states()`: Verifies enum values and alias mapping.
  2. `test_d5_abi_compile_time_assertions()`: Verifies `_Static_assert` and `offsetof` values.
  3. `test_d5_event_sequence_concurrency_and_waitqueue()`: Userspace pthread simulation of a condition variable and atomic sequence counter.
  4. `test_d5_state_machine_14_states()`: Checks local enum variable assignment.
- **Runner Update:** Added Suite 7 to `main()`.

---

## 3. Test Validity Audit of Suite 7

| Test Name | What It Genuinely Verifies | What It Merely Simulates | Kernel Blind Spot | Concurrency Bug Immunity |
| :--- | :--- | :--- | :--- | :--- |
| `test_d5_state_alias_and_extended_states` | Userspace C enum integer values (`0..13`) and macro definition | Nothing | Does not test driver state transitions or client compatibility | None (cannot detect concurrency bugs) |
| `test_d5_abi_compile_time_assertions` | Userspace struct size (32 B) and member byte offsets | Nothing | Does not test ioctl copying or 32/64-bit kernel translation | None (pure layout check) |
| `test_d5_event_sequence_concurrency_and_waitqueue` | Pthread `pthread_cond_t` and `pthread_mutex_t` behavior in userspace | Synthetic `sim_event_wq_t` struct created inside the test file | **Does not execute any kernel driver code.** Does not use Linux `wait_queue_head_t` or kernel memory barriers | **High Risk:** Passes completely despite the fact that `event_sequence` has 0 callers and 0 reads in `kernel/fte3600.c` |
| `test_d5_state_machine_14_states` | That a local stack variable `enum fte3600_state s` can hold numbers 0..13 | Assigns integers to a local variable in a loop | Does not call any driver functions, does not check transitions, does not verify locking | **High Risk:** Pure tautology; proves nothing about the driver |

---

## 4. Identification of Source-Level Defects

### Defect 1: State Machine Leakage into Public UAPI
- **File:** `kernel/fte3600.c:440` and `kernel/fte3600.c:689`
- **Severity:** HIGH (ABI Violation)
- **Description:** In `fte3600_remove()`, `data->state` is set to `FTE3600_STATE_DEVICE_REMOVAL` (13). When a userspace process with an open file descriptor invokes `FTE3600_IOC_GET_STATE`, `snap.state` is assigned directly from `data->state`:
  ```c
  snap.state = (u32)data->state;
  ```
  Userspace receives state `13`. This violates backward compatibility for existing clients expecting states `0..8`, contradicts the documented invariant that extended states are strictly internal, and desynchronizes with existing test `test_scenario_5_get_state_after_removal` which asserts `snap.state == FTE3600_STATE_UNINITIALIZED`.
- **Recommended Minimal Fix:**
  In `fte3600_ioctl_common()`, sanitize or filter internal states before copying to `snap.state`. If `data->state >= FTE3600_STATE_WAITING_FOR_TOUCH`, map it to `FTE3600_STATE_UNKNOWN` or preserve the last public state, and rely on `snap.flags |= FTE3600_FLAG_OFFLINE` to communicate removal.

### Defect 2: Dead Concurrency Code (`event_sequence`)
- **File:** `kernel/fte3600.c:122-127`
- **Severity:** MEDIUM (Dead Code / Unverified Claim)
- **Description:** `fte3600_notify_event()` was implemented and documented as the primary defense against lost wakeups, but it is **never invoked** anywhere in the driver. Furthermore, `data->event_sequence` is never read by any function in `kernel/fte3600.c`.
- **Recommended Minimal Fix:**
  Either defer `event_sequence` until the threaded IRQ handler is implemented in future stages, or wire callers and readers into the driver wait interfaces.

### Defect 3: Unsynchronized State Inspection in Poll
- **File:** `kernel/fte3600.c:562-564`
- **Severity:** LOW/MEDIUM (Data Race)
- **Description:** `fte3600_cdev_poll()` reads `data->state` without holding `data->lock` and without `READ_ONCE()`, while all state modifications are performed under `mutex_lock(&data->lock)`.
- **Recommended Minimal Fix:**
  Use `READ_ONCE(data->state)` or acquire `data->lock` / use an atomic state variable to ensure memory model correctness.

---

## 5. Audit Verdict

**BLOCKED — CRITICAL SOURCE-LEVEL DEFECTS FOUND**
