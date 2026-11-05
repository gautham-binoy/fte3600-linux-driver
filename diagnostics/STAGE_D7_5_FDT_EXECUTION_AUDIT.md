# Stage D7.5 — FDT Calibration Algorithm & Execution Sequence Audit

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source & Design Review  
**Deliverable:** `diagnostics/STAGE_D7_5_FDT_EXECUTION_AUDIT.md`

---

## 1. Executive Summary & Objective

Prior investigation reports contained apparent contradictions regarding FDT calibration:
- Stage D7 referenced "ten stability samples" and "ADC channel minima."
- Stage D7.4 referenced "eight FDT samples."
- Vendor sources reference both 4 and 8 channels, DAC binary search limits of 31 samples, and early-exit stability thresholds.

This audit reconstructs the complete vendor call graph from source code ([fw9369_startup.c](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c), [fw9369_fdt.c](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c), and [calibration.c](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/calibration.c)), deriving the exact execution sequence, loop bounds, register operations, and mathematical dependencies.

---

## 2. Reconciling the Sample & Channel Discrepancies

The apparent discrepancies between "10 samples", "8 samples", "4 channels", and "31 iterations" are fully reconciled by distinguishing the three independent phases of FDT calibration:

| Parameter | Value | Source Location | Architectural Meaning |
| :--- | :--- | :--- | :--- |
| **Active ADC Channels** | **4 channels** | `fw9369_fdt.c:29`, `fw9369_startup.c:163` | In mode 1 (`channel_mode == 1`, standard for FT9362), channels CH0..CH3 are sampled. (Mode 0 supports 8 channels). |
| **Threshold Bank Format** | **8 channels (16 B)** | `fw9369_release.c:266-272` | Hardware register bank `0x80B0`/`0x80E0` allocates 16 bytes for 8 channels; channels 4..7 are padded with 0. |
| **DAC Calibration Search** | **Up to 31 samples** | `calibration.c:23`, `fte3600_calibration.h:14` | Binary search finds DAC value where the 4-channel average crosses 512 (`FTE3600_CALIBRATION_MAX_SAMPLES = 31`). |
| **Stability Measure Loop** | **3 to 10 samples** | `fw9369_startup.c:165-182` | After DAC calibration, measures up to 10 samples; breaks early if readings are stable for 3 consecutive samples. |

**Audit Conclusion:**  
"8 FDT samples" in earlier documentation was a misnomer that conflated the **8-channel threshold register bank** with the **stability measurement loop** (which executes between 3 and 10 samples).

---

## 3. The Complete FDT Execution Call Graph

```
fdt_baseline() [fw9369_startup.c:144]
  |
  +---> Outer Loop: attempt = 0 .. 2 (Up to 3 attempts)
  |       |
  |       +---> Phase 1: DAC Search (FTE3600_FW9369_FDT_CALIBRATE)
  |       |       |
  |       |       +---> fte3600_calibrate_channels() [calibration.c:7]
  |       |               |
  |       |               +---> Binary Search Loop: count = 0 .. 30 (Max 31)
  |       |                       |
  |       |                       +---> physical_sample() -> scan() -> read_channels()
  |       |                       |     Reads 4 ADC channels from 0x80B8/0x80E8
  |       |                       +---> Computes sum / 4
  |       |                       +---> Adjusts DAC until average crosses 512
  |       |
  |       +---> Phase 2: Stability Sampling Loop [fw9369_startup.c:165]
  |       |       |
  |       |       +---> Loop: sample = 0 .. 9 (Max 10 samples)
  |       |               |
  |       |               +---> fdt_call(FTE3600_FW9369_FDT_MEASURE)
  |       |               +---> minima[c] = min(minima[c], channels[c])
  |       |               +---> Check |channels[c] - previous[c]| <= 5
  |       |               +---> If stable >= 3 consecutive samples: BREAK EARLY
  |       |
  |       +---> Phase 3: Sanity Validation & Threshold Calculation
  |               |
  |               +---> Verify for all 4 channels: 300 <= minima[c] <= 700
  |               +---> If in range: Break attempt loop (Success!)
  |
  +---> Phase 4: Threshold Assignment & CRC
          |
          +---> T_touch[c] = T_release[c] = (minima[c] < 31) ? 0 : minima[c] - 30
          +---> fdt_crc = crc(fdt_up, 4) (16-bit CRC across 4 channels)
```

---

## 4. Phase-by-Phase Technical Trace

