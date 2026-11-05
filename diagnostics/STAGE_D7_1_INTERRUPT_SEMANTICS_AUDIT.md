# Stage D7.1 — Adversarial Audit of Interrupt Architecture & `0x1A84` Semantics

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source and Documentation Audit  
**Audit Classification:** Adversarial Challenge & Evidence Qualification  

---

## 1. Executive Summary & Challenge to Stage D7 Findings

In Stage D7, the report asserted: *"Conclusively proved that 0x1A84 is a Write-1-To-Clear (W1TC) register paired with 0x1A82 and 0x1A83."*

This Stage D7.1 audit independently challenges that conclusion:
1. **Lack of Authoritative Silicon Specification:**  
   There is **no vendor datasheet, silicon register manual, or hardware errata** available for the FT9362 / FW9369. The claim of "definitive silicon proof" exceeded the available evidentiary basis.
2. **Evidentiary Reality — Source-Code Usage Pattern vs Silicon Proof:**  
   What the source code actually establishes is a **consistent vendor software usage pattern** (`read status from 0x1A82 -> write observed word to 0x1A84`). In software engineering terms, this demonstrates *vendor programmer intent*, not physical silicon architecture.
3. **Mock Artifact Contradiction:**  
   The project's historical userspace mock (`tools/fw9369-mock/src/fw9369_transport_mock.c:139–140`) treated `0x1A84` as a plain read/write RAM array (`ctx->reg_word_1axx[addr - 0x1A80] = val;`), demonstrating that past local simulations did not even implement or verify W1TC behavior.
4. **Race-Condition & Lost-Wakeup Hazards:**  
   The read-then-write pattern contains an inherent time-of-check to time-of-use (TOCTOU) race condition in asynchronous interrupt environments.

---

## 2. Exhaustive Audit of Register Triad Call Sites

Every read and write operation involving registers `0x1A82`, `0x1A83`, and `0x1A84` across the reference codebase was audited:

### 2.1 Register `0x1A82` (Status / Event Latch)
- **`fw9369_fdt.c:250`:**  
  `read_word(op, 0x1a82, &value)`. Checks `(value & 8) != 0` to detect FDT ADC conversion completion.
- **`capture.c:269–273`:**  
  *Omission in Stage D7:* `capture.c` does **not** read `0x1A82` to wait for image readiness; it polls byte register `0x80 == 0x54`.
- **`ft93xx.c:481`:**  
  `read_word(&op, 0x1a82, &word)`. Checks `(word & 0x20) != 0` for frame ready, then writes `word` to `0x1A84`.
- **`ft93xx.c:1091`:**  
  `read_word(&op->wire, 0x1a82, &word)`. Checks `(word & 8) != 0`, then writes `8` to `0x1A84`.
- **`fw9369_release.c:318, 522, 547, 593`:**  
  Reads `0x1A82` during runtime event processing, copying observed status into `initial`, `late`, or `status`.

### 2.2 Register `0x1A83` (Event Mask / Interrupt Enable)
- **`fw9369_fdt.c:230`:**  
  `modify(op, 0x1a83, 0xffff, 0x0008, 0)`. Sets bit 3 (`0x0008`) to enable FDT conversion interrupts.
- **`capture.c:170–171`:**  
  `modify_word(op, state, 0x1a83, 0xffff, 0x0020, 0)` and `modify_word(op, state, 0x1a83, 0xffff, 0x0040, 0)`. Sets bit 5 (`0x0020`) and bit 6 (`0x0040`) for image FIFO events.
- **`fw9369_startup.c:411–414`:**  
  `write_word(op, 0x1a83, (value & 0xff80) | 0x16)`. Sets bits 1, 2, 4 (`0x0016`) for Linux detection.
- **`fw9369_release.c:286, 567, 582`:**  
  Modifies `0x1A83` to enable or disable bit 5 (`0x0020`) during release polling.

