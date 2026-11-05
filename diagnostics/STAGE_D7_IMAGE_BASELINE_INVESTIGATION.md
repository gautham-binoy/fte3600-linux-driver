# Stage D7 — FT9362 Image Baseline Protocol Investigation

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Investigation Mode:** Read-Only Static Source Analysis (Zero Physical Hardware Communication)  

---

## 1. Executive Summary

This report establishes the complete protocol sequence, hardware register configuration, and algorithmic requirements of the vendor image baseline procedure (`image_baseline()`).

In the vendor architecture, `image_baseline()` is the second mandatory operational stage executed during startup, immediately following `fdt_baseline()`. Its objective is to calibrate the optical/capacitive image sensing array DAC bias voltage, establish the dark current baseline reference matrix, and store an uncorrupted background frame against which touch impressions are normalized during fingerprint capture.

A crucial finding of this investigation is that **`image_baseline()` is NOT merely a register configuration step: it actively captures and consumes physical image data from the hardware FIFO.** Executing `image_baseline()` transfers full 10,246-byte frame payloads across the SPI bus and calculates pixel statistics over a 5,120-pixel array.

---

## 2. Source Discovery & Call-Site Provenance

The image baseline procedure is implemented and called in the following vendor files:

1. **`fw9369_startup.c` (lines 263–309):**  
   Primary implementation of `image_baseline(struct startup *op, uint8_t reset)`. Orchestrates image DAC search (when `reset == 1`), captures baseline frames, updates in-place warm baseline (when `reset == 0`), calculates baseline CRC, and asserts readiness flags.
2. **`fw9369_startup.c` (lines 200–233):**  
   `capture()`. Callback invoking `fte3600_capture_frames()` to acquire raw 10,240-byte image frames from the hardware FIFO and decode them into 16-bit big-endian pixel values.
3. **`fw9369_startup.c` (lines 234–262):**  
   `warm_baseline()`. Performs spatial neighborhood verification and selective baseline adaptation for drift compensation without resetting calibrated DAC values.
4. **`image.c` (lines 75–150):**  
   `fte3600_image_search()`. Successive approximation search algorithm executing multiple physical test captures to optimize the image sensing DAC bias towards median intensity 512.
5. **`image.c` (lines 55–73):**  
   `fte3600_image_refine()`. Micro-stepping refinement algorithm (`DAC ± 1`) minimizing clipped dark (<20) and saturated (>1000) pixels.
6. **`capture.c` (lines 251–282):**  
   `frames()`. Core hardware frame acquisition engine: issues configuration prefix, command strobe `0xC4 0x3B`, polls status register `0x80 == 0x54`, starts conversion, reads FIFO, and clears image interrupt via `0x1A84 = 0x0020`.
7. **`capture.c` (lines 182–199):**  
   `fifo()`. Transmits multi-byte SPI read request (`0x06 0xF9 0x9A 0x05 0x14 0x00` + 10,240 dummy bytes) to extract pixel data from the hardware FIFO.
8. **`fte3600_capture.h` (lines 14–24):**  
   Structure and dimension constants: `FTE3600_CAPTURE_FRAME_BYTES = 10240`, `FTE3600_CAPTURE_TRANSFER_BYTES = 10246` (80 rows x 64 columns x 2 bytes/pixel).

---

## 3. Entry Conditions & Prerequisites

Before entering `image_baseline()`, the driver requires:

1. **Volatile Startup Prefix Completed:**
   - Silicon identified as `0x9362`.
   - Foundry identified as SMIC AA (`0xAA`) or non-SMIC (`0x00`).
   - Line period programmed (`150 us` or `200 us`).
2. **FDT Baseline Successfully Established (`fdt_baseline()`):**
   - FDT DAC calibrated.
   - 4-channel FDT threshold baselines established (`fdt_down`, `fdt_up`).
   - `fdt_down_crc` and `fdt_up_crc` calculated.
