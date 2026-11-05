# STAGE D5: BOUNDED SOURCE-ONLY DRIVER IMPLEMENTATION REPORT

## Document Information
- **Project:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`) Native Linux Fingerprint Driver
- **Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`, CS0)
- **Host Controller:** Intel LPSS SPI2 (`pxa2xx-spi.2`) on Linux `7.2.8-9-omarchy` (x86_64 SMP)
- **Author:** Lead Kernel Architect & Safety Auditor (Antigravity Agent)
- **Status:** COMPLETED BOUNDED SOURCE-ONLY IMPLEMENTATION
- **Safety Boundary:** STRICTLY SOURCE-ONLY IMPLEMENTATION (Zero Hardware Communication, Zero Module Loading)

---

## 1. Executive Summary

Stage D5 executes a strictly bounded, source-only implementation of the core driver architecture and synchronization foundations, derived from the rigorous evidence audits of Stages D4.3 and D4.4. 

This stage implements only those software foundations that can be fully justified from Linux kernel concurrency semantics and standard UAPI guarantees, without relying on unverified physical FT9362 silicon assumptions. 

### Key Implementation Milestones:
1. **Zero Hardware Operations:** No SPI transactions, register reads/writes, GPIO manipulation, module unloads/reloads, or hardware ioctls were executed. The running kernel driver remains loaded and bound in its verified Stage C state (`8 (IDENTIFIED)`).
2. **Strict UAPI v1 Preservation:** All existing ioctl numbers, encodings, structure layouts, alignment bounds, and public state values (0 through 8) remain bit-identical. Output reserved fields remain zero on all v1 query paths.
3. **State Machine Reconciliation:** Formally reconciled the state definitions into a 14-state unified architecture in `kernel/fte3600.h`. Preserved states 0 through 8 as public UAPI states; defined states 9 through 13 as internal driver states; established `#define FTE3600_STATE_INITIALIZED FTE3600_STATE_READY` as a legacy compatibility alias.
4. **Concurrency & Memory Model Hardening:** Added an `atomic64_t event_sequence` to `struct fte3600_data` in `kernel/fte3600.c` to prevent torn 64-bit reads on 32-bit architectures. Implemented `fte3600_notify_event()` with mandatory `smp_mb__after_atomic()` ordering before `wake_up_interruptible()`.
5. **Clean Device Removal Lifecycle:** Enhanced `fte3600_remove()` to set `FTE3600_STATE_DEVICE_REMOVAL` under lock and execute `wake_up_all(&data->event_wq)`, guaranteeing that userspace threads sleeping in waitqueues unblock immediately with `-ENODEV`.
6. **Userspace Test Suite Expansion:** Expanded `kernel/fte3600-test.c` with Suite 7, validating the 14-state transitions, atomic64 sequence ordering, waitqueue unblock semantics, and C11 `_Static_assert` structure bounds. The test harness passed 38 out of 38 tests (100.0%).
7. **Clean Kernel Build:** Compiled the driver against running kernel headers (`7.2.8-9-omarchy`) with zero warnings and zero errors. The resulting `.ko` was verified on disk and NOT loaded into the running kernel.

---

## 2. Modified Source Files & Documented Changes

### 2.1 File: `kernel/fte3600.h`
- **Location:** Lines 27–52, 107–118
- **Why Changed:** To reconcile the state machine with the 14-state architecture, provide the `FTE3600_STATE_INITIALIZED` compatibility alias, and introduce compile-time size assertions.
- **What Changed:**
  1. Updated `enum fte3600_state` to explicitly demarcate public UAPI states 0 through 8, and appended internal states `WAITING_FOR_TOUCH` (9), `TOUCH_DETECTED` (10), `FRAME_READY` (11), `SUSPENDED` (12), and `DEVICE_REMOVAL` (13).
  2. Added `#define FTE3600_STATE_INITIALIZED FTE3600_STATE_READY`.
  3. Added kernel-mode compile-time static assertions (`static_assert`) verifying that `struct fte3600_state_snapshot`, `struct fte3600_init_params`, and `struct fte3600_frame_buffer` are each exactly 32 bytes.
- **UAPI Impact:** NONE. Public states 0..8 have identical numeric values; all structures maintain exact 32-byte size and offsets; all ioctl definitions (`GET_STATE`, `RESET`, `INIT`, `CAPTURE`) are untouched.
- **Hardware Trigger Capability:** ZERO. Header definitions cannot trigger hardware communication.
- **Covering Tests:** `test_uapi_sizes`, `test_uapi_constants`, `test_d5_state_alias_and_extended_states`, `test_d5_abi_compile_time_assertions`.

