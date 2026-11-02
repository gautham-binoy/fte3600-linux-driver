# Stage 2C.5B.2 — Command Sequence Safety and State Machine Integrity Audit

**Document ID:** `STAGE2C5B2_COMMAND_SEQUENCE_SAFETY`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Audit Scope:** Verification of MCU command packet safety, pairing invariants, state transitions, and prohibited command prevention.  
**Hardware Authorization:** NONE (Software & Static Analysis Only).

---

## 1. Executive Summary

This audit evaluates the safety of sensor command sequences within `tools/fw9369-mock/src/fw9369_state.c`, `fw9369_protocol.c`, and the transport validation rules in `fw9369_transport_hw.c`.

The FocalTech FW9369 MCU command decoder requires precise multi-byte framing and structured sequence delimiters. Executing un-paired, malformed, or isolated commands (such as a standalone `0x5A` wake without a terminating `0xA5 0x5A` end delimiter) strands the MCU state machine in an undefined state.

The audit confirms:
1. Exactly four command pairs are authorized by `fw9369_is_command_allowed()`.
2. Every command packet is strictly validated as a 3-byte sequence `[ c1, c2, 0x00 ]`. Packets with incorrect lengths, bad complement bytes, or non-zero trailing bytes are rejected with `-EPERM`.
3. Standalone `0x5A` or `0xA5` single-byte operations are prohibited and impossible to construct under the transport framing rules.
4. The pipeline in `fw9369_state.c` guarantees that Phase 2 MCU idle handshaking is terminated with delimiter `0xA5 0x5A`, and that the `0x9A` analog trim block is unconditionally terminated with lock command `0x9A = 0x00`.

---

## 2. Authorized Command Set & Framing Specification

In `src/fw9369_protocol.c:144–155`:

```c
bool fw9369_is_command_allowed(uint8_t c1, uint8_t c2)
{
    if (c1 == FW9369_CMD_WAKE_A && c2 == FW9369_CMD_WAKE_B)
        return true;
    if (c1 == FW9369_CMD_END_A && c2 == FW9369_CMD_END_B)
        return true;
    if (c1 == FW9369_CMD_EXCEPTION_A && c2 == FW9369_CMD_EXCEPTION_B)
        return true;
    if (c1 == FW9369_CMD_ARM_A && c2 == FW9369_CMD_ARM_B)
        return true;
    return false;
}
```

### 2.1 The Four Approved Command Packets
| Command Name | Byte 0 (`c1`) | Byte 1 (`c2`) | Byte 2 (Padding) | Total Wire Bytes | Purpose & Invariant |
| :--- | :---: | :---: | :---: | :---: | :--- |
| **MCU Wake** | `0x5A` | `0xA5` | `0x00` | 3 | Transitions MCU from sleep/idle to active command mode |
| **MCU End** | `0xA5` | `0x5A` | `0x00` | 3 | Terminates MCU command mode; mandatory delimiter after Phase 2 |
| **Exception Strobe** | `0xC0` | `0x3F` | `0x00` | 3 | Dispatched only if MCU status register `0x80 != 0x50` |
| **Autonomous Arm** | `0xC2` | `0x3D` | `0x00` | 3 | Final Phase 5 command; arms autonomous hardware finger detection |

### 2.2 Wire Framing Rules Enforced by Transport
At the transport boundary (`src/fw9369_transport_hw.c`), command packets are validated against four criteria:
1. `len == 3` (exact length check).
2. `tx[2] == 0x00` (mandatory trailing null byte).
3. `c2 == (c1 ^ 0xFF)` (strict mathematical bit-inversion complement check).
4. `fw9369_is_command_allowed(c1, c2) == true`.

Any packet violating any of these four criteria fails closed and returns `-EPERM` without reaching `ioctl()`.

---

## 3. Command Pairing and State Machine Invariants

### 3.1 Prohibition of Isolated Standalone `0x5A` / `0xA5`
In historical stages (Stage 2B.4/2B.5), isolating `0x5A` or `0xA5` as an independent hardware probe was considered and rejected.
- **Architectural Risk:** In the FW9369 silicon state machine, issuing `0x5A 0xA5` signals the MCU to prepare for initialization parameters. If the host terminates communication without issuing `0xA5 0x5A`, the MCU command parser remains listening and will treat subsequent register reads/writes as malformed command payloads.
- **Enforcement:** In `fw9369_state.c`, Phase 2 executes the complete, atomic sequence:
  ```c
  /* 1. Send Wake Request: [ 0x5A, 0xA5, 0x00 ] */
  ret = fw9369_encode_command(FW9369_CMD_WAKE_A, FW9369_CMD_WAKE_B, cmd, sizeof(cmd));
  ret = t->transfer(t->context, cmd, NULL, 3);
  t->delay_ms(t->context, 1);

  /* 2. Poll Status 0x80 for Idle 0x50 */
  ...
  /* 3. Send End Delimiter: [ 0xA5, 0x5A, 0x00 ] */
  ret = fw9369_encode_command(FW9369_CMD_END_A, FW9369_CMD_END_B, cmd, sizeof(cmd));
  ret = t->transfer(t->context, cmd, NULL, 3);
  ```
- Delimiter `0xA5 0x5A` is unconditionally submitted prior to transitioning to Phase 3.

### 3.2 Structured Protected Block: `0x9A` Lock Guarantee
The analog trim bank (`0x00C0..0x00C2`) requires unlocking via `write_byte(0x9A, 0x5A)`.
- In `fw9369_state.c:213–245`, trim configuration is executed inside a structured RAII-like block.
- On both success and error, `write_byte(0x9A, 0x00)` is submitted.
- In addition, `fw9369_pipeline_abort_cleanup()` provides a secondary safety wrapper that unconditionally submits `0x9A = 0x00` if any subsequent phase encounters an error.

---

## 4. Prohibition of New or Undocumented Commands

The driver strictly maintains silicon safety by prohibiting:
1. **Undocumented Command Opcodes:** Any command opcode pair other than the four listed above is rejected.
2. **Dynamic Command Synthesis:** Command packet encoding is hardcoded into helper functions; no userspace CLI option or dynamic buffer can generate arbitrary command pairs.
3. **Firmware Upgrade Commands:** Bootloader entry and flash erase commands are completely absent from the codebase.

---

## 5. Audit Assessment Summary

| Sequencing Safety Invariant | Implementation Evidence | Verdict |
| :--- | :--- | :---: |
| **Exact 3-Byte Framing** | `len == 3 && tx[2] == 0x00` checked in transport | **PASSED** |
| **Bitwise Inversion Complement**| All 4 commands verify $C_2 = C_1 \oplus \text{0xFF}$ | **PASSED** |
| **Standalone Command Prevention**| Single-byte commands rejected with `-EPERM` | **PASSED** |
| **Wake / End Delimiter Pairing**| `0x5A 0xA5` strictly terminated with `0xA5 0x5A` | **PASSED** |
| **RAII Trim Relock Guarantee**| `0x9A = 0x00` executed unconditionally on success/abort | **PASSED** |
| **Prohibited Command Rejection** | Undocumented commands rejected before ioctl | **PASSED** |

**Conclusion:** Command sequencing and framing adhere to strict protocol integrity rules, preventing sensor state machine desynchronization.
