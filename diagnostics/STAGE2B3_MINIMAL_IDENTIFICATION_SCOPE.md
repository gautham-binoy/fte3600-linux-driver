# Stage 2B.3 — Minimal Identification Scope & 2C.1A Boundary Evaluation

**Document ID:** `STAGE2B3_MINIMAL_IDENTIFICATION_SCOPE`  
**Date:** 2026-11-02  
**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FT9362 / FW9369  
**Status:** COMPLETE — STATIC SCOPE REDUCTION AUDIT  
**Hardware Execution Authorization:** NONE (Static Analysis Only)

---

## 1. Executive Summary

This document evaluates the necessity and exact transaction boundary of proposed **Sub-Stage 2C.1A** in light of the physical results achieved in Stage 2A and the register safety investigations completed in Stage 2B.3.

The evaluation demonstrates that:
1. **Register `0x9B` Must Be Excluded:** Reading `0x9B` without executing the OTP sense sequence (`0xF1`/`0xF4`) reads an un-strobed digital shadow latch (likely `0x00`), which does not reliably determine silicon foundry. Because `0xF1`/`0xF4` are excluded due to `UNKNOWN` persistence safety, valid foundry sensing cannot occur during minimal identification.
2. **Registers `0x80` and `0x1A82` Must Be Excluded:** Both registers are excluded per Investigations 1 and 2 because their pre-initialization behavior is undocumented, they are never read before initialization in reference code, and they provide zero chip identification value.
3. **Chip Identification is Already Conclusively Established:** Stage 2A physical testing (`diagnostics/stage2a-results.txt`) already proved chip ID `0x9362` and verified hardware CCITT CRC `0x1C53`. Repeating `0xC6 = 0x01` and reading `0x1A8B` is completely redundant.
4. **Scope Reduction to Zero Transactions:** When all redundant and uninformative transactions are eliminated, the net informational gain of Sub-Stage 2C.1A is **ZERO**.
5. **Architectural Recommendation:** **Sub-Stage 2C.1A should be SKIPPED ENTIRELY.** The project should proceed directly to defining the safety and execution gates for **Sub-Stage 2C.1B** (Active State Transition & Acknowledgment) and **Stage 2C.2** (FDT Calibration).

---

## 2. Removal of Register 0x9B from Initial Identification

### 2.1. Unstrobed Latch Dynamics
In `STAGE2B2_OTP_SENSING_REVIEW.md`, we proved from `fw9369_startup.c:347-369` that:
- Register `0x9B` is a digital shadow latch.
- It receives data from OTP row `0x13` only after the host pulses `0xF4` (`0xC0 -> 0xC1 -> 0xC0 -> 0xC0`) with `0xF1 = 0x13`.
- If `0x9B` is read directly without pulsing `0xF4`, it reflects either power-on reset defaults (`0x00`) or undefined latch state.
- In `fw9369_startup.c:365`, `code == 0` causes the driver to fall back to default (TSMC) foundry configuration. An un-strobed read returning `0x00` cannot be distinguished from a genuine TSMC chip.

### 2.2. Prohibition of OTP Strobes (`0xF1` / `0xF4`)
Executing the `0xF1`/`0xF4` sequence to clock OTP row `0x13` into `0x9B` is prohibited because writes to high-index registers `0xF1` and `0xF4` carry `UNKNOWN` persistence safety without official FocalTech silicon datasheets.

### 2.3. Exclusion Verdict
Register `0x9B` provides zero valid foundry identification when un-strobed, and its sensing sequence cannot be safely run. Register `0x9B` is **EXCLUDED** from minimal identification.

---

## 3. Granular Step-by-Step Evaluation of Proposed 2C.1A Scope

We evaluate each of the 12 steps proposed for Sub-Stage 2C.1A:

