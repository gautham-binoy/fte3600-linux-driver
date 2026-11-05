# STAGE D6.3 — CHALLENGE & JUSTIFICATION AUDIT OF THE `READY` STATE TRANSITION

**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Host Architecture:** Intel LPSS SPI2 (`spi-FTE3600:00`, CS0) on Dynabook Satellite Pro C40-J  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Date:** 2026-11-04  
**Author:** Antigravity (Advanced Agentic Pair Programming)  
**Classification:** ARCHITECTURAL CHALLENGE & STATE MACHINE AUDIT

---

## 1. Executive Summary

In Stage D6 and D6.2, completion of `fte3600_hw_startup()` assigns:
```c
data->state = FTE3600_STATE_READY;
```
This audit challenges whether authoritative vendor source establishes that the 6-step volatile sequence (wake, idle check, foundry interrogation loop, and reading register `0x9B`) is sufficient to declare the sensor `READY`.

**Audit Conclusion:**
The transition to `FTE3600_STATE_READY` after foundry interrogation is an **architectural over-claim**.
Authoritative vendor source (`fw9369_startup.c`, `ft93xx.c`, `capture.c`) proves that the 6-step sequence corresponds strictly to the pre-operational negotiation phase (`prefix()`). In vendor code, the sensor cannot capture images or detect touch until full analog calibration (`fdt_baseline`, `image_baseline`) and touch arming (`arm()`) are executed. Furthermore, vendor `prefix()` explicitly writes `0x1A84 = 0xFFFF` twice, which the kernel driver currently omits.

---

## 2. Separation of Facts and Claims

### 2.1 What Vendor Source Explicitly Does
1. **Transceiver Wake & Interrogation (`prefix()` in `fw9369_startup.c:321–384`):**
   - Wakes transceiver via `0xC6 = 0x01`.
   - Reads chip ID `0x1A8B`.
   - Clears interrupts via `write_word(op, 0x1A84, 0xFFFF)`.
   - Issues idle handshake: `CMD_WAKE (0x5A/0xA5/0x00)` $\rightarrow$ reads `0x80` $\rightarrow$ `CMD_IDLE_ACK (0xA5/0x5A/0x00)`.
   - Executes 2 iterations of foundry interrogation loop (`0xF1`, `0xF4`, `0xF3`).
   - Reads silicon fab code `0x9B` (detects SMIC `0xAA` vs default) and assigns line period (150 us or 200 us).
   - Re-clears interrupts via `write_word(op, 0x1A84, 0xFFFF)`.
   - Returns `FTE3600_FW9369_STARTUP_OK`.
2. **Post-Prefix Mandatory Operations in `fw9369_startup.c:452–487`:**
   - **`fdt_baseline(&op)`:** Performs dynamic search across DAC registers (values 0..63) to calibrate analog differential sense amplifier baselines.
   - **`image_baseline(&op, reset)`:** Collects 10 background sample frames to compute pixel dark-current noise offsets.
   - **`arm(&op)`:**
     * Unlocks SRAM bank (`write_byte(op, 0x9a, 0x5a)`).
     * Programs 8 threshold words into SRAM (`0x80B0` or `0x80E0`).
     * Relocks SRAM bank (`write_byte(op, 0x9a, 0)`).
     * Configures control register `0x1881`.
     * Programs interrupt mask `0x1A83`.
     * Clears touch latch bits: `write_word(op, 0x1A84, 0x002F)`.
     * Issues touch arm command: `command(op, 0xC2, 0x3D)`.
3. **Vendor State Machine in `ft93xx.c:800`:**
   Upon completing startup, the driver flags:
   ```c
   output->calibration_pending = 1;
   ```
   The device is NOT marked ready to capture until userspace calibration is committed.
`[VENDOR-REFERENCE-OBSERVED]`

---

