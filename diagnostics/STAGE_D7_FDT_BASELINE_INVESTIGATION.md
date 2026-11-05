# Stage D7 — FT9362 FDT Baseline Protocol Investigation

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Investigation Mode:** Read-Only Static Source Analysis (Zero Physical Hardware Communication)  

---

## 1. Executive Summary

This report establishes the complete, ordered protocol sequence and algorithmic requirements of the vendor Finger Detection (FDT) baseline procedure (`fdt_baseline()`).

In the vendor architecture, `fdt_baseline()` is the first mandatory operational procedure executed immediately following the completion of the volatile startup prefix (`prefix()`). Its purpose is to calibrate the analog sensing frontend (specifically the DAC bias voltage) and establish baseline electrical threshold values for the sensor's capacitive finger-detection channels. Without completing `fdt_baseline()`, the sensor cannot distinguish between an idle sensor surface and a physical finger contact.

All findings in this report are backed by exact line citations from the reference implementation in `research/libfprint-fte3600/libfprint/drivers/fte3600/`.

---

## 2. Source Discovery & Call-Site Provenance

The FDT baseline procedure is implemented and called across the following vendor files:

1. **`fw9369_startup.c` (lines 143–199):**  
   Primary implementation of `fdt_baseline(struct startup *op)`. Manages the retry loop (up to 3 attempts), invokes DAC calibration, collects 10 stability samples, validates measurement bounds, and computes baseline threshold offsets.
2. **`fw9369_startup.c` (lines 129–142):**  
   `fdt_call()` wrapper translating `startup` context into `fte3600_fw9369_fdt_run()` calls.
3. **`fw9369_fdt.c` (lines 356–392):**  
   `fte3600_fw9369_fdt_run()`. Entry point dispatching `FTE3600_FW9369_FDT_CONFIGURE`, `FTE3600_FW9369_FDT_MEASURE`, and `FTE3600_FW9369_FDT_CALIBRATE`.
4. **`fw9369_fdt.c` (lines 167–234):**  
   `configure()`. Programs all analog and digital FDT registers (`0x1800` through `0x1888`, bank `0xC0..0xC2`, `0x1A8A`, `0x1A83`).
5. **`fw9369_fdt.c` (lines 236–259):**  
   `scan()`. Initiates single-shot FDT hardware scan conversion and polls status register `0x1A82` bit 3.
6. **`fw9369_fdt.c` (lines 261–274):**  
   `read_channels()`. Issues multi-word SPI read to fetch raw 16-bit ADC values from register `0x80E8` (SMIC AA) or `0x80B8` (non-SMIC).
7. **`calibration.c` (lines 6–74):**  
   `fte3600_calibrate_channels()`. Successive approximation binary search algorithm to find the optimal DAC bias crossing the midpoint ADC target (512).
8. **`fte3600_fw9369_fdt.h` (lines 11–26):**  
   State and default configuration definitions (`FTE3600_FW9369_FDT_STATE_INITIALIZER`, `FTE3600_FW9369_FDT_CONFIG_INITIALIZER`).

---

## 3. Entry Conditions & Prerequisites

Before entering `fdt_baseline()`, the following conditions must be established:

1. **Physical Reset & Wake Negotiation:**
   - Active-low hardware reset pulse issued via Pad 14.
   - Wake handshake (`write 0xC6 = 1`, wait 4 ms, verify `read 0xC6 == 1`) completed.
2. **Silicon Identification:**
   - Chip ID read from register `0x1A8B` verified to equal `0x9362`.
3. **Foundry Interrogation & Timing Setup:**
   - OTP interrogation loop completed, extracting foundry code from register `0x9B` (bits [6:2] == `0x13` denotes SMIC Fab AA, foundry code `0xAA`).
   - Line period assigned: `150 us` for SMIC Fab AA (`0xAA`), `200 us` for non-SMIC (`0x00`).
4. **Default FDT Configuration Parameters (`FTE3600_FW9369_FDT_CONFIG_INITIALIZER`):**
   - `channel_mode = 1` (denoting 4 sensing channels; mode 0 denotes 8 channels).
   - `rate = 120`, `fast_rate = 60` (sample rate in Hz).
   - `integration_multiplier = 4` (sets integrator timing in `0x1804`).
   - `fdt_rows = 4` (sets rows scanned in `0x1808`).
   - `repeat = 1`, `average = 4` (hardware averaging filter).
   - `threshold_low = 45`, `threshold_high = 50`.
   - Initial DAC bias = 27 (for 4-channel mode) or 56 (for 8-channel mode).

---

## 4. Complete Ordered Register Sequence

The execution of `fdt_baseline()` proceeds in three distinct procedural phases:

### Phase 1: Idle & Analog Frontend Configuration (`configure()`)

Before performing any measurements, the driver puts the sensor into idle mode and programs the analog registers:

1. **Idle Command Sequence (`fw9369_fdt.c:66–80`):**
   - Transmit: `0x5A 0xA5 0x00` (Enter Idle Command).
   - Delay: `1 ms`.
   - Read Status byte from register `0x80`: `0x08 0xF7 0x80 0x00 0x00` (5 bytes).
   - If `status != 0x50`: Transmit `0xC0 0x3F 0x00` (Abort/Wake), delay `1 ms`.
   - Transmit: `0xA5 0x5A 0x00` (Exit Idle Command).

2. **Image Prefix Sub-Configuration (`capture.c:148–180`):**
   - Write Word `0x1801 = 0xFC80 | dac` (sets initial DAC bias).
   - Write Word `0x1800 = 0x4FFE` (window mode).
   - Write Word `0x1804 = 0x27C0 | shift | (shift >= 2 ? 8 : 0)` (gain shift).
   - Modify Word `0x1806`: keep `0xC07F`, set `0x0480`.
   - Modify Word `0x180A`: keep `0xC000`, set `0x0483`.
   - Modify Word `0x180B`: keep `0xC000`, set `0x0208`.
   - Write Word `0x1807 = ((period - 1) << 5) | 1`.
   - Write Word `0x1887 = (channel_mode != 0 ? 2 : 0)`.
   - Modify Word `0x1805`: keep `0xFF0F`, set `0x0000`.
   - Modify Word `0x1811`: keep `0xFC00`, set `0x01FE`.
   - Modify Word `0x1A83`: keep `0xFFFF`, set `0x0020` (enables image ready IRQ).
   - Modify Word `0x1A83`: keep `0xFFFF`, set `0x0040`.
   - Write Byte `0x8E = 0xF4`.
   - Write Byte `0x90 = 0x00`.
   - Write Byte `0x91 = 0x07`.
   - Write Byte `0x92 = 0xD0`.
   - Write Byte `0x90 = 0x01`.

3. **FDT Main Register Setup (`fw9369_fdt.c:177–233`):**
   - Write Word `0x1801 = 0xFC80 | dac`.
   - Write Word `0x180C = (calibrating ? 0x0000 : 0x0600)`.
   - Write Word `0x1881 = timing` where `timing = ((1000 / rate - 1) << 8) | ((repeat - 1) << 2)`.
   - Write Word `0x1800 = (channel_mode == 0 ? 0x0890 : 0x07FE)`.
   - Write Word `0x1804 = 0x27C8 | (integration_multiplier - 1)`.
   - Write Word `0x1807 = 0x1671` (if channel_mode == 1).
   - Write Word `0x1808 = (channel_mode == 1 ? 0x0801 : (0x0800 | (fdt_rows - 1)) & 0xF8FF)`.
   - Write Word `0x1887 = (channel_mode == 1 ? 5 : 2)`.
   - Modify Word `0x1806`: keep `0xC07F`, set `0x0980`.
   - Modify Word `0x180A`: keep `0xC000`, set `0x0987`.
   - Modify Word `0x180B`: keep `0xC000`, set `0x0491`.
   - Read Word `0x1805`, clear bit 4 (`value &= 0xFFEF`), write back.
   - Write Word `0x180D = (value & 0xFC00) | 0x0384`.
   - Modify Word `0x1888`: keep `0xFC03`, set `0x0000`.

4. **Bank Unlock & Analog Threshold Configuration (`fw9369_fdt.c:215–224`):**
   - Transmit: `0x09 0xF6 0x9A 0x5A` (Write Byte `0x9A = 0x5A` -> Unlock Bank).
   - If `channel_mode == 1` (base `0xC0`):
     - Modify Word `0xC0`: keep `0xE000`, set `0x0444`.
     - Modify Word `0xC1`: keep `0xFFC0`, set `0x0021`.
     - Modify Word `0xC2`: keep `0xFFFF`, set `0x00C0`.
   - Transmit: `0x09 0xF6 0x9A 0x00` (Write Byte `0x9A = 0x00` -> Relock Bank).
   - Modify Word `0x1880`: keep `0x0000`, set `(threshold_high << 8) | threshold_low` (`0x322D`).
   - Modify Word `0x1881`: keep `0xFFFE`, set `0x0082`.
   - Modify Word `0x1884`: keep `0xFFF8`, set `average - 1` (`0x0003`).
   - Write Word `0x1A8A = 0x00FF`.
   - Modify Word `0x1A83`: keep `0xFFFF`, set `0x0008` (enables FDT measurement interrupt).

---

### Phase 2: Hardware Scan & ADC Readout

To perform a physical FDT measurement, the driver executes `scan()` followed by `read_channels()`:

1. **Scan Trigger Sequence (`fw9369_fdt.c:236–259`):**
   - Read Word `0x1881`: if `(value & 2) == 0`, write `0x1881 = value | 2`.
   - Command Strobe: `0xC2 0x3D 0x00`.
   - Delay: `1 ms`.
   - Write Word `0x1885 = 0x0001` (triggers FDT ADC conversion).
   - **Polling Loop (up to 5 iterations, 1 ms delay per iteration):**
     - Read Word from `0x1A82`: `0x04 0xFB 0x80 0x1A 0x82 0x01 ...`
     - Test bit 3 (`value & 0x0008`):
       - If bit 3 is asserted (`1`): ADC conversion is complete!
       - If 5 iterations expire without bit 3: logs `ready_timeout` in diagnostics.