### 2.2 File: `kernel/fte3600.c`
- **Location:** Lines 34–48, 112–126, 544–568, 598–605, 684–692
- **Why Changed:** To implement software concurrency primitives (`atomic64_t event_sequence`), enforce memory barrier ordering, support poll readiness, and guarantee non-blocking device teardown.
- **What Changed:**
  1. Added `atomic64_t event_sequence;` to `struct fte3600_data`.
  2. Added `fte3600_notify_event()` helper function executing `atomic64_inc(&data->event_sequence)`, followed by `smp_mb__after_atomic()` and `wake_up_interruptible(&data->event_wq)`.
  3. Updated `fte3600_probe()` to initialize `atomic64_set(&data->event_sequence, 0)`.
  4. Updated `fte3600_cdev_poll()` to evaluate event readiness (`EPOLLIN | EPOLLRDNORM`) when `state == FTE3600_STATE_TOUCH_DETECTED` or `state == FTE3600_STATE_FRAME_READY`.
  5. Updated `fte3600_remove()` to set `data->state = FTE3600_STATE_DEVICE_REMOVAL;` under `data->lock`, followed by `wake_up_all(&data->event_wq)`.
- **UAPI Impact:** NONE. Internal synchronization improvement only.
- **Hardware Trigger Capability:** ZERO. Helper is not connected to physical IRQ line; physical sensor register accesses remain strictly barred.
- **Covering Tests:** `test_scenario_1_open_release`, `test_scenario_2_open_remove_release`, `test_scenario_5_get_state_after_removal`, `test_d5_event_sequence_concurrency_and_waitqueue`.

### 2.3 File: `kernel/fte3600-test.c`
- **Location:** Lines 22–32, 1623–1785
- **Why Changed:** To add Suite 7 testing Stage D5 concurrency, extended states, structure alignment, and waitqueue unblocking.
- **What Changed:**
  1. Included `<stddef.h>` for `offsetof` macro.
  2. Added `test_d5_state_alias_and_extended_states()` validating enum values and alias mapping.
  3. Added `test_d5_abi_compile_time_assertions()` validating C11 `_Static_assert` on all UAPI structs and field offsets.
  4. Added `test_d5_event_sequence_concurrency_and_waitqueue()` simulating multi-threaded event signaling and device removal wakeups.
  5. Added `test_d5_state_machine_14_states()` testing legal and error transitions across all 14 states.
  6. Updated `main()` to run all 38 tests.
- **UAPI Impact:** NONE. Test harness code.
- **Hardware Trigger Capability:** ZERO. Operates strictly in userspace simulation.
- **Covering Tests:** Self-verifying test suite.

---

## 3. State Machine Reconciliation & Transition Matrix

The driver architecture formally recognizes **14 distinct states**:

```
[0: UNINITIALIZED]
       | (FTE3600_IOC_RESET / Pad 14 Low)
[1: RESETTING]
       | (Pad 14 High + 10ms settling)
[2: RESET_SEQUENCE_ISSUED] <---------------+
       | (FTE3600_IOC_INIT v1 / Stage C)   |
[8: IDENTIFIED]                            |
       | (FTE3600_IOC_INIT v2 / Calibrate) |
       +---> [3: INITIALIZING]             |
                   |                       |
[4: READY] <-------+                       |
  |  ^                                     |
  |  | (SIGINT abort / Disarm)             |
  |  +----------------+                    |
  v                   |                    |
[9: WAITING_FOR_TOUCH] |                   | (Hardware Reset)
  |                   |                    |
  | (IRQ fired / ack) |                    |
  v                   |                    |
[10: TOUCH_DETECTED]  |                    |
  |                   |                    |
  | (FTE3600_IOC_CAPTURE)                  |
  v                                        |
[5: CAPTURING]                             |
  |                                        |
  | (FIFO read complete)                   |
  v                                        |
[11: FRAME_READY]                          |
  |                                        |
  | (Payload copied to user)               |
  +---> [4: READY]                         |
                                           |
[6: ERROR] --------------------------------+
[7: UNKNOWN]
[12: SUSPENDED]
[13: DEVICE_REMOVAL]
```

### Transition Status & Implementation Boundary:

| State | Name | UAPI Visibility | Implemented in D5? | Transition Boundary |
| :---: | :--- | :--- | :---: | :--- |
| **0** | `UNINITIALIZED` | Public UAPI | **YES** | Initial state set in `probe()`. |
| **1** | `RESETTING` | Public UAPI | **YES** | Set during `FTE3600_IOC_RESET` pulse. |
| **2** | `RESET_SEQUENCE_ISSUED` | Public UAPI | **YES** | Set when electrical pulse completes. |
| **3** | `INITIALIZING` | Public UAPI | **PROPOSAL** | Reserved for future Stage F calibration. |
| **4** | `READY` (alias `INITIALIZED`) | Public UAPI | **PROPOSAL** | Post-calibration standby (quiescent). |
| **5** | `CAPTURING` | Public UAPI | **PROPOSAL** | Reserved for future Stage G frame acquisition. |
| **6** | `ERROR` | Public UAPI | **YES** | Set on hardware bus failure or CRC error. |
| **7** | `UNKNOWN` | Public UAPI | **YES** | Fail-closed state on invalid register responses. |
| **8** | `IDENTIFIED` | Public UAPI | **YES** | Active state from Stage C verification. |
| **9** | `WAITING_FOR_TOUCH` | Internal Ext | **PROPOSAL** | Software constant defined; no active IRQ arming. |
| **10**| `TOUCH_DETECTED` | Internal Ext | **PROPOSAL** | Software constant defined; evaluated in `poll`. |
| **11**| `FRAME_READY` | Internal Ext | **PROPOSAL** | Software constant defined; evaluated in `poll`. |
| **12**| `SUSPENDED` | Internal Ext | **PROPOSAL** | Software constant defined; for future PM callbacks. |
| **13**| `DEVICE_REMOVAL` | Internal Ext | **YES** | Set in `fte3600_remove()` under `data->lock`. |

---

## 4. Software Concurrency & Event Foundation

### 4.1 Monotonic Sequence Counter (`atomic64_t`)
- On 32-bit x86 systems, an ordinary `u64` read requires two 32-bit `mov` instructions. Concurrent reads and writes can interleave, producing a **torn read** (a corrupt composite value combining the upper word of one epoch with the lower word of another).
- `atomic64_t event_sequence` ensures that all reads (`atomic64_read()`) and writes (`atomic64_inc()`) execute atomically via locked bus cycles (`lock cmpxchg8b` on x86-32 or 64-bit load/store on x86-64).

### 4.2 Producer / Consumer Memory Ordering Barrier
Per Linux kernel memory-barrier rules (`Documentation/memory-barriers.txt`):
- `atomic64_inc()` does not imply a memory barrier.
- `wake_up_interruptible()` acquires `wq_head->lock`, which provides ACQUIRE semantics, NOT RELEASE semantics. Preceding writes could leak past or be delayed in CPU store buffers.
- `smp_mb__after_atomic()` provides a bidirectional full memory barrier (`smp_mb()`), guaranteeing:
  $$\text{State Store} \prec \text{Counter Inc} \prec \text{smp\_mb} \prec \text{Waitqueue Wakeup}$$
- On the consumer side, `wait_event_interruptible()` executes `prepare_to_wait_event()`, inserting a barrier between task state configuration (`TASK_INTERRUPTIBLE`) and predicate evaluation.

### 4.3 Lock Ordering & Deadlock Prevention
- **Rule 1:** A mutex MUST NOT be held across a call to `wait_event_interruptible()` if the waking thread (such as the IRQ thread) must acquire that same mutex to process the event.
- **Rule 2:** The wait sequence releases `data->lock` before sleeping and re-acquires `data->lock` upon wake-up.
- **Rule 3:** The device-removal sequence acquires `data->lock`, sets `offline = 1`, releases `data->lock`, and calls `wake_up_all(&data->event_wq)`. All waiters immediately awaken, observe `offline == 1`, and return `-ENODEV`.

---

## 5. Build & Verification Results

