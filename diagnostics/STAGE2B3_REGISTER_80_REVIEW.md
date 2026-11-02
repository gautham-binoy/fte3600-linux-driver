# Stage 2B.3 — Register 0x80 Safety & Protocol Lifecycle Review

**Document ID:** `STAGE2B3_REGISTER_80_REVIEW`  
**Date:** 2026-11-02  
**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FT9362 / FW9369  
**Status:** COMPLETE — STATIC SOURCE CODE AUDIT  
**Hardware Execution Authorization:** NONE (Static Analysis Only)

---

## 1. Executive Summary

This investigation performs an exhaustive static code audit of register `0x80` across the entire reference driver codebase (`research/libfprint-fte3600/` and vendor implementations). 

Register `0x80` is commonly assumed to be an ordinary read-only status register. However, source code analysis reveals that in the FocalTech FW9369/FT93xx architecture, register `0x80` is the **Microcontroller (MCU) Operational State & Handshake Register**. It is accessed exclusively in response to explicit MCU command strobes (`0x5A 0xA5`, `0xC4`, `0xC8`) and is **never read** on uninitialized silicon or prior to issuing an MCU command.

Because reading register `0x80` prior to MCU initialization has no precedent in reference code, has undocumented pre-initialization behavior, and provides zero chip identification value, it is classified as **`UNKNOWN — EXCLUDE FROM 2C.1A`**.

---

## 2. Exhaustive Access Trace: Register 0x80

Every read and write of register `0x80` across the reference codebase was traced.

### 2.1. Writes to Register 0x80
- **Total Writes Found:** **0**
- Register `0x80` is never written via SPI in any driver source file. (Note: `ft93xx.c:618` writes the *value* `0x80` to register `0xF4`, which is an OTP clock strobe, not an access to register `0x80`).

### 2.2. Reads of Register 0x80

| Source File & Line | Calling Function | Preceding Operation | Expected Value | Purpose / Semantics |
| :--- | :--- | :--- | :---: | :--- |
| `fw9369_startup.c:108` | `idle()` | `command(op, 0x5a, 0xa5)` + 1 ms delay | `0x50` | MCU Idle State Verification |
| `fw9369_fdt.c:74` | `idle()` | `command(op, 0x5a, 0xa5)` + 1 ms delay | `0x50` | MCU Idle State Verification |
| `fw9369_release.c:456` | `idle()` | `transfer(wake, 3)` (`0x5a 0xa5 0`) + 1 ms | `0x50` | MCU Idle State Verification |
| `capture.c:223` | `configuration_prefix()` | `command(op, 0x5a, 0xa5)` + 1 ms delay | `0x50` | MCU Idle State Verification |
| `capture.c:270` | `frames()` | `command(&op, 0xc4, 0x3b)` + 1 ms delay | `0x54` | MCU Image Capture Mode Poll |
| `factory_startup.c:152`| `idle()` | `command(op, 0x5a, 0xa5)` + 1 ms delay | `0x50` | MCU Idle State Verification |
| `ft93xx.c:266, 273` | `enter_idle()` | `command(op, 0x5a)` + 1 ms delay | `0x50` | MCU Idle State Poll (up to 5 retries) |
| `ft93xx.c:470` | `capture()` | `command(&op, 0xc4)` + 1 ms delay | `0x54` | MCU Image Capture Mode Poll |
| `ft93xx.c:1548` | `navigation()` | `command(&op->wire, 0xc8)` + 1 ms delay | `0x58` | MCU Navigation Mode Poll |

---

## 3. Operational Role & Lifecycle Analysis

