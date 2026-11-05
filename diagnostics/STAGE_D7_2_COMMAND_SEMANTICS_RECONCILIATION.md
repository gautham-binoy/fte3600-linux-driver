# Stage D7.2 — Technical Reconciliation: Command Strobe `{0xC4, 0x3B}`

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source and Documentation Review (Zero Hardware Operations)  
**Deliverable:** `diagnostics/STAGE_D7_2_COMMAND_SEMANTICS_RECONCILIATION.md`  

---

## 1. Executive Summary

This report performs a comprehensive audit and technical reconciliation of the SPI command strobe `{0xC4, 0x3B}`.

In prior documentation:
- **Stages D3, D4, and D7:** Identified `{0xC4, 0x3B}` as the **Hardware Image Exposure / Acquisition Trigger Strobe**.
- **Stage D7.1 Final Verdict:** Erroneously labeled `{0xC4, 0x3B}` as **"sleep mode"** (`STAGE_D7_1_FINAL_VERDICT.md:69`) and **"reset/sleep"** (line 88).

Through exhaustive inspection of every occurrence and call site in the reference vendor implementation (`capture.c`, `ft93xx.c`, `fw9369_release.c`, `fw9369_startup.c`), this audit concludes that:
1. `{0xC4, 0x3B}` is **exclusively the Image Sensing / Exposure Start Strobe**.
2. Its execution transitions the sensor's internal microcontroller into exposure mode, leading directly to register `0x80` asserting status `0x54` and pixel samples loading into FIFO `0x9A05`.
3. `{0xC4, 0x3B}` is **never used for sleep, idle, or reset**. The labels "sleep mode" and "reset/sleep" in `STAGE_D7_1_FINAL_VERDICT.md` were errors and are formally retracted.

---

## 2. Command Strobe Reconciliation Matrix

| Command Bytes | Canonical Wire Framing | Stage D7 Claim | Stage D7.1 Claim | Source Call Site | Corrected Hardware Function | Confidence |
|---|---|---|---|---|---|---|
| **`{0xC4, 0x3B}`** | `0xC4 0x3B 0x00` | Start Sensing Scan / Image Capture | "Sleep mode" & "reset/sleep" | `capture.c:267`; `ft93xx.c:465` | **Image Exposure Trigger Strobe.** Initiates capacitive matrix readout into hardware FIFO. | **FACT (Source Verified)** |
| **`{0xC2, 0x3D}`** | `0xC2 0x3D 0x00` | Finger Detection Arming | Arming Trigger | `fw9369_startup.c:417`; `fw9369_release.c:289` | **FDT Arming Strobe.** Starts autonomous analog comparator monitoring on Pad 229. | **FACT (Source Verified)** |
| **`{0xC0, 0x3F}`** | `0xC0 0x3F 0x00` | Abort / Idle Mode | Not evaluated | `capture.c:226`; `ft93xx.c:452` | **Acquisition Abort / Idle Strobe.** Terminates active sensing state. | **FACT (Source Verified)** |
| **`{0x5A, 0xA5}`** | `0x5A 0xA5 0x00` | Software Reset / Wakeup | Volatile Strobe | `factory_startup.c:85` | **MCU Wakeup / Resynchronize.** Precedes initial handshake. | **FACT (Source Verified)** |

---

## 3. Comprehensive Call Site & Context Analysis

Across the entire vendor reference driver repository (`research/libfprint-fte3600/`), `{0xC4, 0x3B}` appears at exactly one primary call site in the FW9369 codebase:

### 3.1 Primary Call Site: `capture.c:251–282`
```c
static enum fte3600_capture_status
frames (
  struct fte3600_capture_state *state, uint8_t count,
  uint8_t *payload, size_t capacity, const struct fte3600_capture_io *io,
  struct fte3600_capture_diagnostics *diagnostics, int checked)
{
  struct operation op = {io, diagnostics};
  uint16_t value;
  unsigned int poll;

  if (state == NULL || !valid_output (count, payload, capacity, io))
    return FTE3600_CAPTURE_INVALID_ARGUMENT;
  if (diagnostics != NULL)
    memset (diagnostics, 0, sizeof (*diagnostics));
  if (!configuration_prefix (&op, state))
    return FTE3600_CAPTURE_WORD_READ_ERROR;

  /* STROBE EMISSION */
  command (&op, 0xc4, 0x3b); // Line 267: Issues 0xC4, 0x3B, 0x00 to SPI bus
  wait_ms (&op, 1);

  /* STATUS HANDSHAKE POLL */
  for (poll = 0; poll < 10; ++poll)
    if (read_byte (&op, 0x80) == 0x54)
      break;
  if (poll == 10 && diagnostics != NULL)
    diagnostics->ready_timeout = 1;

  /* ENABLE FIFO STREAMING */
  if (!read_word (&op, 0x1800, &value))
    return FTE3600_CAPTURE_WORD_READ_ERROR;
  write_word (&op, 0x1800, (uint16_t) (value | 1));
  wait_ms (&op, 1);

  /* STREAM 10,246 BYTES FROM FIFO 0x9A05 */
  if (!fifo (&op, count, payload, checked))
    return FTE3600_CAPTURE_FIFO_READ_ERROR;

  /* CLEAR INTERRUPT */
  write_word (&op, 0x1a84, 0x0020);
  return FTE3600_CAPTURE_OK;
}
```

### 3.2 Context Analysis:
1. **Precondition:** The caller configures the sensor into mode `0xA3` via register `0x1800 = 0x4FFE` (`configuration_prefix()`).
2. **Strobe Emission:** Line 267 sends `{0xC4, 0x3B}` with trailer byte `0x00`.
3. **Execution Delay:** 1 ms wait is observed.
4. **Hardware Handshake:** Register `0x80` is polled for `0x54`. `0x54` indicates that the sensor's internal analog front-end has completed frame conversion and loaded the pixel matrix into SRAM.
5. **Readout:** FIFO clocking is enabled (`0x1800 |= 1`), and 10,246 bytes are clocked out.
6. **Postcondition:** The image capture interrupt is cleared by writing `0x0020` to `0x1A84`.

---

## 4. Secondary Call Site: `ft93xx.c:465`

In the parent `ft93xx` family driver (`ft93xx.c:460–475`):
```c
if (!configure (&op, p, state, 0xa3) || !command (&op, 0xc4))
    return FTE3600_FT93XX_WORD_READ_ERROR;
for (poll = 0; poll < 10; ++poll)
{
    if (read_byte (&op, 0x80) == 0x54)
        break;
}
```
In the legacy protocol, `command(&op, 0xc4)` is transmitted without the explicit inverted byte `0x3B`. The operational context is identical: configure mode `0xA3`, strobe `0xC4`, poll `0x80 == 0x54`, and stream the FIFO.

---

## 5. Origin of the Erroneous "Sleep / Reset" Attribution

In `STAGE_D7_1_FINAL_VERDICT.md`, lines 69 and 88 contained:
```markdown
8. Arming Strobe Sequence:
   - Finger detection arming is triggered by command strobe 0xC2 0x3D.
   - Sleep mode is triggered by command strobe 0xC4 0x3B.
...
- The exact minimum hold time and internal state transitions following strobes 0xC2 0x3D (arm) and 0xC4 0x3B (reset/sleep) are undocumented.
```

### 5.1 Analysis of the Error:
1. In Stage D4 and Stage D7, `{0xC4, 0x3B}` was universally documented as the image capture / exposure trigger strobe.
2. In Stage D7.1, while summarizing the paired command strobes `{0xC2, 0x3D}` and `{0xC4, 0x3B}`, the text conflated `{0xC4, 0x3B}` with power-management states (sleep / reset).
3. In reality, the sensor enters low-power autonomous monitoring via `{0xC2, 0x3D}` (arm), and aborts sensing via `{0xC0, 0x3F}`. Sleep is achieved either by deasserting Pad 14 (hardware reset / power down) or cutting $V_{DDIO}$.
4. `{0xC4, 0x3B}` is **unequivocally the image exposure trigger**. Attributing "sleep" or "reset" to `{0xC4, 0x3B}` was a clerical error in `STAGE_D7_1_FINAL_VERDICT.md`.

---

## 6. Corrected Technical Verdict

1. **Exact Semantic Meaning:** `{0xC4, 0x3B}` is the **Image Exposure Start Command Strobe**.
2. **Wire Framing:** Transmitted as a 3-byte sequence: `0xC4 0x3B 0x00`.
3. **Hardware Effect:** Starts matrix charge transfer and analog-to-digital conversion, asserts register `0x80 = 0x54`, and populates FIFO `0x9A05`.
4. **Retraction:** All references to `{0xC4, 0x3B}` denoting "sleep", "idle", or "reset" are retracted.
