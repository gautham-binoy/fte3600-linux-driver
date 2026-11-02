# Stage 2B.4 — Interrupt Acknowledgement (0x1A84) & Status (0x1A82) Review

**Document ID:** `STAGE2B4_INTERRUPT_ACKNOWLEDGEMENT_REVIEW`  
**Date:** 2026-11-02  
**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FT9362 / FW9369  
**Status:** COMPLETE — STATIC SOURCE CODE AUDIT  
**Hardware Execution Authorization:** NONE (Static Analysis Only)

---

## 1. Executive Summary

This investigation resolves the exact bitfield structure, acknowledge mechanics, race condition profile, and safety properties of status register `0x1A82` and companion acknowledge register `0x1A84`.

Using direct evidence from the official Dynabook Windows OEM driver (`research/dynabook-windows-driver/strings/ftWbioUmdfDriverV2.dll.ascii.txt`) and cross-referencing with the Linux reference driver, this audit conclusively decodes all 11 interrupt status bits.

Furthermore, this investigation demonstrates that:
1. `0x1A84` operates strictly as a **Write-1-to-Clear (W1C)** register.
2. In cold startup (`fw9369_startup.c:343` and Windows `fw9369_init_chip`), the driver writes `0xFFFF` unconditionally to clear all boot/power-on flags.
3. No single selective acknowledgement mask can be proven safe or universally applicable prior to full initialization.
4. The MCU idle handshake (`5A A5 ... A5 5A`) operates independently of interrupt flag clearing.
5. Consequently, **interrupt acknowledgement via `0x1A84` must be EXCLUDED from Sub-Stage 2C.1B**.

---

## 2. Definitive Silicon Interrupt Bitfield Decoding

In the Dynabook Windows OEM driver (`ftWbioUmdfDriverV2.dll.ascii.txt:2356`), the vendor debug logger explicitly defines the interrupt bitmask:

```text
[%4d]:Chip INT type: 0x%X(1=idle 2=down 4=up 8=man 10=inv 20=data 40=afeD 80=halfF 100=fullE 200=rst 400=esd )
```

This authoritative string decodes all 11 active bits in registers `0x1A82` and `0x1A84`:

| Bit Index | Bit Mask | Vendor Name | Full Description / Event Semantics | Verified Linux Call Site |
| :---: | :---: | :--- | :--- | :--- |
| **Bit 0** | `0x0001` | `idle` | MCU Idle State Reached / Handshake Acknowledged | Part of `0x002F` arming mask |
| **Bit 1** | `0x0002` | `down` | Finger Touch Event (Capacitive threshold crossed down) | Part of `0x002F` arming mask |
| **Bit 2** | `0x0004` | `up` | Finger Leave Event (Capacitive threshold crossed up) | `fw9369_release.c:600` (`late & 4`) |
| **Bit 3** | `0x0008` | `man` | Manual Measurement / FDT Calibration Sample Ready | `ft93xx.c:1094` (`write_word(0x1a84, 8)`) |
| **Bit 4** | `0x0010` | `inv` | Invalid Touch / High-Frequency Environmental Noise | `fw9369_release.c:558` (error mask `0x610`) |
| **Bit 5** | `0x0020` | `data` | Image FIFO Buffer Ready (5,120 16-bit pixel samples) | `capture.c:280`, `ft93xx.c:485` (`word & 0x20`) |
| **Bit 6** | `0x0040` | `afeD` | Analog Front-End Conversion Done | Internal AFE sequencing |
| **Bit 7** | `0x0080` | `halfF` | Half-Frame Buffer Filled (Streaming mode) | Internal hardware FIFO |
| **Bit 8** | `0x0100` | `fullE` | Full Exposure Complete | Internal frame synchronization |
| **Bit 9** | `0x0200` | `rst` | Hardware Reset / Power-On Reset (POR) Event Asserted | `fw9369_release.c:558` (error mask `0x610`) |
| **Bit 10**| `0x0400` | `esd` | Electrostatic Discharge (ESD) Event Detected | `fw9369_release.c:558` (error mask `0x610`) |

---

## 3. Exhaustive Trace of Acknowledgment Masks (`0x1A84`)

