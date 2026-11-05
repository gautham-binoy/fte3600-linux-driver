# Stage D7 — Final Verdict & Investigation Sign-Off

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Safety Gate:** Active and Untouched (`Option 1` strictly enforced)  
**Final Stage Classification:** **`PASS`**  

---

## 1. Executive Summary

Stage D7 was executed as an independent, strictly read-only source code investigation into the vendor calibration, baseline, and arming procedures (`fdt_baseline()`, `image_baseline()`, and `arm()`).

All procedural sequences, register operations, timing parameters, command strobes, algorithmic search loops, interrupt semantics, and data-flow paths have been completely and independently reconstructed from primary vendor sources (`fw9369_startup.c`, `fw9369_fdt.c`, `capture.c`, `image.c`, `fw9369_release.c`, `calibration.c`, and `ft93xx.c`).

Zero hardware operations were authorized, requested, or performed. The live kernel module and physical fingerprint sensor remain completely untouched under Option 1.

---

## 2. Categorical Findings Separation

To maintain absolute scientific and engineering clarity, the findings are separated into three distinct boundaries:

### Category 1: What Vendor Source Code Proves

1. **Procedural Sequence & Hierarchy:**
   - Full sensor startup follows a strict linear sequence:
     `prefix() -> fdt_baseline() -> image_baseline() -> arm()`.
   - Completion is tracked via diagnostic bitmask: bit 0 = FDT baseline, bit 1 = Image baseline, bit 2 = Arming (`completed == 7`).
2. **FDT Calibration Mechanism (`fdt_baseline()`):**
   - Calibrates 4-channel analog DAC bias via successive approximation binary search towards midpoint ADC value 512.
   - Collects 10 stability samples and verifies that channel minima fall in `[300, 700]`.
   - Computes operational touch detection thresholds: `minima[c] - 30`.
   - Protects threshold configuration using bank unlock `0x9A = 0x5A` and relock `0x9A = 0x00`.
3. **Image Calibration Mechanism (`image_baseline()`):**
   - Actively captures multiple physical 10,246-byte frame buffer payloads from hardware FIFO `0x9A05`.
   - Computes array median across 5,120 pixels to find optimal image DAC bias.
   - Stores dark background reference matrix and asserts readiness flags `baseline_flag_1 = baseline_flag_2 = 1`.
4. **Sensor Arming Mechanism (`arm()`):**
   - Configures capacitive touch detection (FDT), **NOT** image capture.
   - Down arm configures threshold bank for finger touch down; Up arm configures threshold bank for finger release.
   - Issues command strobe `0xC2 0x3D` to place the hardware into autonomous monitoring mode.
5. **Interrupt Controller Semantics (`0x1A84`):**
   - `0x1A82` = Status, `0x1A83` = Mask, `0x1A84` = Write-1-To-Clear (W1TC).
   - Bit 3 = FDT scan ready, Bit 5 = Image frame ready, Bits 0/1 = Touch events.
   - Runtime event handling strictly follows the `read(0x1A82) -> write(0x1A84, word)` paradigm.
6. **Zero Persistent Storage Hazard:**
   - All calibration and baseline registers are volatile SFRs or volatile SRAM. Zero OTP, eFuse, or flash programming facilities are accessed.

---

### Category 2: What the Current Kernel Implementation Supports

1. **State Machine Boundary:**
   - The kernel driver (`kernel/fte3600.c`) correctly advances from `UNINITIALIZED` (0) through `RESET` (1) and `IDENTIFIED` (8) to `INITIALIZED_UNCALIBRATED` (14).
   - In accordance with Stage D6.4 and D6.6, internal state 14 withholds `READY` and asserts telemetry flags `STARTUP_PREFIX_COMPLETED` and `CALIBRATION_PENDING`.
2. **Deterministic Precondition Gating:**
   - `FTE3600_IOC_CAPTURE` is strictly gated on `data->state == FTE3600_STATE_READY`, returning `-EPERM` in all prior states and `-ENODEV` when offline.
   - Unknown initialization flags are strictly rejected with `-EINVAL` prior to hardware access.
3. **SPI Framing & Transport Safety:**
   - Three-byte command strobes (`{cmd, ~cmd, 0x00}`) and validated transfer limits are enforced.
   - Unverified `0x1A84` writes remain deliberately isolated from baseline kernel initialization.

---

### Category 3: What Remains Unknown Without Hardware Validation

1. **Electrical Noise Floor of Specific Laptop Unit:**
   - Whether the Dynabook Satellite Pro C40-J sensor glass and enclosure match the reference nominal baseline of `[300, 700]` ADC counts or require baseline adjustment.
2. **Interrupt Pin Routing in LPSS GPIO Controller:**
   - Whether Pad 229 interrupt signaling operates in level-triggered or edge-triggered mode under real ACPI power transitions.
3. **Optimal In-Kernel vs Userspace Boundary:**
   - Whether full 10 KB image baseline capture should be implemented as an autonomous in-kernel routine (Option A) or delegated to userspace via primitive scanning ioctls (Option B).

---

## 3. Passive Invariant Verification

Throughout Stage D7, passive invariants were checked and found completely unperturbed:
- Module state: `fte3600 20480 0 - Live 0x0000000000000000 (OE)`
- SPI controller statistics: `transfers = 3`, `bytes = 19`
- Device node: `/dev/fte3600` major `511`, minor `0`
- Active file leases: `0`

---

## 4. Final Stage Classification

**Classification Verdict:**
**`PASS`**  
All relevant vendor calibration, baseline, arming, and interrupt procedures have been completely reconstructed with comprehensive source evidence, exact file citations, and clear architectural boundaries. No hardware access was executed or requested.
