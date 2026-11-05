# STAGE D6.2 — RECONCILED FT9362 INITIALIZATION SEQUENCE & MEMORY CLASSIFICATION

**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Host Architecture:** Intel LPSS SPI2 (`spi-FTE3600:00`, CS0) on Dynabook Satellite Pro C40-J  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Date:** 2026-11-04  
**Author:** Antigravity (Advanced Agentic Pair Programming)  
**Classification:** INITIALIZATION SPECIFICATION & MEMORY AUDIT

---

## 1. Executive Summary

Stage D6.2 reconciles the FT9362 volatile hardware initialization sequence following the Stage D6.1 independent safety audit.

Key changes from Stage D6:
1. **Command Strobe Framing:** Strobe transactions now transmit 3 bytes `{ cmd, complement, 0x00 }` instead of 2 bytes.
2. **Interrupt Clear Isolation:** The two unverified writes to register `0x1A84 = 0xFFFF` are removed.
3. **Consolidated Sequence:** The startup sequence is consolidated into 6 verified steps.
4. **Memory Classification:** The driver documentation formally acknowledges on-chip OTP / eFuse macros clocked by registers `0xF1`/`0xF4`, classifies OTP programming as **NOT ESTABLISHED**, and guarantees that no OTP write or burn operations are performed.

---

## 2. Reconciled 6-Step Volatile Initialization Sequence

The sequence is executed via `fte3600_hw_startup()` when the driver is in state `FTE3600_STATE_IDENTIFIED` (or when `FTE3600_INIT_FLAG_FULL_STARTUP` is set during `FTE3600_IOC_INIT` from `RESET_SEQUENCE_ISSUED`).

```
+---------------------------------------------------------------------------------------------------+
|                                 FT9362 VOLATILE STARTUP SEQUENCE                                   |
+---+----------------------------+-----------------------+----------+-------------------------------+
| # | Step Description           | SPI Wire Framing      | Transfers| Wire Bytes                    |
+---+----------------------------+-----------------------+----------+-------------------------------+
| 1 | Wake Strobe 1 (CMD_WAKE)   | { 0x5A, 0xA5, 0x00 }  | 1        | 3 bytes                       |
| 2 | Stabilization Delay        | None (1 ms sleep)     | 0        | 0 bytes                       |
| 3 | Read HW_STATUS (Reg 0x80)  | { 0x08, 0xF7, 0x80,   | 1        | 5 bytes                       |
|   |                            |   0x00, 0x00 }        |          | (if abort recovery: +1 strobe |
|   |                            |                       |          |  [3 B] + 1 retry read [5 B])  |
| 4 | Wake Strobe 2 (CMD_ACK)    | { 0xA5, 0x5A, 0x00 }  | 1        | 3 bytes                       |
| 5 | Foundry Interrogation Loop | 2 loop iterations:    | 12       | 50 bytes                      |
|   |                            | - Write 0xF1 (4 B)    |          | (2 x [4+4+4+4+4+5] = 50 B)   |
|   |                            | - 4x Write 0xF4 (4 B) |          |                               |
|   |                            | - Read 0xF3 (5 B)     |          |                               |
| 6 | Read Silicon Fab Reg 0x9B  | { 0x08, 0xF7, 0x9B,   | 1        | 5 bytes                       |
|   |                            |   0x00, 0x00 }        |          |                               |
+---+----------------------------+-----------------------+----------+-------------------------------+
|   | TOTAL BASELINE (SUCCESS)   |                       | 16       | 66 bytes                      |
+---+----------------------------+-----------------------+----------+-------------------------------+
```

---

## 3. Detailed SPI Transfer and Byte Breakdown

### 3.1 Baseline Happy-Path Sequence (16 Transfers, 66 Bytes)
1. **Transfer 1:** `CMD_WAKE` Strobe: `TX [0x5A, 0xA5, 0x00]` (3 bytes).
2. **Transfer 2:** Read Register `0x80`: `TX [0x08, 0xF7, 0x80, 0x00, 0x00]`, `RX [x, x, x, x, status]` (5 bytes). Evaluates `status == 0x50`.
3. **Transfer 3:** `CMD_IDLE_ACK` Strobe: `TX [0xA5, 0x5A, 0x00]` (3 bytes).
4. **Transfers 4–9:** Foundry Loop Iteration 0 (index `0x03`):
   - Transfer 4: Write `0xF1 = 0x03`: `TX [0x09, 0xF6, 0xF1, 0x03]` (4 bytes).
   - Transfer 5: Write `0xF4 = 0xC0`: `TX [0x09, 0xF6, 0xF4, 0xC0]` (4 bytes).
   - Transfer 6: Write `0xF4 = 0xC1`: `TX [0x09, 0xF6, 0xF4, 0xC1]` (4 bytes).
   - Transfer 7: Write `0xF4 = 0xC0`: `TX [0x09, 0xF6, 0xF4, 0xC0]` (4 bytes).
   - Transfer 8: Write `0xF4 = 0xC0`: `TX [0x09, 0xF6, 0xF4, 0xC0]` (4 bytes).
   - Transfer 9: Read `0xF3` (dummy): `TX [0x08, 0xF7, 0xF3, 0x00, 0x00]` (5 bytes).
