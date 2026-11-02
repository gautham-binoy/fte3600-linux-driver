# Stage 2A v2.0 — Final Change Review and Execution Gate

**Date:** 2026-11-02  
**Target Unit:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Device:** `spi-FTE3600:00` on Intel GSPI #1 (`pxa2xx-spi.2`)  
**Architecture:** Pure `spidev` (v2.0)  
**Status:** FINAL PRE-EXECUTION GATE AUDIT (Strict Zero Hardware Access)

---

## 1. Executive Summary

A comprehensive, line-by-line review of all modifications, documentation updates, and executable artifacts was conducted prior to authorizing manual hardware execution.

### Key Conclusions:
1. **Full Decoupling Verified:** Neither `fte3600.ko` nor `uio.ko` is referenced in the execution path. Zero GPIO or PCH PADCFG registers are claimed or modified.
2. **Execution Path Integrity:** The terminal launcher strictly invokes `scripts/stage2a-test.sh`, which loads mainline `spidev`, binds `spi-FTE3600:00`, verifies `/dev/spidev1.0`, invokes `bin/stage2a-identify`, and automatically rolls back.
3. **Evidence Qualification Harmonized:** All references claiming that register `0xC6` definitely remains set until cold power cycle were corrected across all scripts and reports. The internal state of `0xC6` is formally documented as an unknown sensor-internal state that host cleanup cannot inspect or confirm.
4. **Operator Manual Accuracy:** Section 5 of [STAGE2A_OPERATOR_PROCEDURE.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2A_OPERATOR_PROCEDURE.md) was updated with an explicit alert emphasizing that the sample FT9395 output is an illustrative format example only, and that chip identification is not fingerprint capture.
5. **Readiness Verdict Re-Affirmed:** The previous verdict of **`READY FOR EXPLICIT AUTHORIZATION`** remains 100% valid.

---

## 2. Files Reviewed and Git Diff Summary

### 2.1 Files Inspected
- `scripts/stage2a-test.sh` (Harness script, lines 1–634)
- `scripts/run-stage2a-in-terminal.sh` (Terminal launcher, lines 1–30)
- `tools/stage2a-identify.c` (Native C SPI identification utility, lines 1–375)
- `bin/stage2a-identify` (Compiled ELF binary, freshly compiled with `-Werror`)
- `diagnostics/STAGE2A_OPERATOR_PROCEDURE.md` (Operator manual, lines 1–311)
- `diagnostics/STAGE2A_EXECUTION_CHECKLIST.md` (Operational checklist, lines 1–189)
- `diagnostics/STAGE2A_V2_FINAL_READINESS_AUDIT.md` (Readiness audit, lines 1–172)
- `PROGRESS.md` (Project log, lines 1–648)

### 2.2 Git Diff Summary of Recent Modifications

| File | Modification Summary |
| :--- | :--- |
| `scripts/stage2a-test.sh` (Lines 333–340) | Refined post-rollback notice to replace the definite retention claim with evidence-qualified language: explicitly states that register `0xC6` is an unknown sensor-internal state that host cleanup cannot confirm, while host resources are 100% released and CS is deasserted high. |
| `diagnostics/STAGE2A_OPERATOR_PROCEDURE.md` (Lines 117–127) | Added an explicit alert callout in Section 5 clarifying that the sample FT9395 transcript is an illustrative example of transcript format, not a prejudged identity, and emphasizing that chip identification is not fingerprint capture. |
| `diagnostics/STAGE2A_EXECUTION_CHECKLIST.md` (Section 9) | Qualified sensor-internal uncertainty: distinguished guaranteed host restoration from undocumented internal retention, decay, or watchdog reset behaviors of `0xC6`. |
| `diagnostics/STAGE2A_V2_FINAL_READINESS_AUDIT.md` & `STAGE2A_PURE_SPIDEV_REVISION_REPORT.md` | Synchronized Section 9 / Section 7 to maintain consistent evidential rigor regarding internal sensor state. |
| `PROGRESS.md` | Appended Stage 2A v2.0 Final Execution Preparation entry while preserving full historical continuity. |

---

## 3. Verification of the Actual Execution Path