### 2.3 Register `0x1A84` (Event Acknowledgment / Clear)
- **`fw9369_startup.c:343, 371`:**  
  `write_word(op, 0x1a84, 0xffff)`. Unconditional full-mask write during cold startup prefix.
- **`fw9369_startup.c:416`:**  
  `write_word(op, 0x1a84, 0x2f)`. Unconditional write of `0x002F` (bits 0, 1, 2, 3, 5) immediately prior to arming strobe `0xC2 0x3D`.
- **`capture.c:280`:**  
  `write_word(&op, 0x1a84, 0x0020)`. Explicit write of bit 5 after completing 10,246-byte FIFO transfer.
- **`fw9369_release.c:320`:**  
  `write_word(0x1a84, value)`. Writes observed `0x1A82` value back to `0x1A84`.
- **`fw9369_release.c:554, 597, 601`:**  
  Echoes observed `0x1A82` status back to `0x1A84`.
- **`ft93xx.c:485, 493, 1094, 1351, 1566, 1580`:**  
  Targeted bit writes (`0x20`, `8`, `0x2F`, or echoed status).

---

## 3. Critical Assessment of the W1TC Hypothesis

### 3.1 What is Proven vs What is Inferred

| Assertion | Status | Evidentiary Basis |
|---|---|---|
| Vendor software reads `0x1A82` and writes the same bits to `0x1A84` | **PROVEN** | Directly observed in `fw9369_release.c:320`, `ft93xx.c:485` |
| Specific bits correspond to specific functional events | **PROVEN (Empirically)** | Bit 3 = FDT ready (`fw9369_fdt.c:252`), Bit 5 = FIFO ready (`capture.c:280`) |
| Writing `1` clears the corresponding bit in `0x1A82` | **STRONG INFERENCE** | Inferred from programmer usage; not verified on silicon |
| Writing `0` leaves the bit unchanged | **UNPROVEN ASSUMPTION** | Without hardware testing or datasheet, writing 0 could be a no-op or could have side effects |
| Reading `0x1A84` is valid | **COMPLETELY UNKNOWN** | Vendor code never reads `0x1A84`; it is treated strictly as write-only |
| Silicon contains no level-sensitive interrupt overrides | **UNPROVEN ASSUMPTION** | If a hardware condition (e.g. FIFO full or finger down) persists, W1TC may immediately re-latch |

### 3.2 The Asynchronous Race-Condition Hazard

The pattern `word = read(0x1A82); write(0x1A84, word);` is intended to avoid clearing unobserved events. However, an adversarial concurrency analysis reveals a fundamental vulnerability:

```
  Host / CPU                             Sensor Hardware
  ──────────                             ───────────────
  1. Read 0x1A82 (returns 0x0008, FDT)   
                                         2. User touches sensor!
                                            Sensor latches Bit 0 (0x0001, Touch Down)
  3. Write 0x1A84 = 0x0008 (clears FDT)
```
- In a pure W1TC register, writing `0x0008` clears bit 3 and leaves bit 0 asserted, which is safe.
- **However, if `write 0x1A84 = 0xFFFF` is ever used at runtime:**
  Any concurrent event (such as a touch down occurring between cycles) is destroyed.
- **Furthermore, if `0x1A84` is NOT W1TC, but rather a direct write or strobe register:**
  Writing `0x0008` could overwrite the interrupt controller state entirely, clearing bit 0 without host awareness.

---

## 4. Kernel Architecture Policy

Pending explicit hardware verification authorized in a future stage:

1. **Maintain Isolation:**  
   The current Linux kernel driver (`kernel/fte3600.c`) must **continue to exclude all `0x1A84` writes** from the baseline initialization path.
2. **Prohibit `0xFFFF` at Runtime:**  
   Writing `0xFFFF` to `0x1A84` during active runtime sensing is classified as a severe safety and reliability hazard.
3. **Classification:**  
   Register `0x1A84` semantics are classified as **`[SOURCE-OBSERVED PATTERN — UNVERIFIED ON SILICON]`**.
