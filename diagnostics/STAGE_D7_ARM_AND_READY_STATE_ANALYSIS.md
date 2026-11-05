# Stage D7 — FT9362 Sensor Arming & READY State Transition Analysis

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Investigation Mode:** Read-Only Static Source Analysis (Zero Physical Hardware Communication)  

---

## 1. Executive Summary

This report establishes the hardware mechanism, register operations, and lifecycle semantics of the vendor sensor arming procedure (`arm()`), and reconstructs the authentic hardware state-transition sequence required to reach a legitimately operational `READY` state.

Our audit resolves a fundamental architectural question: **What does arming actually enable?**  
Vendor sources demonstrate that arming does **NOT** start image frame capture. Instead, arming configures the sensor's low-power autonomous **Finger Detection (FDT)** hardware comparator circuit with calibrated threshold baselines. The sensor is placed into a low-power monitoring state, waiting for a user's finger contact (Touch Down) or finger removal (Touch Up). Upon threshold crossing, the sensor asserts the physical hardware interrupt line (Pad 229) and latches status bits into register `0x1A82`. Only after this touch interrupt is delivered does host software initiate active image acquisition.

---

## 2. Source Discovery & Call-Site Provenance

The arming procedure is implemented and called in the following vendor files:

1. **`fw9369_startup.c` (lines 386–421):**  
   `arm(struct startup *op)`. The final initialization stage executed during full startup. Loads calibrated thresholds into the hardware comparator bank, sets interrupt enables, acknowledges existing events, and issues the arm trigger strobe `0xC2 0x3D`.
2. **`fw9369_release.c` (lines 243–293):**  
   `arm_detection()`. Runtime re-arming routine. Supports both `FTE3600_FW9369_DOWN` (arming for finger touch) and `FTE3600_FW9369_UP` (arming for finger lift).
3. **`fw9369_release.c` (lines 301–307):**  
   `fte3600_fw9369_arm_detection_linux()`. Wrapper enabling Linux-specific event routing bits (`(0x1A83 & 0xFF80) | 0x0016`).
4. **`device_session.c` (lines 466–495, 647–651, 709–716):**  
   Session manager demonstrating arm lifecycle, Down arm retention during capture, and Up arm transition during finger release.
5. **`ft93xx.c` (lines 1340–1361):**  
   `fdt_arm()`. Generic FT93xx multi-chip arm implementation confirming threshold bank framing, interrupt clearing, and command strobe sequencing.

---

## 3. Hardware Conditions & Entry Prerequisites

Arming cannot be executed in an arbitrary or uncalibrated state. The vendor code enforces strict prerequisites:

1. **Hardware Calibration Prerequisite:**
   - `fdt_baseline()` must have successfully executed, establishing valid `fdt_down` and `fdt_up` 16-bit threshold arrays and their verified CRC checksums.
   - `image_baseline()` must have successfully executed, setting:
     ```c
     s->baseline_flag_1 = 1;
     s->baseline_flag_2 = 1;
     ```
   - In `device_session.c:702`, any startup attempt where `!next.baseline_flag_1 || !next.baseline_flag_2` immediately fails with `FTE3600_DEVICE_CALIBRATION_ERROR`.

2. **Direction Selection:**
   - **Down Arm (`FTE3600_FW9369_DOWN`):** Configures comparator thresholds from `fdt_down`. Arms hardware to trigger when capacitance rises above baseline (finger touches down).
   - **Up Arm (`FTE3600_FW9369_UP`):** Configures comparator thresholds from `fdt_up`. Arms hardware to trigger when capacitance falls back to baseline (finger lifted).

---

## 4. Complete Ordered Arming Register Sequence

The arming procedure (`arm()` / `arm_detection()`) performs the following exact hardware sequence:

1. **FDT Frontend Configuration (`fw9369_fdt.c:167–234`):**
   - Calls `fdt_run(FTE3600_FW9369_FDT_CONFIGURE)` to ensure all analog biases (`0x1801`), integration timings (`0x1804`, `0x1807`), and channel settings are active.

