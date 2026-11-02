# Stage 2C.5A — Hardware Readiness Final Safety Gate & Decision

**Document ID:** `STAGE2C5A_FINAL_SAFETY_GATE`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Host Environment:** Linux `7.2.8-9-omarchy` (x86_64)  
**Execution Boundary:** Static analysis and architectural audit only. Hardware execution is strictly prohibited.

---

## 1. Executive Summary & Verification Audit

Stage 2C.5A has conducted an independent hardware-readiness and first-initialization safety review for the FocalTech FT9362 / FW9369 sensor on the Dynabook Satellite Pro C40-J laptop.

The review systematically evaluated historical evidence, register bitfields, kernel driver architectures, electrical properties, protocol risks, failure modes, controller capacities, and software interfaces across nine comprehensive diagnostic reports:

1. [STAGE2C5A_CURRENT_HARDWARE_STATE.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C5A_CURRENT_HARDWARE_STATE.md): Reconstructed pristine host isolation baseline and established the four-category state classification matrix.
2. [STAGE2C5A_PAD229_FINAL_REVIEW.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C5A_PAD229_FINAL_REVIEW.md): Resolved Pad 229 discrepancy; proved configuration bits were set by ACPI interrupt handler and bit 1 is live noise on a floating trace; confirmed High-Z input mode with hardware locked transmitter (`[LOCKED tx]`).
3. [STAGE2C5A_PAD14_RESET_CAPABILITY_REVIEW.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C5A_PAD14_RESET_CAPABILITY_REVIEW.md): Deconstructed reset capability into four distinct layers. Proved Pad 14 Tx latch is not hardware-locked, but pure-spidev userspace cannot toggle it due to ACPI `Exclusive` ownership. Established that ACPI S5 cold reboot is the only guaranteed recovery path.
4. [STAGE2C5A_COMPLETE_INITIALIZATION_RISK_REVIEW.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C5A_COMPLETE_INITIALIZATION_RISK_REVIEW.md): Audited the complete 5-phase continuous initialization sequence, establishing strict entry/exit conditions, timing bounds, and RAII protected register handling.
5. [STAGE2C5A_FAILURE_AND_RECOVERY_MATRIX.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C5A_FAILURE_AND_RECOVERY_MATRIX.md): Mapped 15 failure scenarios, proving zero software reset capability and mandating S5 cold reboot on severe stalls.
6. [STAGE2C5A_FIFO_CONTROLLER_READINESS.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C5A_FIFO_CONTROLLER_READINESS.md): Audited the 10,246-byte burst FIFO transfer across seven layers; proved feasibility on PXA2xx and LPSS DMA (`idma64.2`); confirmed requirement for `spidev.bufsiz=65536`.
7. [STAGE2C5A_FUTURE_HARDWARE_TEST_PLAN.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C5A_FUTURE_HARDWARE_TEST_PLAN.md): Designed a staged, operator-controlled roadmap (2C.5B preflight, 2C.5C initialization, 2C.5D capture) with zero isolated intermediate tests.
8. [STAGE2C5A_MOCK_TO_HARDWARE_GAP_ANALYSIS.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C5A_MOCK_TO_HARDWARE_GAP_ANALYSIS.md): Established that 95% of mock protocol code is reusable; isolated the gap to `fw9369_transport_hw.c`, real-time sleeps, and POSIX signal handlers.
9. [STAGE2C5A_FINAL_SAFETY_GATE.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C5A_FINAL_SAFETY_GATE.md): This document.

---

## 2. Safety Review Findings & Risk Mitigations

| Area of Concern | Identified Risk | Engineered Mitigation | Status |
| :--- | :--- | :--- | :---: |
| **Host System Isolation** | Accidental bus contention or resource collision | Confirmed 100% clean rollback. Device unbound; modules absent; zero open device nodes. | **VERIFIED CLEAN** |
| **PCH Pad 229 (IRQ)** | Electrical conflict on interrupt pin | Verified in High-Z input mode (`GPIOTXDIS=1`). Hardware locked against transmitter enablement (`[LOCKED tx]`). | **VERIFIED SAFE** |
| **PCH Pad 14 (Reset)** | Lack of software reset during silicon lock | ACPI S5 system cold reboot established as mandatory, conservative recovery procedure for severe stalls. | **CONTAINED** |
| **Register 0x9A (Trim Lock)** | Accidental corruption of analog trim bank | Strict RAII cleanup unconditionally submits `write_byte(0x9A, 0x00)` on all normal and abort paths. | **CONTAINED** |
| **FIFO Transfer Length** | Linux `spidev` buffer truncation (`-EMSGSIZE`) | Prerequisite established: `spidev` must be loaded with parameter `bufsiz=65536`. | **RESOLVED** |
| **Intermediate State Dwell** | Un-gated clock dissipation from isolated `0x5A` | Staged test plan eliminates isolated tests; enforces continuous execution transitioning directly into AFE tuning. | **CONTAINED** |

---

## 3. Authoritative Stage 2C.5A Verdict

In accordance with the Stage 2C.5A mandate, exactly one of the three specified verdicts is selected:

```
HARDWARE PLAN READY FOR OPERATOR REVIEW
```

---

## 4. Explicit Safety Boundary for Subsequent Work

> [!CRITICAL]
> **ABSOLUTE SAFETY BOUNDARY:**
> **The verdict `HARDWARE PLAN READY FOR OPERATOR REVIEW` authorizes ONLY human operator review of the proposed hardware strategy.**  
> It does **NOT** authorize:
> - Physical SPI communication or sensor transactions.
> - Loading or unloading kernel modules (`modprobe spidev`).
> - Binding `spi-FTE3600:00` or opening `/dev/spidev*`.
> - Toggling GPIO lines or modifying pinctrl registers.
> - Creating or executing hardware test scripts.
> 
> **Any future physical hardware operation requires explicit, separate, affirmative operator authorization before execution.**