### 4.1. Phase 1: DAC Search (`fte3600_calibrate_channels`)
Located in `research/libfprint-fte3600/libfprint/drivers/fte3600/calibration.c:7-74`:
- **Initial DAC Value:** If `valid == 0`, `stored_dac = 27` (line 21).
- **Search Boundaries:** `lower = 0`, `upper = 127`.
- **Measurement Step:** In each iteration:
  1. Driver writes DAC value to register `0x1801: (0xFC80 | dac)`.
  2. Issues scan trigger.
  3. Reads 4 ADC channels from register `0x80B8` (or `0x80E8` for foundry `0xAA`).
  4. Computes channel average:
     $$\text{measurement} = \frac{\sum_{c=0}^{3} \text{channels}[c]}{4}$$
- **Crossing Detection:**
  ```c
  if ((dac == previous_dac + 1 && previous_measurement <= 512 && measurement > 512) ||
      (previous_dac == dac + 1 && measurement <= 512 && previous_measurement > 512)) {
      *stored_dac = max(dac, previous_dac);
      *valid = 1;
      return FTE3600_CALIBRATION_OK;
  }
  ```
- **Stepping Rule:**
  - If $\text{measurement} < 20$: `lower = dac; dac = (dac + upper) / 2;`
  - If $\text{measurement} > 1000$: `upper = dac; dac = (lower + dac) / 2;`
  - Otherwise: `dac = (measurement <= 512) ? dac + 1 : dac - 1;`

### 4.2. Phase 2: Stability Measurement Loop
Located in `research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c:165-182`:
- Initializes `minima[c] = 1023` for $c \in [0..3]$.
- Executes `sample = 0 .. 9` (up to 10 iterations):
  1. Reads ADC channels (`FTE3600_FW9369_FDT_MEASURE`).
  2. Updates running channel minimum: `minima[c] = min(minima[c], channels[c])`.
  3. Tests stability:
     ```c
     if (sample && (abs((int)channels[c] - (int)previous[c]) > 5))
         stable = 1; /* Reset stability counter */
     if (sample && stable >= 3)
         break;      /* Stable baseline achieved, terminate early */
     ```
- **Result:** Stability is reached in as few as 3 samples if ADC jitter is within $\pm 5$ counts, or up to 10 samples if noise is present.

### 4.3. Phase 3: Range Check & Threshold Formula
Located in `research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c:184-198`:
1. **Range Validation:**  
   Every channel minimum must fall strictly within the analog baseline window:
   $$300 \le \text{minima}[c] \le 700 \quad \forall c \in \{0, 1, 2, 3\}$$
   If any channel is out of range, the attempt is marked failed and retried (up to 3 total attempts).
2. **Threshold Formula:**
   $$T_{\text{touch}}[c] = T_{\text{release}}[c] = \begin{cases} 0 & \text{if } \text{minima}[c] < 31 \\ \text{minima}[c] - 30 & \text{otherwise} \end{cases}$$
3. **CRC Generation:**  
   Calculated across the 4 channel thresholds using standard 16-bit CCITT polynomial:
   ```c
   s->fdt_down_crc = s->fdt_up_crc = crc(s->fdt_up, 4);
   ```

---

## 5. Architectural Implications for Hybrid Driver (Stage D8)

1. **Kernel Primitive (`CALIB_FDT_SAMPLE`):**  
   The proposed ioctl `FTE3600_IOC_CALIB_FDT_SAMPLE` executes a **single ADC read** of the 4 channels from the sensor.
2. **Userspace Orchestration:**  
   Userspace runs the binary search loop (`fte3600_calibrate_channels`) and the stability loop (3–10 samples).
3. **Data Dependency:**  
   Userspace computes `T_touch` and `T_release` according to the formula above and provides them to `FTE3600_IOC_CALIB_SET_THRESHOLDS`.
4. **Kernel Verification:**  
   The kernel driver validates that all 4 thresholds satisfy $100 \le T \le 3500$ before writing them to sensor register bank `0x80B0`.

---

## 6. Evidence Classification

| Statement / Finding | Classification | Evidence Reference |
| :--- | :--- | :--- |
| Active channels is strictly 4 in mode 1 | `VENDOR-SOURCE-VERIFIED` | `fw9369_fdt.c:29`, `fw9369_startup.c:163` |
| DAC search max limit is 31 samples | `VENDOR-SOURCE-VERIFIED` | `calibration.c:23`, `fte3600_calibration.h:14` |
| Stability sampling loop takes 3 to 10 samples | `VENDOR-SOURCE-VERIFIED` | `fw9369_startup.c:165, 180` |
| Analog validity range is [300, 700] | `VENDOR-SOURCE-VERIFIED` | `fw9369_startup.c:185` |
| Threshold formula is `minima[c] - 30` | `VENDOR-SOURCE-VERIFIED` | `fw9369_startup.c:194` |
| Threshold register bank size is 8 channels (16 B) | `VENDOR-SOURCE-VERIFIED` | `fw9369_release.c:266` |
| CRC calculated across 4 words | `VENDOR-SOURCE-VERIFIED` | `fw9369_startup.c:197` |
