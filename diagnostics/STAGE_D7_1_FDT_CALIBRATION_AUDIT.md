# Stage D7.1 — Adversarial Audit of FDT Calibration Behavior & Algorithmic Failure Paths

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source and Documentation Audit  
**Audit Classification:** Adversarial Challenge & Evidence Qualification  

---

## 1. Executive Summary

This report delivers a rigorous adversarial audit of the Finger Detection (FDT) calibration subsystem (`fdt_baseline()`), examining algorithmic termination conditions, polling timeouts, CRC byte order, and hardware state retention under failure conditions.

The primary finding of this audit is that **the vendor FDT calibration routine provides zero hardware rollback on failure.** If DAC calibration fails, polling times out, or channel minima violate electrical bounds (`[300, 700]`), the sensor's analog frontend registers (`0x1800` through `0x1888`) and comparator threshold banks (`0xC0..0xC2`) remain modified in a partially configured, non-operational state. The vendor architecture relies entirely on the outer host session manager (`device_session.c`) to issue a physical hardware GPIO reset pulse via Pad 14 to restore the chip to power-on defaults.

---

## 2. Algorithmic Deep Dive & Search Termination

### 2.1 DAC Successive Approximation Search (`calibration.c:6–74`)

The DAC search algorithm (`fte3600_calibrate_channels`) determines the analog bias register `0x1801` [6:0]:

```
  Search State Variables:
  - lower = 0, upper = 127
  - Initial DAC: 27 (4-channel mode) or 56 (8-channel mode)
  - Target: 4-channel average ADC measurement crossing 512
```

1. **Step Logic:**
   - At each iteration, takes a physical ADC measurement across all channels.
   - Computes `measurement = sum / channel_count`.
   - **Boundary Tracking:**
     - If `measurement < 20`: Sets `lower = dac; dac = (dac + upper) / 2`.
     - If `measurement > 1000`: Sets `upper = dac; dac = (lower + dac) / 2`.
     - If `20 <= measurement <= 1000`: Single-step increment/decrement:
       `dac = (measurement <= 512 ? dac + 1 : dac - 1)`.
2. **Termination Conditions:**
   - **Success (Convergence):**
     Terminates when the measurement transitions across 512 between adjacent integer DAC values:
     `((dac == prev + 1 && prev_meas <= 512 && meas > 512) || (prev == dac + 1 && meas <= 512 && prev_meas > 512))`.
     The higher DAC is locked as calibrated.
   - **Failure 1 (Out of Bounds):**
     If `dac < 1 || dac > 125`, immediately returns `FTE3600_CALIBRATION_FAILED`.
   - **Failure 2 (Endpoint Collapse):**
     If `dac == upper` (when `< 20`) or `dac == lower` (when `> 1000`), returns `FTE3600_CALIBRATION_FAILED`.
   - **Failure 3 (Iteration Exhaustion):**
     If convergence is not achieved within `FTE3600_CALIBRATION_MAX_SAMPLES = 10` iterations, returns `FTE3600_CALIBRATION_FAILED`.

---

## 3. Sampling, Polling Timeouts & Foundry Gating

### 3.1 Polling Timeout Behavior (`fw9369_fdt.c:248–258`)

During each physical measurement (`scan()`):
- The driver writes `0x1885 = 1` to trigger an ADC conversion.
- It enters a polling loop reading status register `0x1A82`:
  ```c
  for (unsigned int i = 0; i < 5; ++i) {
      if (!read_word(op, 0x1a82, &value))
          return FTE3600_FW9369_FDT_WORD_READ_ERROR;
      if ((value & 8) != 0)
          return FTE3600_FW9369_FDT_OK;
      delay(op, 1);
  }
  ```
- **Adversarial Finding:**
  If 5 polling attempts expire (5 ms elapsed) without bit 3 asserting, the driver **does not abort with an error code!**
  Lines 256–258 simply record `op->diagnostics->ready_timeout = 1` and return `FTE3600_FW9369_FDT_OK`.
  The driver then proceeds immediately to `read_channels()`, reading unverified ADC registers that may contain un-latched, stale, or incomplete conversion data!

### 3.2 Foundry-Specific ADC Read Address (`fw9369_fdt.c:266–268`)

- The ADC channel registers differ based on the foundry code discovered during `prefix()`:
  - SMIC Fab AA (`foundry == 0xAA`): register `0x80E8`.
  - Non-SMIC (`foundry != 0xAA`): register `0x80B8`.
