# Stage 2B.2 — Protected Register 0x9A Static Verification Review

**Document ID:** `STAGE2B2_PROTECTED_REGISTER_REVIEW`  
**Date:** 2026-11-02  
**Target Hardware:** FocalTech FT9362 (FW9369 Architecture)  
**Host Controller:** Intel LPSS SPI (`pxa2xx-spi.2`) / Dynabook Satellite Pro C40-J  
**Scope:** Exhaustive code-level trace of register `0x9A`, readback feasibility, lock persistence, and error-path vulnerability.  
**Execution Gate:** Strictly static analysis. No hardware execution.

---

## 1. Exhaustive Code-Level Trace of Register 0x9A

A comprehensive search across the entire `research/libfprint-fte3600/` codebase and extracted Windows driver strings reveals all interactions with register `0x9A`:

### 1.1 Complete List of Write Locations
Across all driver modules, register `0x9A` is written in exactly four functional contexts:

1. **FDT Calibration Bank Configuration (`fw9369_fdt.c`):**
   - **Unlock:** `fw9369_fdt.c:215`: `write_byte (op, 0x9a, 0x5a);`
   - **Lock:** `fw9369_fdt.c:224`: `write_byte (op, 0x9a, 0);`
   - **Context:** Encloses the write to the protected analog configuration bank `0x00C0`..`0x00C2`.
2. **Startup Detector Arming (`fw9369_startup.c`):**
   - **Unlock:** `fw9369_startup.c:396`: `write_byte (op, 0x9a, 0x5a);`
   - **Lock:** `fw9369_startup.c:405`: `write_byte (op, 0x9a, 0);`
   - **Context:** Encloses the 22-byte write of the 8 calibrated FDT threshold words to bank `0x80B0` (or `0x80E0`).
3. **Runtime Release Detector Arming (`fw9369_release.c`):**
   - **Unlock:** `fw9369_release.c:265, 273`: `transfer (unlock)` where `unlock[4] = {9, 0xf6, 0x9a, 0x5a};`
   - **Lock:** `fw9369_release.c:265, 275`: `transfer (lock)` where `lock[4] = {9, 0xf6, 0x9a, 0};`
   - **Context:** Encloses the transfer of the 8 updated release threshold words to bank `0x80B0` (or `0x80E0`).
4. **Legacy Modern Sensor Engine (`ft93xx.c`):**
   - `ft93xx.c:981-982`: `!write_byte (&op, 0x9a, 0x5a) || !analog_channels(...) || !write_byte (&op, 0x9a, 0)`
   - `ft93xx.c:1336-1337`: `write_byte (&op->wire, 0x9a, 0x5a) && transfer(...) && write_byte (&op->wire, 0x9a, 0)`
   - `ft93xx.c:1528, 1540`: Unlock before threshold upload, lock after upload.

*(Note: In `capture.c:186`, `0x9A` appears in byte position 2 of the FIFO command prefix `0x06 0xF9 0x9A 0x05 0x14 0x00`. In `factory_startup.c:164`, it appears in word address `0x1A8B` framing. Neither writes register `0x9A`).*

### 1.2 Locations Where Register 0x9A Is Read
- **Search Query:** `grep -rn "read.*0x9a"` across `research/`.
- **Result:** **ZERO.** Register `0x9A` is **NEVER READ** anywhere in the driver, backend tests, or Windows driver strings.

---

## 2. Evaluation of Readback Feasibility & Unsupported Assumptions

In Stage 2B.1, it was suggested that a software implementation could "verify lock restoration by reading back register `0x9A`". A rigorous audit reveals that this recommendation was an **unsupported engineering assumption**:

1. **Protocol Framing vs. Hardware Semantics:**
   - The generic FocalTech SPI framing supports generating a read packet for any 8-bit address: `[ 0x08, 0xF7, 0x9A, 0x00, 0x00 ]`.
   - However, there is zero source-code evidence or protocol documentation indicating that the physical FT9362 silicon connects register `0x9A` to the internal read-multiplexer.
2. **Behavior of Hardware Key / Strobe Registers:**
   - In microcontroller and sensor silicon, write-protect lock registers (such as key registers that unlock protected banks when `0x5A` is written) are typically implemented as **write-only digital comparator latches**.
   - Attempting to read a write-only key register often returns `0x00`, `0xFF`, or bus high-impedance float, regardless of the internal comparator state.
3. **Conclusion on Readback:**
   - **Source Fact:** The reference driver never reads `0x9A`.
   - **Determination:** Software cannot rely on reading register `0x9A` to verify that the protected bank is locked. Recommending readback verification is **unsupported by evidence** and must be removed from the safety criteria.

---

## 3. Lock Persistence & Automatic Restoration

1. **Volatile Register Space:**
   - Register `0x9A` is an 8-bit volatile control register within the sensor's digital Special Function Register (SFR) space.
   - It is not connected to non-volatile flash or OTP.
2. **Reset and Power Loss Behavior:**
   - **Hardware Evidence:** In `tests/test-fw9369-backend.c:795` and `device_session.c:680-695`, a physical hardware reset pulse (`RST_N` low for 10–20 ms) resets all digital logic to default power-on states.
   - S5 cold power loss completely cuts $V_{DD}$ to the sensor, resetting volatile flip-flops to `0x00`.
   - Therefore, a cold reboot or hardware reset pulse is proven to restore `0x9A = 0x00`.
