# Stage 2B.4 — Final Safety Gate & 2C.1B Decision Audit

**Document ID:** `STAGE2B4_FINAL_SAFETY_GATE`  
**Date:** 2026-11-02  
**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FT9362 / FW9369  
**Status:** COMPLETE — STATIC SAFETY GATE AUDIT  
**Hardware Execution Authorization:** NONE (Static Analysis Only)

---

## 1. Executive Summary & Audit Purpose

This document concludes **Stage 2B.4**, the static research and safety audit for **Sub-Stage 2C.1B (Active MCU Idle Handshake)**. 

Every operational ambiguity regarding opcodes `0x5A` and `0xA5`, status register `0x80`, and interrupt acknowledgement register `0x1A84` has been resolved through static source analysis of the Linux reference implementation and the official Dynabook Windows OEM driver.

The audit proves that:
1. The `0x5A` / `0xA5` command sequence is the **MCU Idle Mode Handshake**, formatted according to the universal FocalTech SPI framing structure `[ opcode, ~opcode, 0x00 ]`.
2. Opcode `0x5A` transitions status register `0x80` to `0x50` (`MCU_STATE_IDLE`).
3. Interrupt acknowledgement via register `0x1A84` is non-essential for the idle handshake and cannot be proven safe with any selective mask on cold startup; it is formally **EXCLUDED** from Sub-Stage 2C.1B.
4. Upstream recovery strobes (`0xC0 0x3F`) are **PROHIBITED**; the harness must abort immediately on any status mismatch.
5. The minimal safe sequence is reduced to **exactly 5 bounded SPI transactions**, touching zero protected registers (`0x9A`), zero OTP registers (`0xF1`/`0xF4`), and zero FIFO read commands.

---

## 2. Synthesis of Stage 2B.4 Investigations

### 2.1. Investigation 1 — Semantics of 0x5A and 0xA5
- **Framing Structure:** Proved that all commands follow `[ opcode, ~opcode, 0x00 ]`. The second byte is strictly the bitwise NOT complement checksum.
- **Opcode `0x5A` (`[ 0x5A, 0xA5, 0x00 ]`):** MCU Idle Mode Entry Request.
- **Opcode `0xA5` (`[ 0xA5, 0x5A, 0x00 ]`):** MCU Idle Mode Lock.
- **Status Response:** Querying register `0x80` after 1 ms returns `0x50` (`MCU is idle`).
- **Safety:** Volatile MCU RAM/state transition. Zero persistent storage impact. Safe prior to full initialization.
- **Citation:** [`STAGE2B4_IDLE_HANDSHAKE_REVIEW.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2B4_IDLE_HANDSHAKE_REVIEW.md).

### 2.2. Investigation 2 — Interrupt Acknowledgement (0x1A84)
- **Bitfield Decoding:** Authoritative OEM driver string decodes all 11 interrupt status bits (`1=idle, 2=down, 4=up, 8=man, 10=inv, 20=data, 40=afeD, 80=halfF, 100=fullE, 200=rst, 400=esd`).
- **Mechanics:** `0x1A84` is Write-1-to-Clear (W1C).
- **Startup Problem:** On cold startup, reference code issues blind `0xFFFF`, which clears all diagnostic and error flags. No selective mask can be proven safe or universally applicable prior to full initialization.
- **Handshake Independence:** Proved that `idle()` operates completely independently of `0x1A84`.
- **Verdict:** Excluded from Sub-Stage 2C.1B.
- **Citation:** [`STAGE2B4_INTERRUPT_ACKNOWLEDGEMENT_REVIEW.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2B4_INTERRUPT_ACKNOWLEDGEMENT_REVIEW.md).

### 2.3. Investigation 3 — Minimal Transaction Sequence
- **Defined Sequence:**
  1. `0xC6 = 0x01` (Wakeup write)
  2. Delay 4 ms
  3. `read(0xC6)` (Expect `0x01`)
  4. `command(0x5A)` (`[ 0x5A, 0xA5, 0x00 ]`)
  5. Delay 1 ms
  6. `read(0x80)` (Expect `0x50`; immediate abort if mismatch)
  7. `command(0xA5)` (`[ 0xA5, 0x5A, 0x00 ]`)
