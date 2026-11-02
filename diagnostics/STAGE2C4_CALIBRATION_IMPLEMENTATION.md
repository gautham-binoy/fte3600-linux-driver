# Stage 2C.4 — Calibration Implementation Review

**Document ID:** `STAGE2C4_CALIBRATION_IMPLEMENTATION`  
**Date:** 2026-11-02  
**Target Silicon:** FocalTech FT9362 / FW9369  
**Source Code Baseline:** `tools/fw9369-mock/` (`include/fw9369_calibration.h`, `src/fw9369_calibration.c`)  
**Scope:** Review of the mathematical DAC search algorithms, per-channel minimum tracking, stability exit conditions, and single-frame baseline handling.  
**Execution Boundary:** **STRICTLY MOCK-ONLY.** Physical hardware execution is completely prohibited.

---

## 1. Executive Summary

In strict adherence to the canonical specification ([FW9369_UNIFIED_INITIALIZATION_SPECIFICATION.md](file:///home/titan/fte3600-linux-driver/diagnostics/FW9369_UNIFIED_INITIALIZATION_SPECIFICATION.md)), the calibration engine implemented in [src/fw9369_calibration.c](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_calibration.c) eliminates all previous erroneous assumptions of temporal filtering.

It implements:
1. **DAC Centering Search:** Hybrid binary/linear search targeting the 512 count zero-crossing boundary.
2. **Stability Window:** Up to 10 samples with early exit upon observing 3 consecutive samples within $\pm 5$ counts across all 4 channels.
3. **Per-Channel Minimum Tracking:** Absolute minimum envelope calculation; zero averaging.
4. **Cold Image Baseline Copy:** Direct single-frame `memcpy` into `s->baseline`; zero filtering.
5. **Memory Signatures:** 4-word CRC for FDT thresholds and 128-word CRC for image baseline.

---

## 2. FDT Calibration Algorithm Implementation

### 2.1 Hybrid DAC Centering Search
In [src/fw9369_calibration.c:85-132](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_calibration.c#L85-L132):
- **Initial State:** `dac = 27`, `lower = 0`, `upper = 127`, `prev_dac = 0`, `prev_meas = 0`.
- **Measurement:** In each step, samples 4 physical channels via register `0x1801` and `0x80B8`, computing the spatial channel average:
  $$meas = \frac{ch[0] + ch[1] + ch[2] + ch[3]}{4}$$
- **Zero-Crossing Detection:**
  ```c
  if ((dac == prev_dac + 1 && prev_meas <= 512 && meas > 512) ||
      (prev_dac == dac + 1 && meas <= 512 && prev_meas > 512)) {
      dac = (dac > prev_dac) ? dac : prev_dac;
      centered = true;
      break;
  }
  ```
  Selects the higher DAC value upon crossing the 512 threshold.
- **Adaptive Step Sizing:**
  - If $meas < 20$: binary split upward ($dac \leftarrow (dac + upper) / 2$).
  - If $meas > 1000$: binary split downward ($dac \leftarrow (lower + dac) / 2$).
  - Otherwise: steps linearly by $\pm 1$ ($dac \leftarrow dac + 1$ or $dac - 1$).
- **Trial Cap:** Hard ceiling of 32 trials (`CALIBRATION_MAX_TRIALS`).

---

### 2.2 Stability Sampling Window & Early Exit Condition
In [src/fw9369_calibration.c:135-175](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_calibration.c#L135-L175):
- Initializes: `minima[c] = 1023` for $c \in [0, 3]$.
- Samples up to 10 times at the centered DAC setting:
  - On each step $s$, updates absolute channel minima:
    ```c
    if (ch[c] < cal->minima[c])
        cal->minima[c] = ch[c];
    ```
  - Calculates inter-sample fluctuation against previous sample:
    ```c
    if (s > 0) {
        int delta = (int)ch[c] - (int)prev_channels[c];
        if (delta > 5 || delta < -5)
            stable = 1;
    }
    prev_channels[c] = ch[c];
    ```
  - **Early Exit Trigger:**
    ```c
    if (s > 0 && stable >= 3)
        break;
    ```
    If noise remains within $\pm 5$ counts for 3 consecutive samples, sampling terminates early (typically in 3 to 5 samples).

---

### 2.3 Boundary Validation & Threshold Derivation
In [src/fw9369_calibration.c:177-195](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_calibration.c#L177-L195):
1. **Range Validation:**
   Verifies that every channel satisfies $300 \le minima[c] \le 700$. If any channel violates this window, returns `-ERANGE`.
2. **Threshold Mathematical Derivation:**
   $$fdt\_down[c] = \begin{cases} 0 & \text{if } minima[c] < 31 \\ minima[c] - 30 & \text{otherwise} \end{cases}$$
3. **Integrity Signature:**
   Evaluates `fw9369_crc_words_le(cal->fdt_down, 4)` and stores the result in `cal->fdt_down_crc`.

---

## 3. Cold Image Baseline Acquisition Implementation

In [src/fw9369_calibration.c:198-252](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_calibration.c#L198-L252):
1. **Image DAC Setup:** Configures nominal `image_dac = 54` and `median = 512`.
2. **Burst FIFO Transaction:** Allocates contiguous 10,246-byte buffer, transmits prefix `[ 0x06, 0xF9, 0x9A, 0x05, 0x14, 0x00 ]`, and reads 10,240 pixel bytes.
3. **Pixel Word Deserialization:** Unpacks big-endian pixel bytes into 5,120 16-bit words:
   $$pixel = (wire[2i] \ll 8) \mid wire[2i+1]$$
4. **Single-Frame Baseline Copy:**
   ```c
   memcpy (base->baseline, work_image, sizeof (base->baseline));
   ```
   **Bit-exact memory copy.** Confirms zero mathematical filtering, rolling mean, or multi-frame division.
5. **Baseline CRC Signature:**
   Evaluates `fw9369_crc_words_le(base->baseline, 128)` over the first two rows ($2 \times 64 = 128$ words = 256 bytes) to establish a lightweight host memory signature.
