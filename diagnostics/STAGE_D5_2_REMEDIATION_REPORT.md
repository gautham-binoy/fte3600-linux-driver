# STAGE D5.2: MINIMAL SOURCE-LEVEL DEFECT REMEDIATION REPORT

## Document Information
- **Project:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`) Native Linux Fingerprint Driver
- **Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`, CS0)
- **Host Controller:** Intel LPSS SPI2 (`pxa2xx-spi.2`) on Linux `7.2.8-9-omarchy` (x86_64 SMP)
- **Author:** Lead Kernel Architect & Remediation Specialist (Antigravity Agent)
- **Status:** COMPLETED DEFECT REMEDIATION
- **Safety Boundary:** STRICTLY SOURCE-ONLY REMEDIATION (Zero Hardware Communication, Zero Module Loading)

---

## 1. Executive Summary

Stage D5.2 has successfully resolved the critical source-level defects identified in the Stage D5.1 adversarial audit. All remediations were implemented strictly within source files without altering the existing UAPI numbers, without changing structure memory layouts, without physical hardware communication, and without loading the newly built module into kernel memory.

### Key Remediations Executed:
1. **Public State Range Isolation:** Implemented `fte3600_state_to_uapi()` in `kernel/fte3600.h` and wired it into `FTE3600_IOC_GET_STATE` in `kernel/fte3600.c`. Internal state `FTE3600_STATE_DEVICE_REMOVAL` (13) is strictly mapped to `FTE3600_STATE_UNINITIALIZED` (0) with `FTE3600_FLAG_OFFLINE` asserted. No state value above 8 can leak across the v1 UAPI interface.
2. **Mock Test Desynchronization Fixed:** Updated `sim_driver_remove()` in `kernel/fte3600-test.c` to assign `ctx->state = FTE3600_STATE_DEVICE_REMOVAL` under lock, and updated `sim_driver_ioctl_get_state()` to use `fte3600_state_to_uapi()`. The mock test now genuinely models the real driver behavior.
3. **Dead Concurrency Infrastructure Pruned:** Removed the unused `atomic64_t event_sequence` field from `struct fte3600_data`, removed the unreferenced `fte3600_notify_event()` helper, and removed the synthetic pthread waitqueue simulation that misleadingly claimed to validate kernel waitqueues.
4. **Poll Synchronization & Readiness Hardened:** Cleaned up `fte3600_cdev_poll()` to eliminate bare unsynchronized reads of `data->state`. Removed premature advertising of `EPOLLIN | EPOLLRDNORM` while touch/capture streaming is deferred. Established consistent lockless inspection of `data->offline` for unbind/removal termination.
5. **Rigorous Test Suite Refactored:** Replaced synthetic tests in Suite 7 with 4 authentic tests validating state mapping bounds, ABI structure assertions, removal GET_STATE guarantees, and poll lifecycle behavior. All 38 standalone tests and 9 policy groups passed (100.0%).

---

## 2. Exact Git Diff Summary

```text
 M kernel/fte3600-test.c
 M kernel/fte3600.c
 M kernel/fte3600.h
```

### 2.1 Modifications to `kernel/fte3600.h`
- Added `#define FTE3600_MAX_PUBLIC_STATE 8`.
- Added inline helper `fte3600_state_to_uapi(enum fte3600_state state)` to map all states:
  * Public states `0..8` map to `0..8` unchanged.
  * Internal state `13` (`DEVICE_REMOVAL`) maps to `0` (`UNINITIALIZED`).
  * Internal states `9..11` (`WAITING_FOR_TOUCH`, `TOUCH_DETECTED`, `FRAME_READY`) map to `4` (`READY`).
  * Internal state `12` (`SUSPENDED`) maps to `7` (`UNKNOWN`).
  * Default / out-of-range maps to `0` (`UNINITIALIZED`).
- Preserves legacy alias `#define FTE3600_STATE_INITIALIZED FTE3600_STATE_READY` (4).
- Retained compile-time `static_assert` verifying all UAPI structures are 32 bytes.

