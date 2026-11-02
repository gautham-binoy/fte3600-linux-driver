# Stage 2B.5 — Final Safety Gate & Strategic Decision Audit

**Document ID:** `STAGE2B5_FINAL_SAFETY_GATE`  
**Date:** 2026-11-02  
**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FT9362 / FW9369  
**Status:** COMPLETE — STATIC AUDIT & STRATEGIC GATE DECISION  
**Hardware Execution Authorization:** NONE (Static Analysis Only)

---

## 1. Executive Summary & Audit Objective

This document concludes **Stage 2B.5**, the independent static audit of the proposed Sub-Stage 2C.1B sequence from Stage 2B.4. 

The audit rigorously tested every claim, protocol assumption, failure path, and operational consequence of executing an isolated MCU idle handshake (`0x5A` ... `0xA5`) against the physical FocalTech FT9362 sensor.

The audit exposes three fatal flaws in the isolated 2C.1B design:
1. **The Stranded MCU Failure Mode:** In reference code, command `0x5A` (`wake`) is **always** paired with command `0xA5` (`end`), even on failure. The proposed 2C.1B rule of immediately aborting if `0x80 != 0x50` without sending `0xA5` leaves the sensor's microcontroller in an un-terminated command transaction state with zero error-handling precedent in reference code, requiring an ACPI S5 cold reboot to recover.
2. **The Fragility of Single-Shot 0x80 Checks:** Source code proves that `0x80 == 0x50` is not guaranteed on a single read after 1 ms; all reference drivers implement retries (up to 5 polls in `ft93xx.c`, up to 10 in `fw9369_release.c`) or exception strobes (`C0 3F`). Single-shot execution without retries invites false-positive aborts.
3. **Zero Standalone Informational or Functional Utility:** Stage 2A already definitively proved chip ID `0x9362`, CRC `0x1C53`, and FW9369 driver mapping. Transitioning the MCU into idle in isolation provides zero user capability (no enrollment, no calibration, no capture) while leaving the sensor's internal clocks un-gated (`0xC6=1`) indefinitely.

---

## 2. Synthesis of Stage 2B.5 Investigations

| Investigation | Key Finding | Source Citation | Consequence for 2C.1B |
| :--- | :--- | :--- | :--- |
| **1. Command 0x5A Semantics** | Proved `0x5A` is the opening dialog request `wake` (`[ 0x5A, 0xA5, 0x00 ]`). | `fw9369_release.c:451`, `ft93xx.c:174` | Must never be sent without a matching closing delimiter. |
| **2. Command 0xA5 Semantics** | Proved `0xA5` is the closing delimiter `end` (`[ 0xA5, 0x5A, 0x00 ]`). Unconditionally sent even on error in reference code. | `fw9369_release.c:464`, `ft93xx.c:280` | Aborting after `0x5A` without sending `0xA5` violates protocol framing. |
| **3. Register 0x80 Validity** | Proved `0x80 == 0x50` is not guaranteed on single read; reference drivers all use retry loops or exception strobes. | `ft93xx.c:268-277`, `fw9369_release.c:531-537` | Single-shot check without retries is fragile and prone to false aborts. |
| **4. Critical Failure Scenario** | Aborting on `0x80 != 0x50` leaves MCU stranded in un-closed command dialog. Cannot be reset without cold reboot. | `STAGE2B5_FAILURE_STATE_ANALYSIS.md` | High asymmetric risk; zero software rollback capability. |
| **5. Successful State Analysis** | Success leaves clocks running (`0xC6=1`) and MCU locked in idle (`0x50`) indefinitely with zero follow-up operations. | `STAGE2B5_SUCCESSFUL_STATE_ANALYSIS.md` | Host rollback does not restore silicon baseline. |
| **6. Necessity Reassessment** | Isolated 2C.1B provides zero user capability and <2% new info. Option C (integrated research) is vastly superior. | `STAGE2B5_2C1B_NECESSITY_REVIEW.md` | Sub-Stage 2C.1B is completely unnecessary as a standalone test. |

---

## 3. Authoritative Decision & Single-Choice Verdict

### Authoritative Project Verdict

```
2C.1B UNNECESSARY — DEFER ACTIVE OPERATIONS
```

---

## 4. Operational Directives & Next Steps

1. **Zero Hardware Operations Authorized:** No hardware transactions, scripts, or active commands shall be executed against the sensor.
2. **Sub-Stage 2C.1B Formally Cancelled as an Independent Hardware Test:** The project will not execute an isolated MCU idle handshake.
3. **Preserve Pristine Baseline:** The physical sensor remains in its proven Stage 2A baseline state (decoupled, host isolated, CS high).
4. **Advance to Stage 2C.2 Research:** Focus all future engineering effort on **Stage 2C.2 (FDT Calibration & Frontend Architecture Research)**, investigating capacitive frontend register setup (`0x18xx`), DAC centering algorithms, and baseline measurement vectors as an integrated, complete initialization pipeline.