3. **Startup Mode Parameters:**
   - Parameter `reset == 1`: Cold startup mode (executes full DAC search and initializes baseline frame).
   - Parameter `reset == 0`: Warm runtime maintenance mode (skips DAC search, updates baseline incrementally).
   - Default `image_dac = 54` (`FTE3600_FW9369_STARTUP_STATE_INITIALIZER`).
   - Default `image_median = 512`.
   - Frame geometry: 80 rows x 64 columns = 5,120 pixels (10,240 bytes raw payload).

---

## 4. Hardware Acquisition & Register Sequence

Executing `image_baseline()` involves two distinct levels of hardware interaction: Frontend Configuration and FIFO Frame Readout.

### 4.1 Frontend Capture Configuration (`capture.c:217–249`)

Before triggering any image acquisition, the driver executes `configuration_prefix()`:

1. **Idle Command Sequence:**
   - Transmit: `0x5A 0xA5 0x00` (Enter Idle Command).
   - Delay: `1 ms`.
   - Read Status byte from register `0x80`: `0x08 0xF7 0x80 0x00 0x00` (5 bytes).
   - If `buffer[4] != 0x50`: Transmit `0xC0 0x3F 0x00` (Abort/Wake), delay `1 ms`.
   - Transmit: `0xA5 0x5A 0x00` (Exit Idle Command).

2. **Analog Front-End Programming:**
   - Write Word `0x1801 = 0xFC80 | state->dac` (sets calibrated image DAC bias).
   - Write Word `0x1800 = 0x4FFE` (selects image acquisition window mode).
   - Write Word `0x1804 = 0x27C0 | shift | (shift >= 2 ? 8 : 0)` (gain shift = 2 -> `0x27CA`).
   - Modify Word `0x1806`: keep `0xC07F`, set `0x0480`.
   - Modify Word `0x180A`: keep `0xC000`, set `0x0483`.
   - Modify Word `0x180B`: keep `0xC000`, set `0x0208`.
   - Write Word `0x1807 = ((period - 1) << 5) | 1` (line period timing).
   - Write Word `0x1887 = (channel_mode != 0 ? 2 : 0)`.

3. **Digital & Sequencer Setup (First-Time Initialization):**
   - Modify Word `0x1805`: keep `0xFF0F`, set `0x0000`.
   - Modify Word `0x1811`: keep `0xFC00`, set `0x01FE`.
   - Modify Word `0x1A83`: keep `0xFFFF`, set `0x0020` (enables Image Ready Interrupt).
   - Modify Word `0x1A83`: keep `0xFFFF`, set `0x0040`.
   - Write Byte `0x8E = 0xF4`.
   - Write Byte `0x90 = 0x00`.
   - Write Byte `0x91 = 0x07`.
   - Write Byte `0x92 = 0xD0`.
   - Write Byte `0x90 = 0x01`.

---

### 4.2 Hardware Frame Trigger & FIFO Transfer (`capture.c:267–281`)

To acquire each physical image frame:

1. **Trigger Image Acquisition:**
   - Transmit Command Strobe: `0xC4 0x3B 0x00` (Start Sensing Scan).
   - Delay: `1 ms`.

2. **Status Polling Loop (`capture.c:269–273`):**
   - Poll register `0x80` (up to 10 iterations, 1 ms delay per iteration):
     - Transmit: `0x08 0xF7 0x80 0x00 0x00`.
     - Read response byte `buffer[4]`.
     - When `buffer[4] == 0x54`, the imaging engine has completed exposure and loaded the FIFO!

3. **FIFO Read Arming:**
   - Read Word `0x1800`: `0x04 0xFB 0x80 0x18 0x00 0x01 ...`
   - Write Word `0x1800 = (value | 0x0001)` (enables FIFO SPI readout).
   - Delay: `1 ms`.