2. **Multi-Channel ADC Readout (`fw9369_fdt.c:261–274`):**
   - Selects target register address based on foundry:
     - SMIC Fab AA (`foundry == 0xAA`): register `0x80E8`.
     - Non-SMIC (`foundry != 0xAA`): register `0x80B8`.
   - Transmit framing for 4 channels (count = 4, 14 bytes total):
     `0x04 0xFB 0x80 [0xE8 or 0xB8] 0x00 0x04 [8 dummy/clock bytes]`
   - Extracts 4 big-endian 16-bit channel values from buffer bytes [6..13]:
     ```c
     channels[i] = (buffer[6 + 2 * i] << 8) | buffer[7 + 2 * i];
     ```

---

### Phase 3: Algorithmic Calibration & Baseline Calculation

1. **DAC Binary Search (`calibration.c:6–74`):**
   - The driver executes a binary search to find the DAC bias register value (`0x1801` [6:0]) where the mean ADC measurement of the 4 channels crosses target 512 (range 0..1023).
   - Iteration logic:
     - If `mean < 20`: DAC too low, adjust `lower = dac; dac = (dac + upper) / 2`.
     - If `mean > 1000`: DAC too high, adjust `upper = dac; dac = (lower + dac) / 2`.
     - If `20 <= mean <= 1000`: single-step `dac = (mean <= 512 ? dac + 1 : dac - 1)`.
   - Convergence condition: When DAC transitions across 512 between adjacent integer values, the higher DAC is locked as calibrated.
   - Allowable DAC range: `1 <= dac <= 125`.

2. **Measurement Sampling & Stability Loop (`fw9369_startup.c:165–182`):**
   - With the calibrated DAC locked, the driver acquires 10 consecutive measurement samples.
   - For each sample, updates channel minima:
     `minima[c] = min(minima[c], channels[c])`.
   - Stability detector: if sample-to-sample delta for all 4 channels remains `<= 5` for 3 consecutive samples (`stable >= 3`), breaks early.

3. **Minima Validation (`fw9369_startup.c:184–188`):**
   - All 4 channel minima must fall strictly within the electrical valid window:
     `300 <= minima[c] <= 700`.
   - If any channel falls outside `[300, 700]`, the attempt fails (`FTE3600_FW9369_STARTUP_FAILED`) and the outer loop retries (up to 3 total attempts).

4. **Baseline Threshold Computation (`fw9369_startup.c:192–198`):**
   - For each channel `c` (0..3):
     ```c
     value = minima[c] < 31 ? 0 : (uint16_t)(minima[c] - 30);
     s->fdt_reference[c] = s->fdt_down[c] = s->fdt_up[c] = value;
     ```
   - Subtracting 30 creates the operational touch detection threshold margin.
   - Computes CRC-CCITT (polynomial `0x1021`, seed `0xFFFF`) over `fdt_up[0..3]`:
     `s->fdt_down_crc = s->fdt_up_crc = crc(s->fdt_up, 4);`
   - Returns `FTE3600_FW9369_STARTUP_OK`.

---

## 5. Volatility & Storage Classification

| Register / Address | Operation Type | Purpose / Function | Volatility Classification |
|---|---|---|---|
| `0x80` | Read Byte | Power / Idle state status | Volatile SFR |
| `0x9A` | Write Byte | Register bank access unlock (`0x5A`) / lock (`0x00`) | Volatile SFR |
| `0xC0 .. 0xC2` | Read/Modify Word | FDT analog channel threshold configurations | Volatile SRAM / SFR bank |
| `0x1800 .. 0x1811` | Read/Write Word | Analog sensing frontend window, bias, integrator, timing | Volatile SFRs |
| `0x1880 .. 0x1888` | Read/Write Word | FDT comparator thresholds, averaging, scan trigger | Volatile SFRs |
| `0x1A82` | Read Word | Interrupt / Event status (Bit 3: FDT Scan Complete) | Volatile SFR (Read-Only) |
| `0x1A83` | Read/Modify Word | Interrupt / Event mask (Bit 3: FDT IRQ enable) | Volatile SFR |
| `0x1A8A` | Write Word | Interrupt routing control (`0x00FF`) | Volatile SFR |
| `0x80E8` / `0x80B8` | Multi-Word Read | Raw FDT ADC conversion channels | Volatile ADC FIFO/Registers |

**Critical Audit Conclusion:**  
Every single operation performed during `fdt_baseline()` targets volatile registers, volatile SFRs, or volatile analog front-end flip-flops. **Zero non-volatile memory (OTP, eFuse, EEPROM, or Flash) registers are accessed during this procedure.**
