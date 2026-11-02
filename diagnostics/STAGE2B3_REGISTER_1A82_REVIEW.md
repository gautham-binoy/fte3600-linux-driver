# Stage 2B.3 — Register 0x1A82 Safety & Semantics Review

**Document ID:** `STAGE2B3_REGISTER_1A82_REVIEW`  
**Date:** 2026-11-02  
**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FT9362 / FW9369  
**Status:** COMPLETE — STATIC SOURCE CODE AUDIT  
**Hardware Execution Authorization:** NONE (Static Analysis Only)

---

## 1. Executive Summary

This investigation audits the 16-bit register `0x1A82` and its companion register `0x1A84` across the reference driver codebase (`research/libfprint-fte3600/`).

The audit establishes that:
1. Register `0x1A82` is an **ordinary status and interrupt flag register** with explicit **Write-1-to-Clear (W1C)** semantics executed via companion register `0x1A84`.
2. Reading register `0x1A82` **does not clear any flags**. Repeated polling of `0x1A82` without intermediate writes to `0x1A84` is standard practice throughout the driver.
3. The reference driver **never reads register `0x1A82` during startup or identification**. In `fw9369_startup.c:343`, the driver unconditionally issues `write_word(op, 0x1a84, 0xffff)` without inspecting `0x1A82`.
4. At cold boot, the pre-initialization contents of `0x1A82` are undocumented, lack vendor bitfield specifications, and provide **zero chip identification value**.

Consequently, reading register `0x1A82` in Sub-Stage 2C.1A provides no actionable information and is classified as **`UNKNOWN — EXCLUDE FROM 2C.1A`**.

---

## 2. Exhaustive Access Trace: Register 0x1A82

### 2.1. Writes to Register 0x1A82
- **Total Writes Found:** **0**
- Register `0x1A82` is strictly a read-only hardware register.

### 2.2. Reads of Register 0x1A82

| Source File & Line | Calling Function | Context / Purpose | Polling / Repeated? | Acknowledge via `0x1A84`? |
| :--- | :--- | :--- | :---: | :---: |
| `ft93xx.c:481` | `capture_frame()` | Polls for frame ready (`word & 0x20`) | Yes (up to 5 polls) | Yes: `write_word(0x1a84, word)` |
| `ft93xx.c:1091` | `fdt_ready()` | Polls for FDT ready (`word & 0x08`) | Yes (up to 5 polls) | Yes: `write_word(0x1a84, 8)` |
| `ft93xx.c:1562` | `navigation()` | Polls for frame ready (`word & 0x20`) | Yes (up to 5 polls) | Yes: `write_word(0x1a84, word)` |
| `fw9369_fdt.c:250` | `fdt_calibrate()` | Polls for channel ready (`value & 0x08`) | Yes (up to 5 polls) | No write in loop; returns status |
| `fw9369_release.c:318` | `status()` | Queries active interrupt bitmask | Single read | Yes: `write_word(0x1a84, value)` |
| `fw9369_release.c:522` | `handle_event()` | Reads initial event bitmask on IRQ | Single read | No immediate write |
| `fw9369_release.c:547` | `handle_event()` | Re-reads status post communication | Single read | Yes: `write_word(0x1a84, initial)` |
| `fw9369_release.c:593` | `handle_event()` | Reads "late" event after arming | Single read | Yes: `write_word(0x1a84, late)` |

---

## 3. Clear-on-Read vs. Write-1-to-Clear (W1C) Analysis

### 3.1. Source Evidence of Persistence Across Reads
In `ft93xx.c:1089-1096`, the driver repeatedly reads `0x1A82` in a polling loop:
```c
/* ft93xx.c:1089-1096 */
for (unsigned int poll = 0; poll < 5; ++poll)
  {
    if (!read_word (&op->wire, 0x1a82, &word))
      return 0;
    if (word & 8)
      return write_word (&op->wire, 0x1a84, 8);
    wait_ms (&op->wire, 1);
  }
```
- If register `0x1A82` were clear-on-read (COR), reading it on poll 0 would destructively clear bit 3 (`8`), preventing subsequent verification and breaking the driver's explicit write to `0x1A84` on line 1094.
- Similarly, in `fw9369_release.c:545-547`:
```c
/* Communication retries can enter idle; re-read rather than consuming a
 * stale precheck. A cleared status is still no reason to restart FDT. */
if (!word (0x1a82, &initial, io, d))
  return FTE3600_FW9369_RELEASE_WORD_READ_ERROR;
```
The driver explicitly re-reads `0x1A82` because previous reads did **not** clear the register contents.

### 3.2. Acknowledgment via Register 0x1A84
- Across the entire driver, clearing or acknowledging bits in `0x1A82` is accomplished exclusively by writing the bitmask to register `0x1A84`.
- In `fw9369_release.c:318-321`:
```c
uint16_t value;
if (!word (0x1a82, &value, io, d))
  return FTE3600_FW9369_RELEASE_WORD_READ_ERROR;
if (!write_word (0x1a84, value, io, d))
  return FTE3600_FW9369_RELEASE_TRANSPORT_ERROR;
```
- Conclusion: Register `0x1A82` is an ordinary status register; companion register `0x1A84` is the Write-1-to-Clear (W1C) acknowledge register.

---

## 4. Evaluation for Sub-Stage 2C.1A

### 4.1. Detailed Answers to Investigation Questions

1. **Every read and write?**  
   0 writes. 8 read sites across `ft93xx.c`, `fw9369_fdt.c`, and `fw9369_release.c`.
2. **Whether reading the register clears any flags?**  
   **NO.** Reading `0x1A82` is non-destructive with respect to its flag bits.
3. **Whether the register is clear-on-read, read-to-acknowledge, or ordinary status?**  
   Ordinary status with W1C acknowledgment via `0x1A84`.
4. **Whether writes to `0x1A84` are required to acknowledge its bits?**  
   **YES.** All clearing is performed by writing the corresponding bitmask to `0x1A84`.
5. **Whether the reference driver reads it before initialization?**  
   **NO.** During startup (`fw9369_startup.c:338-344`), `0x1A82` is never read. The driver unconditionally writes `0xFFFF` to `0x1A84` without inspecting `0x1A82`.
6. **Whether reading it can race with asynchronous sensor events?**  
   Yes at runtime during finger touch/release events. However, at cold boot (before Phase 5 arming), asynchronous events do not occur.
7. **Whether repeated reads are safe?**  
   Safe on the SPI bus and idempotent in software polling loops.
8. **Whether a read provides useful information during proposed 2C.1A?**  
   **NO.** Basic identification of the FT9362 was already achieved in Stage 2A via Chip ID register `0x1A8B` (`0x9362`). The cold boot state of `0x1A82` is not used by the reference driver, has no defined diagnostic thresholds, and provides zero identity confirmation.

### 4.2. Final Classification

```
UNKNOWN — EXCLUDE FROM 2C.1A
```

- **Justification:** While `0x1A82` is confirmed to be an ordinary read register (not clear-on-read), the reference driver never reads it prior to initialization, and its pre-initialization bits are undocumented. Including it in a minimal identification stage introduces an unnecessary transaction that yields no actionable identification data.
- **Confidence Level:** HIGH for W1C mechanics and reference driver access patterns.
- **Remaining Uncertainty:** The exact vendor definition of cold-boot power-on flags in `0x1A82`.