4. **Multi-Kilobyte FIFO Transfer (`capture.c:182–199`):**
   - SPI Transaction size: **`10,246 bytes`**.
   - 6-byte header command:
     `0x06 0xF9 0x9A 0x05 0x14 0x00`
     - `0x06`: Read command opcode.
     - `0xF9`: One's complement of opcode (`~0x06`).
     - `0x9A 0x05`: Target register base address (`0x9A05` - Image Frame FIFO).
     - `0x14 0x00`: Transfer length (5,120 words = 10,240 bytes).
   - Following the 6 command bytes, 10,240 clock bytes are transferred simultaneously reading out the 5,120 16-bit pixel samples.

5. **Interrupt Acknowledgment:**
   - Write Word `0x1A84 = 0x0020` (Write-1-To-Clear bit 5, clearing the Image Ready Interrupt flag).

---

## 5. Algorithmic Processing & Baseline Construction

### 5.1 Image DAC Binary Search (`image.c:75–150`)

When `reset == 1`, the driver performs an active search to locate the optimal image DAC:

1. Initializes `state->dac = 54`, `lower_endpoint = 0`, `upper_endpoint = 127`.
2. Up to 10 search trial captures (`FTE3600_IMAGE_MAX_SEARCH_CAPTURES`):
   - Captures a frame at the current `dac`.
   - Computes array median using `fte3600_image_measure()`:
     - Histogram-based median calculation across 5,120 pixels.
     - Counts clipped dark pixels (`< 20`) and saturated pixels (`> 1000`).
   - If `median < 20`: `lower_endpoint = dac; dac = (dac + upper_endpoint) / 2`.
   - If `median > 1000`: `upper_endpoint = dac; dac = (lower_endpoint + dac) / 2`.
   - If `20 <= median <= 1000`: proportional stepping towards target 512.
3. Convergence: When median transitions across 512 between adjacent integer DAC values, locks DAC and calls `fte3600_image_refine()` to test `dac ± 1` for minimum clipping.
4. Stores calibrated DAC in `s->image_dac`, median in `s->image_median`.

---

### 5.2 Baseline Frame Storage & Readiness Assertion (`fw9369_startup.c:287–308`)

1. Captures final baseline frame using `s->image_dac` into `s->work_image`.
2. When `reset == 1`:
   - Directly copies: `memcpy(s->baseline, s->work_image, sizeof(s->baseline))`.
   - `s->baseline` now contains the dark reference matrix (5,120 16-bit words).
3. Computes CRC-CCITT across the first 128 words (256 bytes) of the baseline:
   `s->baseline_crc = crc(s->baseline, 128);`
4. **Asserts Readiness Flags:**
   ```c
   s->baseline_flag_1 = 1;
   s->baseline_flag_2 = 1;
   ```
   These two flags constitute the prerequisite hardware assertion required before `arm()` will execute.

---

## 6. Volatility & Storage Safety Audit

| Register / Resource | Operation | Purpose | Volatility Classification |
|---|---|---|---|
| `0x80` | Read Byte | Image engine ready status (`0x54`) | Volatile SFR |
| `0x8E` .. `0x92` | Write Byte | FIFO timing & clock dividers | Volatile SFRs |
| `0x1800 .. 0x1811` | Read/Write Word | Image window, DAC bias, line period, FIFO enable | Volatile SFRs |
| `0x1A83` | Read/Modify Word | Event mask (enables Image Ready bit 5: `0x0020`) | Volatile SFR |
| `0x1A84` | Write Word | Event ACK (clears Image Ready bit 5: `0x0020`) | Volatile SFR (W1TC) |
| `0x9A05` (FIFO) | 10,246B Multi-Read | Raw Frame Buffer FIFO (5,120 words) | Volatile Hardware FIFO SRAM |

**Audit Conclusion:**  
1. All registers programmed and read during `image_baseline()` are 100% volatile SFRs and volatile FIFO SRAM.
2. Zero non-volatile (OTP, eFuse, Flash) memory access occurs.
3. **Data Consumption Reality:** `image_baseline()` cannot be treated as a pure register-write operation. It actively streams and processes real frame buffer data across SPI.