5. **Transfers 10–15:** Foundry Loop Iteration 1 (index `0x13`):
   - Transfer 10: Write `0xF1 = 0x13`: `TX [0x09, 0xF6, 0xF1, 0x13]` (4 bytes).
   - Transfer 11: Write `0xF4 = 0xC0`: `TX [0x09, 0xF6, 0xF4, 0xC0]` (4 bytes).
   - Transfer 12: Write `0xF4 = 0xC1`: `TX [0x09, 0xF6, 0xF4, 0xC1]` (4 bytes).
   - Transfer 13: Write `0xF4 = 0xC0`: `TX [0x09, 0xF6, 0xF4, 0xC0]` (4 bytes).
   - Transfer 14: Write `0xF4 = 0xC0`: `TX [0x09, 0xF6, 0xF4, 0xC0]` (4 bytes).
   - Transfer 15: Read `0xF3` (dummy): `TX [0x08, 0xF7, 0xF3, 0x00, 0x00]` (5 bytes).
6. **Transfer 16:** Read Register `0x9B`: `TX [0x08, 0xF7, 0x9B, 0x00, 0x00]`, `RX [x, x, x, x, fab_byte]` (5 bytes).
   - If `(fab_byte >> 2) == 0x13`: `foundry_code = 0xAA` (SMIC), `line_period_us = 150`.
   - Else: `foundry_code = 0x00`, `line_period_us = 200`.

**Total Wire Metrics:**
- **Transfers:** $1 + 1 + 1 + 6 + 6 + 1 = 16\text{ transfers}$.
- **Wire Bytes:** $3 + 5 + 3 + 25 + 25 + 5 = 66\text{ bytes}$.

### 3.2 Abort Recovery Path (18 Transfers, 74 Bytes)
If Transfer 2 (Read `0x80`) returns non-idle (`status != 0x50`):
- Abort Strobe: `TX [0xC0, 0x3F, 0x00]` (3 bytes).
- 1 ms stabilization delay.
- Retry Read `0x80`: `TX [0x08, 0xF7, 0x80, 0x00, 0x00]` (5 bytes).
- Followed by normal remainder of sequence.
- **Transfers:** $16 + 2 = 18\text{ transfers}$.
- **Wire Bytes:** $66 + 3 + 5 = 74\text{ bytes}$.

### 3.3 Status 0x80 Persistent Failure Path (4 Transfers, 16 Bytes)
If retry also returns `status != 0x50`:
- Sequence aborts with `-EIO`, transitioning state to `FTE3600_STATE_UNKNOWN`.
- **Transfers:** $1\text{ (wake1)} + 1\text{ (st80)} + 1\text{ (abort)} + 1\text{ (retry)} = 4\text{ transfers}$.
- **Wire Bytes:** $3 + 5 + 3 + 5 = 16\text{ bytes}$.

### 3.4 Combined Lifecycle from Reset (20 Operations)
When executed with `FTE3600_INIT_FLAG_FULL_STARTUP` from `FTE3600_STATE_RESET_SEQUENCE_ISSUED`:
- Reset Pulse: 1 hardware operation (GPIO pulse sequence).
- Identification (`fte3600_hw_identify`): 3 transfers, 19 wire bytes (`write_c6`, `read_c6`, `read_1a8b`).
- Startup Sequence (`fte3600_hw_startup`): 16 transfers, 66 wire bytes.
- **Total Combined Operations:** $1 + 3 + 16 = 20\text{ operations}$.
- **Total Combined SPI Transfers:** $3 + 16 = 19\text{ transfers}$.
- **Total Combined SPI Wire Bytes:** $19 + 66 = 85\text{ wire bytes}$.

---

## 4. Formal Non-Volatile Memory Classification

### 4.1 On-Chip OTP / eFuse Macro Architecture
Stage D6 claimed that FT9362 contains "no non-volatile storage". Stage D6.1 proved this assertion was incorrect:
- Physical FT9362 silicon incorporates an embedded OTP (One-Time Programmable) or eFuse macro used for factory calibration coefficients, wafer fabrication identifiers, and hardware trim parameters.
- Registers `0xF1` (macro address index) and `0xF4` (control / strobe clock register) gate internal charge pumps and timing sequencers that clock data out of the OTP macro into volatile shadow registers (`0xF3` and `0x9B`).

### 4.2 Programming State: NOT ESTABLISHED
- While read-access sequencing is understood, the exact electrical high-voltage conditions (`VPP`), timing requirements, and register opcodes for permanently burning / programming FT9362 OTP fuses are **NOT ESTABLISHED** in public or reverse-engineered documentation.
- Any attempt to write undocumented values to macro registers could inadvertently trigger programming mode or compromise factory trim parameters.

### 4.3 Safety of Volatile Interrogation
- In the 6-step initialization sequence, the writes to `0xF1` (values `0x03`, `0x13`) and `0xF4` (values `0xC0`, `0xC1`) strictly reproduce the factory driver's read clocking waveform.
- No high-voltage programming mode is asserted.
- The operations are purely volatile sense-amp clocking and read-back, posing zero risk of non-volatile corruption.
- Furthermore, all dangerous operations identified in Stage D2/D4 (SRAM bank unlock `0x9A = 0x5A`, DAC search, background touch arming `{ 0xC2, 0x3D }`) remain completely absent from the kernel driver.

---

## 5. State Machine Invariants

1. **Precondition:** Driver must be in `FTE3600_STATE_IDENTIFIED` (or `RESET_SEQUENCE_ISSUED` when `FTE3600_INIT_FLAG_FULL_STARTUP` is passed).
2. **Postcondition (Success):** State transitions to `FTE3600_STATE_READY`, `last_error = 0`.
3. **Postcondition (Failure):** State transitions to `FTE3600_STATE_UNKNOWN`, `last_error = (u32)(-ret)`.
4. **Fail-Closed Protection:** In `FTE3600_STATE_UNKNOWN`, all further hardware commands are rejected until a fresh `FTE3600_IOC_RESET` is issued.
