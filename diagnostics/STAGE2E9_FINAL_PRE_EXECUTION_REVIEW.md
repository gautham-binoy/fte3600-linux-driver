# Stage 2E.9 Final Pre-Execution Safety Review and Authorization Readiness

**Target System:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Target SPI Device:** `spi-FTE3600:00` on Intel LPSS SPI2 (`pxa2xx-spi.2`, CS0)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (x86_64, SMP PREEMPT_DYNAMIC)  
**Task Type:** Static-Only Final Safety Review and Authorization Readiness Assessment  
**Status:** **STAGE B HARDWARE EXECUTION ON HOLD — AUTHORIZATION NOT GRANTED BY THIS TASK**

---

## 1. Executive Summary & Safety Restrictions

This document provides a final, independent, static-only pre-execution safety review of the complete Stage B codebase and host environment prior to any human authorization decision. It synthesizes and verifies the findings of the five preceding independent audits (Stages 2E.8.1 through 2E.8.5).

### Strict Safety Boundaries Enforced During This Review
- **No hardware execution:** Neither `scripts/stage-b-test.sh` nor `scripts/run-stage-b-in-terminal.sh` was launched.
- **No kernel module loading or removal:** `insmod`, `modprobe`, `rmmod`, and `delete_module()` were strictly prohibited.
- **No SPI binding or unbinding:** `spi-FTE3600:00` remained untouched.
- **No device node access or ioctls:** `/dev/fte3600` was not opened, and no ioctl was issued.
- **No GPIO, MMIO, or bus operations:** Pad 14 and Pad 229 were not toggled; no SPI transactions were performed.
- **No source code modifications:** Zero kernel, tool, or script source files were modified.
- **No git commits:** The git repository state was preserved without commits.

---

## 2. Audited File Inventory and Cryptographic Integrity Snapshot

Every component involved in Stage B execution was inspected and hashed to establish a definitive cryptographic review baseline.

### 2.1 Component Hash Table

| Component File | Size (Bytes) | SHA-256 Digest | Status vs Prior Audits |
| :--- | :--- | :--- | :--- |
| `kernel/fte3600.c` | 24,015 | `29d2726c9a63772f0981bf65321f48934882eeb6702fe4a02629794b410805e8` | **MATCHED** (Identical to 2E.8.1–2E.8.5) |
| `kernel/fte3600.h` | 3,171 | `1ffca1a4e04fad026efbfde409ed0fdb67508ac57b3944aaf1cc57f3dbc35754` | **MATCHED** (Identical to 2E.8.1–2E.8.5) |
| `tools/fte3600-tool.c` | 2,746 | `1b021a4d7f4770e8a627d6cd29116859dae908e2e5bf10739c0d05253d31d4f7` | **MATCHED** (Identical to 2E.8.1–2E.8.5) |
| `bin/fte3600-tool` | 16,920 | `b91d7dc3cb0ca172d7020613af884b649c451be79a55e1836d31dc6f66217d12` | **MATCHED** (Verified compiled ELF binary) |
| `tools/fw9369-mock/src/fw9369_main_hw.c` | 2,568 | `0ef7e497c6ab20adbda11a5d3e435cb51f14e185d6247ab6c56279c55bf38a83` | **MATCHED** (Legacy safety gate source) |
| `bin/fw9369-hw-tool` | 16,776 | `576874d860c3ee27fa5fcf2472d7ec4633df3c68c2eaffecce5ea1b930ba119b` | **MATCHED** (Legacy safety gate binary) |
| `scripts/stage-b-test.sh` | 51,757 | `d7b08c4508b1acf1554b382eb7523fa96989bfba3e8fb190c1d72fb4976256f5` | **MATCHED** (Identical to 2E.8.1–2E.8.5) |
| `scripts/run-stage-b-in-terminal.sh` | 1,489 | `0d45f94459124430c2b64c9bef9d820c22c984032a959a0d20b88248ed9b4e27` | **MATCHED** (Identical to 2E.8.1–2E.8.5) |
| `scratch/test_synthetic_suite.py` | 39,268 | `49bc015b383fb930f6fad094bd98a9f0b567356cbafaffa26e4d06d800474398` | **MATCHED** (27-test offline test harness) |
| `PROGRESS.md` | 114,888 | `83278ea120a78c8ab8be6c8d5ce44bdd06b62fce62a599fb06245744ec8ae584` | **MATCHED** (Project tracking baseline) |