1. **Launcher Target:** [run-stage2a-in-terminal.sh](file:///home/titan/fte3600-linux-driver/scripts/run-stage2a-in-terminal.sh) executes `sudo "$SCRIPT_DIR/stage2a-test.sh"`. It cannot invoke any other script or binary.
2. **Binary Consistency:** `bin/stage2a-identify` was verified to be compiled from the current source of `tools/stage2a-identify.c` (ELF 64-bit LSB executable, x86-64, dynamically linked).
3. **SPI Device Scoping:** Target is hardcoded to `SPI_DEV="spi-FTE3600:00"`. Pre-flight checks verify sysfs existence at `/sys/bus/spi/devices/spi-FTE3600:00` and confirm it is `UNBOUND`.
4. **SPI Bus Configuration:** Configures Mode 0 (`SPI_MODE_0`), 8 bits/word (`SPI_IOC_WR_BITS_PER_WORD`), and 1,000,000 Hz (`SPI_IOC_WR_MAX_SPEED_HZ`), with readback verification via `SPI_IOC_RD_*`.
5. **Exact Framing Invariant:**
   - Op 1: `0x09 0xF6 0xC6 0x01` (4 bytes, Write `0xC6 = 0x01`)
   - Op 2: 4 ms delay (`usleep(4000)`, CS deasserted high)
   - Op 3: `0x08 0xF7 0xC6 0x00 0x00` (5 bytes, Read `0xC6`, requires `buffer[4] == 0x01`)
   - Op 4: `0x04 0xFB 0x9A 0x8B 0x00 0x00 0x00 0x00 0x00 0x00` (10 bytes, Read Chip ID)
   - Op 5: `0x04 0xFB 0x98 0x16 0x00 0x00 0x00 0x00 0x00 0x00` (10 bytes, Read sub-rev *strictly if ID == `0x9391`*)
6. **Zero Prohibited Commands:** Static inspection confirms zero occurrences of `0x5A/0xA5` (sleep), `0xC0` (flash), `0xFD/0xFE` (analog trim), or `0x1A84/0x1A8E` (sequencer/capture).
7. **Immediate Halt on Failure:** Exit codes 1–9 cover all failure modes (invocation, config, transport, verify mismatch, bus float, CRC mismatch, unknown chip ID, sub-rev failure). Every failure path closes the file descriptor immediately with zero retries.
8. **Rollback Determinism:** Cleanup trap handles `EXIT`, `INT`, `TERM`. Critical sections defer signals during state transitions. Cleanup unbinds `spi-FTE3600:00`, clears `driver_override`, and unloads `spidev`.
9. **No Device Spillage:** Device nodes are verified before and after. Pre-flight checks abort if any unexpected `/dev/spidev*` device exists.

---

## 4. Audit of Operator Procedure Manual

The operator manual ([STAGE2A_OPERATOR_PROCEDURE.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2A_OPERATOR_PROCEDURE.md)) was audited for accuracy:
- **Sample Output vs Actual Identity:** Section 5 contains a prominent alert emphasizing that the sample FT9395 output is an illustrative format example, not a prejudged identity.
- **Host Cleanup vs Sensor State:** Section 8 explicitly states that host cleanup guarantees host isolation only, and that the sensor internal state cannot be confirmed restored without further bus operations.
- **Chip Identification vs Fingerprint Capture:** Section 1 and Section 5 explicitly clarify that Stage 2A performs silicon identification only, with zero biometric scanning, frame acquisition, or image capture.

---

## 5. Offline Validation Results

| Test Category | Command Executed | Result |
| :--- | :--- | :---: |
| **C Compilation** | `gcc -O2 -Wall -Wextra -Werror -o bin/stage2a-identify tools/stage2a-identify.c` | **PASS (0 warnings)** |
| **Harness Syntax** | `bash -n scripts/stage2a-test.sh` | **PASS (0 errors)** |
| **Launcher Syntax** | `bash -n scripts/run-stage2a-in-terminal.sh` | **PASS (0 errors)** |
| **CLI Argument Bounds** | `bin/stage2a-identify` (no args) -> Exited code 1<br>`bin/stage2a-identify /dev/nonexistent` -> Exited code 1<br>`bin/stage2a-identify README.md` -> Exited code 1 (`!S_ISCHR`) | **PASS** |
| **CRC-CCITT Verification** | Python unit test verifying `0x9362` (`0x1C53`), `0x9365` (`0x6CB4`), `0x9391` (`0xC32F`), `0x0FFF` (`0x13C1`) | **PASS** |
| **Prohibited Operations Check** | Grep scan for `gpiod_*`, `/dev/gpiochip*`, `fte3600.ko`, `uio`, `0x5A`, `0xA5`, `0xC0`, `0xFD`, `0xFE` | **PASS (0 violations)** |

---

## 6. Confirmation of System Baseline & Zero Hardware Access

**I hereby confirm:**
- No kernel modules were loaded or unloaded.
- No SPI devices were bound or unbound.
- No character devices were opened.
- No SPI transactions were transmitted.
- No GPIO lines or PCH registers were accessed or modified.
- No `sudo` commands were executed.
- The host system and hardware remain in their exact, pristine baseline state:
  - `spidev`: `ABSENT`
  - `fte3600`: `ABSENT`
  - `spi-FTE3600:00`: `UNBOUND`
  - `driver_override`: `""`
  - `/dev/spidev*`: `ABSENT`

---

## 7. Execution Gate & Final Instructions

### Status: **`READY FOR EXPLICIT AUTHORIZATION`**

The Stage 2A v2.0 pure-spidev implementation is complete, statically verified, and fully prepared for supervised operator execution.

### Exact Manual Execution Command:
When you are ready to authorize and oversee the test, run the terminal wrapper:

```bash
cd /home/titan/fte3600-linux-driver
./scripts/run-stage2a-in-terminal.sh
```

---

### Mandatory Stop Boundary

**STOPPING.**  
Hardware operations remain strictly unauthorized. Awaiting your separate, explicit instruction.
