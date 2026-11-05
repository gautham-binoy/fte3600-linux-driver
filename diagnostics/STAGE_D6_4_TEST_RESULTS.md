# STAGE D6.4: TEST RESULTS AND VERIFICATION REPORT

**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Workspace:** `/home/titan/fte3600-linux-driver/`  
**Execution Mode:** Standalone Userspace Simulation (Zero Hardware Access)  
**Safety Gate:** STRICTLY PRESERVED. Zero physical SPI or ioctl operations.  

---

## 1. Executive Summary

All 60 unit tests across Suites 1 through 9 passed with 100% success. In addition, the 9 architecture and glue policy groups in `kernel/fte3600-policy-test` passed cleanly. The newly built kernel module `kernel/fte3600.ko` compiled with zero compiler warnings and zero errors under kernel `7.2.8-9-omarchy`.

---

## 2. Test Execution Log

```
make: Entering directory '/home/titan/fte3600-linux-driver/kernel'
cc -std=c11 -Wall -Wextra -Werror -O2 -pthread -o fte3600-test fte3600-test.c
./fte3600-test
===================================================================
 FTE3600 Unified Kernel Driver Test Suite (Stage D6.4 Remediated)
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
  Testing: Scenario 11: INIT from invalid state (UNINIT, INITING, READY, UNKNOWN) ... [PASS]
  Testing: Scenario 12: INIT while device offline (-ENODEV) ... [PASS]
  Testing: Scenario 13: Repeated INIT after success (-EPERM) ... [PASS]
  Testing: Scenario 14: Concurrent RESET and INIT serialization under mutex ... [PASS]
  Testing: Scenario 15: Removal during an operation ... [PASS]
  Testing: Scenario 16: SPI completion semantics (actual_length == len, status, ret) ... [PASS]
  Testing: Scenario 17: Semantic boundary (IDENTIFIED vs READY, CAPTURE rejection) ... [PASS]

--- Test Suite 7: Stage D5.2 Remediation, UAPI Mapping, and Poll Verification ---
  Testing: Stage D5.2: State mapping and UAPI range isolation ([0..8]) ... [PASS]
  Testing: Stage D5.2: ABI structure layout (32 B) and reserved field assertions ... [PASS]
  Testing: Stage D5.2: Removal GET_STATE guarantee (state 13 never leaks, OFFLINE flag set) ... [PASS]
  Testing: Stage D5.2: Poll lifecycle simulation (offline terminal condition, no phantom readiness) ... [PASS]

--- Test Suite 8: Stage D6.2 FT9362 Volatile Startup Verification ---
  Testing: Scenario 1: Startup successful (SMIC Fab AA: 0x4C, period 150 us, UNCALIBRATED, 16 ops) ... [PASS]
  Testing: Scenario 2: Startup successful (Non-SMIC Fab: 0x00, period 200 us, UNCALIBRATED, 16 ops) ... [PASS]
  Testing: Scenario 3: Status 0x80 non-idle recovery path (abort strobe recovery, 18 ops) ... [PASS]
  Testing: Scenario 4: Status 0x80 persistent failure (returns -EIO, state UNKNOWN, 4 ops) ... [PASS]
  Testing: Scenario 5: Command strobe 3-byte wire framing ({cmd, ~cmd, 0x00}) ... [PASS]
  Testing: Scenario 6: Wake strobe 1 failure (returns -ETIMEDOUT, state UNKNOWN, 1 op) ... [PASS]
  Testing: Scenario 7: Foundry interrogation loop failure (returns -EIO, state UNKNOWN, 4 ops) ... [PASS]
  Testing: Scenario 8: Zero 0x1A84 writes in baseline startup (isolated to Stage D7, 16 ops) ... [PASS]
  Testing: Scenario 9: Precondition enforcement (invalid states rejected with -EPERM) ... [PASS]
  Testing: Scenario 10: Full startup flag (FTE3600_INIT_FLAG_FULL_STARTUP) from RESET (20 ops) ... [PASS]
  Testing: Scenario 11: Removal / offline concurrency during startup (-ENODEV) ... [PASS]
  Testing: Scenario 12: Blocked operations (threshold bank, DAC, arm touch) excluded ... [PASS]

--- Test Suite 9: Stage D6.4 State Semantics and Capture-Gating Verification ---
  Testing: Scenario 1: Prefix completion withholds READY and sets status flags ... [PASS]
  Testing: Scenario 2: Capture is rejected in IDENTIFIED (-EPERM) ... [PASS]
  Testing: Scenario 3: Capture is rejected in prefix-complete/uncalibrated state (-EPERM) ... [PASS]
  Testing: Scenario 4: Capture is rejected in UNKNOWN (-EPERM) ... [PASS]
  Testing: Scenario 5: Capture is rejected when OFFLINE (-ENODEV, 0 hw accesses) ... [PASS]
  Testing: Scenario 6: Capture is rejected after failed initialization (-EPERM) ... [PASS]
  Testing: Scenario 7: Capture can only be admitted after a valid future-ready state transition ... [PASS]
  Testing: Scenario 8: Legacy zero-flag INIT remains identification-only ... [PASS]
  Testing: Scenario 9: Public UAPI values, structure sizes and ioctl numbers remain unchanged ... [PASS]
  Testing: Scenario 10: Existing D5.4 lifetime and OFFLINE behavior remain intact ... [PASS]

===================================================================
 Stage D6.4 Test Results: 60 / 60 Passed (100.0%)
===================================================================
./fte3600-policy-test
FTE3600 glue policy: 9 groups passed (GPIO/ACPI IRQ, CS capability, polarity, lease, PM, removal)
make: Leaving directory '/home/titan/fte3600-linux-driver/kernel'
```

