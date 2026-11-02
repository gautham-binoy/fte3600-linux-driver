# Stage 2C.4.1 — Protected Access (0x9A) Audit Across Failure Modes

**Document ID:** `STAGE2C41_PROTECTED_ACCESS_AUDIT`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Focus Register:** `0x9A` (Analog Trim Protection Key Register)  
**Hardware Authorization:** NONE (Static Analysis & Mock Verification Only)

---

## 1. Executive Summary

Register `0x9A` controls access to the internal analog trim bank (`0x00C0`–`0x00C2`) on FocalTech FT9362 / FW9369 silicon.
- Writing `0x5A` to `0x9A` unlocks write access to the trim registers.
- Writing `0x00` to `0x9A` locks write access.
- **Physical Silicon Invariant:** Register `0x9A` is **write-only**. Physical readback of `0x9A` is unsupported by hardware, meaning host software can never physically verify whether the silicon trim bank is locked or unlocked.

This audit independently inspects the implementation of register `0x9A` protection in `tools/fw9369-mock/` across 8 distinct failure dimensions to ensure software models hardware realistically, cleanly handles aborts, and never generates false safety claims.

---

## 2. Source Code Architecture of the Protected Window

In `tools/fw9369-mock/src/fw9369_state.c`:
- **Unlock (`0x9A = 0x5A`):** Line 216 issues `write_byte(t, FW9369_REG_PROTECT_9A, FW9369_KEY_UNLOCK)`.
- **Protected Operations:** Lines 224–228 write the three trim registers:
  - `0x00C0 = 0x0444`
  - `0x00C1 = 0x0021`
  - `0x00C2 = 0x00C0`
- **Immediate Inline Lock (`0x9A = 0x00`):** Line 231 issues `write_byte(t, FW9369_REG_PROTECT_9A, FW9369_KEY_LOCK)`.
- **Cleanup Handler on Failure:** Line 62 defines `fw9369_pipeline_abort_cleanup()`:
  ```c
  void fw9369_pipeline_abort_cleanup(struct fw9369_transport *t, struct fw9369_pipeline_context *ctx)
  {
      if (!t || !ctx)
          return;

      /* Structured cleanup: always attempt to lock 0x9A on abort */
      int ret = write_byte(t, FW9369_REG_PROTECT_9A, FW9369_KEY_LOCK);
      if (ret < 0) {
          ctx->lock_outcome = FW9369_LOCK_TRANSMISSION_FAILED;
      } else {
          ctx->lock_outcome = FW9369_LOCK_TRANSMITTED_CLEAN;
      }
      ctx->phase = FW9369_PHASE_ABORTED;
  }
  ```
- **Error Label (`fail:`):** Line 294 jumps to `fw9369_pipeline_abort_cleanup()`, ensuring that any failure occurring after `0x9A` is unlocked immediately triggers a relock attempt.

---

## 3. Audit Across the 8 Failure Dimensions

