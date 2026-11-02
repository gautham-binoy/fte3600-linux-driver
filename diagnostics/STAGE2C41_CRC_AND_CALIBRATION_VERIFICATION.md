# Stage 2C.4.1 — CRC, Calibration, and FIFO Verification Audit

**Document ID:** `STAGE2C41_CRC_AND_CALIBRATION_VERIFICATION`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Audit Scope:** CRC algorithms, FDT calibration routines, cold baseline handling, and 10,246-byte FIFO framing  
**Hardware Authorization:** NONE (Static Analysis & Mock Verification Only)

---

## 1. Executive Summary

This audit evaluates the algorithmic correctness of the mathematical routines and data structures implemented in `tools/fw9369-mock/src/fw9369_crc.c` and `tools/fw9369-mock/src/fw9369_calibration.c`.

The audit specifically examines:
1. **CRC-CCITT Implementations:** Distinction between MSB-first big-endian wire CRCs and little-endian host-word CRCs.
2. **FDT Calibration:** Hybrid DAC centering search, stability window exit logic, per-channel absolute minimum tracking (zero averaging), bounds checking `[300, 700]`, and the threshold derivation \(fdt\_down[c] = \max(0, minima[c] - 30)\).
3. **Cold Image Baseline:** Single-frame acquisition via bit-exact `memcpy` (zero temporal filtering, zero multi-frame averaging), and 128-word signature CRC calculation.
4. **FIFO Framing:** 10,246-byte packet layout with 6-byte command prefix.
5. **Physical Protocol vs. Simulation Approximation Boundary:** Clear demarcation of physical sensor constants versus simulated analog behavioral approximations.

---

## 2. CRC Implementations & Test Vectors

### 2.1 Wire CRC-CCITT (`fw9369_crc_wire`)
- **Specification:** Standard CRC-CCITT (MSB-first, polynomial `0x1021`, initial seed `0xFFFF`, no bit reversal, final XOR `0x0000`).
- **Application:** Sensor-generated telemetry transmitted on SPI MISO:
  - 2-byte Chip ID reading (`0x9362`).
  - 8-byte FDT ADC telemetry from register `0x80B8` (4 channels $\times$ 2 bytes).
- **Physical Validation:**
  - Chip ID vector: `bytes = { 0x93, 0x62 }`
  - Computed CRC: `0x1C53`
  - *Result:* Exact match with physical hardware Stage 2A empirical reading (`diagnostics/stage2a-results.txt`).

### 2.2 Host Little-Endian Word CRC (`fw9369_crc_words_le`)
- **Specification:** Standard CRC-CCITT (polynomial `0x1021`, initial seed `0xFFFF`, no bit reversal, final XOR `0x0000`) calculated over an array of 16-bit host words.
- **Serialization Rule:** Each 16-bit word is unpacked little-endian (byte 0 = LSB, byte 1 = MSB) before feeding into the MSB-first bit-shifting CRC loop:
  ```c
  for (unsigned int byte = 0; byte < 2; ++byte) {
      uint8_t b = (uint8_t)(words[i] >> (8 * byte));
      crc = (uint16_t)(crc ^ ((uint16_t)b << 8));
      for (unsigned int bit = 0; bit < 8; ++bit) {
          crc = (uint16_t)(((uint16_t)crc << 1) ^
                           ((crc & 0x8000) ? 0x1021u : 0u));
      }
  }
  ```
- **Application:**
  - 4-word FDT threshold vector (`fdt_down[4]`).
  - 128-word Cold Baseline signature (first 2 image rows).
- **Test Vectors & Verification:**
  - `[ 0, 0, 0, 0 ]` -> `0x313E` (**PASS**)
  - `[ 512, 512, 512, 512 ]` -> `0x5B7C` (**PASS**)
  - `[ 482, 482, 482, 482 ]` -> `0xE4AA` (**PASS**)
  - Big-endian vs Little-endian divergence test: `be_stream = { 0x02, 0x00, ... }` yields different CRC than LE words (**PASS**).

### 2.3 Documentation Discrepancy Note (Non-Critical)
In `tools/fw9369-mock/include/fw9369_crc.h` (line 32), the doc comment states:
```c
*   [ 482, 482, 482, 482 ] -> 0x081D
```
- **Analysis:** `0x081D` was an initial scratch calculation that erroneously serialized the word `482` (`0x01E2`) as big-endian bytes `[ 0x01, 0xE2, ... ]`.
- Little-endian serialization of `482` produces bytes `[ 0xE2, 0x01, 0xE2, 0x01, 0xE2, 0x01, 0xE2, 0x01 ]`, which mathematically yields CRC `0xE4AA`.
- The implementation in `src/fw9369_crc.c` and the test assertion in `tests/test_runner.c:50` correctly use `0xE4AA`.
- **Verdict:** The code and tests are 100% mathematically correct. The header comment in `include/fw9369_crc.h:32` contains an errant comment string from early scratch work and can be cosmetic-cleaned in a later stage.

---

## 3. FDT Calibration Verification

The implementation of `fw9369_calibrate_fdt()` in `src/fw9369_calibration.c` was audited against the canonical specification:

### 3.1 DAC Centering Search (Register `0x1801`)
- **Initial Seed:** `dac = 27` (line 100), matching the reference driver's 4-channel mode default.
- **Search Logic:**
  - Coarse binary search when ADC average is saturated ($< 20$ or $> 1000$).
  - Fine linear increments/decrements near target 512 ($meas \le 512 \implies dac+1$; $meas > 512 \implies dac-1$).
  - Zero-crossing detection: when consecutive DAC steps bracket 512 (`prev_meas <= 512 && meas > 512`), the loop selects the higher DAC setting and terminates.
