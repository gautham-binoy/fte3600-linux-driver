# Stage 2C.2 — Image DAC Calibration & Baseline Acquisition Review

**Document ID:** `STAGE2C2_IMAGE_DAC_AND_BASELINE_REVIEW`  
**Date:** 2026-11-02  
**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FT9362 / FW9369  
**Status:** COMPLETE — STATIC SOURCE CODE AUDIT  
**Hardware Execution Authorization:** NONE (Static Analysis Only)

---

## 1. Executive Summary

This investigation audits the image sensor analog frontend (AFE) calibration, DAC binary search algorithm, burst FIFO readout framing, and baseline frame construction in `fw9369_startup.c`, `capture.c`, and `image.c`.

The audit verifies that:
1. **Sensor Array Dimensions:** The physical capacitive sensing matrix is **64 columns $\times$ 80 rows** = **5,120 pixels**.
2. **Pixel Encoding & Frame Sizing:** Each pixel is a 16-bit unsigned integer transmitted in **big-endian byte order** (`(buf[0] << 8) | buf[1]`). Total raw pixel payload per frame is exactly $5,120 \times 2 = \mathbf{10,240\text{ bytes}}$.
3. **Burst FIFO Transport Framing:** The SPI burst read requires a 6-byte header prefix `[ 0x06, 0xF9, 0x9A, 0x05, 0x14, 0x00 ]` followed by 10,240 dummy TX bytes, making the complete full-duplex transfer **10,246 bytes**.
4. **Image DAC Search Target:** The binary search algorithm sweeps `image_dac` (initial 54, bounds 1..125) targeting a **median pixel intensity of 512 counts** across the 5,120-pixel histogram.
5. **Baseline Storage & CRC:** The baseline frame is stored exclusively in host RAM and validated with a **128-word (256-byte) CCITT CRC**. No baseline image data is written to non-volatile sensor memory.
6. **FDT Calibration Dependency:** In the reference initialization pipeline (`fw9369_startup.c:452-478`), FDT calibration (Phase 3) is a mandatory prerequisite that must complete successfully before image baseline calibration (Phase 4) is invoked.

---

## 2. Image Capture Protocol & FIFO Framing

### 2.1. Capture Trigger Sequence
In `capture.c:251-282`, capturing an image frame follows a 7-step sequence:
1. `configuration_prefix()`: Issues MCU idle handshake (`5A A5 -> check 0x80 == 0x50 -> A5 5A`), writes `0x1801 = 0xFC80 | image_dac`, writes window register `0x1800 = 0x4FFE`.
2. Hardware Capture Strobe: Transmits command packet `[ 0xC4, 0x3B, 0x00 ]` (Opcode `0xC4`, Image Mode Strobe).
3. Status Polling: Waits 1 ms, then polls status register `0x80` up to 10 times until `0x80 == 0x54` (`MCU_STATE_CAPTURE`).
4. Read/Modify Window: Reads `0x1800` into `val`, writes back `val | 1` (initiates pixel readout to hardware FIFO).
5. Inter-stage Delay: Waits 1 ms for pixel A/D conversion and hardware FIFO buffering.
6. FIFO Burst Read: Issues 10,246-byte full-duplex transfer.
7. Interrupt Acknowledgment: Clears frame ready flag by writing `write_word(op, 0x1a84, 0x0020)`.

### 2.2. Exact 10,246-Byte SPI Transfer Structure
```
+-------------------------------------------------------+-------------------------------------------------------+
| Prefix Header (6 Bytes)                               | 5,120 Big-Endian Pixel Samples (10,240 Bytes)        |
+-------------------------------------------------------+-------------------------------------------------------+
| 0x06 | 0xF9 | 0x9A | 0x05 | 0x14 | 0x00               | Pixel 0 (MSB, LSB) ... Pixel 5119 (MSB, LSB)         |
+-------------------------------------------------------+-------------------------------------------------------+
|<---------------------------- Total Length: 10,246 Bytes Contiguous ----------------------------------------->|
```
- **Byte 0 (`0x06`):** SPI FIFO read command opcode.
- **Byte 1 (`0xF9`):** Bitwise complement checksum (`~0x06 = 0xF9`).
- **Bytes 2–5 (`0x9A, 0x05, 0x14, 0x00`):** Hardware FIFO memory address and stream length parameters.
- **Bytes 6–10,245:** 10,240 received bytes holding 5,120 raw 16-bit ADC samples.

---

## 3. Image DAC Binary Search Algorithm

In `image.c:75-140`, `fte3600_image_search()` executes an adaptive binary search:
- **Initial DAC:** 54.
- **Valid Search Bounds:** `lower_endpoint = 0`, `upper_endpoint = 127`.
- **Termination / Convergence:** When the calculated frame median crosses 512 between two adjacent DAC steps:
  ```c
  /* image.c:103-108 */
  if (median > 512 && previous_median <= 512 && state->dac == previous_dac + 1)
    {
      state->stored_median = observed.median;
      return fte3600_image_refine (state, pixels, capture, context);
    }
  ```
- **Adaptive Step Sizing:**
  - If `median < 20`: binary subdivision upward (`dac = (dac + upper) / 2`).
  - If `median > 1000`: binary subdivision downward (`dac = (lower + dac) / 2`).
  - If `20 <= median <= 512`: proportional step (`step = (median <= 311) ? (512 - median) / 150 : 1; dac += step`).
  - If `512 < median <= 1000`: linear decrement (`dac -= 1`).
- **Refinement:** Once bracketed, `fte3600_image_refine()` checks surrounding DAC values ($\pm 1$) to choose the DAC setting that minimizes pixel saturation (`low_count + high_count`).

---

## 4. Baseline Frame Construction & Storage

### 4.1. Frame Averaging & Multi-Frame Baseline
In `fw9369_startup.c:287-295`:
- The driver captures `image_count` frames (default 1).
- If multiple frames are configured, raw 16-bit pixel values are summed into `work_image[p]`.

### 4.2. Cold Baseline Storage vs. Warm Drift Update
- **Cold Boot (`reset == 1`):**  
  The raw captured frame is copied directly into host memory:
  `memcpy (s->baseline, s->work_image, sizeof (s->baseline));`
- **Warm Update (`reset == 0`):**  
  In `fw9369_startup.c:234-262`, `warm_baseline()` checks neighborhood pixel differences. If a pixel differs from baseline by $> 300$ counts, it requires at least one 4-neighbor pixel to differ by $\ge 100$ counts to be accepted as real drift (preventing single-pixel salt-and-pepper noise from corrupting baseline).
- **Integrity Validation:**  
  `s->baseline_crc = crc (s->baseline, 128);`  
  Calculates a 16-bit CCITT CRC over the first 128 words of the baseline image.

---

## 5. Prerequisite Architecture: FDT Before Image

In `fw9369_startup.c:452-478`:
```c
/* fw9369_startup.c:452-478 */
if (operation != FTE3600_FW9369_IMAGE_BASELINE)
  {
    result = fdt_baseline (&op);
    if (result != FTE3600_FW9369_STARTUP_OK) goto done;
  }
if (operation != FTE3600_FW9369_FDT_BASELINE)
  {
    result = image_baseline (&op, reset);
    if (result != FTE3600_FW9369_STARTUP_OK) goto done;
  }
```
- Phase 3 (FDT Baseline) sets up internal capacitive balance registers (`0x1800`..`0x180D`, bank `0x00C0`).
- If FDT calibration fails, the driver aborts immediately and does **not** attempt image baseline acquisition.
- Thus, FDT calibration is an architectural prerequisite for image calibration.
