# Stage 2C.5C.3 — Offline Test Report

## Executive Summary & Build Verification

The complete automated test suite for the FW9369 mock and hardware transport backend was compiled and executed in `/home/titan/fte3600-linux-driver/tools/fw9369-mock/`.

- **Target Binary:** `../../bin/fw9369-tests`
- **Compiler Invocations:** `cc -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Iinclude`
- **Build Status:** **CLEAN (0 Warnings, 0 Errors)**
- **Execution Mode:** Strictly offline in-memory execution (Zero physical hardware access)
- **Total Test Results:** **181 / 181 Passed (100.0%)** (Up from 159 in Stage 2C.5C.2)

---

## 1. Test Suite Summary Table

| Suite # | Test Suite Description | Total Assertions | Status |
| :---: | :--- | :---: | :---: |
| 1 | CRC Implementations & Vectors | 5 | **PASS** |
| 2 | Protocol Framing & Allowlist | 15 | **PASS** |
| 3 | Mock Transport Invariants & Safety | 7 | **PASS** |
| 4 | FDT Calibration & Minimum Tracking | 7 | **PASS** |
| 5 | Image Baseline & Single-Frame Copy | 5 | **PASS** |
| 6 | Full Pipeline & Protected Register Cleanup | 8 | **PASS** |
| 7 | Deterministic Failure Injection Scenarios | 9 | **PASS** |
| 8 | Hardware Transport Offline Validation & Isolation | 13 | **PASS** |
| 9 | Physical Device Identity & Sysfs Hierarchy | 14 | **PASS** |
| 10 | Adversarial Protocol Hardening & Edge Cases | 44 | **PASS** |
| 11 | Sensor State Uncertainty & Recovery-State Hardening | 22 | **PASS** |
| 12 | **Independent Lock-State Transition & Recovery Audit (NEW)** | **32** | **PASS** |
| **Total** | **Comprehensive Automated Test Suite** | **181** | **100.0% PASS** |

---

## 2. Detailed Breakdown of Stage 2C.5C.3 New Test Suite (Suite 12)

Test Suite 12 was added to verify every state transition and edge case mandated by the Stage 2C.5C.3 audit specification:

### 1. Lock Attempted Directly from UNKNOWN
- **Test:** Context created via `fw9369_hw_transport_create_with_ops()` begins in `FW9369_LOCK_STATE_UNKNOWN`. Software transmits lock packet `[0x09, 0xF6, 0x9A, 0x00]`.
- **Result:** `ret == 0`, state transitions to `FW9369_LOCK_STATE_ASSUMED_LOCKED`.
- **Finding:** Verified audited implementation behavior. Documented as an unproven assumption that should be fail-closed in future revisions.

### 2. Repeated Lock Command While Already ASSUMED_LOCKED
- **Test:** Transport is in `ASSUMED_LOCKED`; software transmits another lock packet `0x9A = 0x00`.
- **Result:** `ret == 0`, state remains `FW9369_LOCK_STATE_ASSUMED_LOCKED`.

### 3. Unlock Attempted Directly from UNKNOWN
- **Test:** Context in `UNKNOWN`; software transmits unlock packet `[0x09, 0xF6, 0x9A, 0x5A]`.
- **Result:** `ret == 0`, state transitions to `FW9369_LOCK_STATE_ASSUMED_UNLOCKED`.

### 4. Lock Attempted After Uncertain Unlock (Scenario A)
- **Test 4a (Lock Fails):** Unlock fails with simulated `-EIO`, transitioning state to `UNKNOWN`. Subsequent lock packet also fails with `-EIO`.
- **Result:** `ret == -EIO`, state safely preserved as `UNKNOWN`.
- **Test 4b (Lock Succeeds):** Subsequent lock packet succeeds with `ret == 0`.
- **Result:** State transitions to `ASSUMED_LOCKED`.

### 5. Lock Attempted After Uncertain Lock (Scenario B)
- **Test 5a (Lock Fails):** Transport unlocked (`ASSUMED_UNLOCKED`), lock attempt fails with `-EIO`, transitioning state to `UNKNOWN`. Subsequent lock attempt also fails with `-EIO`.
- **Result:** State safely preserved as `UNKNOWN`.
- **Test 5b (Lock Succeeds):** Subsequent lock attempt succeeds with `ret == 0`.
- **Result:** State transitions to `ASSUMED_LOCKED`.

### 6. Protected Write During `UNLOCK_COMMAND_SENT` (In-Flight State)
- **Test:** System-call hook intercepts the active `ioctl(SPI_IOC_MESSAGE(1))` while transmitting the unlock packet. While inside the hook, `fw9369_hw_get_lock_state(t)` is verified to be `FW9369_LOCK_STATE_UNLOCK_COMMAND_SENT`. The hook attempts a protected write to analog trim register `0x00C0`.
- **Result:** The write is rejected with `-EPERM` at the allowlist boundary without calling `ioctl`. `hook_in_flight_unlock_observed` and `hook_in_flight_unlock_trim_rejected` both evaluated to `true`.

### 7. Protected Write During `LOCK_COMMAND_SENT` (In-Flight State)
- **Test:** System-call hook intercepts the active `ioctl` while transmitting the lock packet. While inside the hook, `fw9369_hw_get_lock_state(t)` is verified to be `FW9369_LOCK_STATE_LOCK_COMMAND_SENT`. The hook attempts a protected write to analog trim register `0x00C0`.
- **Result:** The write is rejected with `-EPERM`. `hook_in_flight_lock_observed` and `hook_in_flight_lock_trim_rejected` both evaluated to `true`.

### 8. Context Destruction While ASSUMED_UNLOCKED
- **Test:** Context is placed into `ASSUMED_UNLOCKED` and destroyed via `fw9369_hw_transport_destroy(t)`.
- **Result:** The file descriptor is deterministically closed, memory is freed cleanly, and no assertion or crash occurs.

### 9. Context Recreation Followed by Protected Write (Scenario D)
- **Test:** A new transport context is created. Before any unlock command is issued, software attempts a protected write to `0x00C0`.
- **Result:** The transfer returns `-EPERM` immediately without calling `ioctl`.

### 10. Clean Teardown of Multiple Contexts
- **Test:** Verified clean destruction of remaining contexts (`t`, `t_fresh`).
- **Result:** All allocated resources released cleanly.

---

## 3. Epistemic Boundary Notice

> [!IMPORTANT]
> All 181 tests verify solely the software logic, memory safety, system-call hooking, and protocol framing invariants of the userspace mock and hardware transport drivers. They **DO NOT** prove physical silicon behavior, nor do they validate FT9362 internal register states.
