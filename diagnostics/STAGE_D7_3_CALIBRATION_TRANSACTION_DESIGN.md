# Stage D7.3 — Calibration Transaction Boundaries & Primitive Operation Design

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source Architecture Review (Zero Hardware Operations)  
**Deliverable:** `diagnostics/STAGE_D7_3_CALIBRATION_TRANSACTION_DESIGN.md`  

---

## 1. Executive Summary

This report decomposes the vendor initialization and calibration sequence (`fw9369_startup.c`, `fw9369_fdt.c`, `calibration.c`, `capture.c`, `image.c`) into the smallest safe, bounded logical operations for the proposed hybrid architecture (Design C).

Every operation is analyzed for its inputs, outputs, register accesses, timing, transfer limits, side effects, retry safety, and failure characteristics. Proposed kernel mechanisms are rigorously distinguished from vendor reference code.

---

## 2. Inventory of Calibration Transaction Boundaries

```
  PROPOSED HYBRID CALIBRATION STAGES:

  [STAGE 1: Prefix Completion (Existing in fte3600_hw_startup)]
          │ (Hardware enters INITIALIZED_UNCALIBRATED)
          ▼
  [STAGE 2: Calibration Session Begin (FTE3600_IOC_CALIB_BEGIN)]
          │
          ├──> [STAGE 3: FDT Analog Bias DAC Search (CALIB_FDT_SAMPLE)]
          │      └── Input: Candidate DAC (1..125) -> Output: 4 ADC channel values
          │
          ├──> [STAGE 4: FDT Multi-Sample Minima Evaluation]
          │      └── Runs in userspace or bounded loop; checks [300, 700]
          │
          ├──> [STAGE 5: Threshold Calculation]
          │      └── Pure computation: value = minima[c] - 30 (userspace)
          │
          ├──> [STAGE 6: Image Baseline DAC Search (CALIB_READ_FRAME)]
          │      └── Input: Image DAC (0..127) -> Output: 10,240-byte raw pixel frame
          │
          ├──> [STAGE 7: Dark Frame Reference Caching]
          │      └── Userspace stores frame 2 as background dark frame
          │
          ├──> [STAGE 8: Threshold Bank Programming (CALIB_SET_THRESHOLDS)]
          │      └── Input: 4 verified thresholds -> Writes 0x80B0 bank under 0x9A=0x5A
          │
          ├──> [STAGE 9: Touch Monitoring Arm (CALIB_COMMIT)]
          │      └── Sets 0x1881 mode, 0x1A83 mask, 0x1A84 clear, strobe 0xC2 0x3D
          ▼
  [STAGE 10: Final READY Transition (State enters FTE3600_STATE_READY)]
```

---

## 3. Transaction Specification Breakdown

---

### Operation 1: Prefix Completion
- **Vendor Reference:** `fw9369_startup.c:321–366` (`prefix()`).
- **Kernel Implementation:** `kernel/fte3600.c:487–600` (`fte3600_hw_startup()`).
- **Inputs:** None (requires state `IDENTIFIED` (8) or `RESET_SEQUENCE_ISSUED` (2)).
- **Outputs:** Foundry code (e.g. `0xAA`), configured line period (`150` or `200` $\mu\text{s}$).
- **Registers Accessed:**
  - Strobes: `0x5A 0xA5`, `0xA5 0x5A`, abort `0xC0 0x3F` if required.
  - SFRs: Status `0x80`, Interrogation `0xF1`, `0xF4`, `0xF3`, Fab ID `0x9B`.
- **Validation:** `0x80 == 0x50` (idle status); `(0x9B >> 2) == 0x13` or `0`.
- **Max Transfer Size:** 3 bytes (strobe) to 5 bytes (register access).
- **Execution Time:** ~15–20 ms (including settling delays).
- **Side Effects / State:** Awakens microcontroller, configures analog line periods.
- **Retry Safety:** Can only be retried after an active-low hardware reset pulse (`FTE3600_IOC_RESET`).
- **Failure Impact:** Sets `data->state = UNKNOWN` (7).

---

### Operation 2: Session Initiation (`BEGIN`)
- **Proposed IOCTL:** `FTE3600_IOC_CALIB_BEGIN`.
- **Inputs:** `struct fte3600_calib_session_params` (version, flags, timeout_ms).
- **Outputs:** `session_id` (32-bit cryptographically secure or pseudo-random token).
- **Registers Accessed:** None (pure kernel state initialization).
- **Validation:** Device must be in `INITIALIZED_UNCALIBRATED` (14); no active session.
- **Max Transfer Size:** 0 bytes (no SPI activity).
- **Execution Time:** $<10 \mu\text{s}$.
- **Side Effects / State:** Stores `session_id`, sets owner `filp`, starts watchdog timer.
- **Retry Safety:** Retriable.

