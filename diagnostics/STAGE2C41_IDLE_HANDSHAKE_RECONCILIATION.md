# Stage 2C.4.1 — Idle Handshake Reconciliation (0x5A / 0xA5)

**Document ID:** `STAGE2C41_IDLE_HANDSHAKE_RECONCILIATION`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Focus Commands:** `0x5A` (MCU Wake / Idle Request) and `0xA5` (Command End Delimiter)  
**Hardware Authorization:** NONE (Static Analysis & Mock Verification Only)

---

## 1. Executive Summary

During Stage 2B.5, an independent audit concluded that Sub-Stage 2C.1B (an isolated hardware test of commands `0x5A` and `0xA5` against physical silicon) must be **cancelled**. The primary concern was that halting physical hardware in an intermediate MCU idle state without proceeding to calibration or finger detection leaves silicon clocks and analog charge pumps in an un-gated, intermediate state indefinitely.

In contrast, the mock implementation in `tools/fw9369-mock/` includes the `0x5A` / `0xA5` sequence in Phase 2 of its 5-phase pipeline.

This audit reconciles this architectural choice, proving that:
1. The mock implementation correctly includes `0x5A` / `0xA5` because it models the **complete, end-to-end initialization sequence** rather than an isolated, suspended hardware test.
2. The mock tool immediately transitions from Phase 2 into Phase 3 (AFE tuning and calibration) and Phase 5 (finger detection arming).
3. The successful execution of the mock handshake proves **protocol encoding correctness**, and is **never conflated with physical hardware safety**.

---

## 2. Review of Stage 2B.5 Cancellation Findings

The Stage 2B.5 safety audit (`diagnostics/STAGE2B5_2C1B_NECESSITY_REVIEW.md` and `diagnostics/STAGE2B5_FINAL_SAFETY_GATE.md`) identified the following risks associated with running `0x5A` / `0xA5` as an *isolated test*:

1. **Intermediate State Indefinite Dwell:**
   - In an isolated test, the test script sends `0x5A`, verifies `0x80 == 0x50`, sends `0xA5`, and exits.
   - The physical sensor MCU remains in active idle mode without calibrated baseline parameters or armed low-power finger detection, resulting in uncharacterized thermal and power dissipation.
2. **Redundancy with Full Pipeline:**
   - Testing `0x5A` in isolation provides no additional silicon identification value beyond what Stage 2A already established (Chip ID `0x9362`, CRC `0x1C53`).
   - The idle handshake is strictly a prerequisite for Phase 3 calibration, not an independent identification milestone.
3. **Stage 2B.5 Verdict:**
   - Sub-Stage 2C.1B was cancelled as a standalone hardware operation.
   - The sequence was designated to be evaluated strictly within the context of the complete, continuous initialization pipeline.

---

## 3. Mock Implementation Architecture of Phase 2

In `tools/fw9369-mock/src/fw9369_state.c` (lines 146–194), Phase 2 is implemented as an integrated pipeline stage:

```c
/* =========================================================================
 * Phase 2: MCU Idle Handshake & Foundry Setup
 * ========================================================================= */
ctx->phase = FW9369_PHASE_2_IDLE;

/* Opening wake command: 0x5A 0xA5 */
ret = send_command(t, FW9369_CMD_WAKE_A, FW9369_CMD_WAKE_B);
if (ret < 0) {
    ctx->last_error = ret;
    goto fail;
}
t->delay_ms(t->context, 1);

/* Poll status 0x80 expecting 0x50 */
uint8_t status = 0;
bool idle_ok = false;
for (int i = 0; i < 5; ++i) {
    ret = read_byte(t, FW9369_REG_STATUS_80, &status);
    if (ret < 0) {
        ctx->last_error = ret;
        goto fail;
    }
    if (status == FW9369_STATUS_IDLE) {
        idle_ok = true;
        break;
    }
    /* Send exception strobe if needed */
    (void)send_command(t, FW9369_CMD_EXCEPTION_A, FW9369_CMD_EXCEPTION_B);
    t->delay_ms(t->context, 1);
}

/* Closing delimiter: 0xA5 0x5A (always sent) */
(void)send_command(t, FW9369_CMD_END_A, FW9369_CMD_END_B);

if (!idle_ok) {
    ctx->last_error = -EPROTO;
    goto fail;
}
ctx->mcu_status = status;

/* Foundry test strobes */
for (unsigned int i = 0; i < 2; ++i) {
    (void)write_byte(t, FW9369_REG_FOUNDRY_F1, i ? 0x13 : 0x03);
    (void)write_byte(t, FW9369_REG_FOUNDRY_F4, 0xC0);
    (void)write_word(t, FW9369_REG_STROBE_1A83, 0x0000);
    (void)write_word(t, FW9369_REG_STROBE_1A83, 0x0010);
    (void)write_word(t, FW9369_REG_STROBE_1A83, 0x0000);
}
```

### Key Architectural Characteristics
1. **Continuous Progression:** Phase 2 does not exit back to userspace or halt; it proceeds directly into Phase 3 (AFE tuning), Phase 4 (Image baseline), and Phase 5 (Autonomous finger detection arming).
2. **Delimiter Guarantee:** The closing delimiter `[ 0xA5, 0x5A, 0x00 ]` is unconditionally transmitted regardless of whether the idle poll succeeded on the first attempt or required exception strobes.
3. **Allowlist Enforcement:** `fw9369_is_command_allowed()` specifically permits `0x5A / 0xA5` and `0xA5 / 0x5A`, rejecting any unexpected opcodes.

---

## 4. Distinction Between Protocol Verification and Physical Safety

| Aspect | Mock Implementation Verification | Physical Hardware Reality |
| :--- | :--- | :--- |
| **Command Framing** | Verified: 3-byte packets `[ 0x5A, 0xA5, 0x00 ]` and `[ 0xA5, 0x5A, 0x00 ]` match reference driver. | Physical SPI framing requires identical 3-byte sequences at $\le 1$ MHz. |
| **Status Polling** | Verified: Register `0x80` byte read packet `[ 0x08, 0xF7, 0x80, 0x00, 0x00 ]` decodes byte from index 4. | Physical silicon asserts `0x50` on MISO byte 4 when MCU idle task is ready. |
| **Silicon Safety** | Verified: In-memory simulation transitions state without electrical consequence. | **Unverified on physical hardware.** Halting in Phase 2 could overheat or lock MCU. |
| **Gate Meaning** | Confirms userspace state machine can transition through Phase 2 without logic bugs. | **Does NOT authorize running Phase 2 in isolation on hardware.** |

---

## 5. Audit Conclusion

1. **Reconciliation Complete:** The inclusion of `0x5A` / `0xA5` in the mock pipeline is correct and necessary to model the full reference driver flow, whereas the cancellation of Sub-Stage 2C.1B on physical hardware was correct and necessary to avoid unsafe intermediate silicon states.
2. **Explicit Safety Boundary Maintained:** The mock tool's passing test results for Phase 2 prove protocol encoding compliance only; they do not imply physical hardware safety for standalone execution.
3. Idle handshake reconciliation is **ACCEPTED**.