### 2.3 Prior Audit Reports Baseline
- `diagnostics/STAGE2E8_1_INDEPENDENT_IMPLEMENTATION_AUDIT.md`: `c6bc4d35241fb849ebdca7f8e850935c459ebedb9a7aaa37e971e5fcbb836749`
- `diagnostics/STAGE2E8_2_AUDIT_CORRECTIONS.md`: `312b69b9ec474db687500a74c2e92dbe1e88dc89a7eee1b43b4ca8a0f77c47dd`
- `diagnostics/STAGE2E8_3_KERNEL_RESET_PATH_AUDIT.md`: `6269bb8f04bcdd50a920c67602e2800ec649f3268088782de9a8948832c5161e`
- `diagnostics/STAGE2E8_4_DRIVER_LIFETIME_AUDIT.md`: `226614b517a77c9db328344b5cbcf173c7dc8b9593a8c3c511913ebb22ea2cfe`
- `diagnostics/STAGE2E8_5_INFLIGHT_OPEN_REMOVE_RACE_AUDIT.md`: `2e6bbd5915fc3dc3cd6b838b57a1ac222e9e01538d9bc39c2778c6e7cfab1837`

**Integrity Verification:** 100% of all audited components match their previously audited source states. Zero components are marked as unverified.

---

## 3. Stage B Execution Sequence Verification