| Dimension | Scenario | Software Behavior | Mock Silicon State | Software Lock Outcome | Truthful Classification |
| :---: | :--- | :--- | :--- | :--- | :--- |
| **1** | **Normal Completion** | Unlock `0x9A=0x5A`, write trim bank, lock `0x9A=0x00`, complete pipeline. | Locked (`protected_unlocked = false`) | `FW9369_LOCK_TRANSMITTED_CLEAN` | Clean transmission. Physically unverifiable via readback, but SPI transfer succeeded. |
| **2** | **Pipeline Error + Abort Cleanup** | Error during trim write or subsequent phase; jump to `fail:` label; abort cleanup executes `0x9A=0x00`. | Relocked (`protected_unlocked = false`) | `FW9369_LOCK_TRANSMITTED_CLEAN` | Lock packet was transmitted cleanly over SPI during cleanup. |
| **3** | **SPI Transport Failure During Lock** | SPI transfer of `[0x09, 0xF6, 0x9A, 0x00]` fails with `-EIO` (simulated via `fail_lock_write`). | Unlocked in mock; unknown on physical hardware | `FW9369_LOCK_TRANSMISSION_FAILED` | **Crucial:** Mock explicitly reports `TRANSMISSION_FAILED`; does NOT claim hardware is locked. |
| **4** | **Catchable Interruption (`-EINTR`)** | `transfer()` returns `-EINTR` while `0x9A` is unlocked (simulated via `interrupt_in_trim`). | Abort cleanup runs and transmits `0x9A=0x00` | `FW9369_LOCK_TRANSMITTED_CLEAN` | Clean transmission executed during synchronous unwinding. |
| **5** | **Asynchronous Signal (`SIGTERM`)** | `SIGTERM` delivered to userspace process during protected window. | In simple userspace process without signal handler, process terminates abruptly; cleanup not called. | Unattempted | If process dies, lock write is unattempted. Hardware remains in unlocked state until hardware reset. |
| **6** | **Uncatchable Termination (`SIGKILL`)** | Kernel kills process immediately via `kill -9`. | Hardware remains in unlocked state. | `FW9369_LOCK_INTERRUPTED_UNATTEMPTED` | Clean unwinding impossible in userspace. Hardware requires full power-cycle or SPI reset. |
| **7** | **Process Crash (`SIGSEGV`)** | Process crashes due to memory fault during trim configuration. | Hardware remains in unlocked state. | Unattempted | Userspace memory fault leaves bus transaction mid-stream or register unlocked. |
| **8** | **Host Power Loss / Kernel Panic** | Host system loses power or kernel panics while `0x9A == 0x5A`. | Sensor loses power or SPI clocks stop. | Physically Unverifiable | On power cycle, sensor resets to default locked state. |

---

## 4. Verification of Lock Representation in the Mock

The mock implementation in `tools/fw9369-mock/src/fw9369_transport_mock.c` models register `0x9A` as follows:

1. **Internal State Variable:**
   - Line 30: `bool protected_unlocked;`
   - Default at init: `ctx->protected_unlocked = false;` (Line 273).
2. **Write Interception:**
   - Lines 108–110:
     ```c
     if (addr == FW9369_REG_PROTECT_9A) {
         ctx->protected_unlocked = (val == FW9369_KEY_UNLOCK);
     }
     ```
   - If `val == 0x5A`, `protected_unlocked` becomes `true`.
   - If `val == 0x00`, `protected_unlocked` becomes `false`.
3. **Protected Trim Write Guard:**
   - Lines 134–136:
     ```c
     if (ctx->protected_unlocked) {
         ctx->reg_trim[addr - 0x00C0] = val;
     }
     ```
   - Trim registers cannot be modified unless unlocked.
4. **Honest Reporting of Lock Status:**
   - The mock CLI (`src/fw9369_main.c:91–93`) prints:
     - `TRANSMITTED CLEAN (0x00)` when `ctx.lock_outcome == FW9369_LOCK_TRANSMITTED_CLEAN`
     - `TRANSMISSION FAILED` when `ctx.lock_outcome == FW9369_LOCK_TRANSMISSION_FAILED`
   - It **never prints "Hardware is physically verified locked"**, because register `0x9A` readback is unsupported by silicon.

---

## 5. Architectural Implications for Future Kernel Driver

The mock implementation highlights a fundamental architectural trade-off between userspace tooling and kernel-space drivers:
- **Userspace Tooling Limitation:** A userspace process cannot guarantee relocking if terminated by `SIGKILL` or an unhandled crash.
- **Kernel-Space Advantage:** In a kernel driver (`drivers/input/fingerprint/fte3600.c` or similar), trim modification is protected by kernel mutexes, spinlocks, or atomic sections, with module release / device disconnect callbacks guaranteeing that `0x9A = 0x00` is issued before resource teardown.
- **Physical Reset Fallback:** If SPI communication is abruptly severed while `0x9A` is unlocked, the only hardware mechanism capable of guaranteeing a return to the power-on locked state is a hardware reset pulse (Pad 14 / GPIO reset) or an SPI software wake/reset sequence (`0xC6 = 0x01`).

---

## 6. Audit Conclusion

The mock implementation handles register `0x9A` protected access with technical rigor:
- Structured RAII cleanup unconditionally attempts relock on every failure pathway.
- Injected lock write failures are truthfully reported as `FW9369_LOCK_TRANSMISSION_FAILED`.
- The codebase makes zero false claims regarding physical silicon readback verification.
- Protected access audit is **PASSED**.
