# Stage D7.6 — FDT Sampling, Stability & Threshold Generation Adversarial Audit

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source Code Audit  
**Deliverable:** `diagnostics/STAGE_D7_6_FDT_SAMPLING_AND_THRESHOLD_AUDIT.md`  
**Date:** 2026-11-04  

---

## 1. Executive Summary & Objective

Historical stage documentation contained conflicting references to "8 FDT samples," "10 stability samples," and varying DAC search parameters.

This adversarial audit scrutinizes the vendor reference source code in [`research/libfprint-fte3600/libfprint/drivers/fte3600/`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/) to definitively establish the mathematical and electrical truth of the FDT calibration algorithm.

### Key Adversarial Findings:
1. **Loop Structure:** The 10-sample loop is a **single shared loop** across all 4 active channels, not executed per-channel.
2. **Stability Criterion:** "Three consecutive stable passes" means 3 consecutive measurement passes where **every active channel** remains within $\pm 5$ counts of its immediately preceding sample.
3. **Channel Mode 1 Exclusivity:** `channel_mode == 1` is strictly the only valid runtime mode on FT9362/FW9369. Any other mode returns an immediate error.
4. **31 Iterations vs 31 Values:** The number 31 represents the **maximum iteration sample limit** (`FTE3600_CALIBRATION_MAX_SAMPLES`), not the number of possible DAC values. The DAC operates across 125 valid levels ($1 \le \text{DAC} \le 125$).
5. **Resolution of 8 vs 10 Ambiguity:**
   - **8** is the hardware threshold register bank width (8 channels = 16 bytes written to `0x80B0`/`0x80E0`).
   - **10** is the maximum sample count of the stability verification loop.

---

## 2. Detailed Source Verification of FDT Sampling

### 2.1. The 10-Sample Stability Loop: `fw9369_startup.c:165-182`
```c
/* research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c:165-182 */
for (unsigned int sample = 0; sample < 10; ++sample)
  {
    status = fdt_call (op, FTE3600_FW9369_FDT_MEASURE, channels);
    if (status != FTE3600_FW9369_STARTUP_OK)
      return status;
    ++stable;
    for (size_t c = 0; c < 4; ++c)
      {
        if (channels[c] < minima[c])
          minima[c] = channels[c];
        if (sample && ((int) channels[c] - previous[c] > 5 ||
                       (int) previous[c] - channels[c] > 5))
          stable = 1;
        previous[c] = channels[c];
      }
    if (sample && stable >= 3)
      break;
  }
```

#### Step-by-Step State Analysis:
- **Shared Loop:** One loop iteration measures all channels in a single hardware transaction via `fdt_call(MEASURE, channels)`.
- **Per-Channel Drift Test:** If **any** of the 4 active channels deviates by more than 5 counts from its previous value (`|(int) channels[c] - previous[c]| > 5`), `stable` is immediately reset to 1.
- **Three Consecutive Stable Passes:**
  - `sample = 0`: `stable` becomes 1. Baseline `previous[c]` is recorded.
  - `sample = 1`: `stable` becomes 2 if all channels within $\pm 5$, else reset to 1.
  - `sample = 2`: `stable` becomes 3 if all channels within $\pm 5$ again. Condition `sample && stable >= 3` triggers `break`.
  - **Conclusion:** The earliest successful exit requires **3 physical measurements** (samples 0, 1, 2). If stability is not achieved, it executes up to **10 physical measurements**.

---

## 3. Active Channels & Channel Mode Validation