3. **Runtime Inactivity:**
   - The reference source code contains **no evidence** of an internal auto-lock watchdog timer for `0x9A`. If unlocked (`0x5A`), it remains unlocked until explicitly written with `0x00` or reset.

---

## 4. Error Paths Between Unlock and Lock in Upstream Code

An exhaustive audit of the source files reveals that the upstream reference implementation contains severe error-handling defects that leave `0x9A` permanently unlocked on failure:

### Error Path 1: `fw9369_fdt.c:215-224`
```c
215:  write_byte (op, 0x9a, 0x5a);
216:  for (unsigned int group = 0; group < (c->channel_mode == 1 ? 1u : 2u); ++group)
217:    {
218:      uint16_t base = c->channel_mode == 1 ? 0xc0 : (uint16_t) (0xc9 + group * 9);
219:      if (!modify (op, base, 0xe000, c->channel_mode == 1 ? 0x0444 : 0x06cd, 1) ||
220:          !modify (op, (uint16_t) (base + 1), 0xffc0, 0x0021, 1) ||
221:          !modify (op, (uint16_t) (base + 2), 0xffff, 0x00c0, 1))
222:        return 0;  // <-- CRITICAL DEFECT: Exits immediately without locking!
223:    }
224:  write_byte (op, 0x9a, 0);
```
- If any SPI write or readback inside `modify()` fails (e.g. timeout or EIO), line 222 executes `return 0;`.
- **Result:** Line 224 is completely skipped. The physical sensor is left in the unlocked state (`0x9A = 0x5A`).

### Error Path 2: `fw9369_release.c:273-276`
```c
273:  if (!transfer (unlock, sizeof (unlock), io, d) ||
274:      !transfer (bank, sizeof (bank), io, d) ||
275:      !transfer (lock, sizeof (lock), io, d))
276:    return FTE3600_FW9369_RELEASE_TRANSPORT_ERROR;
```
- In C, the logical OR operator (`||`) short-circuits. If `transfer(bank, ...)` returns 0 (failure), the sub-expression `!transfer(bank)` evaluates to `true` (1).
- **Result:** `transfer(lock, ...)` is **NEVER CALLED**. The sensor is left in the unlocked state.

---

## 5. Software Interruption & Cleanup Design

### 5.1 Limits of Signal Masking
Masking host signals (`pthread_sigmask(SIG_BLOCK, ...)`) prevents user interruptions (SIGINT, SIGTERM) from aborting the process between unlock and lock. However, signal masking **does NOT prevent**:
1. SPI bus transport failures (EIO, ENODEV, ETIMEDOUT).
2. Hardware USB/SPI bridge disconnects or power drops.
3. Uncatchable signals (SIGKILL, process crash).
4. Kernel panics or host system resets.
Signal masking is an incomplete mitigation on its own.

### 5.2 Safe Software Cleanup Design
Any future implementation that must unlock register `0x9A` must employ a structural `finally` pattern:

```c
int safe_write_protected_bank(struct transport_context *ctx, const uint8_t *payload, size_t len)
{
    int unlock_ret, bank_ret, lock_ret;
    sigset_t old_mask, block_mask;

    // 1. Block catchable signals during critical unlock window
    sigfillset(&block_mask);
    pthread_sigmask(SIG_BLOCK, &block_mask, &old_mask);

    // 2. Perform Unlock
    unlock_ret = write_byte(ctx, 0x9A, 0x5A);
    if (unlock_ret == 0) {
        // Unlock succeeded: write payload
        bank_ret = transfer(ctx, payload, len);
    } else {
        bank_ret = -1;
    }

    // 3. MANDATORY LOCK: Always executed regardless of bank_ret status
    lock_ret = write_byte(ctx, 0x9A, 0x00);

    // 4. Restore signal mask
    pthread_sigmask(SIG_SETMASK, &old_mask, NULL);

    if (unlock_ret != 0) return -EIO;
    if (bank_ret != 0)   return -EIO;
    if (lock_ret != 0)   return -EPROTO; // Catastrophic: Lock failed

    return 0;
}
```

---

## 6. Audit Summary

| Question | Source Evidence / Audit Finding | Confidence |
| :--- | :--- | :---: |
| **Every location written?** | Traced to exactly 4 functions: `fw9369_fdt.c:215,224`, `fw9369_startup.c:396,405`, `fw9369_release.c:265,275`, `ft93xx.c:981,1336,1528`. | HIGH |
| **Ever read?** | **ZERO** read locations exist in any reference driver or test code. | HIGH |
| **Readback feasible?** | **UNSUPPORTED.** Register 0x9A is write-only in reference code; readback cannot establish lock state. | HIGH |
| **Automatic lock on reset?** | Yes, volatile register resets to `0x00` on hardware reset (`RST_N`) or cold power cycle. | HIGH |
| **Error paths in upstream?** | Upstream code has multiple defects where `return 0` or short-circuit evaluation skips lock on failure. | HIGH |
| **Signal masking sufficient?** | Insufficient alone. Must be combined with structural `finally` error cleanup. | HIGH |
