# Stage 2B.5 — Independent Verification of Command 0xA5 Semantics

**Document ID:** `STAGE2B5_COMMAND_A5_VERIFICATION`  
**Date:** 2026-11-02  
**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FT9362 / FW9369  
**Status:** COMPLETE — STATIC SOURCE CODE AUDIT  
**Hardware Execution Authorization:** NONE (Static Analysis Only)

---

## 1. Executive Summary

This investigation independently traces command `0xA5` across the reference codebase to determine its precise protocol semantics, calling logic, and relationship to command `0x5A` and status register `0x80`.

The audit reveals that:
1. In `fw9369_release.c:451`, the array transmitting `0xA5` is explicitly named **`end[3] = {0xa5, 0x5a, 0}`**.
2. Command `0xA5` does not mean "enter idle" by itself; it is the **Command Transaction Terminator / End Delimiter** that concludes the idle dialog initiated by `wake` (`0x5A`).
3. Crucially, across all reference implementations, **command `0xA5` is ALWAYS transmitted**, even when the preceding `0x80` status check fails.
4. Omitting `0xA5` on an `0x80` mismatch (as originally proposed in the Stage 2B.4 abort rule) leaves the sensor's microcontroller in an un-terminated command transaction state with zero precedent in reference code.

---

## 2. Exhaustive Trace of Command 0xA5 Call Sites

### 2.1. Call Sites & Surrounding Logic

| Source File & Line | Enclosing Function | Exact Transmission Code | Condition for Execution | Role / Semantic |
| :--- | :--- | :--- | :--- | :--- |
| `fw9369_release.c:451, 464` | `idle()` | `uint8_t end[3] = {0xa5, 0x5a, 0}; return transfer(end, 3, io, d);` | **UNCONDITIONAL** (executes even if `value != 0x50`) | Closes dialog (`end`) |
| `ft93xx.c:280` | `idle()` | `return command (op, 0xa5);` | **UNCONDITIONAL** (executes even if 5 retries fail and `value != 0x50`) | Closes command transaction |
| `fw9369_startup.c:113` | `idle()` | `command (op, 0xa5, 0x5a);` | **UNCONDITIONAL** (follows `0x80` check and optional `C0 3F`) | Terminates idle routine |
| `fw9369_fdt.c:79` | `idle()` | `command (op, 0xa5, 0x5a);` | **UNCONDITIONAL** (follows `0x80` check and optional `C0 3F`) | Terminates idle routine |
| `capture.c:228` | `configuration_prefix()` | `command (op, 0xa5, 0x5a);` | **UNCONDITIONAL** (follows `0x80` check and optional `C0 3F`) | Terminates prefix routine |
| `factory_startup.c:158`| `idle()` | `command (op, 0xa5, 0x5a);` | **UNCONDITIONAL** | Terminates factory idle |

---

## 3. Analysis of Execution Flow in Reference Drivers

### 3.1. Implementation in `fw9369_release.c:448-465`
```c
/* fw9369_release.c:448-465 */
static int
idle (const struct fte3600_capture_io           *io,
      struct fte3600_fw9369_release_diagnostics *d)
{
  uint8_t wake[3] = {0x5a, 0xa5, 0}, stop[3] = {0xc0, 0x3f, 0}, end[3] = {0xa5, 0x5a, 0}, value;

  if (!transfer (wake, sizeof (wake), io, d))
    return 0;
  io->wait_ms (io->context, 1);
  if (!byte_read (0x80, &value, io, d))
    return 0;
  if (value != 0x50)
    {
      if (!transfer (stop, sizeof (stop), io, d))
        return 0;
      io->wait_ms (io->context, 1);
    }
  return transfer (end, sizeof (end), io, d);
}
```
Notice:
- `wake` (`0x5A`) begins the dialog.
- If `value != 0x50`, `stop` (`0xC0`) is sent as an exception strobe.
- Line 464 **unconditionally** transmits `end` (`0xA5`).

### 3.2. Implementation in `ft93xx.c:258-281`
```c
/* ft93xx.c:258-281 */
static int
idle (const struct operation *op)
{
  uint8_t value;

  if (!command (op, 0x5a))
    return 0;
  wait_ms (op, 1);
  if (!read_byte (op, 0x80, &value))
    return 0;
  for (unsigned int i = 0; value != 0x50 && i < 5; ++i)
    {
      if (!command (op, 0xc0))
        return 0;
      wait_ms (op, 1);
      if (!read_byte (op, 0x80, &value))
        return 0;
      if (value != 0x50)
        wait_ms (op, 1);
    }
  if (op->diagnostics != NULL)
    op->diagnostics->idle_unacknowledged |= value != 0x50;
  return command (op, 0xa5);
}
```
Even if `value != 0x50` after 5 retry attempts, line 280 still calls `command(op, 0xa5)`!

---

## 4. Architectural Semantic of Command 0xA5

1. **Protocol Role:** Command `0xA5` (`[ 0xA5, 0x5A, 0x00 ]`) is the **Mandatory Closing Delimiter** (`end`) of the MCU command dialog.
2. **Coupling with 0x5A:** Command `0x5A` opens the command dialog; command `0xA5` closes it.
3. **Flaw in Previous 2C.1B Abort Design:** 
   The Stage 2B.4 proposal stated:
   > *"If status register `0x80` fails to report `0x50`, the harness aborts immediately and `0xA5` is never sent."*
   This audit proves that this rule violates the protocol logic of the sensor: if `0x5A` has been transmitted, omitting `0xA5` leaves the MCU internal command state machine un-closed and hanging.
