# Stage 2C.5C.2 — Final Safety Gate and State Model Hardening Review

**Document ID:** `STAGE2C5C2_FINAL_SAFETY_GATE`  
**Date:** 2026-11-02  
**Target Hardware:** FocalTech FT9362 / FW9369 on Dynabook Satellite Pro C40-J  
**Subsystems Hardened & Audited:**
- Register `0x9A` 5-State Software Knowledge Model
- Uncertain SPI Transfer Handling (Dual-Failure Transitions to `UNKNOWN`)
- Fail-Closed Context Initialization & Recreation Policy
- Orderly Cleanup vs. Uncatchable Abnormal Termination Audit
- Historical Documentation Inconsistencies & Rigorous Epistemic Boundary
- Expanded Offline Test Runner (159/159 Passed, 100.0%)
- Hardware Execution Gate Integrity & Fail-Closed Boundary
**Execution Boundary:** Strictly static code analysis and offline mock test verification. Zero hardware operations authorized.

---

## 1. Executive Summary & Authoritative Verdict

Stage 2C.5C.2 successfully executed an exhaustive, software-only hardening of the FW9369 transport state model. The simplistic `is_unlocked` boolean has been completely replaced by an explicit 5-state software knowledge architecture that mathematically prevents false certainty, intercepts illegal operations, rejects automatic retries, and guarantees fail-closed context initialization.

All 15 offline test requirements specified in Section 6 have been fully implemented and verified in Test Suite 11, bringing the automated test runner to **159 / 159 passing tests (100.0%)** under strict compiler flags (`-Wall -Wextra -Werror -pedantic -std=c11 -O2`).

The hardware execution gate in `bin/fw9369-hw-tool` was independently re-audited and confirmed to remain **STRICTLY CLOSED**, unconditionally halting execution with exit code 1 prior to any device access.

However, because software cannot manufacture hardware telemetry where none exists in silicon (the FT9362 provides no readable lock status register and PCH Pad 14 cannot be pulsed from userspace), physical hardware operations remain subject to fundamental platform limitations.

### Authoritative Final Verdict:

$$\mathbf{STATE\ MODEL\ HARDENING\ PASSED\ WITH\ BLOCKING\ LIMITATIONS}$$

> [!CAUTION]
> **ZERO HARDWARE EXECUTION IS AUTHORIZED.**
> The hardware gate in `bin/fw9369-hw-tool` remains **STRICTLY CLOSED**.
> The SPI character device `/dev/spidev1.0` remains **ABSENT**.
> The target SPI device `spi-FTE3600:00` remains **UNBOUND**.
> Do not execute any hardware operation. Await explicit operator direction.

---

## 2. Synthesis of Stage 2C.5C.2 Deliverables

| Deliverable Report | Core Contributions & Technical Proofs | Status |
| :--- | :--- | :---: |
| **1. Lock State Model**<br>[`STAGE2C5C2_LOCK_STATE_MODEL.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C5C2_LOCK_STATE_MODEL.md) | - Implemented `enum fw9369_lock_state` with 5 explicit states (`UNKNOWN=0`, `ASSUMED_LOCKED`, `UNLOCK_COMMAND_SENT`, `ASSUMED_UNLOCKED`, `LOCK_COMMAND_SENT`).<br>- Documented epistemic boundary: software knowledge states vs. physical silicon reality.<br>- Gated analog trim bank (`0x00C0..0x00C2`) strictly behind `ASSUMED_UNLOCKED`. | **VERIFIED** |
| **2. Uncertain Transfer Analysis**<br>[`STAGE2C5C2_UNCERTAIN_TRANSFER_ANALYSIS.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C5C2_UNCERTAIN_TRANSFER_ANALYSIS.md) | - Analyzed wire dynamics: negative ioctl does not prove command non-reception.<br>- Formulated Dual Failure Principle: both failed unlocks and failed locks transition to `UNKNOWN`.<br>- Enforced zero automatic retries to prevent out-of-sync command fragmentation. | **VERIFIED** |
| **3. Context Recreation Safety**<br>[`STAGE2C5C2_CONTEXT_RECREATION_SAFETY.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C5C2_CONTEXT_RECREATION_SAFETY.md) | - Eliminated `calloc()` heap initialization vulnerability.<br>- Enforced Invariant 1: all new contexts begin unconditionally in `UNKNOWN`.<br>- Proved protected writes are blocked after context recreation until state is synchronized. | **VERIFIED** |
| **4. Cleanup & Failure Review**<br>[`STAGE2C5C2_CLEANUP_AND_FAILURE_REVIEW.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C5C2_CLEANUP_AND_FAILURE_REVIEW.md) | - Audited 6 termination scenarios.<br>- Struck down "guaranteed RAII relock" myth: abnormal termination (`SIGKILL`, crash, power loss) bypasses userspace completely.<br>- Confirmed ACPI S5 cold reboot is the only physical recovery after uncatchable aborts. | **VERIFIED** |
| **5. Documentation Corrections**<br>[`STAGE2C5C2_DOCUMENTATION_CORRECTIONS.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C5C2_DOCUMENTATION_CORRECTIONS.md) | - Corrected 6 specific claims: Pad 229 electrical risk, S5 15-second universal reset, State D hard reboot certainty, literal atomicity, context allocation proof, failed unlock reception proof.<br>- Preserved historical Stage 2A physical evidence. | **VERIFIED** |
| **6. Offline Test Report**<br>[`STAGE2C5C2_OFFLINE_TEST_REPORT.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C5C2_OFFLINE_TEST_REPORT.md) | - Added Test Suite 11 (22 assertions) testing all 15 uncertainty scenarios.<br>- Expanded test suite to **159 / 159 passing tests (100.0%)** with 0 warnings. | **VERIFIED** |