### 2.2 What the Kernel Implementation Currently Does
In [`kernel/fte3600.c:480–594`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L480-L594):
- Executes the transceiver wake, idle stabilization, foundry loop, and fab code read `0x9B`.
- Omits both `0x1A84 = 0xFFFF` writes.
- Omits all DAC baseline calibration, image offset acquisition, SRAM threshold programming, and touch arming.
- Assigns `data->state = FTE3600_STATE_READY`.
- Permits userspace to issue `FTE3600_IOC_CAPTURE`.
`[SOURCE-OBSERVED]`

---

### 2.3 What Is Inferred
- It is inferred that the sensor's digital clock tree, internal finite state machine, and SPI communications link are stable and ready to receive further commands.
- It is inferred that the sensor does not immediately lock up if `0x1A84 = 0xFFFF` is omitted during idle negotiation.
- It is inferred that `FTE3600_STATE_READY` is a software-level placeholder representing the end of the driver's current implementation milestone rather than the silicon's true operational state.
`[INFERRED]`

---

### 2.4 What Remains Unverified
1. **Interrupt Line State:** Whether omitting `0x1A84 = 0xFFFF` leaves the physical ACPI GPIO interrupt line (`INT3510 / GPP_A17`) permanently asserted high in silicon.
2. **Silicon Capture Behavior:** Whether issuing `FTE3600_IOC_CAPTURE` to an uncalibrated FT9362 silicon core causes FIFO timeouts, hung transactions, or unrecoverable sensor states.
3. **Missing Hardware Steps:** Whether the digital core requires the SRAM threshold initialization (`0x9A = 0x5A`) before sensor analog array clocks can be powered up for frame scanning.
`[NOT ESTABLISHED]`

---

## 3. Analysis of the Semantic Conflict

In Linux kernel driver conventions and UAPI definitions:
- State `READY` communicates to userspace (e.g. `fprintd`, `libfprint`) that the peripheral is fully prepared to perform its primary function: capture fingerprint frames and wait for finger touch events.
- In `fte3600.h`, `FTE3600_IOC_CAPTURE` enforces:
  ```c
  if (data->state != FTE3600_STATE_READY)
      return -EPERM;
  ```
- If a client calls `FTE3600_IOC_CAPTURE` while the driver is in `FTE3600_STATE_READY`, but the underlying silicon has uncalibrated sense amplifiers and uninitialized baseline thresholds, the capture operation cannot succeed.
- Declaring `FTE3600_STATE_READY` immediately following foundry interrogation therefore misrepresents the operational capability of the hardware.

---

## 4. State Machine Recommendations

To ensure architectural truthfulness and prevent premature invocation of uncalibrated capture operations, the state machine should be refined in upcoming stages:

### Recommendation Options:

1. **Introduce Intermediate Internal State (Recommended):**
   - Retain `FTE3600_STATE_IDENTIFIED` (8) or introduce `FTE3600_STATE_INITIALIZED_UNCALIBRATED` (extended internal state).
   - In `fte3600_state_to_uapi()`, map this state to a public snapshot that keeps `state == FTE3600_STATE_IDENTIFIED` with a new flag `FTE3600_FLAG_STARTUP_COMPLETE (1U << 4)`.
   - Reserve `FTE3600_STATE_READY` strictly for the post-calibration, post-arming milestone (Stage D7/D8).
2. **Gate Capture on Calibration Flag:**
   - Retain `state == FTE3600_STATE_READY`, but add `data->calibration_valid` and reject `FTE3600_IOC_CAPTURE` with `-ENEEDAUTH` or `-EPERM` until valid calibration data is committed.

---

## 5. Summary Verdict on `READY` State

The current assignment of `FTE3600_STATE_READY` at line 588 of `kernel/fte3600.c` is:
- **Framing & Syntax:** Syntactically correct and type-safe.
- **Protocol Justification:** **UNRESOLVED / INSUFFICIENTLY SUPPORTED** by vendor reference implementations. Vendor reference code establishes that the device is uncalibrated and not ready for capture.
