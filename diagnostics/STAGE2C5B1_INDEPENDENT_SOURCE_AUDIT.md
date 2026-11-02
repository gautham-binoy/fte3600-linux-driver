# Stage 2C.5B.1 — Independent Source Code & Architectural Audit

**Document ID:** `STAGE2C5B1_INDEPENDENT_SOURCE_AUDIT`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Audit Scope:** Independent, adversarial inspection of `tools/fw9369-mock/` codebase and Stage 2C.5B implementation.  
**Hardware Authorization:** NONE (Software & Static Analysis Only).

---

## 1. Executive Summary

This document presents the findings of an independent, adversarial static audit of the Stage 2C.5B Linux SPI hardware transport implementation. Every line of source code, header declaration, Makefile rule, and test assertion across `tools/fw9369-mock/` was subjected to adversarial scrutiny.

### Summary Audit Verdict Matrix
| Review Area | Audit Finding | Assessment |
| :--- | :--- | :---: |
| **Physical Device Identity** | Path validation checks literal string `"/dev/spidev1.0"` only; does not verify underlying sysfs backing (`spi-FTE3600:00` / `pxa2xx-spi.2`). | **CORRECTION IDENTIFIED** |
| **SPI Configuration** | Mode 0, 8-bit, MSB-first, speed $\le 1$ MHz validated and read back via ioctl. Does not mistakenly claim hardware ACK. | **PASSED** |
| **Timeout & Error Semantics** | `nanosleep()` provides delay only. Blocking `SPI_IOC_MESSAGE()` has no userspace timeout (relies on kernel controller). Distinguishes short transfers. | **ANALYZED & CLARIFIED** |
| **Opcode & Address Allowlist** | `hw_transfer()` allowlist check contained a fallthrough vulnerability where unrecognized opcodes could bypass rejection; register direction (`is_write`) was un-enforced. | **DEFECTS IDENTIFIED & CORRECTED** |
| **FIFO & Chip Select Boundaries** | `cs_change = 0` enforced; non-segmentation rule enforced. Correctly avoids claiming physical continuous CS is proven on silicon. | **PASSED** |
| **Mock / Hardware Separation** | Strict separation: `bin/fw9369-tool` links zero `fw9369_hw_*` symbols; hardware tool requires explicit target `make hw-tool`. | **PASSED** |

---

## 2. Line-by-Line Codebase Inventory & Inspection

### 2.1 `tools/fw9369-mock/src/fw9369_transport_hw.c` (373 LOC)
- **Lines 58–72 (`fw9369_hw_validate_device_path`):**
  Validates `path != NULL` and checks `strcmp(path, "/dev/spidev1.0") == 0`.
  *Audit Observation:* While effective at preventing generic device fallback, pathname string comparison alone cannot detect whether `/dev/spidev1.0` has been remapped to a different physical SPI bus or if `spi-FTE3600:00` is unbound.
- **Lines 74–104 (`fw9369_hw_validate_config`):**
  Enforces $0 < speed \le 1,000,000$, $mode == 0$, $bits == 8$, $lsb == 0$, and $0 < len \le 10,246$.
  *Audit Observation:* Clean, rigorous parameter validation.
- **Lines 106–124 (`hw_delay_ms`, `hw_delay_us`):**
  Uses `nanosleep()` to provide real-time physical settling delays.
  *Audit Observation:* Correctly identified as delay timers, NOT transfer timeouts.
- **Lines 135–154 (`decode_register_addr`):**
  Decodes address from byte and word packets.
  *Audit Observation:* Does not validate that packet length matches opcode expectations (e.g. byte read must be 5 bytes, byte write must be 4 bytes).
- **Lines 156–232 (`hw_transfer`):**
  *CRITICAL AUDIT FINDING (Defect 1):* The allowlist checking block in lines 174–191 used an `if / else if` chain that failed to catch unrecognized opcodes:
  ```c
  if (len == 3 && tx[2] == 0x00) { ... }
  else if (len == FW9369_FIFO_TRANSFER_LEN ...) { ... }
  else if (tx[0] == FW9369_OP_BYTE_WRITE || ...) { ... }
  // Any packet with an unknown opcode (e.g. 0x33) fell through to ioctl!
  ```
  This is a critical violation of fail-closed safety.
- **Lines 234–325 (`fw9369_hw_transport_create_with_ops`):**
  Configures SPI Mode, Bits, Speed, and Bit order via ioctl, reading back effective values.
  *Audit Observation:* Correctly handles ioctl failures and cleans up fd.

### 2.2 `tools/fw9369-mock/src/fw9369_protocol.c` (155 LOC)
- **Lines 104–142 (`fw9369_is_register_allowed`):**
  *AUDIT FINDING (Defect 2):* Line 139 contained `(void)write;`.
  The function accepted a `bool write` parameter but completely ignored it, treating read-only telemetry registers (`0x80`, `0x1A8B`, `0x1A82`, `0x80B8`) as writable, and write-only registers (`0x9A`, `0xF1`, `0xF4`) as readable.
  This allows potential accidental writes to read-only registers.

### 2.3 `tools/fw9369-mock/src/fw9369_main_hw.c` (72 LOC)
- Enforces an offline configuration validation mode (`--validate-config`) and an unconditional fail-closed gate that halts physical execution with exit code 1.
- *Audit Observation:* Operates cleanly; completely blocks physical hardware execution.

### 2.4 `tools/fw9369-mock/Makefile` (47 LOC)
- Isolates `TOOL_BIN` from `HW_TOOL_BIN`.
- `TOOL_BIN` links only `OBJS_COMMON` + `fw9369_main.o`.
- `HW_TOOL_BIN` requires explicit target `make hw-tool`.
- *Audit Observation:* Robust build-level isolation.

---

## 3. Corrective Actions Required

Prior to operator sign-off, the following software corrections are required:
1. **Refactor `hw_transfer()` in `src/fw9369_transport_hw.c`:**
   Convert the allowlist check into an explicit, exhaustive fail-closed parser:
   - Validate packet length for each opcode.
   - Validate that opcode complement byte matches `tx[0] ^ 0xFF`.
   - Validate register address and read/write permission.
   - For commands, validate 3-byte length and `tx[2] == 0x00`.
   - Default: Unconditionally reject any non-matching packet with `-EPERM`.
2. **Implement Directional Permissions in `src/fw9369_protocol.c`:**
   Differentiate read-only registers (`0x80`, `0x1A8B`, `0x1A82`, `0x80B8`), write-only registers (`0x9A`, `0xF1`, `0xF4`, `0x1A83`), and read/write registers (`0xC6`, `0x1800`–`0x1887`, `0x00C0`–`0x00C2`, `0x80B0`–`0x80B3`).
3. **Add Tests for Defect Remediation in `tests/test_runner.c`:**
   Verify that unknown opcodes, malformed complements, and direction violations are cleanly rejected with `-EPERM`.

---

## 4. Audit Conclusion

The Stage 2C.5B implementation provides a solid framework, but adversarial static analysis uncovered two significant safety defects in the transport allowlist parser. Correcting these defects is strictly necessary before the hardware transport can be accepted.