| Step # | Proposed Step | Purpose in 2C.1A | Prior Stage 2A Status | New Information Gained? | Redundant / Necessary? | State Impact |
| :---: | :--- | :--- | :--- | :---: | :---: | :--- |
| **1** | Preflight host state | Verify baseline isolation | PASSED in Stage 2A | None | Pre-requisite | Read-Only |
| **2** | Load/bind mainline spidev | Provide userspace SPI fd | PASSED in Stage 2A | None | Unnecessary if no SPI ops | Host resource binding |
| **3** | `0xC6 = 0x01` write | Un-gate transceiver clocks | PASSED in Stage 2A | None | **REDUNDANT** | **Sensor clock un-gated** |
| **4** | Wait 4 ms | Oscillator stabilization | PASSED in Stage 2A | None | **REDUNDANT** | Host delay |
| **5** | Read back `0xC6` | Confirm wake-up (`0x01`) | PASSED in Stage 2A (`0x01`) | None | **REDUNDANT** | Bus read |
| **6** | Read Chip ID `0x1A8B` | Verify ID & CRC | PASSED in Stage 2A (`0x9362`, `0x1C53`) | None | **REDUNDANT** | Bus read |
| **7a**| Read Status `0x80` | Observe MCU mode | Not in Stage 2A | Unverified/Undefined | **EXCLUDED (Investigation 1)** | Unknown MCU effect |
| **7b**| Read IRQ/Status `0x1A82`| Observe boot flags | Not in Stage 2A | Undocumented bitmask | **EXCLUDED (Investigation 2)** | No ID value |
| **7c**| Read Foundry `0x9B` | Observe boot latch | Not in Stage 2A | Un-strobed latch (`0x00`)| **EXCLUDED (Section 2)** | No ID value |
| **8** | Close SPI fd & deassert CS | Release bus | PASSED in Stage 2A | None | Unnecessary if no SPI ops | Bus release |
| **9** | Unbind spidev | Decouple SPI device | PASSED in Stage 2A | None | Unnecessary if no SPI ops | Host unbind |
| **10**| Restore driver_override | Return override to `""` | PASSED in Stage 2A | None | Unnecessary if no SPI ops | Host sysfs write |
| **11**| Unload spidev | Unload kernel module | PASSED in Stage 2A | None | Unnecessary if no SPI ops | Module unload |
| **12**| Verify host rollback | Confirm 0 errors | PASSED in Stage 2A | None | Unnecessary if no SPI ops | Read-Only |

---

## 4. Synthesis: Facts vs. Inferences & Net Information Gain

### 4.1. Previously Established Facts
- **Host Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), SPI device `spi-FTE3600:00` on controller `pxa2xx-spi.2`, CS 0 (`diagnostics/stage2a-results.txt:10-25`).
- **Silicon Identity:** Chip ID register `0x1A8B` returned `0x9362` with CRC `0x1C53` (`stage2a-results.txt:88-95`).
- **Sensor Model:** FocalTech FT9362 operating under the FW9369 firmware architecture (`STAGE2B_FW9369_RESEARCH.md`).
- **Firmware Blob Requirement:** Conclusively proven that FT9362 / FW9369 requires **NO external firmware binary blob** (`linux_firmware.c:134-150`, `STAGE2B_FW9369_RESEARCH.md:4.1`).

### 4.2. Analysis of Potential New Information in 2C.1A
- **Will reading `0x1A8B` again tell us anything new?**  
  No. Physical silicon ID does not change.
- **Will reading `0x80` tell us anything valid?**  
  No. Reading `0x80` prior to `0x5A 0xA5` is unverified and context-dependent.
- **Will reading `0x1A82` tell us anything valid?**  
  No. Pre-initialization flags are not documented and not checked in reference startup.
- **Will reading `0x9B` tell us anything valid?**  
  No. Without OTP strobing, `0x9B` holds un-strobed latch contents.

### 4.3. Net Informational Gain
$$\text{Net Informational Gain of Sub-Stage 2C.1A} = 0 \text{ bytes / } 0 \text{ bits}$$

### 4.4. State Impact of Unnecessary Execution
Repeating `0xC6 = 0x01` un-gates internal sensor clocks. As established in Stage 2A audits, sensor-internal clock gating cannot be proven to restore to sleep by host-side rollback alone. Executing transactions that yield zero new knowledge violates the fundamental safety principle of minimal hardware interaction.

---

## 5. Definitive Boundary & Skip Recommendation

### 5.1. Smallest Possible 2C.1A Transaction Sequence
The smallest possible transaction sequence that achieves the objective of 2C.1A without redundancy is:
$$\text{Transaction Count} = 0$$

### 5.2. Formal Recommendation
**Sub-Stage 2C.1A should be SKIPPED ENTIRELY.**

The physical evidence collected in Stage 2A satisfies all requirements for sensor identification. Rather than re-running redundant identification cycles:
1. The project should accept Stage 2A as the authoritative minimal identification milestone.
2. The project should focus research and safety gating directly on the transition to **Sub-Stage 2C.1B** (Selective IRQ Acknowledgment & MCU Idle Handshake) and **Sub-Stage 2C.2** (FDT Calibration).