| Mask Written | Source File & Line | Calling Function | Derivation Method | Safety / Consequence |
| :---: | :--- | :--- | :--- | :--- |
| `0xFFFF` | `fw9369_startup.c:343, 371` | `prefix()` | Blind hardcoded constant | Global clear: wipes all boot/fault flags unconditionally |
| `0xFFFF` | `ft93xx.c:547, 797` | `reset_harness()`, `detect()` | Blind hardcoded constant | Global clear during probe |
| `0x0020` | `capture.c:280`, `ft93xx.c:493` | `frames()`, `capture()` | Hardcoded `data` ready flag | Clears only image frame ready flag |
| `0x0008` | `ft93xx.c:1094` | `fdt_ready()` | Hardcoded `man` ready flag | Clears only FDT measurement sample flag |
| `0x002F` | `fw9369_startup.c:416`, `ft93xx.c:1351` | `fw9369_startup()`, `arm()` | Composite operational mask | Clears `idle|down|up|man|data` before arming IRQ |
| `value` | `fw9369_release.c:320` | `status()` | Derived: `read(0x1a82) -> write` | Selectively clears only active asserted bits |
| `initial` | `fw9369_release.c:554` | `handle_event()` | Derived: `read(0x1a82) -> write` | Selectively clears only event bits read on IRQ |
| `late` | `fw9369_release.c:597, 601` | `handle_event()` | Derived: `read(0x1a82) -> write` | Selectively clears late-arriving event bits |

---

## 4. Race Condition & Information Loss Analysis

### 4.1. Hardware W1C Semantics
Register `0x1A84` is a true Write-1-to-Clear interface:
- Writing `1` to bit $k$ clears bit $k$ in status register `0x1A82`.
- Writing `0` to bit $k$ leaves bit $k$ in `0x1A82` unchanged.

### 4.2. Derived Read-Then-Acknowledge Safety
When the driver reads `0x1A82` into `val` and writes `val` to `0x1A84`:
- **Atomic Safety:** If a new event occurs and asserts bit $j$ *after* the read of `0x1A82`, bit $j$ is `0` in `val`. Writing `val` to `0x1A84` does **not** clear bit $j$. Bit $j$ remains asserted in `0x1A82` and will be caught by the next read or interrupt handler.
- Therefore, derived selective acknowledgement is immune to clearing un-read concurrent events.

### 4.3. Danger of Hardcoded Acknowledgement on Startup
- **Blind `0xFFFF`:** Destroys power-on diagnostic flags (such as bit 9 `rst` or bit 10 `esd`) before the host has ever read or recorded them.
- **Blind Hardcoded Masks (e.g. `0x002F`):** If applied on cold startup without reading `0x1A82`, they clear operational bits but leave fault bits (`0x0200` RST or `0x0400` ESD) unacknowledged, leading to stuck interrupt lines.
- **No Documented Pre-Init Mask:** Neither the reference driver nor the vendor documentation defines a valid selective acknowledgement mask for pre-initialization hardware.

---

## 5. Independence of MCU Idle Handshake

The central architectural question for Stage 2C.1B is:  
**Does the MCU Idle Handshake (`0x5A` / `0xA5`) require writing to `0x1A84`?**

Source code analysis across the codebase confirms: **NO.**
1. In `fw9369_fdt.c:174`, `idle(op)` is called repeatedly before calibration without writing to `0x1A84`.
2. In `capture.c:221`, `configuration_prefix()` executes the idle handshake (`5A A5 -> check 0x80 -> A5 5A`) without writing to `0x1A84`.
3. In `ft93xx.c:259`, `enter_idle(op)` executes without writing to `0x1A84`.
4. Register `0x1A84` interacts with the host interrupt / event reporting subsystem, whereas opcodes `0x5A` and `0xA5` communicate with the embedded microcontroller execution core.

---

## 6. Investigation Conclusion & Safety Gate Decision

1. **Selective Mask Cannot Be Proven Safe:** Because cold boot state varies, applying an arbitrary selective mask (e.g. `0x0001` or `0x002F`) is unsupported by evidence and risks leaving fault flags unhandled.
2. **Global Mask (`0xFFFF`) Discards Information:** Blindly clearing all flags destroys boot telemetry.
3. **Acknowledgment is Non-Essential for 2C.1B:** The MCU idle handshake succeeds completely without writing to `0x1A84`.
4. **Authoritative Verdict:** **Register `0x1A84` acknowledgement must be EXCLUDED from Sub-Stage 2C.1B.**
