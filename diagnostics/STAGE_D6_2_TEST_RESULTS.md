# STAGE D6.2 — USERSAPCE TEST SUITE AND POLICY VERIFICATION RESULTS

**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Host Architecture:** Intel LPSS SPI2 (`spi-FTE3600:00`, CS0) on Dynabook Satellite Pro C40-J  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Date:** 2026-11-04  
**Author:** Antigravity (Advanced Agentic Pair Programming)  
**Classification:** VERIFICATION LOG & TEST REPORT

---

## 1. Executive Summary

In Stage D6.2, all unit tests and policy checks were executed in standalone userspace simulation mode (`make -C kernel check` and `./kernel/fte3600-policy-test`).

**Summary Metrics:**
- **Unit Test Suite:** 50 / 50 Passed (100.0%)
- **Policy Check Suite:** 9 / 9 Groups Passed (100.0%)
- **Kernel Module Compilation:** Zero warnings, zero errors (`make -C kernel all`)
- **Live Hardware Invariants:** Untouched and intact (`transfers=3, bytes=19`, state 8 `IDENTIFIED`)

---

## 2. Unit Test Suite Execution Log

Command: `make -C kernel check`

```
make: Entering directory '/home/titan/fte3600-linux-driver/kernel'
cc -std=c11 -Wall -Wextra -Werror -O2 -pthread -o fte3600-test fte3600-test.c
./fte3600-test
===================================================================
 FTE3600 Unified Kernel Driver Test Suite (Stage D6.2 Remediated)
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
  Testing: Scenario 1: Startup successful (SMIC Fab AA: 0x4C, period 150 us, READY, 16 ops) ... [PASS]
  Testing: Scenario 2: Startup successful (Non-SMIC Fab: 0x00, period 200 us, READY, 16 ops) ... [PASS]
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

===================================================================
 Stage D6.2 Test Results: 50 / 50 Passed (100.0%)
===================================================================
./fte3600-policy-test
FTE3600 glue policy: 9 groups passed (GPIO/ACPI IRQ, CS capability, polarity, lease, PM, removal)
make: Leaving directory '/home/titan/fte3600-linux-driver/kernel'
```

---

## 3. Kernel Module Compilation Verification

Command: `make -C kernel all`

```
make: Entering directory '/home/titan/fte3600-linux-driver/kernel'
make -C /lib/modules/7.2.8-9-omarchy/build M=/home/titan/fte3600-linux-driver/kernel modules
make[1]: Entering directory '/usr/lib/modules/7.2.8-9-omarchy/build'
make[2]: Entering directory '/home/titan/fte3600-linux-driver/kernel'
  CC [M]  fte3600.o
  MODPOST Module.symvers
  CC [M]  fte3600.mod.o
  LD [M]  fte3600.ko
  BTF [M] fte3600.ko
make[2]: Leaving directory '/home/titan/fte3600-linux-driver/kernel'
make[1]: Leaving directory '/usr/lib/modules/7.2.8-9-omarchy/build'
make: Leaving directory '/home/titan/fte3600-linux-driver/kernel'
```

Compiler exit code: `0`.  
Compiler diagnostics: Zero warnings, zero errors.

---

## 4. Hardware Safety Audit

Command: `grep fte3600 /proc/modules && cat /sys/bus/spi/devices/spi-FTE3600:00/statistics/transfers && cat /sys/bus/spi/devices/spi-FTE3600:00/statistics/bytes && ls -l /dev/fte3600`

```
fte3600 20480 0 - Live 0x0000000000000000 (OE)
3
19
crw------- 1 root root 511, 0 Nov  4 09:32 /dev/fte3600
```

Confirmation:
- Driver state remains `8 (IDENTIFIED)`.
- SPI transfers remain strictly `3`.
- SPI wire bytes remain strictly `19`.
- Device node `/dev/fte3600` remains major `511`, minor `0`.
- Zero hardware access was performed during test execution.