The entire reachable call graph from [`scripts/run-stage-b-in-terminal.sh`](file:///home/titan/fte3600-linux-driver/scripts/run-stage-b-in-terminal.sh) through [`scripts/stage-b-test.sh`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh) was re-traced. The exact execution sequence remains strictly bounded:

1. **Read-Only Preflight Checks (Phase 1, lines 1037–1205):**
   - Check 0: Root UID validation (`id -u == 0`).
   - Check 1: Kernel module artifact existence (`kernel/fte3600.ko`).
   - Check 2: Cooperative locking (`flock -n 200 /var/lock/fte3600-stage-b.lock`).
   - Check 3: Intel LPSS SPI2 controller presence (`pxa2xx-spi.2`).
   - Check 4: PCH LPSS GPIO controller presence (`INT34C8:00`).
   - Check 5: Read-only debugfs inspection of PCH Pad 14 and Pad 229.
   - Check 6: Executable verification of validation utility (`bin/fte3600-tool`).
   - Check 7: Legacy userspace safety gate audit (`bin/fw9369-hw-tool` sealed status 1 verified).
   - Check 8: Initial module presence audit via `query_module_state` (detects external modules).
2. **Controlled Module Insertion (Phase 2, lines 1208–1296):**
   - Signal traps deferred around `insmod kernel/fte3600.ko`.
   - Post-insertion state queried; traps restored.
   - `THIS_INVOCATION_INSERTED=1` assigned; device node identity strictly verified against sysfs major:minor.
3. **Telemetry Snapshot 1 (Phase 3, lines 1308–1311):**
   - Invokes `bin/fte3600-tool get-state` to query initial state via `FTE3600_IOC_GET_STATE`.
4. **Controlled Single RESET IOCTL (Phase 4, lines 1313–1326):**
   - Invokes `bin/fte3600-tool reset` **exactly once**.
   - Issues `FTE3600_IOC_RESET` on `/dev/fte3600`.
   - Kernel executes `fte3600_hw_reset_pulse()`: Pad 14 asserted LOW for 20 ms with 10 ms settle intervals.
   - Captures `RESET_RC`. Zero retry loops exist.
5. **Telemetry Snapshot 2 (Phase 5, lines 1328–1340):**
   - Invokes `bin/fte3600-tool get-state` to record post-reset state.
6. **Userspace File Descriptor Release:**
   - `bin/fte3600-tool` closes `/dev/fte3600` immediately upon return ([line 101](file:///home/titan/fte3600-linux-driver/tools/fte3600-tool.c#L101)).
7. **Read-Only Post-Test Diagnostics (lines 1342–1360):**
   - Reads Pad 14 and Pad 229 post-test state from debugfs.
   - Captures kernel ring buffer messages (`dmesg`).
   - Resolves final exit status preserving primary reset failure.
8. **Option 1 Post-Test Resource Release Handler (`cleanup_handler`, lines 895–1026):**
   - Read-only audit of module, character device, and SPI binding.
   - Read-only open descriptor audit (`check_open_descriptors`) without killing processes.
   - Emits mandatory Option 1 policy banners.
   - Releases cooperative lock (FD 200).
   - **Preserves `fte3600.ko` loaded and bound in kernel.**

**Verification Confirmation:** Zero hidden commands, secondary resets, or undocumented writes exist before, during, or after this sequence.

---

## 4. Independent Safety Invariant Verification

| Invariant Requirement | Implementation Verification | Status |
| :--- | :--- | :--- |
| **No automatic `rmmod`** | Automated AST and regex scan across all reachable scripts/tools confirmed 0 occurrences. | **VERIFIED** |
| **No `modprobe -r`** | 0 occurrences in entire repository call graph. | **VERIFIED** |
| **No `delete_module()`** | 0 occurrences in all userspace tools and scripts. | **VERIFIED** |
| **No forced unload (`-f`)** | Prohibited; zero occurrences. | **VERIFIED** |
| **No SPI driver unbind** | Zero writes to `/sys/bus/spi/drivers/fte3600/unbind` or `spi-FTE3600:00/driver/unbind`. | **VERIFIED** |
| **No process termination** | Zero `kill`, `pkill`, or `killall` calls in any cleanup or diagnostic path. | **VERIFIED** |
| **No automatic reset retry** | `bin/fte3600-tool reset` is called strictly once at line 1319 without retry or loop constructs. | **VERIFIED** |
| **No second reset ioctl** | Call graph confirmed to contain exactly one `FTE3600_IOC_RESET` invocation. | **VERIFIED** |
| **No INIT or CAPTURE ioctl** | Stage B scripts invoke exclusively `get-state` and `reset`. Zero `INIT` or `CAPTURE` calls. | **VERIFIED** |
| **No SPI transfers** | Reset path in `kernel/fte3600.c:131-158` contains zero calls to `spi_sync()` or transfers. | **VERIFIED** |
| **No sensor register access** | Zero SPI commands dispatched; sensor CS0 line remains deasserted HIGH. | **VERIFIED** |
| **No firmware loading** | `request_firmware()` does not exist anywhere in `kernel/fte3600.c`. | **VERIFIED** |
| **No manual Pad register writes**| Pinctrl pad registers are read-only inspected via `/sys/kernel/debug/pinctrl`; no MMIO writes. | **VERIFIED** |
| **No safety gate bypass** | Legacy safety gate `bin/fw9369-hw-tool` is executed and audited in Pre-flight Check 7 (sealed). | **VERIFIED** |
| **No background processes** | Process table verified clean; script runs strictly in foreground terminal window. | **VERIFIED** |

---

## 5. Option 1 Cleanup Policy Verification

The Option 1 policy ("Never Automatically Unload the Fingerprint Driver") was audited line-by-line in [`scripts/stage-b-test.sh:895-1026`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L895-L1026):
1. **Unconditional Invariant:** The harness never automatically unloads or unbinds `fte3600.ko` under any outcome (success, failure, signal, or syntax error).
2. **Ownership Respect:** If `INITIAL_FTE3600_LOADED=1`, the harness preserves the module and explicitly prints that it does not own the module.
3. **Honest Reporting:** Interrupted insertions are flagged as `INSERTION_STATE=UNCERTAIN; DO NOT AUTOMATICALLY REMOVE`.
4. **Idempotence:** Guarded by `if [ "$CLEANUP_IN_PROGRESS" -eq 1 ]; then return 0; fi`.
5. **Resource Scope:** Cleanup closes strictly script-owned FD 200 (`exec 200>&-`). Foreign file descriptors are never touched.
6. **Read-Only Diagnostics:** `/proc/*/fd` inspection is strictly read-only; zero processes are terminated.
7. **Manual Cleanup Instructions:** When manual removal is appropriate, instructions (`sudo rmmod fte3600`) are printed to stdout for operator execution.

---

## 6. Reset Failure and Interruption Handling Verification

1. **Single Execution:** Executed at [`scripts/stage-b-test.sh:1319`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L1319) under `set +e`.
2. **Zero Retries:** No timeout retry, signal retry, error retry, or retry on missing post-reset telemetry.
3. **Interruption Classification:** Exit status $rc \ge 128$ is formally recognized as ambiguous between pre-ioctl signal termination and post-pulse signal delivery; classified as:
   $$\text{\bf RESET OUTCOME UNCERTAIN — DO NOT RETRY}$$
4. **Electrical Boundary Acknowledgment:** Successful return code 0 proves register writes were dispatched to the Intel PCH LPSS GPIO controller; it does not claim proof of physical voltage on Pad 14 or sensor silicon health.
5. **Secondary Operation Immunity:** Failure in Snapshot 2 or dmesg logging propagates exit codes without re-triggering hardware operations.

---

## 7. Current Host State Invariant Verification

Read-only system queries executed during this review verified:

| Host Property | Command / Query | Verified Value | Compliance Status |
| :--- | :--- | :--- | :--- |
| **Kernel Version** | `uname -a` | `7.2.8-9-omarchy` (x86_64) | **COMPLIANT** |
| **Driver Module** | `lsmod \| grep fte3600` | Absent (not loaded) | **COMPLIANT** |
| **Spidev Module** | `lsmod \| grep spidev` | Absent (not loaded) | **COMPLIANT** |
| **Character Device Node** | `ls -la /dev/fte3600` | Absent (`No such file or directory`) | **COMPLIANT** |
| **SPI Driver Binding** | `ls -la /sys/.../spi-FTE3600:00/driver` | Absent (`No such file or directory`) | **COMPLIANT** |
| **Legacy Safety Gate** | `bin/fw9369-hw-tool` | Exit status 1 (Gate SEALED) | **COMPLIANT** |
| **Test Background Processes**| `ps aux \| grep -E "fte3600\|stage-b"`| 0 processes running | **COMPLIANT** |

---

## 8. Offline Synthetic Test Suite Results

The offline synthetic test suite (`scratch/test_synthetic_suite.py`) was executed in an isolated environment without hardware access:
- **Total Tests Executed:** 27
- **Total Tests Passed:** 27
- **Total Tests Failed:** 0
- **Hardware Access:** Zero (strictly mocked sysfs, proc, and devtmpfs environments).
- **Module Loading:** Zero (offline function extraction via `awk`).
- **Coverage Areas:** Decimal/hex rdev conversion, bounds checking, INT64 overflow, cooperative flock contention, signal traps (INT, TERM, HUP), re-entrancy, open descriptor detection, pre-existing module protection, uncertain insertion, failed reset preservation, and static AST code inspection (Path 14).

---

## 9. Remaining Limitations and Notes

1. **Hardware Open-Circuit Blindness:** `gpiod_set_value_cansleep()` writes to GPIO controller MMIO registers with `void` return; broken PCB traces or disconnected flex cables cannot be detected at this stage.
2. **Pre-flight Check 7 Policy:** If `fte3600` is already loaded before starting, the script preserves it and sets `INITIAL_FTE3600_LOADED=1`. Operators requiring a clean probe test must ensure the module is absent prior to launching.

---

## 10. Final Recommendation and Authorization Readiness Decision

### **Selected Conclusion: Conclusion A**

$$\text{\bf Conclusion A: READY FOR HUMAN AUTHORIZATION}$$

**Technical Justification:**
1. All audited source files, binaries, and scripts match their verified baselines with 100% cryptographic integrity.
2. The Stage B execution sequence is strictly bounded to a single controlled reset with zero retries, zero SPI transfers, and zero sensor register accesses.
3. The Option 1 no-auto-unload policy is rigorously implemented across all normal, error, signal, and syntax-error paths.
4. Concurrency, open-file lifetimes, and GPIO devres protections were independently proven safe across all interleavings in Stages 2E.8.4 and 2E.8.5.
5. All 27 offline synthetic tests passed cleanly.
6. The host environment is completely clean and invariant-compliant.

---

## 11. Required Final Status Declarations

- **AUTOMATIC MODULE UNLOAD:** DISABLED
- **AUTOMATIC SPI UNBIND:** DISABLED
- **FINAL PRE-EXECUTION REVIEW:** COMPLETED
- **STAGE B HARDWARE EXECUTION:** NOT PERFORMED
- **STAGE B AUTHORIZATION:** NOT GRANTED BY THIS TASK