---

## 3. Audit of the Hardware Execution Gate

An independent adversarial inspection of the hardware tool was conducted:

1. **Gate Source Code:** Lines 76–79 in [`tools/fw9369-mock/src/fw9369_main_hw.c`](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_main_hw.c#L76-L79):
   ```c
   printf("\n[SECURITY ALERT] Stage 2C.5B is static preparation only.\n");
   printf("[SAFETY GATE PASS] Physical device access is blocked by policy.\n");
   printf("To verify hardware transport mechanics offline, execute bin/fw9369-tests.\n");
   return 1;
   ```
2. **Binary Execution Verification:**
   ```bash
   bin/fw9369-hw-tool; echo "Exit code: $?"
   # Exits with code 1; zero system calls issued to /dev/spidev*
   ```
3. **No Enablement Mechanism Added:**
   - Static analysis confirmed zero `getenv()` calls.
   - Zero CLI flags enable hardware mode.
   - The hardware gate remains **100% fail-closed**.

---

## 4. List of Unresolved Issues & Blocking Limitations

The operator must recognize that while software hardening is complete, the following six platform-level limitations cannot be resolved in software alone:

1. **Unverified Linux Kernel Timeout in `spi-pxa2xx`:**
   Upstream driver blocks on `wait_for_completion(&done)` in `TASK_UNINTERRUPTIBLE` (state `D`). If the SPI controller hangs on physical hardware, the userspace process locks permanently and cannot be killed.
2. **Unverified Continuous CS# Assertion for 10,246 Bytes:**
   Reading an image baseline in Phase 4 requires shifting 10,246 bytes under continuous CS# low for 82 ms. PCH DMA latency or FIFO stalls could deassert CS# high, resetting the FT9362 internal read pointer.
3. **Kernel Module Parameter Prerequisite (`spidev.bufsiz=65536`):**
   The upstream kernel default buffer size of 4,096 B rejects the Phase 4 frame transfer with `-EMSGSIZE`. Changing this requires root privileges to reload `spidev`.
4. **No In-Band Physical Lock Telemetry:**
   Register `0x9A` is write-only. Software can track its own transmissions (`ASSUMED_LOCKED`, `ASSUMED_UNLOCKED`), but physical silicon latch state remains fundamentally unverifiable in-band.
5. **No Userspace Hardware Reset Pulse:**
   PCH Pad 14 (reset) is firmware-locked high (`0x84000201 [LOCKED]`). Software cannot issue a hardware reset pulse to recover a wedged FT9362.
6. **Unknown Silicon Starting State Since Stage 2A:**
   Volatile register `0xC6` was written `0x01` in Stage 2A. Without an authentic ACPI S5 cold reboot, the sensor internal logic state is physically unknown.

---

## 5. Final Authoritative Verdict

$$\mathbf{STATE\ MODEL\ HARDENING\ PASSED\ WITH\ BLOCKING\ LIMITATIONS}$$

**Gate Status:** **CLOSED.**  
**Hardware Mode:** **DISABLED.**  
**Awaiting Explicit Operator Direction.**
