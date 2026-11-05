# STAGE D6.2 — FINAL SOURCE AUDIT AND COMPLIANCE REVIEW

**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Host Architecture:** Intel LPSS SPI2 (`spi-FTE3600:00`, CS0) on Dynabook Satellite Pro C40-J  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Date:** 2026-11-04  
**Author:** Antigravity (Advanced Agentic Pair Programming)  
**Classification:** FINAL AUDIT & COMPLIANCE REVIEW

---

## 1. Audit Scope & Objectives

This audit independently reviews all source changes introduced in Stage D6.2:
- Remediation of command strobe wire framing from 2 bytes to 3 bytes (`{ cmd, complement, 0x00 }`).
- Removal of unverified interrupt-clear operations (`0x1A84 = 0xFFFF`) from baseline startup.
- Formal non-volatile memory classification acknowledging on-chip OTP / eFuse macros and setting programming state to **NOT ESTABLISHED**.
- Reconciled 6-step volatile initialization sequence (16 transfers, 66 wire bytes).
- Userspace test suite verification (`50 / 50` passed).
- Preservation of Option 1: Live module loaded and bound, transfers = 3, bytes = 19.

---

## 2. Line-by-Line Code Review

### 2.1 `kernel/fte3600.c`
1. **Command Strobe Framing Helper (`fte3600_spi_cmd_strobe`):**
   ```c
   static int fte3600_spi_cmd_strobe(struct fte3600_data *data, u8 cmd, u8 complement)
   {
       data->tx_buf[0] = cmd;
       data->tx_buf[1] = complement;
       data->tx_buf[2] = 0x00;
       return fte3600_spi_sync_transfer(data, data->tx_buf, NULL, 3);
   }
   ```
   *Audit Finding:* Transmits exactly 3 bytes with `tx_buf[2] = 0x00`. Meets vendor reference implementation (`fw9369_startup.c`, `capture.c`, `ft93xx.c`) and mock assertion requirements.
2. **Unused Warning Prevention:**
   `fte3600_spi_write_reg16` and `fte3600_spi_read_reg16` are decorated with `__maybe_unused`.
   *Audit Finding:* Compiles cleanly with 0 compiler warnings under `-Wall -Wextra -Werror`.
3. **Consolidated Startup Sequence (`fte3600_hw_startup`):**
   - Step 1: Wake strobe 1 `{ 0x5A, 0xA5, 0x00 }` (3 bytes)
   - Step 2: 1 ms stabilization sleep
   - Step 3: Read status `0x80`, verify idle `0x50` (with abort strobe `{ 0xC0, 0x3F, 0x00 }` + retry)
   - Step 4: Wake strobe 2 `{ 0xA5, 0x5A, 0x00 }` (3 bytes)
   - Step 5: Foundry interrogation loop (writes to `0xF1`, `0xF4`, read `0xF3`)
   - Step 6: Read silicon fab register `0x9B`
   *Audit Finding:* Both occurrences of `0x1A84 = 0xFFFF` have been completely removed. Exactly 16 SPI transfers are executed in the baseline sequence.
4. **State Machine Integrity:**
   - Precondition: `data->state == FTE3600_STATE_IDENTIFIED` strictly enforced.
   - Success: `data->state = FTE3600_STATE_READY`, `last_error = 0`.
   - Failure: `data->state = FTE3600_STATE_UNKNOWN`, `last_error = -ret`.
   - Concurrency: `mutex_lock(&data->lock)` held throughout execution.
   - Removal Safety: `atomic_read(&data->offline) || !data->spi` checked under mutex.

### 2.2 `kernel/fte3600.h`
1. **ABI Compatibility:**
   - Struct sizes remain strictly 32 bytes (`static_assert(sizeof(...) == 32)`).
   - Internal states `9..13` are shielded behind `fte3600_state_to_uapi()`.
   - Public UAPI states strictly bounded to `[0..8]`.
   - `FTE3600_FLAG_OFFLINE` remains bit 0 (`1U << 0`).
2. **Initialization Flags:**
   - `FTE3600_INIT_FLAG_IDENTIFY_ONLY = 0x00000000U`
   - `FTE3600_INIT_FLAG_FULL_STARTUP = 0x00000001U`

### 2.3 `kernel/fte3600-test.c`
1. `mock_spi_startup_profile_t` updated to remove `irq_clear` fields.
2. `sim_driver_hw_startup()` updated to remove both IRQ clear transfers and align with 16 baseline transfers.
3. Test Suite 8:
   - Scenario 1 & 2: Assert baseline `hardware_access_count == 16`.
   - Scenario 3: Assert abort recovery `hardware_access_count == 18`.
   - Scenario 4: Assert status 80 persistent failure `hardware_access_count == 4`.
   - Scenario 5: Command strobe 3-byte wire framing test (`{cmd, ~cmd, 0x00}`).
   - Scenario 6: Wake 1 failure `hardware_access_count == 1`.
   - Scenario 7: Foundry interrogation failure `hardware_access_count == 4`.
   - Scenario 8: Zero `0x1A84` writes in baseline startup (asserts 16 baseline transfers, zero `0x1A84` writes).
   - Scenario 10: Combined reset + identify + full startup asserts `hardware_access_count == 20`.

---

## 3. Legacy Tools & Safety Gates Audit

1. **`bin/fw9369-hw-tool` Integrity:**
   The hardware safety gate in `bin/fw9369-hw-tool` remains untouched and functional.
2. **Hardware Scripts:**
   No scripts were executed against physical hardware during this stage.

---

## 4. Hardware Safety Invariant Verification

| Invariant Parameter | Expected Baseline | Observed Value | Status |
| :--- | :--- | :--- | :--- |
| Kernel Module State | `fte3600 20480 0 - Live` | `fte3600 20480 0 - Live` | PASS |
| Device Node Major, Minor | `511, 0` | `511, 0` | PASS |
| Device Node File | `/dev/fte3600` | `/dev/fte3600` | PASS |
| SPI Transfer Count | `3` | `3` | PASS (Untouched) |
| SPI Byte Count | `19` | `19` | PASS (Untouched) |
| Driver Internal State | `8 (IDENTIFIED)` | `8 (IDENTIFIED)` | PASS |
| Prohibited Operations | Zero | Zero | PASS |

---

## 5. Compliance Verdict

All remediation requirements of Stage D6.2 have been satisfied in full without hardware communication or ABI regressions:
- Confirmed command-strobe framing defect: **REMEDIATED (3 BYTES)**
- Unverified interrupt-clear operations: **ISOLATED & REMOVED (0 WRITES)**
- Non-volatile memory classification: **FORMALLY CORRECTED (NOT ESTABLISHED)**
- Volatile initialization sequence: **RECONCILED (6 STEPS, 16 TRANSFERS, 66 BYTES)**
- Userspace test suite: **50 / 50 PASSED (100%)**
- Hardware safety: **100% PRESERVED UNDER OPTION 1**

**Classification:** `PASS — MINIMAL SOURCE REMEDIATION COMPLETE`