---

### Operation 3: FDT Analog Bias DAC Sampling (`FDT_SAMPLE`)
- **Vendor Reference:** `fw9369_fdt.c:167–205`, `fw9369_fdt.c:333–360` (`physical_sample()`).
- **Proposed IOCTL:** `FTE3600_IOC_CALIB_FDT_SAMPLE`.
- **Inputs:** `session_id`, candidate 8-bit DAC bias `fdt_dac` ($1 \le \text{dac} \le 125$).
- **Outputs:** 4 16-bit raw ADC channel values (`uint16_t channels[4]`).
- **Registers Accessed:**
  - Write `0x1801 = (0xFC80 | fdt_dac)` (Bias DAC SFR).
  - Write `0x1881` (scan parameters), write `0x1885 = 1` (scan trigger).
  - Strobe `0xC2 0x3D 0x00` (FDT scan initiate).
  - Poll `0x1A82` bit 3 (ADC conversion complete, max 10 polls, 100 $\mu\text{s}$ delay).
  - Read `0x80B8` (or `0x80E8` for SMIC AA): 4 16-bit words.
  - Clear interrupt: Write `0x1A84 = 0x0008` (W1TC scan ready bit).
- **Validation:** Rejects `dac < 1 || dac > 125` with `-EINVAL`; returns `-ETIMEDOUT` if `0x1A82` bit 3 fails to assert within 10 ms.
- **Max Transfer Size:** 14 bytes (address prefix + 8 bytes payload).
- **Execution Time:** ~1.2 ms to ~1.8 ms.
- **Side Effects / State:** Overwrites volatile register `0x1801`.
- **Retry Safety:** Fully retriable (completely volatile SFR write).

---

### Operation 4: Stability Sampling & Range Verification
- **Vendor Reference:** `fw9369_startup.c:165–189`.
- **Orchestration:** Userspace helper loops 10 times calling `FTE3600_IOC_CALIB_FDT_SAMPLE` with the converged DAC value.
- **Outputs:** Computes `minima[c] = min(samples[0..9][c])`.
- **Validation:** Verifies that all 4 channel minima satisfy:
  $$300 \le minima[c] \le 700$$
- **Failure Impact:** If any channel minimum is outside `[300, 700]`, userspace helper invokes `ABORT`. Sensor remains in `INITIALIZED_UNCALIBRATED` or transitions to `UNKNOWN`.

---

### Operation 5: Threshold Calculation
- **Vendor Reference:** `fw9369_startup.c:192–196`.
- **Orchestration:** Pure software computation in userspace helper.
- **Formula:**
  $$\text{threshold}[c] = \begin{cases} 0 & \text{if } minima[c] < 31 \\ minima[c] - 30 & \text{otherwise} \end{cases}$$
- **Registers Accessed:** None.
- **Execution Time:** $<1 \mu\text{s}$.

---

### Operation 6: Image Baseline DAC Search & Frame Streaming (`READ_FRAME`)
- **Vendor Reference:** `capture.c:251–282` (`frames()`), `image.c:75–150` (`fte3600_image_search()`).
- **Proposed IOCTL:** `FTE3600_IOC_CALIB_READ_FRAME`.
- **Inputs:** `session_id`, candidate 8-bit `image_dac` ($0 \le \text{dac} \le 127$).
- **Outputs:** Single 10,240-byte raw image frame (5,120 16-bit pixels).
- **Registers Accessed:**
  - Write `0x1800 = 0x4FFE` (Mode 0xA3 configuration prefix).
  - Strobe `0xC4 0x3B 0x00` (Image exposure start).
  - Delay 1 ms.
  - Poll `0x80 == 0x54` (MCU frame ready, max 10 polls).
  - Write `0x1800 |= 1` (FIFO readout enable).
  - Delay 1 ms.
  - Burst Read FIFO `0x9A05`: `06 F9 9A 05 14 00` + 10,240 clock bytes.
  - Clear interrupt: Write `0x1A84 = 0x0020`.
- **Validation:** Fails with `-ETIMEDOUT` if `0x80 != 0x54`; fails with `-EIO` if FIFO read fails.
- **Max Transfer Size:** Exactly **10,246 wire bytes**.
- **Execution Time:** ~85 ms to ~87 ms.
- **Side Effects / State:** Discharges pixel sensor matrix; clears FIFO buffer.
- **Retry Safety:** Fully retriable.

---