- If foundry code was misidentified during startup, FDT reads will pull data from incorrect hardware register offsets, resulting in garbage channel readings and subsequent calibration failure.

---

## 4. Threshold Calculation & CRC Byte Order

### 4.1 Minima Validation & Baseline Margining (`fw9369_startup.c:184–198`)

1. **Validation Window:**
   Across 10 stability samples, the minimum observed value for each of the 4 channels must satisfy:
   `300 <= minima[c] <= 700`.
   If any channel minimum is `< 300` (short circuit / saturation) or `> 700` (open circuit / floating pad), `fdt_baseline()` returns `FTE3600_FW9369_STARTUP_FAILED`.
2. **Threshold Margin:**
   The operational touch baseline is calculated by subtracting an empirical noise margin of 30:
   ```c
   uint16_t value = minima[c] < 31 ? 0 : (uint16_t)(minima[c] - 30);
   op->s->fdt_reference[c] = op->s->fdt_down[c] = op->s->fdt_up[c] = value;
   ```
   If `minima[c] < 31`, it saturates at `0` to prevent unsigned 16-bit integer underflow.

### 4.2 Subtle CRC Byte-Order Discovery (`fw9369_startup.c:116–128`)

An important protocol detail uncovered during this audit concerns the CRC computation:
- When writing the threshold bank over the SPI bus, words are transmitted in **Big-Endian** order:
  `bytes[6 + 2*c] = thresholds[c] >> 8; bytes[7 + 2*c] = thresholds[c] & 0xFF;`
- **However, when calculating `fdt_up_crc`:**
  ```c
  for (size_t i = 0; i < count; ++i)
      for (unsigned int byte = 0; byte < 2; ++byte) {
          result ^= (uint16_t)(((uint16_t)(uint8_t)(words[i] >> (8 * byte))) << 8);
          /* ... CRC polynomial 0x1021 math ... */
      }
  ```
  The loop iterates with `byte = 0` first (`(words[i] >> 0) & 0xFF`), then `byte = 1` (`(words[i] >> 8) & 0xFF`).
- **Critical Finding:** The internal CRC validation engine processes words in **LITTLE-ENDIAN BYTE ORDER**, contrasting directly with the Big-Endian wire framing! Any driver implementation computing CRC over the raw SPI wire buffer would produce a mismatched checksum.

---

## 5. Failure States & Hardware Rollback Absence

The following table documents every possible failure state in `fdt_baseline()` and the resulting hardware residual condition:

| Failure Mode | Trigger Condition | Software Return | Hardware Residual State | Rollback Action |
|---|---|---|---|---|
| **SPI Transport Error** | Failed transfer in `configure()` | `FTE3600_FW9369_FDT_WORD_READ_ERROR` | Partially written registers | **None (Zero Rollback)** |
| **Idle Timeout** | Status byte `0x80 != 0x50` | Continues after `0xC0 0x3F` | Strobe issued, potential stall | **None** |
| **DAC Convergence Failure** | 10 samples exhausted or DAC outside `[1, 125]` | `FTE3600_CALIBRATION_FAILED` | Calibrated flag unset, DAC left at last trial | **None (Zero Rollback)** |
| **ADC Conversion Timeout** | Bit 3 of `0x1A82` not asserted after 5 ms | Returns OK, sets `ready_timeout` | Conversion incomplete, reads stale ADC | **None** |
| **Minima Out of Bounds** | Any `minima[c] < 300` or `> 700` | `FTE3600_FW9369_STARTUP_FAILED` | Analog registers left at calibrated values | **None (Zero Rollback)** |
| **Attempts Exhausted** | 3 attempts fail bounds check | `FTE3600_FW9369_STARTUP_UNDEFINED_BASELINE` | Sensor left in uncalibrated analog state | **None (Zero Rollback)** |

### 5.1 Architectural Implication for Kernel Driver
Because the vendor hardware logic contains **no automatic register restoration**, a failure during in-kernel calibration cannot simply return `-EIO` or `-EPERM` while leaving the hardware online.
A failed calibration must unconditionally transition the driver state machine to `FTE3600_STATE_UNKNOWN` (7), requiring an explicit physical hardware reset (`FTE3600_IOC_RESET`) before any subsequent initialization can be attempted.
