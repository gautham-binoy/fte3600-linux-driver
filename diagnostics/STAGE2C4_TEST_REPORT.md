# Stage 2C.4 — Automated Test Suite Report

**Document ID:** `STAGE2C4_TEST_REPORT`  
**Date:** 2026-11-02  
**Target Silicon:** FocalTech FT9362 / FW9369  
**Test Suite Executable:** `bin/fw9369-tests`  
**Source Code Baseline:** `tools/fw9369-mock/tests/test_runner.c`  
**Compiler Flags:** `gcc -Wall -Wextra -Werror -pedantic -std=c11 -O2` (0 warnings, 0 errors)  
**Execution Mode:** In-Memory Mock Testing (Zero Hardware Access)  
**Execution Privilege:** Unprivileged non-root userspace execution (`titan`)

---

## 1. Executive Summary & Verdict

The automated unit and integration test suite (`bin/fw9369-tests`) was executed directly on the host system without root privileges and with zero hardware device node interaction.

```text
===================================================================
 Final Test Results: 53 / 53 Passed (100.0%)
===================================================================
```

**Verdict:** **100% PASS RATE ACROSS ALL 53 TEST CASES.**

---

## 2. Test Suite Breakdown & Verification Results

### Test Suite 1: CRC Implementations & Test Vectors (5 / 5 Passed)
- `[PASS]` Wire CRC matches Chip ID `0x9362` test vector (`0x1C53`).
- `[PASS]` Host LE CRC matches `[ 0, 0, 0, 0 ]` test vector (`0x313E`).
- `[PASS]` Host LE CRC matches `[ 512, 512, 512, 512 ]` test vector (`0x5B7C`).
- `[PASS]` Host LE CRC matches `[ 482, 482, 482, 482 ]` test vector (`0xE4AA`).
- `[PASS]` Byte ordering distinction verified: big-endian wire stream produces different checksum than little-endian word stream.

### Test Suite 2: Protocol Framing & Allowlist Enforcement (12 / 12 Passed)
- `[PASS]` Byte write packet framing correct (`[ 0x09, 0xF6, addr, val ]`).
- `[PASS]` Byte read packet framing correct (`[ 0x08, 0xF7, addr, 0, 0 ]`).
- `[PASS]` Word write packet framing correct (`[ 0x05, 0xFA, ... ]`).
- `[PASS]` Command wake framing correct (`[ 0x5A, 0xA5, 0x00 ]`).
- `[PASS]` FIFO burst read packet length (10,246 B) and 6-byte prefix verified.
- `[PASS]` Register allowlist permits documented register `0xC6`.
- `[PASS]` Register allowlist permits documented register `0x1A8B`.
- `[PASS]` Register allowlist rejects unauthorized address `0x0000`.
- `[PASS]` Register allowlist rejects unauthorized address `0xFFFF`.
- `[PASS]` Command allowlist permits wake dialog `[ 0x5A, 0xA5 ]`.
- `[PASS]` Command allowlist permits end delimiter `[ 0xA5, 0x5A ]`.
- `[PASS]` Command allowlist rejects unauthorized opcode `[ 0x12, 0x34 ]`.

### Test Suite 3: Mock Transport Invariants & Safety (7 / 7 Passed)
- `[PASS]` Mock transport created successfully entirely in user memory.
- `[PASS]` Initial power-on register `0xC6` default verified as `0x00`.
- `[PASS]` Initial protected analog trim bank verified as locked.
- `[PASS]` Mock accurately stores byte register write to `0xC6 = 0x01`.
- `[PASS]` Mock returns `0x01` on register `0xC6` readback.
- `[PASS]` Transaction logger accurately records packet counts and byte layouts.
- `[PASS]` **Hardware transport creation is unconditionally REJECTED with `-ENOTSUP` without accessing any device nodes.**

### Test Suite 4: FDT Calibration & Minimum Tracking (7 / 7 Passed)
- `[PASS]` FDT calibration executes cleanly against simulated analog AFE.
- `[PASS]` Calibration valid flag asserted upon convergence.
- `[PASS]` DAC centering search converges at target setting (`dac = 28`).
- `[PASS]` Early exit condition triggered when 3 consecutive stable samples observed.
- `[PASS]` All tracked minima fall strictly within valid bounds `[300, 700]`.
- `[PASS]` Threshold mathematical derivation strictly matches \(fdt\_down[c] = minima[c] - 30\).
- `[PASS]` FDT threshold CRC matches 4-word little-endian calculation.

### Test Suite 5: Image Baseline & Single-Frame Copy (5 / 5 Passed)
- `[PASS]` Image baseline acquired cleanly on mock.
- `[PASS]` Baseline valid flag asserted.
- `[PASS]` Image DAC nominal setting (`image_dac = 54`) preserved.
- `[PASS]` Cold baseline is an exact un-averaged copy (`memcpy`) of captured raw frame.
- `[PASS]` Baseline CRC signature covers exactly the first 128 words (256 bytes).

### Test Suite 6: Full Pipeline & Protected Register Cleanup (8 / 8 Passed)
- `[PASS]` Full 5-phase pipeline executes to completion.
- `[PASS]` Final phase is `FW9369_PHASE_COMPLETED`.
- `[PASS]` Silicon identified as FT9362 (`0x9362`).
- `[PASS]` Hardware Chip ID wire CRC validated (`0x1C53`).
- `[PASS]` MCU acknowledged idle handshake (`0x50`).
- `[PASS]` Autonomous finger detection successfully armed.
- `[PASS]` Lock command `write_byte(0x9A, 0x00)` submitted cleanly.
- `[PASS]` Simulated analog trim bank relocked.

### Test Suite 7: Deterministic Failure Injection Scenarios (9 / 9 Passed)
- `[PASS]` Scenario 1: SPI transfer failure aborts cleanly.
- `[PASS]` Scenario 2: Unexpected Chip ID (`0x9368`) aborts cleanly.
- `[PASS]` Scenario 3: Corrupt wire CRC aborts cleanly.
- `[PASS]` Scenario 4: ADC status polling timeout aborts cleanly.
- `[PASS]` Scenario 5: Out of bounds minima ($< 300$) aborts cleanly.
- `[PASS]` Scenario 6: 10,246-byte FIFO transfer failure aborts cleanly.
- `[PASS]` Scenario 7: Lock write failure reports `FW9369_LOCK_TRANSMISSION_FAILED`.
- `[PASS]` Scenario 8: Interruption in trim access caught and handled.
- `[PASS]` Scenario 8: Cleanup handler unconditionally submitted lock command.

---

## 3. CLI Binary Execution Validation

The userspace command-line binary `bin/fw9369-tool` was validated under all modes:

1. **Standard Mock Run:**
   - Command: `bin/fw9369-tool`
   - Output: Executes all 5 phases, logs 60 transactions, identifies `0x9362`, validates CRC `0x1C53`, converges DAC at 28, arming successful (`>> Pipeline Execution: SUCCESS`).
2. **Hardware Rejection Test:**
   - Command: `bin/fw9369-tool --hardware`
   - Output: `[SECURITY ALERT] Hardware mode is strictly prohibited in Stage 2C.4!` followed by `[SAFETY GATE PASS] Hardware backend was rejected without accessing any device node.` Exits with code 1.
3. **Failure Injection Test:**
   - Command: `bin/fw9369-tool --inject-fail=crc`
   - Output: Aborts at Phase 1 with wire CRC mismatch (`0xE3AC` vs `0x1C53`), logs 4 transactions, exits with code 1 (`-EBADMSG`).