### Operation 7: Dark Frame Reference Caching
- **Vendor Reference:** `fw9369_startup.c:287–307`.
- **Orchestration:** Userspace helper stores the second acquired frame in its process heap as `image_raw_dark`.
- **Registers Accessed:** None.
- **Execution Time:** $<0.1 \text{ ms}$ (memcpy).

---

### Operation 8: Threshold Bank Hardware Programming (`SET_THRESHOLDS`)
- **Vendor Reference:** `fw9369_startup.c:396–405`, `fw9369_release.c:265–274`.
- **Proposed IOCTL:** `FTE3600_IOC_CALIB_SET_THRESHOLDS`.
- **Inputs:** `session_id`, 4 validated threshold words (`uint16_t thresholds[4]`).
- **Outputs:** None.
- **Registers Accessed:**
  - Write `0x9A = 0x5A` (Bank unlock).
  - Burst Write SRAM `0x80B0` (or `0x80E0`): `05 FA 80 B0 00 08` + 8 big-endian words (16 bytes).
  - Write `0x9A = 0x00` (Bank relock).
- **Validation:** Kernel strictly bounds-checks that $270 \le \text{thresholds}[c] \le 670$. Rejects out-of-range thresholds with `-EINVAL`.
- **Max Transfer Size:** 22 bytes (`bytes[22]`).
- **Execution Time:** ~0.3 ms.
- **Side Effects / State:** Programs the analog comparator thresholds into on-chip SRAM bank.
- **Failure Impact:** Sets `data->state = UNKNOWN` (7).

---

### Operation 9: Touch Monitoring Arm & Final Commit (`COMMIT`)
- **Vendor Reference:** `fw9369_startup.c:406–421`, `fw9369_release.c:275–293`.
- **Proposed IOCTL:** `FTE3600_IOC_CALIB_COMMIT`.
- **Inputs:** `session_id`, `flags` (Down Arm vs Up Arm).
- **Outputs:** None.
- **Registers Accessed:**
  - Read/Modify `0x1881`: set mode bit `0x81` (Down arm).
  - Read/Modify `0x1A83`: set mask bits `0x16` (Touch event Linux routing).
  - Clear interrupt: Write `0x1A84 = 0x002F`.
  - Command strobe: `0xC2 0x3D 0x00` (Autonomous touch monitoring trigger).
  - Delay 1 ms.
- **Validation:** Verifies that all session phase flags are complete (`PREFIX_DONE`, `FDT_DONE`, `IMAGE_DONE`, `THRESHOLDS_SET`).
- **Execution Time:** ~1.5 ms.
- **Side Effects / State:** Puts sensor analog comparator into autonomous low-power monitoring; transitions driver state to **`FTE3600_STATE_READY`** (4).
- **Postcondition:** Closes calibration session; releases session token.

---

## 4. Operation Characteristic Summary Table

| Step | Operation Name | Transport Size | Pure Bus Time | Est. Wall Time | Kernel Primitives Used | Side Effects on Silicon | Retriable? |
|---|---|---|---|---|---|---|---|
| **1** | Prefix Completion | 28 B | 0.22 ms | ~18 ms | Strobes, SFR writes/reads | MCU boots, PLL stabilizes | No (Needs Reset) |
| **2** | Session Begin | 0 B | 0.00 ms | 0.01 ms | Kernel memory only | None | Yes |
| **3** | FDT Sample | 28 B | 0.22 ms | ~1.5 ms | SFR writes, Strobe `0xC2`, Read `0x80B8` | Updates `0x1801` DAC | Yes |
| **4** | Stability Evaluation | 0 B | 0.00 ms | 0.01 ms | Userspace math | None | Yes |
| **5** | Threshold Math | 0 B | 0.00 ms | 0.01 ms | Userspace math | None | Yes |
| **6** | Read Frame | 10,246 B | 81.97 ms | ~86 ms | Strobe `0xC4`, Poll `0x80`, FIFO burst | Exposes matrix, clears FIFO | Yes |
| **7** | Dark Cache | 0 B | 0.00 ms | 0.1 ms | Userspace memory | None | Yes |
| **8** | Set Thresholds | 30 B | 0.24 ms | ~0.4 ms | Unlock `0x9A=0x5A`, Write `0x80B0` | Overwrites threshold SRAM | Yes (within session) |
| **9** | Arm & Commit | 22 B | 0.18 ms | ~1.5 ms | Modify `0x1881/1A83`, Strobe `0xC2` | Enables Pad 229 IRQ | No |
| **10**| State -> READY | 0 B | 0.00 ms | 0.01 ms | Driver state lock | Unlocks capture gate | N/A |
