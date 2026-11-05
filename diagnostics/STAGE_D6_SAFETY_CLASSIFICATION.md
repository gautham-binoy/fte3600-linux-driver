# Stage D6 — FT9362 Safety Classification and Boundary Report

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Controller  
**Document Ref:** `diagnostics/STAGE_D6_SAFETY_CLASSIFICATION.md`  

---

## 1. Safety Classification Taxonomy

Every register and SPI operation associated with the FT9362 controller is classified into one of four safety tiers:

1. **TIER 1 — SAFE VOLATILE (Implemented in Stage D6):** Operations that configure volatile internal logic, perform transient bus wakes, clear interrupt latches, or read read-only identity registers. Zero risk of silicon wear, uncalibrated state latching, or uncommanded interrupt storms.
2. **TIER 2 — BLOCKED / CALIBRATION (Deferred to Userspace Biometrics):** Operations that require per-device calibration parameters (DAC offset vectors, pixel gain/offset maps, finger touch sensitivity thresholds). Executing these without valid calibrated data will cause corrupted biometric capture and erratic touch thresholds.
3. **TIER 3 — BLOCKED / IRQ ACTIVATION (Deferred to Touch Milestone):** Operations that arm the hardware sensing engine to monitor Pad 229 and pulse interrupt lines autonomously. Executing these without fully bound interrupt handlers causes unhandled IRQ loops.
4. **TIER 4 — HAZARDOUS / NON-VOLATILE WRITES (Permanently Blocked / Non-Existent):** Operations that write to OTP, EFUSE, or Flash memory. 

---

## 2. Operation Inventory and Status

| Operation | Target Register / Command | Tier | Status in Stage D6 | Rationale & Safety Boundary |
| :--- | :--- | :---: | :---: | :--- |
| **Clear Interrupt Latches** | `write_reg16(0x1A84, 0xFFFF)` | 1 | **IMPLEMENTED** | Pure volatile latch reset. Clears power-on interrupt flags. Essential for safe initialization. |
| **Wake Strobe 1** | `cmd_strobe(0x5A, 0xA5)` | 1 | **IMPLEMENTED** | Pure volatile wake pulse (`CMD_WAKE`). Moves controller out of low-power sleep. |
| **Status 0x80 Poll** | `read_sfr(0x80)` | 1 | **IMPLEMENTED** | Read-only status verification (checks for idle code `0x50`). |
| **Abort Strobe** | `cmd_strobe(0xC0, 0x3F)` | 1 | **IMPLEMENTED** | Pure volatile abort fallback (`CMD_ABORT`). Restores controller to idle if stuck. |
| **Wake Strobe 2** | `cmd_strobe(0xA5, 0x5A)` | 1 | **IMPLEMENTED** | Pure volatile idle acknowledge (`CMD_IDLE_ACK`). Arms command decoder. |
| **Sense-Amp Interrogation**| SFR `0xF1`, `0xF4`, `0xF3` | 1 | **IMPLEMENTED** | Volatile interrogation sequence. Cycles sense-amp timing state to latch fab ID. |
| **Read Foundry Code** | `read_sfr(0x9B)` | 1 | **IMPLEMENTED** | Read-only register interrogation. Detects SMIC Fab AA (`(rx[4] >> 2) == 0x13`). |
| **SRAM Bank Unlock** | `write_sfr(0x9A, 0x5A)` | 2 | **BLOCKED** | Unlocks internal SRAM configuration bank. Part of touch threshold write sequence. |
| **Threshold Bank Write** | `write_reg16(0x00E0, val)` | 2 | **BLOCKED** | Programs baseline finger detection capacitance threshold. Requires calibrated userspace profile. |
| **Dynamic DAC Search** | Hill-climbing algorithm | 2 | **BLOCKED** | Requires iterative frame capture and biometric contrast calculation. Inappropriate for kernel space. |
| **Arm Touch Engine** | `cmd_strobe(0xC2, 0x3D)` | 3 | **BLOCKED** | `CMD_ARM_TOUCH`: Triggers autonomous scanning and physical electrical interrupts on Pad 229. |
| **OTP / EFUSE Writes** | High-voltage programming | 4 | **BLOCKED** | FT9362 contains no writeable non-volatile memory or Flash $V_{PP}$ programming voltage. |

---

## 3. Analysis of Critical Boundaries

### 3.1 SRAM Bank Unlock & Threshold Configuration
- **Hardware Mechanism:** Register `0x9A` acts as an unlock key. Writing `0x5A` unlocks SRAM bank 0.
- **Wire Address Finding:** Vendor startup sequences write to `0x00E0` (framed as `0x05, 0xFA, 0x80, 0xE0...`), NOT `0x18E0`.
- **Safety Decision:** This operation is strictly blocked from baseline kernel initialization. Setting an arbitrary threshold causes spurious touch interrupts or permanently disables touch detection. It will be exposed via a dedicated userspace ioctl when biometric calibration utilities are deployed.

### 3.2 Dynamic DAC Calibration
- **Hardware Mechanism:** The FT9362 analog front-end requires tuning reference DAC voltages to balance pixel DC bias across environmental temperature shifts.
- **Biometric Context:** The algorithm requires capturing frames, calculating standard deviation and mean grayscale levels, and adjusting DAC values until pixel histograms center at 128.
- **Safety Decision:** Biometric histogram analysis and hill-climbing search violate kernel separation-of-concerns. This logic belongs exclusively in userspace (e.g., `libfprint` driver backend).

### 3.3 Touch Arming (`CMD_ARM_TOUCH`)
- **Hardware Mechanism:** Strobing `{ 0xC2, 0x3D }` switches the silicon into ultra-low-power touch-sensing mode.
- **Interrupt Impact:** Once armed, any physical touch on the sensor plate pulls Pad 229 low, triggering an edge-triggered ACPI interrupt.
- **Safety Decision:** Blocked until Stage D7 / IRQ handling milestone, preventing unhandled interrupt loops.
