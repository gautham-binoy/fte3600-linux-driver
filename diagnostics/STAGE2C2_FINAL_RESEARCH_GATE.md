# Stage 2C.2 — Final Research Gate & Architecture Synthesis

**Document ID:** `STAGE2C2_FINAL_RESEARCH_GATE`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`, CRC `0x1C53`)  
**Status:** COMPLETE — STATIC ARCHITECTURAL RESEARCH & SAFETY GATE DECISION  
**Hardware Execution Authorization:** NONE (Static Analysis Only)

---

## 1. Executive Summary & Research Scope

Stage 2C.2 has completed an exhaustive, static reverse-engineering investigation of the entire FocalTech FW9369 initialization, AFE configuration, FDT calibration, image baseline acquisition, and autonomous finger-detection pipeline.

Following the definitive hardware identification of the FT9362 sensor in Stage 2A (Chip ID `0x9362`, CRC `0x1C53`), earlier piecemeal hardware proposals (such as an isolated MCU idle handshake in Stage 2C.1B) were cancelled due to asymmetric operational risk and lack of standalone utility. Stage 2C.2 was chartered to reconstruct the complete operational architecture before any further hardware operations are planned or authorized.

All findings have been derived purely from source code analysis of `research/libfprint-fte3600/`, kernel drivers (`pxa2xx-spi`, `intel-pinctrl`), and Dynabook Windows vendor telemetry, with **ZERO hardware execution**.

---

## 2. Synthesis of Stage 2C.2 Investigations

| Investigation & Document | Scope & Core Technical Findings | Source Citations | Safety & Architectural Impact |
| :--- | :--- | :--- | :--- |
| **1. Initialization Sequence**<br>[`STAGE2C2_INITIALIZATION_SEQUENCE.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C2_INITIALIZATION_SEQUENCE.md) | Reconstructed complete 7-phase flow: Phase 0 (Reset Prereqs) -> Phase 1 (Wakeup & ID) -> Phase 2 (MCU Idle & Foundry) -> Phase 3 (AFE Tuning & FDT Calibration) -> Phase 4 (Image Baseline Acquisition) -> Phase 5 (Detection Arming) -> Phase 6 (Runtime Capture). | `fw9369_release.c:438-510`, `ft93xx.c:166-320`, `fw9369_fdt.c:205-240` | Proves initialization is an integrated atomic pipeline that cannot be tested in fragments without stranding MCU state. |
| **2. FDT Calibration Review**<br>[`STAGE2C2_FDT_CALIBRATION_REVIEW.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C2_FDT_CALIBRATION_REVIEW.md) | Cataloged all 18 AFE registers (`0x1800`..`0x180D`, `0x1880`..`0x1888`, protected bank `0x00C0`..`0x00C2`). Decoded hybrid DAC centering search (`0..127` targeting 512). Verified 10-sample baseline loop and exact threshold formula: `fdt_down[c] = max(0, minima[c] - 30)`. Verified 4-word CRC. | `fw9369_fdt.c:27-142`, `fw9369_fdt.c:158-196`, `fw9369_fdt.c:215-235` | Thresholds and baselines exist strictly in volatile host/sensor RAM. Calibration is mandatory on every cold start. |
| **3. Image DAC & Baseline Review**<br>[`STAGE2C2_IMAGE_DAC_AND_BASELINE_REVIEW.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C2_IMAGE_DAC_AND_BASELINE_REVIEW.md) | Proved sensor dimensions are $64 \times 80 = 5,120$ pixels (10,240 bytes big-endian 16-bit). Decoded 10,246-byte burst FIFO transfer (prefix `0x06 0xF9 0x9A 0x05 0x14 0x00`). Verified adaptive DAC search targeting median 512 counts. Validated 128-word CRC. | `fw9369_fdt.c:83-100`, `fw9369_fdt.c:113-145`, `ft93xx.c:350-410` | FDT calibration is a mandatory prerequisite for baseline image acquisition. Baseline frames are kept in volatile host memory. |
| **4. Protected Register Safety**<br>[`STAGE2C2_PROTECTED_REGISTER_SAFETY.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C2_PROTECTED_REGISTER_SAFETY.md) | Audited every site accessing unlock key `0x9A = 0x5A` and lock `0x9A = 0x00`. Uncovered upstream bugs leaving `0x9A` unlocked on transport failure (`fw9369_fdt.c:219`, `fw9369_release.c:272`). Proved readback of `0x9A` is unsupported by transport protocol. | `fw9369_fdt.c:219-224`, `fw9369_release.c:272`, `fw9369_release.c:475` | Protocol requires mandatory structured RAII / `finally` lock enforcement to prevent accidental OTP/EFUSE exposure. |
| **5. Failure State Matrix**<br>[`STAGE2C2_FAILURE_STATE_MATRIX.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C2_FAILURE_STATE_MATRIX.md) | Analyzed 14 distinct failure modes across communication, calibration, and FIFO timeouts. Proved reference driver reset recovery (pulsing Pad 14 low for 15 ms) is impossible on this platform due to pinctrl hardware locking. | Platform ACPI DSDT, `drivers/pinctrl/intel/pinctrl-tigerlake.c` | ACPI S5 cold reboot is the only non-intrusive recovery path if sensor MCU locks up. Hard timeout caps required on all loops. |
| **6. SPI FIFO Feasibility**<br>[`STAGE2C2_SPI_FIFO_FEASIBILITY.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C2_SPI_FIFO_FEASIBILITY.md) | Proved mainline `spidev` default `bufsiz=4096` causes `-EMSGSIZE` on 10,246-byte frames. Sensor FIFO does not support segmentation. Verified Intel LPSS SPI (`pxa2xx-spi.2`) and DMA (`idma64.2`) support 10,246-byte single-burst transfers (~82 ms at 1 MHz). | `drivers/spi/spidev.c:89`, `drivers/spi/spi-pxa2xx.c:980` | Loading `spidev` must explicitly specify module parameter `bufsiz=65536`. |
| **7. Hardware State Record**<br>[`STAGE2C2_HARDWARE_STATE_RECORD.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C2_HARDWARE_STATE_RECORD.md) | Documented complete hardware invariance across Stages 2B.1 through 2C.2. Confirmed host rollback remains 100% clean (0 errors). Documented Pad 14 (`0x84000201`), Pad 229 High-Z input (`0x82000100`), and unknown `0xC6` persistence. | `diagnostics/stage2a-results.txt`, `/sys/kernel/debug/pinctrl/` | Explicit distinction between host rollback and silicon internal state. Raw MMIO writes strictly prohibited. |
| **8. Future Roadmap Decomposition**<br>[`STAGE2C2_FUTURE_STAGE_DECOMPOSITION.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C2_FUTURE_STAGE_DECOMPOSITION.md) | Structured the forward plan into 4 logical stages: 2C.3 (Static Safety Review), 2C.4 (Userspace Tooling with Mock/Dry-Run), 2C.5 (Supervised Hardware Execution), and 2C.6 (Production Driver Integration). | Project architectural consensus | Eliminates fragmented hardware experiments in favor of a disciplined, gated engineering pipeline. |

---

## 3. Authoritative Decision & Single-Choice Verdict

The research phase is fully concluded. The register maps, calibration mathematics, buffer sizing constraints, protected register vulnerabilities, and hardware state realities have been exhaustively documented and validated against source code.

### Authoritative Project Verdict

```
RESEARCH COMPLETE — READY FOR A SEPARATE INITIALIZATION SAFETY REVIEW
```

---

## 4. Hardware Safety Boundary & Operational Directives

> [!IMPORTANT]
> **This verdict DOES NOT authorize hardware execution.**  
> Zero SPI transactions, GPIO manipulations, module loads, or character device interactions may be performed.

1. **Host State Remained Untouched:** `spidev` remains unloaded, `spi-FTE3600:00` remains unbound, and `/dev/spidev*` does not exist.
2. **Next Objective (Stage 2C.3):** Conduct a formal static safety review of the unified startup and calibration sequence specification, verifying structured RAII lock handling and cycle-accurate packet structures prior to authoring userspace tool code.