### 3.1. Cold & Warm Startup Lifecycle
In `fw9369_startup.c:330-345`, the cold startup sequence is:
```c
/* fw9369_startup.c:330-345 */
write_byte (op, 0xc6, 1);
wait_ms (op, 4);
if (read_byte (op, 0xc6) == 1)
  break;

if (!read_word (op, 0x1a8b, &id))
  return FTE3600_FW9369_STARTUP_WORD_READ_ERROR;

reset_cached (op->s);
write_word (op, 0x1a84, 0xffff);
observe (op, FTE3600_FW9369_PHASE_WAKE, 1, FTE3600_FW9369_STARTUP_OK);
idle (op);
```
- During cold startup, the reference driver reads `0xC6` (clock gate) and `0x1A8B` (Chip ID).
- **Register `0x80` is NEVER read during this identification phase.**
- Register `0x80` is only read inside `idle(op)` (line 345), which begins by transmitting the 3-byte command packet `[0x5A, 0xA5, 0x00]`.

### 3.2. Command Handshake Semantics
The implementation of `idle()` in `fw9369_startup.c:103-114` illustrates the exact role of `0x80`:
```c
/* fw9369_startup.c:103-114 */
static void
idle (struct startup *op)
{
  command (op, 0x5a, 0xa5);
  wait_ms (op, 1);
  if (read_byte (op, 0x80) != 0x50)
    {
      command (op, 0xc0, 0x3f);
      wait_ms (op, 1);
    }
  command (op, 0xa5, 0x5a);
}
```
1. `command (op, 0x5a, 0xa5)` requests the MCU to enter IDLE mode.
2. `read_byte (op, 0x80)` queries whether the MCU acknowledged the transition by asserting status code `0x50`.
3. If `0x80 != 0x50`, the driver executes `command (op, 0xc0, 0x3f)` (an exception abort / reset pulse) and waits 1 ms.
4. `command (op, 0xa5, 0x5a)` locks the MCU into IDLE mode.

### 3.3. MCU State Values
Across all source files, register `0x80` returns distinct MCU state codes:
- `0x50`: **MCU_STATE_IDLE** (Acknowledged entry into idle/ready state).
- `0x54`: **MCU_STATE_CAPTURE** (Acknowledged entry into active capacitive image capture mode).
- `0x58`: **MCU_STATE_NAVIGATION** (Acknowledged entry into finger navigation/tracking mode).

---

## 4. Safety Determination & Classification

### 4.1. Evaluation Questions

1. **Every read and write of `0x80`?**  
   0 writes. 9 reads across 6 files, every single one occurring strictly after an MCU command (`0x5A 0xA5`, `0xC4`, or `0xC8`).
2. **Every function that accesses it?**  
   `idle()`, `enter_idle()`, `configuration_prefix()`, `frames()`, `capture()`, `navigation()`.
3. **Its role during cold initialization, warm initialization, command execution, and runtime capture?**  
   It is an MCU handshake acknowledgment register. It verifies that the embedded microcontroller has transitioned to the requested operational mode.
4. **Whether reading it has documented or source-demonstrated side effects?**  
   Transport-level read framing is `[0x08, 0xF7, 0x80, 0x00, 0x00]`. While an SPI register read is nominally non-destructive on the bus, reading an internal MCU state latch when the MCU is in an un-clocked or boot-ROM phase has completely unverified consequences on the internal state machine.
5. **Whether the value is stable before initialization?**  
   **UNKNOWN.** The reference driver never queries `0x80` prior to sending `0x5A 0xA5`. Its power-on default value is not documented and could reflect floating internal logic or transient boot states.
6. **Whether the reference driver reads it before issuing MCU commands?**  
   **NO.** In 100% of cases in the reference driver, an MCU command precedes the read of `0x80`.
7. **Register Category:**  
   Context-dependent MCU Handshake & Status Register.

### 4.2. Final Classification

```
UNKNOWN — EXCLUDE FROM 2C.1A
```

- **Justification:** Reading register `0x80` without first issuing the MCU idle handshake (`0x5A 0xA5`) is completely unprecedented in reference code. Because `0x5A 0xA5` is an active state transition that is formally excluded from minimal identification, reading `0x80` prior to that handshake provides no validated data and risks interacting unpredictably with the MCU boot state.
- **Confidence Level:** HIGH (verified against all occurrences in reference codebase).
- **Remaining Uncertainty:** The silicon-internal electrical response of the MCU when querying `0x80` on raw uninitialized silicon without prior handshake.
