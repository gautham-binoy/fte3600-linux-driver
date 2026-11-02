# Stage 2C.4.1 — Independent Audit Final Review and Gate Decision

**Document ID:** `STAGE2C41_FINAL_REVIEW`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Audit Scope:** Full synthesis of Stage 2C.4.1 independent audit across Investigations 1–6  
**Hardware Authorization:** NONE (Software & Static Analysis Only)

---

## 1. Executive Summary & Audit Synthesis

Stage 2C.4.1 has conducted an independent audit of the FocalTech FW9369 mock userspace tooling located in `tools/fw9369-mock/`. This audit evaluated the source code, build system, test harness, command-line interface, protected register mechanics, protocol reconciliation, and calibration routines against the canonical specification (`diagnostics/FW9369_UNIFIED_INITIALIZATION_SPECIFICATION.md`).

### Comprehensive Audit Matrix
| Investigation Area | Audit Focus | Audit Status | Key Verification Finding |
| :--- | :--- | :---: | :--- |
| **Investigation 1: Source & Architecture** | Codebase cleanliness, memory safety, zero hardware I/O | **PASSED** | 100% in-memory mock. Zero calls to `open()`, `ioctl()`, `mmap()`, or `/dev/*`. Clean build with `-Wall -Wextra -Werror -pedantic -std=c11 -O2`. |
| **Investigation 2: Protected Access (0x9A)** | Handling of `0x9A` across 8 failure dimensions | **PASSED** | Structured cleanup unconditionally submits relock `0x9A=0x00`. Distinguishes between clean transmission, transmission failure, and unattempted interruption. Never falsely claims physical silicon is locked. |
| **Investigation 3: Idle Handshake** | Reconciliation of `0x5A/0xA5` with Stage 2B.5 findings | **PASSED** | Correctly integrates `0x5A/0xA5` into continuous 5-phase pipeline transitioning immediately to AFE calibration, resolving the intermediate dwell hazard of cancelled Sub-Stage 2C.1B. |
| **Investigation 4: Build, Test & CLI** | Compiler outputs, test runner (53/53), CLI execution, exit code 1 | **PASSED** | All 53 assertions passed (100.0%). Exit code 1 verified as intentional Unix status for hardware mode rejection and injected pipeline failures. |
| **Investigation 5: CRC & Calibration** | FDT minimum tracking, cold baseline single-frame copy, FIFO layout | **PASSED** | Exact big-endian wire CRC `0x1C53` match; correct little-endian host word CRCs (`0x313E`, `0x5B7C`, `0xE4AA`); exact threshold derivation \(minima - 30\); bit-exact single-frame baseline copy (`memcpy`, zero averaging). |
| **Investigation 6: Hardware Assumptions** | Explicit tracking of physical hardware uncertainties | **PASSED** | All hardware-dependent parameters (DMA buffer limits, CS timing, physical FIFO readiness) remain strictly classified as unverified until Stage 2C.5. |

---

## 2. Inventory of Unverified Physical Hardware Assumptions

The mock tooling operates exclusively against simulated software models. The following physical hardware behaviors remain **unverified on physical Dynabook hardware** and must be resolved only during a future, explicitly authorized hardware stage (Stage 2C.5):

1. **Host SPI Controller DMA Buffer Limit (`pxa2xx-spi.2`):**
   - The Linux `spidev` driver default maximum transfer buffer size is typically 4,096 bytes (`bufsiz=4096`).
   - The burst FIFO read requires a single continuous transfer of **10,246 bytes**.
   - If physical `spidev` module parameters are not reconfigured (e.g., `spidev.bufsiz=65536`) or if the PXA2xx DMA engine fragments the SPI transaction by deasserting Chip Select between DMA bursts, the physical FT9362 sensor will abort the burst read and corrupt the image frame.
2. **Sensor FIFO Ready Latency:**
   - The mock backend instantly returns simulated pixel data upon receiving the 10,246-byte packet prefix.
   - Physical FT9362 silicon requires an uncharacterized internal settling delay between sensor scan trigger and FIFO buffer readiness.
3. **Chip Select Continuity & Clock Timing:**
   - Physical sensor SPI protocol mandates continuous CS assertion throughout the entire 10,246-byte transfer at a clock frequency $\le 1$ MHz (or calibrated higher speed).
   - Any CS glitch causes immediate frame synchronization loss.
4. **Physical Register `0x9A` Silicon State:**
   - Software cannot read back register `0x9A`. While the software cleanup handler submits `0x00`, only physical hardware testing with subsequent write rejection tests can confirm that the silicon relocked.

---

## 3. Non-Critical Observations

During the audit, one minor documentation discrepancy was noted:
- **`tools/fw9369-mock/include/fw9369_crc.h` (Line 32):**
  - A comment string states `[ 482, 482, 482, 482 ] -> 0x081D`.
  - The actual mathematical CRC for `[ 482, 482, 482, 482 ]` under little-endian word serialization is `0xE4AA`.
  - The implementation in `src/fw9369_crc.c` and the test assertion in `tests/test_runner.c:50` both use `0xE4AA` and pass cleanly.
  - *Recommendation:* Update the doc comment in `include/fw9369_crc.h` to `0xE4AA` during the next routine codebase maintenance pass. This has zero impact on compilation or functional correctness.

---

## 4. Authoritative Independent Audit Verdict

Based on the verified 100% pass rate (53/53 tests), complete zero-hardware memory safety, strict compiler enforcement, truthful failure modeling, and faithful protocol adherence:

```
INDEPENDENT AUDIT PASSED — MOCK IMPLEMENTATION ACCEPTED
```

---

## 5. Explicit Safety Boundary for Subsequent Work

> [!IMPORTANT]
> **This verdict authorizes ONLY acceptance of the userspace mock implementation.**  
> It does **NOT** authorize physical hardware execution, SPI bus transactions, `/dev/spidev*` access, GPIO toggling, ACPI table alteration, or kernel module loading.

Prior to any execution against physical Dynabook hardware (Stage 2C.5):
1. A separate Stage 2C.5 hardware execution pre-flight review must be authorized.
2. The `spidev` 10,246-byte buffer capacity and DMA constraints must be resolved.
3. An operator checklist with exact pre- and post-execution rollback procedures must be approved by the operator.
