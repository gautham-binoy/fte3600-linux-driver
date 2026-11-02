# Stage 2C.3 — Final Safety Gate & Specification Verdict

**Document ID:** `STAGE2C3_FINAL_SAFETY_GATE`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369  
**Status:** COMPLETE — STATIC AUDIT & SPECIFICATION GATE  
**Hardware Execution Authorization:** NONE (Static Analysis Only)

---

## 1. Executive Summary & Audit Completion

Stage 2C.3 has completed an independent static safety review and reconciliation of the entire FW9369 initialization, calibration, and capture architecture.

All key claims from previous stages were re-tested directly against the original source code (`calibration.c`, `fw9369_fdt.c`, `fw9369_startup.c`, `image.c`, `fw9369_release.c`, `linux_board.c`, and Linux kernel drivers). Critical conflations and inaccuracies in earlier reports were identified, dissected, and formally corrected. A consolidated canonical specification ([FW9369_UNIFIED_INITIALIZATION_SPECIFICATION.md](file:///home/titan/fte3600-linux-driver/diagnostics/FW9369_UNIFIED_INITIALIZATION_SPECIFICATION.md)) has been established.

---

## 2. Summary of Discrepancies Found & Corrected

| Area | Previous Assertion in Earlier Reports | Verified Source Code Reality | Impact & Resolution |
| :--- | :--- | :--- | :--- |
| **FDT 10 Samples** | Described as "10-sample baseline averaging loop". | **Zero averaging.** Up to 10 samples tracking **per-channel absolute minima** with early break if \(\ge 3\) consecutive samples are stable within \(\pm 5\) counts. | Corrected in [STAGE2C3_FDT_SAMPLING_RECONCILIATION.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C3_FDT_SAMPLING_RECONCILIATION.md). Specification updated with exact minimum logic. |
| **Image Baseline Frame** | Inferred multi-frame filter or average. | **Unaltered raw frame copy.** `image_count` is 1; `s->baseline` is a bit-exact `memcpy` of a single captured frame. | Zero image filtering required on cold baseline. |
| **Image Frame CRC** | Inferred full 10,240-byte frame CRC. | **First 128 words (256 bytes) only.** Evaluated as a fast header fingerprint in host RAM. | Corrected CRC boundaries in [STAGE2C3_CRC_RECONCILIATION.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C3_CRC_RECONCILIATION.md). |
| **FIFO Wire Transfer CRC** | Inferred transport CRC. | **Zero CRC on FIFO wire transfer.** Sensor outputs pure raw pixel stream. | SPI bus electrical integrity and DMA continuity must be relied upon. |
| **Pad 14 Lock Status** | Asserted as "fully locked in hardware". | `PADCFGLOCK` is set (config locked), but `PADCFGLOCKTX` is **NOT set** (Tx latch not locked). However, userspace access is blocked because ACPI marks the line `Exclusive`. | Detailed 5-boundary analysis in [STAGE2C3_RESET_AND_PAD14_REVIEW.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C3_RESET_AND_PAD14_REVIEW.md). |
| **Upstream `0x9A` Bugs** | Undetected in upstream code. | Severe lock leaks: `fw9369_fdt.c:219` and `fw9369_release.c:272` execute early return on error without calling lock write `0x9A = 0x00`. | Remedied by mandatory structured RAII wrapper pattern in [STAGE2C3_PROTECTED_REGISTER_FINAL_AUDIT.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C3_PROTECTED_REGISTER_FINAL_AUDIT.md). |
| **FIFO Buffer Requirement** | Inferred `bufsiz=65536` was sufficient. | `bufsiz=65536` is **necessary but not sufficient**. It satisfies the Linux syscall gateway, but requires DMA and valid sensor triggering. | Detailed in [STAGE2C3_FIFO_FEASIBILITY_REVIEW.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C3_FIFO_FEASIBILITY_REVIEW.md). |

---

## 3. What Remains Unknown

In strict alignment with empirical reality, the following states cannot be determined without physical operations:
1. **Physical Persistence of Register `0xC6`:** Whether `0xC6` maintains `0x01` or decays across prolonged idle while laptop standby power is applied is **UNKNOWN**. Software must unconditionally assert `0xC6 = 0x01` on every initialization.
2. **Physical Verification of Register `0x9A` Lock State:** Because the FT9362 transport protocol does not support reading register `0x9A`, the physical latch state in silicon following a write **CANNOT BE INDEPENDENTLY CONFIRMED VIA READBACK**.
3. **Silicon MCU Recovery without S5 Reboot:** If the internal FT9362 MCU enters an un-clocked or locked state, software cannot recover it via GPIO or SPI. An ACPI S5 cold reboot is the only known recovery path.

---

## 4. Authoritative Project Verdict

```
SPECIFICATION VERIFIED — READY FOR MOCK TOOLING DESIGN
```

---

## 5. Explicit Safety Boundary for the Next Stage

> [!IMPORTANT]
> **This verdict DOES NOT authorize physical hardware execution.**  
> Physical hardware execution remains strictly prohibited.

The next stage (Stage 2C.4) must be restricted to **non-hardware userspace tooling development**:
- Implementation of a C utility (`tools/fte3600-tool.c`).
- Built-in `--mock` and `--dry-run` backends that simulate sensor register responses and FIFO buffers entirely in user memory.
- Offline unit tests for mathematical DAC searches, minimum tracking, and little-endian word CRCs.
- Zero interactions with physical `/dev/spidev*` character devices.
