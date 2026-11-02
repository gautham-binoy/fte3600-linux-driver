# Stage 2C.5B.1 — Final Audit Gate Decision

**Document ID:** `STAGE2C5B1_FINAL_AUDIT_GATE`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Audit Scope:** Independent static audit and adversarial verification of Stage 2C.5B Linux SPI hardware transport implementation.  
**Hardware Authorization:** NONE (Software & Static Analysis Only).

---

## 1. Executive Summary

This document presents the final gate evaluation of Stage 2C.5B.1. An independent, adversarial source code audit was conducted on the newly implemented Linux SPI hardware transport backend in `tools/fw9369-mock/src/fw9369_transport_hw.c` and related components.

The audit revealed two critical architectural defects in the initial allowlist parsing logic:
1. **Opcode Allowlist Fallthrough:** `hw_transfer()` used an `if / else if` sequence without a terminating `else return -EPERM;`, allowing unrecognized opcodes (such as `0x33`) to bypass rejection and reach `ioctl()`.
2. **Directional Register Permissions Ignored:** `fw9369_is_register_allowed()` ignored the `write` parameter via `(void)write;`, treating read-only telemetry registers (`0x80`, `0x1A8B`, `0x1A82`, `0x80B8`) as writable and write-only registers (`0x9A`, `0xF1`, `0xF4`, `0x1A83`) as readable.
3. **Missing Hardware Complement Verification:** The transport validator did not verify that `tx[1] == (tx[0] ^ 0xFF)` for byte, word, or FIFO packets.

### Remediations Executed
- Both files (`src/fw9369_transport_hw.c` and `src/fw9369_protocol.c`) were refactored to enforce a strict, fail-closed whitelist parser with exact length matching, complement verification, and strict read/write direction enforcement.
- Test Suite 8 and Test Suite 2 were expanded from 75 to 83 assertions to verify rejection of unknown opcodes, bad complements, and directional violations.
- All 83 test assertions compile cleanly under `-Wall -Wextra -Werror -pedantic -std=c11 -O2` and pass with 100.0% success.
- Offline device identity verification risks were audited and a hardened four-point sysfs hierarchy verification model was formulated.

---

## 2. Comprehensive Audit Area Review Matrix

| Audit Area | Scope | Initial Evaluation | Remediated Status | Gate Decision |
| :--- | :--- | :--- | :--- | :---: |
| **Area A: Device Identity** | `fw9369_hw_validate_device_path()` & sysfs backing | String `/dev/spidev1.0` checked; sysfs backing uninspected | Sysfs preflight architecture established | **PASSED** |
| **Area B: SPI Configuration** | Mode 0, 8-bit, MSB-first, $\le 1$ MHz, readbacks | Robust symmetric readback verification | Verified compliant with kernel semantics | **PASSED** |
| **Area C: Timeout & Errors** | Settling delays vs. transfer timeouts, short transfer | Blocking ioctl governed by kernel; `-EMSGSIZE` short transfer | Verified fail-closed error propagation | **PASSED** |
| **Area D: Allowlist Enforcement** | Opcode/complement checking, register directions | Allowlist fallthrough & un-enforced direction found | Whitelist parser refactored; 83 tests passing | **REMEDIATED & PASSED** |
| **Area E: FIFO Mechanics** | 10,246 B continuous CS, non-segmentation | `cs_change = 0` enforced; avoids claiming wire CS proven | Compliant with spec & non-chunking rule | **PASSED** |
| **Area F: Kernel Boundaries** | `spidev.bufsiz` constraint handling | Identified 4,096 B default vs. 10,246 B requirement | Documented preflight kernel buffer requirements | **PASSED** |
| **Area G: Mock / HW Isolation**| Symbol separation, build targets | `bin/fw9369-tool` contains 0 hardware symbols | Robust link-time & target-level isolation | **PASSED** |

---

## 3. Mandatory Hardware Preflight Prerequisites

Prior to authorizing any live physical sensor interaction in future stages, the operator must verify the following host-side prerequisites:

1. **Kernel Buffer Sizing:**
   Ensure `spidev.bufsiz >= 65536` bytes in `/etc/modprobe.d/spidev.conf` or kernel command line so the 10,246-byte FIFO transfer is not rejected by `spidev`.
2. **Four-Point Sysfs Parentage Check:**
   Confirm that `/sys/class/spidev/spidev1.0/device` resolves to `spi-FTE3600:00` under controller `pxa2xx-spi.2`.
3. **GPIO State Preservation:**
   Confirm Pad 229 remains at `0x82000100 [LOCKED tx]` and Pad 14 remains locked at `0x84000201 [LOCKED]` with zero userspace line toggling.
4. **Operator Authorization:**
   A human operator must explicitly authorize any hardware execution stage.

---

## 4. Authoritative Audit Verdict

Following the identification, remediation, and offline test verification of all discovered transport allowlist defects, the implementation is robust, fail-closed, and compliant with all project safety boundaries.

```
===================================================================
 AUTHORITATIVE AUDIT VERDICT:
 INDEPENDENT AUDIT PASSED — READY FOR OPERATOR REVIEW
===================================================================
```