---

## 3. Detailed Suite 9 Scenario Matrix

| Scenario | Objective | Conditions Tested | Expected Behavior | Observed Result | Status |
|---|---|---|---|---|---|
| **Scenario 1** | Prefix completion withholds READY | Execute volatile startup prefix | Internal state `14`, mapped to `8`; flags `STARTUP_PREFIX_COMPLETED \| CALIBRATION_PENDING` | `snap.state == 8`, `snap.state != 4`, flags set | **PASS** |
| **Scenario 2** | Capture rejected in IDENTIFIED | Call `FTE3600_IOC_CAPTURE` from state 8 | State is not `READY`; must reject immediately | Returns `-EPERM`, state remains `8` | **PASS** |
| **Scenario 3** | Capture rejected in UNCALIBRATED | Call `FTE3600_IOC_CAPTURE` from state 14 | State is not `READY`; must reject immediately | Returns `-EPERM`, state remains `14` | **PASS** |
| **Scenario 4** | Capture rejected in UNKNOWN | Call `FTE3600_IOC_CAPTURE` from state 7 | Fail-closed state; must reject immediately | Returns `-EPERM`, state remains `7` | **PASS** |
| **Scenario 5** | Capture rejected when OFFLINE | Call `FTE3600_IOC_CAPTURE` after unbind | Device offline; must abort without SPI access | Returns `-ENODEV`, 0 hw accesses | **PASS** |
| **Scenario 6** | Capture rejected after failed INIT | Provoke INIT error, then call CAPTURE | State is UNKNOWN; must reject immediately | Returns `-EPERM`, state remains `7` | **PASS** |
| **Scenario 7** | Capture admitted ONLY in READY | Test gating across states 14, 8, and simulated 4 | States 14 and 8 reject with `-EPERM`; state 4 admits to stub | Returns `-EOPNOTSUPP` only in state 4 | **PASS** |
| **Scenario 8** | Legacy zero-flag INIT identification only | Call `INIT` with `flags = 0` | Identify sequence (3 SPI transfers); no startup prefix | State is `8`, flags have zero startup bits | **PASS** |
| **Scenario 9** | UAPI constants and sizes stable | Check sizes (32 B), ioctl encodings, and states | Struct sizes 32 B; state range `[0..8]`; flags intact | All asserts hold | **PASS** |
| **Scenario 10** | D5.4 lifetime and OFFLINE intact | Device removal during open fd | GET_STATE reports `0 + OFFLINE`; all ioctls return `-ENODEV`; clean release | Memory freed cleanly (`atomic_freed == 1`) | **PASS** |

---

## 4. Test Scope and Silicon Boundary Statement

**CRITICAL CLARIFICATION ON MOCK SCOPE:**
The standalone test harness in `kernel/fte3600-test.c` operates entirely in userspace with simulated hardware abstractions (mock SPI transfers, simulated GPIO descriptors, and simulated kernel mutex / waitqueue primitives).

1. **What the Mocks Prove:**
   - Driver state machine transitions and locking are deterministically serialized.
   - Race conditions between ioctl execution and device unbind/removal are cleanly resolved without null pointer dereferences or leaked memory.
   - Capture is strictly gated and cannot be executed while the driver is uncalibrated or in non-READY states.
   - UAPI structure memory footprints, offsets, and public value bounds are 100% compliant with ABI specifications.
2. **What the Mocks DO NOT Prove:**
   - Silicon analog pixel matrix readiness.
   - Correctness of physical sensor dark current or baseline DAC offsets.
   - Proprietary interrupt latch behavior on register `0x1A84`.
   - Physical SPI bus signal integrity or timing under extreme thermal conditions.

Silicon behavior can only be proven through authorized, hardware-executed test stages. The current stage maintains strictly zero hardware communication.