### 5.1 Standalone Test Harness (`make -C kernel check`)
```text
===================================================================
 FTE3600 Unified Kernel Driver Test Suite (Stage D5 Hardened)
 Mode: Standalone Userspace Simulation (Zero Hardware Access)
===================================================================

--- Test Suite 1: UAPI & Data Structure Alignment ---
  Testing: UAPI structures exact 32-byte size and alignment ... [PASS]
  Testing: UAPI protocol constants (10246 B wire, 10240 B image, v1) ... [PASS]
  Testing: IOCTL command encodings (magic 'F', sizes, directions) ... [PASS]

--- Test Suite 2: Protocol CRC Vector Validation ---
  Testing: Wire CRC (0x1C53) and Word LE CRC (0x313E, 0x5B7C, 0xE4AA) ... [PASS]

--- Test Suite 3: State Machine Transitions & Invariants ---
  Testing: 9-state model, UNKNOWN fail-closed invariant, reset recovery ... [PASS]

--- Test Suite 4: Stage 2E.2.2 Mandatory Lifecycle & Concurrency Scenarios ---
  Testing: Scenario 1: Open followed by normal release ... [PASS]
  Testing: Scenario 2: Open followed by device removal and release ... [PASS]
  Testing: Scenario 3: Removal racing with ioctl waiting for mutex ... [PASS]
  Testing: Scenario 4: Removal racing with ioctl holding mutex ... [PASS]
  Testing: Scenario 5: GET_STATE after removal ... [PASS]
  Testing: Scenario 6: RESET attempted after removal ... [PASS]
  Testing: Scenario 7: Multiple open attempts (single-client exclusive lease) ... [PASS]
  Testing: Scenario 8: Reference balance on probe/open error paths ... [PASS]
  Testing: Scenario 9: SPI helper rejects invalid arguments (NULL, 0-len) ... [PASS]
  Testing: Scenario 10: SPI helper rejects transfers beyond max limit ... [PASS]
  Testing: Scenario 11: No hardware access possible after offline ... [PASS]

--- Test Suite 5: UAPI Validation & Parameter Rejections ---
  Testing: IOCTL validation, version check, reserved fields, and -EOPNOTSUPP ... [PASS]

--- Test Suite 6: Stage 2E.4.2 Chip Identification & Completion Semantics ---
  Testing: Scenario 1: Valid wake response (rx_c6[4] == 0x01) ... [PASS]
  Testing: Scenario 2: Invalid wake response (rx_c6[4] != 0x01) ... [PASS]
  Testing: Scenario 3: Valid FT9362 ID and CRC (0x9362 and 0x1C53) ... [PASS]
  Testing: Scenario 4: CRC mismatch ... [PASS]
  Testing: Scenario 5: Unexpected chip ID (0x9368, 0x9391) ... [PASS]
  Testing: Scenario 6: All-zero response (0x0000) ... [PASS]
  Testing: Scenario 7: All-ones response (0xFFFF) ... [PASS]
  Testing: Scenario 8: SPI transfer error (-EIO) ... [PASS]
  Testing: Scenario 9: Short/incomplete transfer (m.status != 0, actual_length < len) ... [PASS]
  Testing: Scenario 10: Failure after wake write ... [PASS]
  Testing: Scenario 11: INIT from invalid state (UNINIT, INITING, READY, IDENTIFIED, UNKNOWN) ... [PASS]
  Testing: Scenario 12: INIT while device offline (-ENODEV) ... [PASS]
  Testing: Scenario 13: Repeated INIT after success (-EPERM) ... [PASS]
  Testing: Scenario 14: Concurrent RESET and INIT serialization under mutex ... [PASS]
  Testing: Scenario 15: Removal during an operation ... [PASS]
  Testing: Scenario 16: SPI completion semantics (actual_length == len, status, ret) ... [PASS]
  Testing: Scenario 17: Semantic boundary (IDENTIFIED vs READY, CAPTURE rejection) ... [PASS]

--- Test Suite 7: Stage D5 Architecture & Concurrency Foundations ---
  Testing: Stage D5: State alias INITIALIZED == READY (4) and extended states (9..13) ... [PASS]
  Testing: Stage D5: UAPI struct 32-byte layout & field offset static assertions ... [PASS]
  Testing: Stage D5: atomic64 sequence ordering & waitqueue unblock simulation ... [PASS]
  Testing: Stage D5: Unified 14-state model transition path & error recovery ... [PASS]

===================================================================
 Stage D5 Test Results: 38 / 38 Passed (100.0%)
===================================================================
./fte3600-policy-test
FTE3600 glue policy: 9 groups passed (GPIO/ACPI IRQ, CS capability, polarity, lease, PM, removal)
```

### 5.2 Kernel Module Compilation (`make -C kernel all`)
- **Host Kernel Build:** Linux `7.2.8-9-omarchy` (x86_64)
- **Toolchain:** GCC with standard kernel kbuild flags
- **Compilation Output:** Zero warnings, zero errors. Built `fte3600.ko` with valid BTF type information.
- **Loading Status:** **NOT LOADED**. The module was compiled on disk for binary verification only; the live running driver was not reloaded.

---

## 6. Final Passive Invariant Verification

Passive checks confirm that the physical hardware and running driver remained completely quiescent and untouched throughout Stage D5:

| Metric / Parameter | Pre-Stage Value | Post-Stage Value | Status |
| :--- | :---: | :---: | :---: |
| **Driver Module Status** | `fte3600 20480 0 - Live` | `fte3600 20480 0 - Live` | **CONFIRMED** |
| **SPI Slave Device** | `spi-FTE3600:00` | `spi-FTE3600:00` | **CONFIRMED** |
| **Character Device Node** | `511, 0` (`/dev/fte3600`) | `511, 0` (`/dev/fte3600`) | **CONFIRMED** |
| **Driver State Machine** | `8 (IDENTIFIED)` | `8 (IDENTIFIED)` | **CONFIRMED** |
| **SPI Bus Transfers** | `3` | `3` | **CONFIRMED** |
| **SPI Bus Bytes Transferred** | `19` | `19` | **CONFIRMED** |

---

## 7. Final Classification

**PASS — BOUNDED SOURCE-ONLY IMPLEMENTATION COMPLETE**
