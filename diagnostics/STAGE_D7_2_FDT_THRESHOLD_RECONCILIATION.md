# Stage D7.2 — Technical Reconciliation: FDT Threshold Calculations

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source and Documentation Review (Zero Hardware Operations)  
**Deliverable:** `diagnostics/STAGE_D7_2_FDT_THRESHOLD_RECONCILIATION.md`  

---

## 1. Executive Summary

This report reconciles the two conflicting mathematical formulas for Finger Detection Thresholds (FDT) reported in earlier stages:
1. **Stage D7 Report:** Channel baseline minima calculation: `threshold = minima[c] - 30`.
2. **Stage D7.1 Final Verdict:** Asserted formula: `touch_down = (dac - 1) * 4`, `touch_up = (dac + 1) * 4`.

Through exhaustive line-by-line tracing of the vendor source code (`fw9369_startup.c`, `fw9369_fdt.c`, `calibration.c`, and `fw9369_release.c`), this audit definitively resolves the discrepancy:
- `minima[c] - 30` is the **sole authentic source-derived threshold formula** implemented by the vendor for FT9362/FW9369 finger detection.
- `(dac - 1) * 4` was an **erroneous formula** introduced into `STAGE_D7_1_FINAL_VERDICT.md:55` that conflated the DAC single-step adjustments in `calibration.c:69` with the 4-channel ADC averaging logic. No such formula exists anywhere in the vendor codebase.

---

## 2. Comprehensive Reconciliation Matrix

| Calculation / Concept | Stage D7 Claim | Stage D7.1 Claim | Vendor Source Evidence | Corrected Interpretation | Confidence Level |
|---|---|---|---|---|---|
| **DAC Bias Calibration** | Successive approximation binary search to target 512 | Binary DAC search bounded to `[1, 125]`, max 10 samples | `calibration.c:6–74`; `fw9369_fdt.c:385` | Finds single analog bias byte `s->dac` (written to `0x1801`). Adjusts `dac` towards ADC mean 512. | **CONFIRMED FACT (Source Verified)** |
| **Operational Threshold Formula** | `value = minima[c] - 30` | `touch_down = (dac - 1) * 4`<br>`touch_up = (dac + 1) * 4` | `fw9369_startup.c:194`; `fw9369_release.c:205` | `minima[c] - 30` is the actual formula. `(dac - 1) * 4` was an error in D7.1. | **CORRECTED FACT (D7 Verified; D7.1 Refuted)** |
| **Threshold Bank Address** | `0x80E0` (foundry `0xAA`) / `0x80B0` (non-SMIC) | Bank `0x80–0x8B` under `0x9A=0x5A` | `fw9369_startup.c:388, 398`; `fw9369_release.c:266` | 22-byte SPI transfer writing 8 16-bit words to `0x80B0` (or `0x80E0`) with `0x9A = 0x5A` unlocked. | **CONFIRMED FACT (Source Verified)** |
| **Initial Down vs Up Thresholds** | Both set to `value` | Inferred as separate calculations | `fw9369_startup.c:195` | Startup assigns identical baseline values: `fdt_reference = fdt_down = fdt_up = value`. | **CONFIRMED FACT (Source Verified)** |

---

## 3. End-to-End Variable & Pipeline Trace

### Phase 1: Bias DAC Binary Calibration (`calibration.c:6–74` & `fw9369_fdt.c:384–392`)
1. **Declaration:**
   `uint8_t *stored_dac`, initialized to 27 (for 4-channel mode) or 56 (for 8-channel mode) (`calibration.c:21`).
2. **Measurement Callback:**
   `sample(context, *stored_dac, channels)` invokes `physical_sample()` (`fw9369_fdt.c:333`), which writes `0x1801` with `(uint16_t)(0xfc80 | s->dac)` (`fw9369_fdt.c:177`).
3. **Evaluation:**
   Sums all channels: `sum += channels[channel]`; `measurement = sum / channel_count` (`calibration.c:36`).
4. **DAC Step Adjustment:**
   - If `measurement < 20`: `lower = dac; *stored_dac = (dac + upper) / 2;`
   - If `measurement > 1000`: `upper = dac; *stored_dac = (lower + dac) / 2;`
   - Otherwise (`20 <= measurement <= 1000`):
     ```c
     *stored_dac = (uint8_t) (measurement <= 512 ? dac + 1 : dac - 1); // calibration.c:69
     ```
   - Convergence occurs when two adjacent DAC values bracket 512 (`calibration.c:38–46`).
5. **Output:** The converged 8-bit value is stored in `state->dac` and permanently programmed into volatile register `0x1801`.

---

### Phase 2: Multi-Sample Channel Baseline Acquisition (`fw9369_startup.c:162–189`)
With the optimal DAC bias locked into `0x1801`, `fdt_baseline()` acquires physical ADC samples:
1. **Minima Array Initialization (`fw9369_startup.c:163–164`):**
   ```c
   uint16_t minima[4], previous[4], channels[8], stable = 0;
   for (size_t c = 0; c < 4; ++c)
       minima[c] = 1023;
   ```
2. **Measurement Loop (`fw9369_startup.c:165–182`):**
   Executes up to 10 iterations of `fdt_call(op, FTE3600_FW9369_FDT_MEASURE, channels)`.
   For each sample and each channel `c` (0 to 3):
   ```c
   if (channels[c] < minima[c])
       minima[c] = channels[c];
   ```
   Loop terminates early if 3 consecutive samples are stable within $\pm 5$ counts.