### 3.1. Strict Mode 1 Enforcement
In [`fw9369_release.c:350, 513`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_release.c#L350) and [`fw9369_startup.c:575`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L575):
```c
if (!valid_state (s) || s->config.channel_mode != 1 || !valid_io (io) || ...)
  return FTE3600_FW9369_RELEASE_INVALID_ARGUMENT;
```
- Mode 0 (8 channels) is legacy/unsupported on FT9362.
- Mode 1 (4 channels) is mandatory. In [`fw9369_fdt.c:29`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c#L29):
  ```c
  return config->channel_mode == 1 ? 4 : 8;
  ```
- Therefore, the number of active ADC channels is strictly **4** (channels 0..3).

---

## 4. DAC Search Mathematics & Bounds

### 4.1. Implementation: `calibration.c:7-74`
```c
/* research/libfprint-fte3600/libfprint/drivers/fte3600/calibration.c:20-71 */
if (*valid == 0)
  *stored_dac = channel_count == 4 ? 27 : 56;

for (unsigned int count = 0; count < FTE3600_CALIBRATION_MAX_SAMPLES; ++count)
  {
    uint16_t channels[8];
    uint32_t sum = 0;
    unsigned int dac = *stored_dac;
    unsigned int measurement;

    if (sample (context, *stored_dac, channels) != 0)
      return FTE3600_CALIBRATION_SAMPLE_ERROR;
    for (size_t channel = 0; channel < channel_count; ++channel)
      sum += channels[channel];
    measurement = (unsigned int) ((sum & UINT32_C (65535)) / channel_count);

    if ((dac == previous_dac + 1 && previous_measurement <= 512 && measurement > 512) ||
        (previous_dac == dac + 1 && measurement <= 512 && previous_measurement > 512))
      {
        *stored_dac = (uint8_t) (dac > previous_dac ? dac : previous_dac);
        *valid = 1;
        return FTE3600_CALIBRATION_OK;
      }

    if (dac < 1 || dac > 125)
      return FTE3600_CALIBRATION_FAILED;
    ...
  }
```

### 4.2. Mathematical Parameters:
- **Maximum Samples:** `FTE3600_CALIBRATION_MAX_SAMPLES = 31` ([`include/fte3600_calibration.h:14`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/include/fte3600_calibration.h#L14)). This is an iteration count bound, not a value set.
- **Starting DAC:** Strictly **27** for 4-channel mode (`channel_count == 4`).
- **Valid DAC Operating Range:** $1 \le \text{DAC} \le 125$. Any value outside this range returns `FTE3600_CALIBRATION_FAILED`.
- **Termination:**
  - Success when two consecutive steps cross 512: `*stored_dac = max(dac, previous_dac)`.
  - Failure if bisection bounds cross or 31 iterations elapse without convergence.

---

## 5. Threshold Formula & Hardware Bank Programming

### 5.1. Formula & Boundary Clamping: `fw9369_startup.c:184-196`
```c
for (size_t c = 0; c < 4; ++c)
  if (minima[c] < 300 || minima[c] > 700)
    result = FTE3600_FW9369_STARTUP_FAILED;

for (size_t c = 0; c < 4; ++c)
  {
    uint16_t value = minima[c] < 31 ? 0 : (uint16_t) (minima[c] - 30);
    op->s->fdt_reference[c] = op->s->fdt_down[c] = op->s->fdt_up[c] = value;
  }
```
- Formula: $T_{\text{touch}} = T_{\text{release}} = \max(0, \min(\text{ADC}_c) - 30)$.
- **Range Enforcement:** If any minimum is outside $[300, 700]$, the startup routine marks the baseline FAILED.
- If $\min < 31$, it clamps to 0.

### 5.2. Threshold Bank Layout: `fw9369_release.c:265-273`
```c
uint8_t unlock[4] = {9, 0xf6, 0x9a, 0x5a}, lock[4] = {9, 0xf6, 0x9a, 0};
uint8_t bank[22] = {5, 0xfa, 0x80, s->config.foundry == 0xaa ? 0xe0 : 0xb0, 0, 8};
const uint16_t *thresholds = selection == FTE3600_FW9369_UP ? s->fdt_up : s->fdt_down;
for (unsigned c = 0; c < 8; ++c)
  {
    bank[6 + c * 2] = (uint8_t) (thresholds[c] >> 8);
    bank[7 + c * 2] = (uint8_t) thresholds[c];
  }
```
- **Burst Write:** Command `0x05` (burst write) to address `0x80B0` (or `0x80E0` if foundry `0xAA`), preceded by unlock key `0x5A` at register `0x9A` and locked with `0x00` at `0x9A`.
- **Bank Width:** Exactly 8 words (16 data bytes).
- **Zero-Padding:** Channels 0..3 contain the calculated thresholds. Channels 4..7 are strictly zero-padded (`0x0000`).
- **Endianness:** Big-endian (`MSB` at `bank[6+c*2]`, `LSB` at `bank[7+c*2]`).

---

## 6. Definitive Resolution of Historical Ambiguities

| Parameter | Historical Confusion | Source-Proven Reality | Evidence Reference |
| :--- | :--- | :--- | :--- |
| **8 vs 10 Samples** | Confused "8 FDT samples" with stability loop. | **8** is the hardware register bank size (8 channels / 16 bytes). **10** is the maximum measurement loop sample count. | [`fw9369_release.c:266`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_release.c#L266), [`fw9369_startup.c:165`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L165) |
| **Active Channels** | Whether 4 or 8 channels are used. | **4 active channels** (`c = 0..3`). Mode 1 is mandatory; upper 4 channels are zero-padded in the bank write. | [`fw9369_release.c:350`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_release.c#L350), [`fw9369_fdt.c:29`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c#L29) |
| **Stability Counter** | Meaning of 3 stable passes. | Requires 3 consecutive passes where **every** active channel is within $\pm 5$ counts of previous pass. Earliest exit is at Sample 2. | [`fw9369_startup.c:170-181`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L170) |
| **DAC Iterations** | 31 values vs 31 iterations. | **31 is the maximum sample count** for convergence. Valid DAC values range from 1 to 125. | [`calibration.c:23, 48`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/calibration.c#L23) |