- **Safety Bounds:** 20 bytes total transferred, $\sim 5.2$ ms total execution time, 0 flag clears, 0 protected bank unlocks, 0 OTP strobes.
- **Citation:** [`STAGE2B4_MINIMAL_2C1B_SEQUENCE.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2B4_MINIMAL_2C1B_SEQUENCE.md).

### 2.4. Investigation 4 — Failure Paths & Hardware Rollback
- **Protected Register `0x9A`:** Immunity guaranteed; `0x9A` is never written and remains locked (`0x00`).
- **Immediate Abort Rule:** If `0x80 != 0x50`, harness aborts immediately; sending recovery command `[ 0xC0, 0x3F, 0x00 ]` is prohibited.
- **Host Cleanup vs. Silicon Restoration:** Host rollback isolates the host and halts the bus (CS high), but does not un-gate volatile register `0xC6` or reset the MCU.
- **Citation:** [`STAGE2B4_FAILURE_AND_RECOVERY_REVIEW.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2B4_FAILURE_AND_RECOVERY_REVIEW.md).

### 2.5. Investigation 5 — Execution Harness Requirements
- **Invariants Defined:** 14 mandatory architectural invariants, including canonical sysfs child resolution (`/sys/bus/spi/devices/spi-FTE3600:00/spidev/`) eliminating loose `head -n 1` fallbacks, single-shot transfers, and signal-protected rollback.
- **Citation:** [`STAGE2B4_EXECUTION_HARNESS_REQUIREMENTS.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2B4_EXECUTION_HARNESS_REQUIREMENTS.md).

---

## 3. Authoritative Decision & Verdict

### 3.1. Prerequisite Checklist

| Prerequisite | Status | Verification Detail |
| :--- | :---: | :--- |
| Command Semantics Proven | **PASS** | `0x5A` (Idle Request) and `0xA5` (Idle Lock) decoded and verified |
| Framing Architecture Decoded | **PASS** | Universal `[ opcode, ~opcode, 0x00 ]` proven across all commands |
| Interrupt Acknowledgment Scoped | **PASS** | Excluded from 2C.1B to prevent information loss and indeterminate states |
| Recovery Commands Bounded | **PASS** | `[ 0xC0, 0x3F, 0x00 ]` strictly prohibited; immediate abort enforced |
| Protected Bank `0x9A` Locked | **PASS** | Zero access to `0x9A`; remains hardware locked `0x00` |
| Harness Invariants Documented | **PASS** | Canonical sysfs child device resolution specified |
| Zero Hardware Access in Audit | **PASS** | 100% static read-only analysis; no sudo, no test scripts executed |

---

### 3.2. Authoritative Single-Choice Verdict

```
READY FOR A SEPARATELY AUTHORIZED 2C.1B OPERATION
```

### 3.3. Explicit Constraints & Scope Boundary
1. **No Hardware Execution Authorized:** This `READY` verdict signifies that the static research, safety analysis, and protocol specification for Sub-Stage 2C.1B are complete. It does **NOT** grant authorization to execute hardware operations.
2. **Strict Scope Enforcement:** Any future implementation of Sub-Stage 2C.1B must be restricted strictly to the 5-packet sequence defined in [`STAGE2B4_MINIMAL_2C1B_SEQUENCE.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2B4_MINIMAL_2C1B_SEQUENCE.md) and must enforce all 14 invariants in [`STAGE2B4_EXECUTION_HARNESS_REQUIREMENTS.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2B4_EXECUTION_HARNESS_REQUIREMENTS.md).
3. **No Automatic Progression:** Sub-Stage 2C.1B does not authorize Sub-Stage 2C.2 (FDT Calibration) or any subsequent initialization step.