3. **Validation Window (`fw9369_startup.c:184–186`):**
   ```c
   for (size_t c = 0; c < 4; ++c)
       if (minima[c] < 300 || minima[c] > 700)
           result = FTE3600_FW9369_STARTUP_FAILED;
   ```
   Every channel's minimum must fall strictly within `[300, 700]`.

---

### Phase 3: Threshold Calculation (`fw9369_startup.c:192–198`)
This is the exact threshold calculation executed by vendor code:
```c
for (size_t c = 0; c < 4; ++c)
{
    uint16_t value = minima[c] < 31 ? 0 : (uint16_t) (minima[c] - 30);
    op->s->fdt_reference[c] = op->s->fdt_down[c] = op->s->fdt_up[c] = value;
}
op->s->fdt_down_crc = op->s->fdt_up_crc = crc (op->s->fdt_up, 4);
```
- **Subtraction Margin:** Exactly **30 ADC counts** are subtracted from each channel's minimum observed value.
- **Underflow Clamp:** If `minima[c] < 31`, `value` is clamped to 0.
- **Identity Assignment:** `fdt_reference`, `fdt_down`, and `fdt_up` receive identical values at startup.
- **CRC Validation:** CRC-CCITT is computed across the 4 16-bit threshold words.

---

### Phase 4: Threshold Bank Hardware Programming (`fw9369_startup.c:386–405`)
During sensor arming (`arm()` in startup, or `fte3600_fw9369_arm_detection()` in release):
1. **Unlock Bank Register:**
   `write_byte (op, 0x9a, 0x5a);` (`fw9369_startup.c:396`)
2. **Buffer Preparation:**
   ```c
   uint8_t bytes[22] = {5, 0xfa, 0x80, 0xb0, 0, 8};
   if (op->s->config.foundry == 0xaa)
       bytes[3] = 0xe0; // Address 0x80E0 for SMIC AA foundry
   for (size_t c = 0; c < 8; ++c)
   {
       bytes[6 + c * 2] = (uint8_t) (op->s->fdt_down[c] >> 8);
       bytes[7 + c * 2] = (uint8_t) op->s->fdt_down[c];
   }
   ```
3. **SPI Multi-Byte Write:**
   `transfer (op, bytes, sizeof (bytes));` writes 8 16-bit words (16 bytes payload) to SRAM bank `0x80B0` (or `0x80E0`).
4. **Relock Bank Register:**
   `write_byte (op, 0x9a, 0);` (`fw9369_startup.c:405`)

---

## 4. Origin of the Erroneous "(dac - 1) * 4" Formula

In `STAGE_D7_1_FINAL_VERDICT.md:55`, the following bullet appeared:
```text
Threshold calculation formula: touch_down = (dac - 1) * 4, touch_up = (dac + 1) * 4.
```
This formula was an erroneous conflation of two unrelated code structures:
1. `calibration.c:69`:
   `*stored_dac = (uint8_t) (measurement <= 512 ? dac + 1 : dac - 1);`
   (Linear stepping of the single analog bias DAC byte).
2. `fw9369_fdt.c:189`:
   `((uint32_t) (fast ? 1 : (uint16_t) (c->repeat - 1u)) << 2)`
   (A bitshift by 2, equivalent to multiplying by 4, for repeat scan timing).

The report mistakenly synthesized these fragments into `(dac \pm 1) * 4`. That formula does not exist in any vendor source file and is hereby formally retracted.

---

## 5. Worked Example Using Authentic Source Logic

Suppose the vendor initialization sequence executes on physical hardware:

1. **DAC Calibration (`calibration.c`):**
   - Initial DAC trial starts at `stored_dac = 27`.
   - After binary search iterations, `stored_dac` converges to **`34`** (`0x22`).
   - Register `0x1801` is programmed with `0xFC80 | 0x22 = 0xFCA2`.

2. **Stability Sampling (`fw9369_startup.c:165–182`):**
   - 10 measurements are taken across channels 0 through 3.
   - Channel measurements stabilize with the following recorded minima:
     - `minima[0] = 512`
     - `minima[1] = 488`
     - `minima[2] = 530`
     - `minima[3] = 504`
   - Validation check: All values are between 300 and 700 (`300 <= minima[c] <= 700`). Validation passes.

3. **Threshold Calculation (`minima[c] - 30`):**
   - Channel 0: `512 - 30 = 482` (`0x01E2`)
   - Channel 1: `488 - 30 = 458` (`0x01CA`)
   - Channel 2: `530 - 30 = 500` (`0x01F4`)
   - Channel 3: `504 - 30 = 474` (`0x01DA`)
   - Channels 4–7: Unused in 4-channel mode (`0x0000`).

4. **Hardware Wire Transfer (`bytes[22]`):**
   - Address prefix: `05 FA 80 B0 00 08` (Write 8 words starting at `0x80B0` under `0x9A=0x5A`)
   - Channel 0 (`0x01E2`): `0x01, 0xE2`
   - Channel 1 (`0x01CA`): `0x01, 0xCA`
   - Channel 2 (`0x01F4`): `0x01, 0xF4`
   - Channel 3 (`0x01DA`): `0x01, 0xDA`
   - Channels 4–7: `0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00`
   - Full 22-byte wire frame:
     `05 FA 80 B0 00 08 01 E2 01 CA 01 F4 01 DA 00 00 00 00 00 00 00 00`