### 2.2 Modifications to `kernel/fte3600.c`
- Removed `atomic64_t event_sequence` from `struct fte3600_data`.
- Removed dead helper `fte3600_notify_event()`.
- Removed `atomic64_set(&data->event_sequence, 0)` from `fte3600_probe()`.
- Updated `FTE3600_IOC_GET_STATE` handler:
  * `snap.state = fte3600_state_to_uapi(data->state);`
  * Asserted `snap.flags |= FTE3600_FLAG_OFFLINE` if `offline` is set or if state is `DEVICE_REMOVAL`.
- Hardened `fte3600_cdev_poll()`:
  * Removed unsynchronized read of `data->state`.
  * Removed unreachable `EPOLLIN | EPOLLRDNORM` checks.
  * Returns `EPOLLERR | EPOLLHUP` if offline; otherwise registers on `data->event_wq` and returns 0.

### 2.3 Modifications to `kernel/fte3600-test.c`
- Included `<sys/epoll.h>`.
- Updated `sim_driver_remove()` to set `ctx->state = FTE3600_STATE_DEVICE_REMOVAL`.
- Updated `sim_driver_ioctl_get_state()` to invoke `fte3600_state_to_uapi(ctx->state)`.
- Updated Scenario 4 and Scenario 15 post-removal assertions to verify transition to `DEVICE_REMOVAL`.
- Replaced synthetic Suite 7 with 4 verified tests:
  1. `test_d5_2_state_mapping_and_uapi_isolation`: Verifies all public and extended mappings.
  2. `test_d5_2_abi_compile_time_and_reserved_assertions`: Verifies 32-byte layout, member offsets, and zeroed reserved words.
  3. `test_d5_2_removal_get_state_and_uapi_guarantee`: Proves removal never leaks state 13 to userspace.
  4. `test_d5_2_poll_lifecycle_and_offline_conditions`: Proves poll returns 0 when online and `EPOLLERR | EPOLLHUP` upon removal.

---

## 3. Four-Tier Verification Boundary Review

| Verification Tier | Execution Environment | Status | Scope |
| :--- | :--- | :---: | :--- |
| **Tier 1: Source Checks** | Compiler Static Assertions | **VERIFIED** | Enforces 32-byte struct layout and offset invariants |
| **Tier 2: Mock Tests** | Userspace Simulation (`make check`) | **VERIFIED** | 38 / 38 passed (100.0%); 9 / 9 policy groups passed |
| **Tier 3: Kbuild Build** | Kernel Kbuild against `7.2.8-9-omarchy` | **VERIFIED** | Clean build (0 warnings, 0 errors); module NOT loaded |
| **Tier 4: Physical Hardware** | SPI Bus / Sensor Silicon | **NOT PERFORMED** | Barred by safety gates; physical state 100% untouched |

---

## 4. Passive Invariant Verification

| Host Invariant | Pre-Stage Value | Post-Stage Value | Status |
| :--- | :---: | :---: | :---: |
| **Module Load State** | `fte3600 20480 0 - Live` | `fte3600 20480 0 - Live` | **CONFIRMED UNCHANGED** |
| **SPI Slave Device** | `spi-FTE3600:00` | `spi-FTE3600:00` | **CONFIRMED BOUND** |
| **Device Node** | `/dev/fte3600` (`511, 0`) | `/dev/fte3600` (`511, 0`) | **CONFIRMED UNTOUCHED** |
| **Driver State Machine** | `8 (FTE3600_STATE_IDENTIFIED)` | `8 (FTE3600_STATE_IDENTIFIED)` | **CONFIRMED UNCHANGED** |
| **Physical SPI Transfers** | `3` | `3` | **CONFIRMED (0 added)** |
| **Physical SPI Bytes** | `19` | `19` | **CONFIRMED (0 added)** |

---

## 5. Final Stage Verdict

**PASS — D5.2 SOURCE DEFECTS REMEDIATED**
