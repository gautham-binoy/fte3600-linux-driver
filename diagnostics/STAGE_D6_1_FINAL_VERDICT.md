# Stage D6.1 — Final Audit Verdict and Verification Synthesis

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Hardware:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`, Intel LPSS SPI2)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Document Ref:** `diagnostics/STAGE_D6_1_FINAL_VERDICT.md`  
**Audit Classification:** `BLOCKED — UNSAFE OR UNVERIFIED INITIALIZATION OPERATION`

---

## 1. Executive Synthesis

This report provides the comprehensive synthesis of the independent adversarial audit of Stage D6.

While the Stage D6 implementation cleanly separated driver state management, preserved Option 1 live driver invariants (`transfers=3, bytes=19`), maintained UAPI backward compatibility, and passed all 50 userspace mock unit tests, **the implementation cannot be accepted as verified safe for physical hardware execution**.

The audit uncovered two critical blocking technical issues and one major safety contradiction:
1. **Command Strobe Wire Length Defect:** In `kernel/fte3600.c:241-246`, command strobes (`fte3600_spi_cmd_strobe`) transmit **2 bytes** (`len = 2`). Every single factory driver file (`fw9369_startup.c:65`, `fw9369_fdt.c:61`, `capture.c:66`, `ft93xx.c:177`) and the factory test mock (`test-fw9369-backend.c:308, 347, 359`) transmit **3 bytes** (`{ cmd, complement, 0x00 }`) and explicitly assert `length == 3`. Physical silicon requires this 3rd byte for SPI clocking.
2. **Unverified Silicon W1C Semantics for `0x1A84`:** The blanket write of `0xFFFF` to `0x1A84` (Steps 1 and 8) relies on Write-1-to-Clear behavior established only in a userspace mock test (`test-fw9369-backend.c:473`). Writing '1' to undocumented upper bits 6–15 remains completely unverified on physical silicon.
3. **Contradicted Non-Volatile Memory Claims:** The D6 safety report claimed non-volatile memory was "verified non-existent". However, the codebase proves that on-chip OTP / eFuse memory does exist and is actively clocked during Steps 5 and 6 (`efuse_byte` in `ft93xx.c`, `discover_otp` in `discovery.c`).

---

## 2. Verification of the Eight-Step Startup Sequence

The implementation of `fte3600_hw_startup()` in `kernel/fte3600.c:474-596` was audited step-by-step:

| Step # | Operation Name | Implementation Function & Source Line | Transmitted Wire Bytes (Hex) | Expected Response | Delay / Timeout | Failure Behavior | Source Evidence | Confidence Classification |
| :---: | :--- | :--- | :--- | :--- | :---: | :--- | :--- | :---: |
| **1** | Pre-wake IRQ Clear | `fte3600_spi_write_reg16(data, 0x1A84, 0xFFFF)`<br>([`fte3600.c:490`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L490)) | `05 FA 9A 84 00 01 FF FF` (8 B) | SPI ACK (`ret == 0`) | None | `goto err;`<br>State $\rightarrow$ `UNKNOWN` | `fw9369_startup.c:343` | `SOURCE-OBSERVED BUT HARDWARE SEMANTICS UNVERIFIED` |
| **2** | Wake Strobe 1 | `fte3600_spi_cmd_strobe(data, 0x5A, 0xA5)`<br>([`fte3600.c:497`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L497)) | `5A A5` (2 B)<br>*(Defect: Factory code sends 3 B)* | None (Write-only) | None | `goto err;`<br>State $\rightarrow$ `UNKNOWN` | `fw9369_startup.c:106` | `DEFECTIVE WIRE FRAMING` |
| **3** | Stabilization Wait | `usleep_range(1000, 1500)`<br>([`fte3600.c:504`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L504)) | — | — | 1.0–1.5 ms | None | `fw9369_startup.c:107` | `VERIFIED TIMING` |
| **4a** | Status 0x80 Poll | `fte3600_spi_read_sfr(data, 0x80, &status_80)`<br>([`fte3600.c:507`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L507)) | `08 F7 80 00 00` (5 B) | `rx[4] == 0x50` (Idle) | None | If `!= 0x50`, branch to 4b | `fw9369_startup.c:108` | `VERIFIED READ-ONLY` |
| **4b** | Abort Strobe (Fallback) | `fte3600_spi_cmd_strobe(data, 0xC0, 0x3F)`<br>([`fte3600.c:517`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L517)) | `C0 3F` (2 B)<br>*(Defect: Factory code sends 3 B)* | None | 1.0–1.5 ms delay, then retry read | If retry `!= 0x50`, `goto err;` $\rightarrow$ `UNKNOWN` | `fw9369_startup.c:110` | `DEFECTIVE WIRE FRAMING` |
| **5** | Wake Strobe 2 | `fte3600_spi_cmd_strobe(data, 0xA5, 0x5A)`<br>([`fte3600.c:532`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L532)) | `A5 5A` (2 B)<br>*(Defect: Factory code sends 3 B)* | None | None | `goto err;`<br>State $\rightarrow$ `UNKNOWN` | `fw9369_startup.c:113` | `DEFECTIVE WIRE FRAMING` |
| **6** | Foundry Interrogation | 2 iterations of `{ 0xF1=idx, 0xF4=0xC0, 0xC1, 0xC0, 0xC0, read 0xF3 }`<br>([`fte3600.c:539-560`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L539-L560)) | Total 50 B across 12 SPI transfers | All SPI ACKs | None | `goto err;`<br>State $\rightarrow$ `UNKNOWN` | `fw9369_startup.c:347-355` | `SOURCE-OBSERVED BUT HARDWARE SEMANTICS UNVERIFIED (OTP CLOCKING)` |
| **7** | Read Foundry Code | `fte3600_spi_read_sfr(data, 0x9B, &fab_byte)`<br>([`fte3600.c:565`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L565)) | `08 F7 9B 00 00` (5 B) | Up to 10 polls: `(rx[4] >> 2) == 0x13` | None | `goto err;`<br>State $\rightarrow$ `UNKNOWN` | `fw9369_startup.c:357-369` | `VERIFIED READ-ONLY` |
| **8** | Post-wake IRQ Clear | `fte3600_spi_write_reg16(data, 0x1A84, 0xFFFF)`<br>([`fte3600.c:580`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L580)) | `05 FA 9A 84 00 01 FF FF` (8 B) | SPI ACK (`ret == 0`) | None | `goto err;`<br>State $\rightarrow$ `UNKNOWN` | `fw9369_startup.c:371` | `SOURCE-OBSERVED BUT HARDWARE SEMANTICS UNVERIFIED` |
| — | Transition to READY | `data->state = FTE3600_STATE_READY;`<br>([`fte3600.c:585`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L585)) | None (Internal) | State is 4 | None | Unreachable if any step failed | Architecture specification | `VERIFIED FAIL-CLOSED` |

### 2.1 Verification of Fail-Closed Behavior
- **Audit Result: CONFIRMED.**
- Every single SPI transaction in `fte3600_hw_startup()` checks `ret`.
- If `ret != 0`, the driver unconditionally jumps to `err:`, sets `data->state = FTE3600_STATE_UNKNOWN`, and records `data->last_error = (u32)(-ret)`.
- The transition to `FTE3600_STATE_READY` occurs strictly at line 585 after Step 8 completes cleanly.
- It is physically and logically impossible for an interrupted, aborted, or failing startup sequence to report `READY`.

---

## 3. UAPI and State Machine Verification

1. **Legacy Compatibility:** In `fte3600_ioctl_common()`, calling `FTE3600_IOC_INIT` with `flags = 0` from `RESET_SEQUENCE_ISSUED` executes only `fte3600_hw_identify()` and transitions to `IDENTIFIED` (state 8). Stage C behavior is 100% preserved.
2. **Invalid State Protection:** `FTE3600_INIT_FLAG_FULL_STARTUP` is gated strictly by `data->state == FTE3600_STATE_RESET_SEQUENCE_ISSUED`. Calling it from `UNINITIALIZED`, `INITIALIZING`, `READY`, `UNKNOWN`, or `RESETTING` returns `-EPERM`.
3. **Structure Integrity:** `sizeof(struct fte3600_init_params)` is verified to be 32 bytes via compile-time `static_assert`. Ioctl number `0xc0204603` is unchanged.
4. **Reserved Fields:** All 6 reserved fields (`reserved[0..5]`) are explicitly checked for 0, returning `-EINVAL` on non-zero.
5. **State Clamping:** `fte3600_state_to_uapi()` strictly confines all returned public states to `[0..8]`. Internal state 13 (`DEVICE_REMOVAL`) is mapped to `0 (UNINITIALIZED)` with `FTE3600_FLAG_OFFLINE` asserted.
6. **Concurrency & Lifetime:** `INIT` and `RESET` are serialized under `data->lock`. Unbind marks the device offline and nullifies pointers before releasing locks, fully preserving the Stage D5.4 lifetime guarantees.

---

## 4. Mock Test Accuracy Audit (Suite 8)

All 12 Suite 8 tests in `kernel/fte3600-test.c` were evaluated:

| Test # | Test Function Name | Code Actually Executed | SPI Mock Behavior | Byte Precision | Delay Simulation | Scope of Validation |
| :---: | :--- | :--- | :--- | :---: | :---: | :--- |
| **1** | `test_d6_startup_scenario_1_smic_fab_aa` | Userspace harness `sim_driver_hw_startup()` | Returns 0x4C for 0x9B; returns 0 for writes | Counts 18 transfers | Omitted | Validates simulation state flow; does not execute kernel module |
| **2** | `test_d6_startup_scenario_2_non_smic_default` | Userspace harness `sim_driver_hw_startup()` | Returns 0x00 for 0x9B; returns 0 for writes | Counts 18 transfers | Omitted | Validates non-SMIC fallback period (200 µs) |
| **3** | `test_d6_startup_scenario_3_status_80_recovery` | Userspace harness `sim_driver_hw_startup()` | Returns 0x30 on first 0x80 read, 0x50 on retry | Counts 20 transfers | Omitted | Validates abort fallback branching logic |
| **4** | `test_d6_startup_scenario_4_status_80_persistent_failure` | Userspace harness `sim_driver_hw_startup()` | Returns 0xFF on first and retry 0x80 read | Counts 5 transfers | Omitted | Validates -EIO fail-closed transition to UNKNOWN |
| **5** | `test_d6_startup_scenario_5_irq_clear1_failure` | Userspace harness `sim_driver_hw_startup()` | Injects -EIO on Step 1 | Counts 1 transfer | Omitted | Validates error exit on initial IRQ clear |
| **6** | `test_d6_startup_scenario_6_wake1_failure` | Userspace harness `sim_driver_hw_startup()` | Injects -ETIMEDOUT on Step 2 | Counts 2 transfers | Omitted | Validates timeout exit on wake strobe |
| **7** | `test_d6_startup_scenario_7_loop_failure` | Userspace harness `sim_driver_hw_startup()` | Injects -EIO on Step 6 loop write | Counts 5 transfers | Omitted | Validates failure abort during interrogation loop |
| **8** | `test_d6_startup_scenario_8_irq_clear2_failure` | Userspace harness `sim_driver_hw_startup()` | Injects -EIO on Step 8 | Counts 18 transfers | Omitted | Validates failure abort on final IRQ clear |
| **9** | `test_d6_startup_scenario_9_precondition_enforcement` | Userspace harness `sim_driver_ioctl_init()` | None (Rejected before SPI) | 0 transfers | Omitted | Validates precondition state filtering |
| **10** | `test_d6_startup_scenario_10_full_startup_flag_combined` | Userspace harness `sim_driver_ioctl_init()` | Combines identify profile + startup profile | Counts 22 transfers | Omitted | Validates single-call full startup |
| **11** | `test_d6_startup_scenario_11_removal_concurrency` | Userspace harness `sim_driver_ioctl_init()` | Device marked offline before call | 0 transfers | Omitted | Validates -ENODEV offline bail-out |
| **12** | `test_d6_startup_scenario_12_blocked_operations_absent` | Static compile-time & runtime assertions | Evaluates wire address & opcode constants | Direct C constants | None | Validates exclusion of threshold bank & touch strobe |

### 4.1 Mock Test Assessment
- **Finding:** The Suite 8 mock tests successfully verify the algorithmic state machine and transfer counts of the userspace simulation model.
- **Limitation:** The mock tests do **NOT** execute the compiled `kernel/fte3600.ko` module code, do not check the third byte of command strobes, and do not validate physical silicon responsiveness.

---

## 5. Final Source Diff Review

Static audit of the full git diff confirmed:
- Zero modifications to reset timing or GPIO control.
- Zero modifications to `fte3600_hw_identify()`.
- Zero probe-time hardware transactions.
- Zero automatic touch arming (`{0xC2, 0x3D}` remains completely excluded).
- Zero frame capture logic (`FTE3600_IOC_CAPTURE` returns `-EOPNOTSUPP`).
- Zero safety-gate weakening in userspace tools.
- Zero ABI layout regressions.
- Full retention of Stage D5.4 devres nullification and object lifetime design.

---

## 6. Final Classification & Verdict

In accordance with the required classifications:

**FINAL CLASSIFICATION:**
### `BLOCKED — UNSAFE OR UNVERIFIED INITIALIZATION OPERATION`

### Detailed Blocking Reasons:
1. **Defective Command Strobe Length (2 Bytes vs 3 Bytes):** The Stage D6 implementation transmits only 2 bytes for command strobes (`0x5A/0xA5`, `0xA5/0x5A`, `0xC0/0x3F`). All factory source files and test harness mocks mandate 3 bytes (`{cmd, complement, 0x00}`). On physical silicon, omitting the 3rd byte risks clock desynchronization and command rejection.
2. **Unverified Silicon Semantics for Register `0x1A84`:** Blindly writing `0xFFFF` to an undocumented register without authoritative silicon proof of W1C behavior risks undefined side effects on reserved bits 6–15.
3. **Contradicted Safety Claim Regarding Non-Volatile Memory:** The Stage D6 claim that non-volatile memory is non-existent is contradicted by the on-chip OTP / eFuse macro actively clocked during Steps 5 and 6.

---

## 7. Remediation Roadmap for Stage D6.2

Before physical hardware validation can be authorized:
1. **Fix Command Strobe Wire Framing:** Update `fte3600_spi_cmd_strobe()` to transmit 3 bytes: `{ cmd, complement, 0x00 }` with `len = 3`.
2. **Harden `0x1A84` Interrupt Acknowledgment:** Replace blanket `0xFFFF` writes with an observed-status read-and-acknowledge sequence (`status & 0x003F`), avoiding writes to reserved bits.
3. **Correct Safety Documentation:** Formally update safety documentation to acknowledge on-chip OTP / eFuse presence while noting that startup access is intended as read-only sensing.
