# Stage 2B.5 — Independent Validation of Register 0x80 Status Check

**Document ID:** `STAGE2B5_REGISTER_80_VALIDITY_REVIEW`  
**Date:** 2026-11-02  
**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FT9362 / FW9369  
**Status:** COMPLETE — STATIC SOURCE CODE AUDIT  
**Hardware Execution Authorization:** NONE (Static Analysis Only)

---

## 1. Executive Summary

In Stage 2B.3, register `0x80` was classified as `UNKNOWN — EXCLUDE FROM 2C.1A` because reading it on uninitialized silicon prior to issuing an MCU command has zero precedent in reference code. 

Stage 2B.4 proposed reading `0x80` *after* transmitting command `0x5A`, claiming that `0x5A` establishes valid MCU handshake semantics. This investigation independently audits whether reading `0x80` after `0x5A` is genuinely justified, whether `0x50` is guaranteed, and whether a single-shot read without retries is viable.

The audit proves that:
1. Reading `0x80` after `0x5A` + 1 ms delay is the universal pattern used in reference code.
2. However, status `0x50` is **NOT guaranteed** on a single read.
3. Every reference driver implements polling (up to 5 retries in `ft93xx.c`, up to 10 retries in `fw9369_release.c`) or exception strobes (`C0 3F` in `fw9369_startup.c`).
4. A single-shot read of `0x80` with immediate abort (and no `0xA5` termination) introduces severe fragility and creates an unrecoverable failure state.

---

## 2. Analysis of Register 0x80 Access Patterns

### 2.1. Delay Evidence: Is 1 ms Universal?
Across all reference driver implementations, a 1 ms delay between command `0x5A` and read of `0x80` is universally implemented:
- `fw9369_startup.c:107`: `wait_ms (op, 1);`
- `fw9369_fdt.c:72`: `delay (op, 1);`
- `fw9369_release.c:455`: `io->wait_ms (io->context, 1);`
- `capture.c:222`: `wait_ms (op, 1);`
- `ft93xx.c:265`: `wait_ms (op, 1);`

This proves that the 1 ms delay is an architectural silicon timing requirement for the internal microcontroller instruction pipeline to acknowledge the command and update status latch `0x80`.

### 2.2. Is Status `0x50` Guaranteed After 1 ms?
**NO.** Source code directly proves that `0x80 == 0x50` is not guaranteed immediately after 1 ms:
1. In `ft93xx.c:268-277`:
```c
/* ft93xx.c:268-277 */
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
```
If `0x50` were guaranteed after 1 ms, this 5-iteration loop with exception recovery strobes (`0xC0`) would be completely redundant.
2. In `fw9369_release.c:531-537`:
```c
/* fw9369_release.c:531-537 */
for (unsigned retry = 0; ready == 0 && retry < 10; ++retry)
  {
    if (!idle (io, d))
      return FTE3600_FW9369_RELEASE_TRANSPORT_ERROR;
    io->wait_ms (io->context, 20);
    ready = communication (io, d);
  }
```
The driver allows up to 10 retries with 20 ms delays because MCU idle transitions can be delayed or rejected during internal housekeeping.

---

## 3. The Dilemma of Single-Shot 2C.1B Execution

Stage 2B.4 established two strict safety rules:
1. **No Automatic Retries:** Transactions must be single-shot.
2. **Immediate Abort on Mismatch:** If `0x80 != 0x50`, abort immediately and do not send recovery command `C0 3F`.

### The Inherent Vulnerability
If the sensor hardware requires more than 1.0 ms to assert `0x50` (or if power-on boot timing introduces transient delay):
1. The single-shot read of `0x80` will read something other than `0x50` (e.g. `0x00` or transient busy state).
2. The harness will abort immediately.
3. The closing command `0xA5` will **never be transmitted**.
4. The sensor's microcontroller will be left stranded in an open command dialog initiated by `0x5A`.

---

## 4. Conclusion

While reading `0x80` after `0x5A` is semantically correct in full drivers that support retries and exception handling, enforcing a rigid single-shot check with an immediate abort without closing the dialog creates an unacceptable failure mode.
