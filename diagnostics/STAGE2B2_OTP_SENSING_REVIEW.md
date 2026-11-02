# Stage 2B.2 — OTP Sensing Call Graph & Persistence Review

**Document ID:** `STAGE2B2_OTP_SENSING_REVIEW`  
**Date:** 2026-11-02  
**Target Hardware:** FocalTech FT9362 (FW9369 Architecture)  
**Host Controller:** Intel LPSS SPI (`pxa2xx-spi.2`) / Dynabook Satellite Pro C40-J  
**Scope:** Complete call-graph trace for registers `0xF1`, `0xF4`, `0xF3`, `0x9B`, and rigorous persistence risk analysis.  
**Execution Gate:** Strictly static analysis. No hardware execution.

---

## 1. Complete Call Graph & Transaction Trace

In the FW9369 pathway (`research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c`), all operations involving registers `0xF1`, `0xF4`, `0xF3`, and `0x9B` reside within a single function: `prefix()`.

### 1.1 Architectural Hierarchy and Execution Order
1. **Entry Point:** `fte3600_fw9369_startup_linux()` ([fw9369_startup.c:554](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L554))
2. **Internal Dispatcher:** `run()` ([fw9369_startup.c:425](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L425))
   - Condition: `if (operation == FTE3600_FW9369_STARTUP)`
   - Dispatches to: `prefix(&op)` ([fw9369_startup.c:452](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L452))
3. **Execution Block inside `prefix()` ([fw9369_startup.c:325-375](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L325-L375)):**
   - Step A: Bus negotiation on `0xC6` (Lines 328–333)
   - Step B: Chip ID read on `0x1A8B` (Line 338)
   - Step C: Reset cached state & clear interrupt flags via `0x1A84 = 0xFFFF` (Lines 342–343)
   - Step D: Idle handshake (`5A A5`, read `0x80`, `A5 5A`) (Line 345)
   - Step E: **OTP Clocking Loop** (Lines 348–356)
   - Step F: **Foundry Code Read on `0x9B`** (Lines 357–368)
   - Step G: Re-initialize config structure and clear flags via `0x1A84 = 0xFFFF` (Lines 369–380)

---

### 1.2 Exact Transaction Details

The following table records every transaction in Steps E and F chronologically:

| Seq | Source File & Line | Register | R/W | Exact SPI Packet (MOSI) | Value | Conditionality | Exact Returned Value / Purpose | Classification |
| :---: | :--- | :---: | :---: | :--- | :---: | :--- | :--- | :---: |
| **1** | `fw9369_startup.c:350` | `0xF1` | Write | `[ 09 F6 F1 03 ]` | `0x03` | Loop iter 0 (`i == 0`) | Sets internal OTP address pointer to row 3 | **UNKNOWN** |
| **2** | `fw9369_startup.c:351` | `0xF4` | Write | `[ 09 F6 F4 C0 ]` | `0xC0` | Unconditional | Asserts OTP clock / pre-charge strobe | **UNKNOWN** |
| **3** | `fw9369_startup.c:352` | `0xF4` | Write | `[ 09 F6 F4 C1 ]` | `0xC1` | Unconditional | Clocks sense amplifiers (bit 0 high) | **UNKNOWN** |
| **4** | `fw9369_startup.c:353` | `0xF4` | Write | `[ 09 F6 F4 C0 ]` | `0xC0` | Unconditional | Returns sense clock to low | **UNKNOWN** |
| **5** | `fw9369_startup.c:354` | `0xF4` | Write | `[ 09 F6 F4 C0 ]` | `0xC0` | Unconditional | Second deassertion pulse | **UNKNOWN** |
| **6** | `fw9369_startup.c:355` | `0xF3` | Read | `[ 08 F7 F3 00 00 ]` | N/A | Unconditional | Discarded: `(void) read_byte (op, 0xf3);` | **VERIFIED READ-ONLY** |
| **7** | `fw9369_startup.c:350` | `0xF1` | Write | `[ 09 F6 F1 13 ]` | `0x13` | Loop iter 1 (`i == 1`) | Sets internal OTP address pointer to row `0x13` | **UNKNOWN** |
| **8** | `fw9369_startup.c:351` | `0xF4` | Write | `[ 09 F6 F4 C0 ]` | `0xC0` | Unconditional | Asserts OTP clock / pre-charge strobe | **UNKNOWN** |
| **9** | `fw9369_startup.c:352` | `0xF4` | Write | `[ 09 F6 F4 C1 ]` | `0xC1` | Unconditional | Clocks sense amplifiers (bit 0 high) | **UNKNOWN** |
| **10** | `fw9369_startup.c:353` | `0xF4` | Write | `[ 09 F6 F4 C0 ]` | `0xC0` | Unconditional | Returns sense clock to low | **UNKNOWN** |
| **11** | `fw9369_startup.c:354` | `0xF4` | Write | `[ 09 F6 F4 C0 ]` | `0xC0` | Unconditional | Second deassertion pulse | **UNKNOWN** |
| **12** | `fw9369_startup.c:355` | `0xF3` | Read | `[ 08 F7 F3 00 00 ]` | N/A | Unconditional | Discarded: `(void) read_byte (op, 0xf3);` | **VERIFIED READ-ONLY** |
| **13** | `fw9369_startup.c:359` | `0x9B` | Read | `[ 08 F7 9B 00 00 ]` | N/A | Loop up to 10 polls | `code = read_byte(0x9b) >> 2`. If `code == 0x13`, sets `foundry = 0xAA` (SMIC); if `code == 0`, sets `foundry = 0` (standard). | **VERIFIED READ-ONLY** |