2. **Bank Unlock Operation (`fw9369_startup.c:396`):**
   - Transmit: `0x09 0xF6 0x9A 0x5A` (Write Byte `0x9A = 0x5A` -> Unlock Comparator Bank).

3. **Threshold Bank Multi-Word Transfer (`fw9369_startup.c:388, 397–404`):**
   - Selects target register address based on foundry code:
     - SMIC Fab AA (`foundry == 0xAA`): target address `0x80E0`.
     - Non-SMIC (`foundry != 0xAA`): target address `0x80B0`.
   - Prepares 22-byte SPI transaction buffer:
     ```c
     uint8_t bytes[22] = {
         0x05, 0xFA, 0x80, (foundry == 0xAA ? 0xE0 : 0xB0), 0x00, 0x08,
         /* 8 big-endian 16-bit threshold words from fdt_down or fdt_up */
     };
     ```
     - `0x05`: Multi-word write opcode.
     - `0xFA`: One's complement opcode (`~0x05`).
     - `0x80 [0xE0 or 0xB0]`: Comparator threshold base address.
     - `0x00 0x08`: Word count (8 words = 16 bytes).
     - Bytes [6..21]: 8 threshold words (`thresholds[c] >> 8`, `thresholds[c] & 0xFF`).
   - Transfers the 22-byte buffer over SPI.

4. **Bank Relock Operation (`fw9369_startup.c:405`):**
   - Transmit: `0x09 0xF6 0x9A 0x00` (Write Byte `0x9A = 0x00` -> Relock Bank).

5. **Arming Mode Register Modification (`fw9369_startup.c:406–408`):**
   - Read Word `0x1881`.
   - Modify:
     - If Down Arm: `(value & 0xFF7C) | 0x0081` (asserts bit 7 and bit 0).
     - If Up Arm: `(value & 0xFF7C) | 0x0000` (clears bit 7 and bit 0).
   - Write Word `0x1881` back to hardware.

6. **Linux Event Routing Mask (`fw9369_startup.c:409–415`):**
   - When running on Linux (`linux_detection == 1`):
     - Read Word `0x1A83`.
     - Modify: `(value & 0xFF80) | 0x0016` (sets bits 4, 2, 1).
     - Write Word `0x1A83` back to hardware.

7. **Interrupt Pre-Clear / Acknowledgment (`fw9369_startup.c:416`):**
   - Write Word `0x1A84 = 0x002F` (Write-1-To-Clear bits 0, 1, 2, 3, 5).
   - Clears any stale or pending touch and measurement interrupt flags before activation.

8. **Arm Strobe Trigger Command (`fw9369_startup.c:417`):**
   - Transmit: `0xC2 0x3D 0x00` (3-byte command strobe activating the FDT comparator engine).

9. **Settling Delay (`fw9369_startup.c:418`):**
   - Wait: `1 ms`.
   - Arming is now complete (`completed |= 4`). The sensor hardware is autonomously scanning for touch.

---

## 5. Touch Detection vs Image Acquisition

A critical architectural distinction established by this audit is the strict operational decoupling between Finger Detection and Image Capture:

```
   ┌────────────────────────────────────────────────────────┐
   │ 1. Autonomous Touch Monitoring State (ARMED)           │
   │    Hardware periodically scans 4 capacitive FDT pads.  │
   │    Sensing array & image ADC are powered down.         │
   │    Power consumption is minimal (~microamps).          │
   └────────────────────────┬───────────────────────────────┘
                            │ [User places finger on sensor]
                            ▼
   ┌────────────────────────────────────────────────────────┐
   │ 2. Hardware Touch Interrupt Generation                 │
   │    Capacitance exceeds calibrated threshold bank.      │
   │    Sensor asserts Pad 229 (IRQ) low/edge.              │
   │    Sensor latches status into register 0x1A82.         │
   └────────────────────────┬───────────────────────────────┘
                            │ [Kernel ISR / Host Notification]
                            ▼
   ┌────────────────────────────────────────────────────────┐
   │ 3. Active Image Acquisition Trigger                    │
   │    Host reads & clears 0x1A82 / 0x1A84.                │
   │    Host powers on imaging array via 0x1800.            │
   │    Host sends Command Strobe 0xC4 0x3B.                │
   │    Host reads 10,246 bytes from FIFO 0x9A05.           │
   └────────────────────────────────────────────────────────┘
```

