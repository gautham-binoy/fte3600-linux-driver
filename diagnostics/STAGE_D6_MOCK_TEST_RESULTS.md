# Stage D6 — Mock Test Execution Results

**Project:** `/home/titan/fte3600-linux-driver/`  
**Test Harness:** `kernel/fte3600-test.c` (Standalone Userspace Simulation)  
**Execution Timestamp:** 2026-11-04 17:13:47 UTC  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Result:** 50 / 50 Unit Tests Passed (100.0%) | 9 / 9 Policy Groups Passed  
**Document Ref:** `diagnostics/STAGE_D6_MOCK_TEST_RESULTS.md`  

---

## 1. Test Suite Summary Table

| Suite # | Suite Description | Tests Run | Passed | Failed | Pass Rate |
| :---: | :--- | :---: | :---: | :---: | :---: |
| **Suite 1** | UAPI & Data Structure Alignment | 3 | 3 | 0 | 100.0% |
| **Suite 2** | Protocol CRC Vector Validation | 1 | 1 | 0 | 100.0% |
| **Suite 3** | State Machine Transitions & Invariants | 1 | 1 | 0 | 100.0% |
| **Suite 4** | Mandatory Lifecycle & Concurrency Scenarios | 11 | 11 | 0 | 100.0% |
| **Suite 5** | UAPI Validation & Parameter Rejections | 1 | 1 | 0 | 100.0% |
| **Suite 6** | Chip Identification & Completion Semantics | 17 | 17 | 0 | 100.0% |
| **Suite 7** | State Mapping, UAPI Isolation & Poll Verification | 4 | 4 | 0 | 100.0% |
| **Suite 8** | FT9362 Volatile Startup Verification (Stage D6) | 12 | 12 | 0 | 100.0% |
| **Total** | **Unified FTE3600 Driver Test Suite** | **50** | **50** | **0** | **100.0%** |

---

## 2. Detailed Results for Suite 8 (Stage D6 Volatile Startup)

Suite 8 specifically validates all aspects of the FT9362 volatile startup routine implemented in Stage D6:

| # | Test Name | Invariants Verified | Result |
| :---: | :--- | :--- | :---: |
| **1** | `test_d6_startup_scenario_1_smic_fab_aa` | Normal startup with SMIC Fab AA (`0x4C`): detects `foundry_code = 0xAA`, configures `line_period_us = 150`, transitions from `IDENTIFIED` to `READY`, exactly 18 transfers. | **PASS** |
| **2** | `test_d6_startup_scenario_2_non_smic_default` | Normal startup with non-SMIC silicon (`0x00`): detects `foundry_code = 0x00`, configures `line_period_us = 200`, transitions to `READY`, exactly 18 transfers. | **PASS** |
| **3** | `test_d6_startup_scenario_3_status_80_recovery` | Status 0x80 non-idle recovery: initial read returns `0x30` $\rightarrow$ issues abort strobe `{ 0xC0, 0x3F }` $\rightarrow$ retries read of 0x80 (returns `0x50`) $\rightarrow$ transitions to `READY`, exactly 20 transfers. | **PASS** |
| **4** | `test_d6_startup_scenario_4_status_80_persistent_failure` | Persistent non-idle fault: retry returns `0xFF` $\rightarrow$ aborts with `-EIO`, transitions state to `UNKNOWN`, asserts `last_error = EIO`. | **PASS** |
| **5** | `test_d6_startup_scenario_5_irq_clear1_failure` | Step 1 pre-wake IRQ clear failure: simulated SPI bus error (`-EIO`) causes immediate fail-closed transition to `UNKNOWN`. | **PASS** |
| **6** | `test_d6_startup_scenario_6_wake1_failure` | Step 2 wake strobe failure: simulated timeout (`-ETIMEDOUT`) transitions to `UNKNOWN`. | **PASS** |
| **7** | `test_d6_startup_scenario_7_loop_failure` | Step 6 foundry interrogation loop failure: simulated SPI write error during sense-amp cycling transitions to `UNKNOWN`. | **PASS** |
| **8** | `test_d6_startup_scenario_8_irq_clear2_failure` | Step 8 post-interrogation IRQ clear failure: simulated SPI write error transitions to `UNKNOWN`. | **PASS** |
| **9** | `test_d6_startup_scenario_9_precondition_enforcement` | Calling startup from invalid states (`UNINITIALIZED`, `RESETTING`, `UNKNOWN`, `READY`) is strictly rejected with `-EPERM`. | **PASS** |
| **10** | `test_d6_startup_scenario_10_full_startup_flag_combined` | Single-stage initialization: calling INIT with `FTE3600_INIT_FLAG_FULL_STARTUP` from `RESET_SEQUENCE_ISSUED` executes identification (3 transfers) and startup (18 transfers), transitioning directly to `READY` (total 22 operations). | **PASS** |
| **11** | `test_d6_startup_scenario_11_removal_concurrency` | Offline concurrency: when device removal occurs prior to startup, INIT immediately returns `-ENODEV` without touching hardware. | **PASS** |
| **12** | `test_d6_startup_scenario_12_blocked_operations_absent` | Static assertion and wire framing check: verifies threshold bank wire address is `0x00E0`, opcodes match protocol, and hazardous operations are completely excluded from startup path. | **PASS** |

---

## 3. Policy Test Suite Results

The architectural policy validation (`fte3600-policy-test`) verified:
- **Group 1:** ACPI / Pad 229 IRQ configuration constraints.
- **Group 2:** Intel LPSS SPI2 CS0 chip-select timing and capability.
- **Group 3:** SPI clock polarity (Mode 0: CPOL=0, CPHA=0).
- **Group 4:** Exclusive character device lease semantics.
- **Group 5:** Power management lifecycle barriers.
- **Group 6:** Clean removal and pointer nullification order.
- **Group 7:** Public UAPI boundary confinement (`[0..8]`).
- **Group 8:** ABI struct 32-byte alignment.
- **Group 9:** Lockdep and concurrency invariants.

**Result:** `FTE3600 glue policy: 9 groups passed`.