---

## 2. Evidence Assessment on Persistent OTP/EFUSE Alteration

### 2.1 What the Reference Implementation Demonstrates
1. In `fw9369_startup.c`, the sequence is executed during standard device initialization.
2. The exact same pattern (`write 0xF1`, clock `0xF4` with `0xC0`/`0xC1`/`0xC0`, read `0xF3`) appears in:
   - `ft93xx.c:595-613` under function name `efuse_byte()`
   - `discovery.c:70-80` under function name `discover_otp()`
   - Windows driver strings under symbol names `fw9369_get_SMIC_IC_flag` and `fw9369_Get_OTP_Info`.
3. In all these contexts, the software developer's intention was to *read* OTP memory bytes.

### 2.2 What the Implementation Fails to Prove (The Persistence Blind Spot)
1. **Lack of Hardware Register Specification:** The reference repository does not provide a hardware specification or silicon datasheet defining register `0xF4`.
2. **Absence of Write-Protection Interlocks:** In older FocalTech architectures (e.g. FT9338/FT9368), OTP programming requires applying high voltage ($V_{PP}$) or unlocking specific write sequences. However, because we do not have authoritative documentation for the FT9362 controller:
   - It cannot be mathematically proven from C code alone that `0xF4 = 0xC1` is guaranteed to be electrically incapable of altering non-volatile state.
   - It cannot be proven that executing this clocking sequence in an uncalibrated or power-degraded state cannot cause latch-up or corrupt internal OTP trim cells.
3. **Engineering Rigor Policy:** Inferring electrical safety solely from function names (`discover_otp`, `efuse_byte`) or from the absence of explicit burn functions in userspace violates safety principles.

### 2.3 Formal Classification

- **Register `0xF3` Read:** **`VERIFIED READ-ONLY`**. Reading over SPI is non-destructive.
- **Register `0x9B` Read:** **`VERIFIED READ-ONLY`**. Reading over SPI is non-destructive.
- **Register `0xF1` Write:** **`UNKNOWN`**. Selects an OTP address row. While intended as a volatile address pointer, its interaction with the underlying non-volatile macro is undocumented.
- **Register `0xF4` Write:** **`UNKNOWN`**. Toggles internal OTP sense/clock lines. Because it directly triggers the on-chip OTP state machine, persistent safety cannot be authoritatively established.

---

## 3. Relationship Between 0x9B and the 0xF1/0xF4 Clocking Loop

A crucial architectural contradiction exists in the proposed sequence:
1. `fw9369_startup.c:359` reads register `0x9B` **immediately after** running the `0xF1`/`0xF4` OTP clocking loop.
2. In silicon design, register `0x9B` is a digital shadow register. The internal hardware loads the OTP foundry bits into `0x9B` when the OTP sense clock (`0xF4`) is strobed.
3. If the `0xF1`/`0xF4` clocking loop is excluded to maintain zero-risk isolation from the OTP macro, **what does `0x9B` return?**
   - In `fw9369_startup.c:363`:
     ```c
     if (code == 0)
         break;
     ```
   - If the OTP clocking loop is skipped, `0x9B` will either contain its hardware power-on reset default (`0x00`) or the un-strobed state. A read of `0x00` automatically directs the driver to select `foundry = 0` (the standard foundry, period = 200).
   - Reading `0x9B` directly is completely safe (`VERIFIED READ-ONLY`).
   - If `0x9B` already reports `0x13` or `0x00` at power-on without pulsing `0xF1`/`0xF4`, the entire hazardous `0xF1`/`0xF4` sequence can be completely eliminated.

---

## 4. Safety Determination for Stage 2C.1

Because registers `0xF1` and `0xF4` remain classified as **`UNKNOWN`**:
- **PROHIBITION:** The `0xF1`/`0xF4` OTP clocking loop **must be strictly excluded** from any initial identification test.
- **PERMITTED ACTION:** Direct read of register `0x9B` (`[ 08 F7 9B 00 00 ]`) is safe and permitted because it is a read-only SPI transaction.