- **Convergence:** Under mock AFE simulation (`512 + (dac - 27) * 6`), the search deterministically converges at `dac = 28` within 2 trials.

### 3.2 Stability Window & Early Exit
- **Sample Count:** Up to 10 samples (lines 158–183).
- **Stability Window:**
  - Evaluates per-channel delta: $|ch[c] - prev[c]| \le 5$.
  - If delta exceeds 5 counts, `stable` counter resets to 1.
  - If delta $\le 5$ counts across all 4 channels, `stable` increments.
  - Early exit condition: `if (s > 0 && stable >= 3) break;` (line 181).
- **Verification:** In mock testing with stable simulated ADC, early exit cleanly triggers after 3 consecutive stable samples (`sample_count = 3` to `4`), satisfying the reference driver's optimization.

### 3.3 Per-Channel Minimum Tracking (Zero Averaging)
- **Minimum Formula:**
  ```c
  if (ch[c] < cal->minima[c])
      cal->minima[c] = ch[c];
  ```
- **Audit:** Zero arithmetic averaging or low-pass filtering is applied across the stability samples. Each channel independently records its absolute minimum raw ADC code.

### 3.4 Range Validation & Threshold Calculation
- **Bounds Check:** Ensures all 4 minima satisfy $300 \le minima[c] \le 700$. Returns `-ERANGE` (-71) if out of bounds.
- **Threshold Derivation:**
  ```c
  cal->fdt_down[c] = (cal->minima[c] < 31) ? 0 : (uint16_t)(cal->minima[c] - 30);
  ```
  - Underflow protection: clamps to 0 if $minima[c] < 31$.
  - Offset subtraction: exactly 30 ADC counts subtracted from each channel minimum.
- **Threshold Vector CRC:** 4-word little-endian CRC computed via `fw9369_crc_words_le(cal->fdt_down, 4)`.

---

## 4. Cold Image Baseline Verification

The implementation of `fw9369_acquire_image_baseline()` in `src/fw9369_calibration.c` was audited:

### 4.1 Nominal Parameters
- `image_dac` is set to nominal default `54` (line 213).
- Target median is recorded as `512`.

### 4.2 10,246-Byte Burst FIFO Transfer Framing
- **Buffer Allocation:** Dynamically allocates 10,246 bytes on heap (line 217).
- **Prefix Header:** Encodes 6-byte prefix `[ 0x06, 0xF9, 0x9A, 0x05, 0x14, 0x00 ]`:
  - `0x06`: FIFO burst read opcode
  - `0xF9`: One's complement of opcode
  - `0x9A, 0x05, 0x14, 0x00`: 4-byte reference burst header
- **Pixel Payload:** Followed by 10,240 bytes representing $64 \times 80 = 5,120$ 16-bit words.
- **Unpacking:** Wire bytes unpacked as big-endian 16-bit words:
  `work_image[i] = (rx[prefix + i*2] << 8) | rx[prefix + i*2 + 1]`.

### 4.3 Cold Baseline Copy Discipline (Zero Filtering)
- **Copy Mechanism:**
  ```c
  memcpy(base->baseline, work_image, sizeof(base->baseline));
  ```
- **Audit:** Single raw captured frame copied bit-for-bit. Zero multi-frame averaging, zero rolling background subtraction, zero spatial or temporal filtering.
- **128-Word CRC Signature:**
  ```c
  base->baseline_crc = fw9369_crc_words_le(base->baseline, 128);
  ```
  Calculated across exactly the first 128 words (256 bytes, corresponding to the first 2 physical pixel rows), matching the reference driver's baseline validation signature.

---

## 5. Physical Protocol vs. Simulation Approximation Boundary

To preserve hardware integrity, the boundary between exact physical protocol framing and synthetic mock behavior is clearly defined:

| Pipeline Element | Implementation Nature | Physical Hardware Reality | Mock Implementation Status |
| :--- | :---: | :--- | :--- |
| **SPI Complement Opcodes** | **Exact Protocol** | `0x09/0xF6`, `0x08/0xF7`, `0x05/0xFA`, `0x04/0xFB`, `0x06/0xF9` | 100% Bit-exact to reference driver. |
| **Register Addresses** | **Exact Protocol** | `0xC6`, `0x80`, `0x9A`, `0x1800`–`0x1887`, `0x1A82`–`0x1A8B`, `0x80B0`–`0x80B8` | 100% Bit-exact to specification. |
| **Wire CRC-CCITT** | **Exact Protocol** | MSB-first CRC-CCITT (poly `0x1021`, init `0xFFFF`) | Matches Stage 2A physical chip CRC `0x1C53`. |
| **Host LE Word CRC** | **Exact Protocol** | Host memory little-endian word serialization CRC | Matches test vectors `0x313E`, `0x5B7C`, `0xE4AA`. |
| **AFE ADC Transfer Function** | *Simulation Approximation* | Silicon analog front-end response to DAC and parasitic capacitance | Modeled linearly as `512 + (dac - 27) * 6`. |
| **Image Gradient Pattern** | *Simulation Approximation* | Physical silicon photodiode/capacitive sensor array noise floor | Synthetic gradient `500 + (p % 25)`. |
| **10,246 B Burst Transfer** | *Protocol Framing Only* | Physical SPI DMA buffer capacity, CS pin continuity, bus clocking | Verifies userspace packet framing; physical bus timing deferred to Stage 2C.5. |

---

## 6. Audit Conclusion

The mathematical formulas, CRC engines, calibration search routines, and baseline framing implemented in `tools/fw9369-mock/` are verified to be mathematically sound, conformant to the canonical specification, and properly segregated from hardware-dependent assumptions.
- CRC, calibration, and FIFO verification is **PASSED**.