**Key Takeaways:**
1. Arming enables **capacitive touch detection only**, not image sensor exposure.
2. The image sensor is only powered on and triggered after a valid touch interrupt is received.
3. Repeated arming (`arm_detection()`) is standard and completely safe: on finger release, the driver issues an Up arm, waits for finger lift, and then re-arms with a Down arm without needing to recalibrate baselines.

---

## 6. Authentic Hardware State-Transition Sequence

Synthesizing the findings from Stages D6.2, D6.4, D6.6, and D7, the true complete state lifecycle of the FT9362 silicon is:

```
  ┌─────────────────────────────────┐
  │  FTE3600_STATE_UNINITIALIZED    │ (0) Initial driver state / offline mapping
  └────────────────┬────────────────┘
                   │ FTE3600_IOC_RESET (Pad 14 active-low pulse)
                   ▼
  ┌─────────────────────────────────┐
  │  FTE3600_STATE_RESET            │ (1) Hardware reset asserted & deasserted
  └────────────────┬────────────────┘
                   │ FTE3600_IOC_INIT (flags = IDENTIFY_ONLY)
                   ▼
  ┌─────────────────────────────────┐
  │  FTE3600_STATE_IDENTIFIED       │ (8) Wake negotiated (0xC6=1), ID verified (0x9362)
  └────────────────┬────────────────┘
                   │ FTE3600_IOC_INIT (flags = FULL_STARTUP)
                   ▼
  ┌─────────────────────────────────┐
  │ FTE3600_STATE_UNCALIBRATED      │ (14) Volatile prefix complete (foundry read, period set)
  │ (PREFIX_COMPLETE)               │      [Public: state 8 + PREFIX_COMPLETED + CALIB_PENDING]
  └────────────────┬────────────────┘
                   │ [Stage D8: fdt_baseline() execution]
                   ▼
  ┌─────────────────────────────────┐
  │ (Internal) FDT_CALIBRATED       │ FDT DAC calibrated, 4-channel thresholds established
  └────────────────┬────────────────┘
                   │ [Stage D8: image_baseline() execution]
                   ▼
  ┌─────────────────────────────────┐
  │ (Internal) BASELINE_ACQUIRED    │ Image DAC calibrated, dark reference frame stored
  └────────────────┬────────────────┘
                   │ [Stage D8: arm() execution]
                   ▼
  ┌─────────────────────────────────┐
  │  FTE3600_STATE_READY            │ (4) Threshold bank written, FDT armed, IRQ enabled
  │                                 │     [Genuinely operational, CAPTURE permitted]
  └────────────────┬────────────────┘
                   │ FTE3600_IOC_CAPTURE (on touch interrupt)
                   ▼
  ┌─────────────────────────────────┐
  │  FTE3600_STATE_CAPTURING        │ (5) 0xC4 0x3B strobe, FIFO streamed (10246 bytes)
  └─────────────────────────────────┘
```

---

## 7. Operational Verdict

1. **State 14 is Not Ready:** Stage D6.4 was entirely correct to withhold `READY` after prefix completion. Operational readiness requires the FDT baseline, Image baseline, and Arming procedures.
2. **Transition Requirements for `READY`:** State 4 (`READY`) can only be legitimately assigned after `arm()` succeeds and asserts `completed |= 4`.
3. **Capture Gating Integrity:** Gating `FTE3600_IOC_CAPTURE` on `data->state == FTE3600_STATE_READY` is strictly validated by vendor architecture. Calling capture before arming would fail because the imaging engine is unconfigured and the sensor is uncalibrated.
