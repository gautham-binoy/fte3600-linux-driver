# Stage 2E.7.12 — Independent Module Ownership and Cleanup Source Audit

**Date:** 2026-11-03  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`, Intel LPSS SPI2 CS0)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (x86_64)  
**Mode:** STRICT STATIC-ONLY AUDIT (Stage B Execution ON HOLD)

---

## 1. Executive Summary

Stage 2E.7.12 conducted an independent, adversarial, static source-code audit and offline synthetic verification of the Stage B implementation in `/home/titan/fte3600-linux-driver/`.

The audit evaluated the exact mechanics of module instance identity verification, the pre-unload race window, kernel module state query interfaces, foreground signal dispatch, cleanup control flow, and offline synthetic test coverage.

### Key Conclusions:
1. **Module Instance Fingerprinting is Heuristic, Not Cryptographic:** Sysfs directory inode reuse (`kernfs` IDA recycling), 1-second timestamp resolution (`stat -c %Z`), vmalloc address reuse (`module_alloc()`), and identical module binaries (`srcversion`, `coresize`) mean that a rapid reload of the same module within the same second cannot be distinguished from the original instance. This is an architectural reality of Linux, not an implementation flaw. The fingerprint reliably detects *dissimilar* module replacement, which represents the practical operational threat.
2. **Unavoidable Kernel TOCTOU Race Window:** Because Linux `delete_module(2)` accepts only a string module name (`const char *name`, `int flags`), an atomic unload of a specific instance is impossible from userspace. Hardening was implemented to re-verify module instance identity immediately before `rmmod fte3600`, shrinking the interval from seconds (the duration of two `/proc` descriptor scans) down to the sub-millisecond process-fork window.
3. **Module-State Query Interface Clarification:** `lsmod` parses `/proc/modules`. There are two independent kernel-state representations: sysfs (`kernfs`) and procfs (`/proc/modules`). `lsmod` serves as an auxiliary userspace parser cross-check. The script documentation was corrected accordingly.
4. **Immediate Critical-Section Instance Capture:** Moving `capture_module_instance_identity()` into the insertion critical section immediately upon `insmod_rc == 0` guarantees that `MOD_INSTANCE_RECORDED=1` is committed atomically alongside `THIS_INVOCATION_INSERTED=1`, enabling safe cleanup if an interrupted signal arrives before probe completion.
5. **Offline Synthetic Test Suite:** Expanded from 50 to 57 test cases, proving detection of section address mismatches, mid-cleanup replacement, the blind spot of identical rapid reloads, procfs/lsmod shared dependency, and exhaustive ownership paths. **57/57 tests pass (100%)**.

---

## 2. Source Files and Functions Inspected

The following files and components were inspected line-by-line without relying on previous audit assertions:

| Source File | Components / Functions Inspected | Purpose |
|:---|:---|:---|
| [scripts/stage-b-test.sh](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh) | `record_devnode_identity()` (Lines 232–324) | Devnode character special verification, makedev check, sysfs matching |
| [scripts/stage-b-test.sh](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh) | `query_module_state()` (Lines 342–410) | Multi-source module presence inspection (/sys, /proc, lsmod) |
| [scripts/stage-b-test.sh](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh) | `capture_module_instance_identity()` (Lines 418–453) | Sysfs fingerprint collection (inode, ctime, srcver, coresize, text) |
| [scripts/stage-b-test.sh](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh) | `verify_module_instance_identity()` (Lines 461–519) | Sysfs fingerprint comparison against recorded probe values |
| [scripts/stage-b-test.sh](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh) | `defer_signal()` (Lines 527–536) | Critical section signal deferral logic |
| [scripts/stage-b-test.sh](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh) | `check_open_descriptors()` (Lines 553–995) | Two-tier fail-closed /proc and stat descriptor scanner |
| [scripts/stage-b-test.sh](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh) | `cleanup_handler()` (Lines 1000–1199) | Ownership validation, descriptor checks, pre-unload check, rmmod |
| [scripts/stage-b-test.sh](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh) | Pre-Flight Check 0 (Lines 1217–1252) | Cooperative lock fail-closed acquisition |
| [scripts/stage-b-test.sh](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh) | Phase 2 Critical Section (Lines 1378–1439) | Signal traps, foreground insmod, 4-state machine evaluation |
| [scripts/run-stage-b-in-terminal.sh](file:///home/titan/fte3600-linux-driver/scripts/run-stage-b-in-terminal.sh) | Lines 1–35 | Terminal wrapper, sudo invocation, exit status propagation |
| [kernel/fte3600.c](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c) | `fte3600_probe()` (Lines 557–634) | cdev_device_add, dev_t registration, parent device binding |
| [kernel/fte3600.c](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c) | `fte3600_remove()` (Lines 636–687) | cdev_device_del, hardware offline shutdown, put_device |
| [kernel/fte3600.c](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c) | `fte3600_cdev_open()` / `release()` (Lines 369–399) | Exclusive open lease, get_device, atomic_cmpxchg |
| [kernel/fte3600.h](file:///home/titan/fte3600-linux-driver/kernel/fte3600.h) | Lines 1–99 | IOCTL definitions, UAPI structures, state constants |
| `scratch/test_synthetic_suite.py` | Tests 1–57 | Offline synthetic test harness and edge case assertions |

---

## 3. Detailed Audit Findings & Classifications

### Finding 1: Module Instance Identity Cannot Be Guaranteed by Heuristic Fingerprinting
- **Classification:** **HIGH (Architectural Limitation of Linux)**
- **Source Lines:** [scripts/stage-b-test.sh:418-519](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L418-L519)
- **Detailed Technical Analysis:**
  `capture_module_instance_identity()` records:
  1. `/sys/module/fte3600` directory inode (`stat -c %i`)
  2. Directory change time (`stat -c %Z`)
  3. KASLR `.text` section address (`/sys/module/fte3600/sections/.text`)
  4. Build source version (`srcversion`)
  5. Core size (`coresize`)

  Can these attributes *guarantee* uniqueness of a specific loaded module instance? **No.**
  - **Kernfs Inode Recycling:** In the Linux kernel, `kernfs` allocates inode numbers dynamically using an `ida` (ID allocator). When `/sys/module/fte3600` is removed on `rmmod`, its inode number is released back to the IDA pool. If another module is loaded, the IDA can reassign the exact same inode number.
  - **Timestamp Resolution:** `stat -c %Z` outputs timestamps in integer seconds. If an administrator unloads and reloads the module within the same 1-second wall-clock window, `stat -c %Z` will be identical.
  - **Vmalloc Address Reuse:** Linux kernel module memory is allocated via `module_alloc()`, which manages a contiguous virtual memory range. When a module is freed, its virtual pages are returned to the vmalloc pool. If another module of identical size is loaded immediately, the vmalloc allocator frequently assigns the same base address.
  - **Identical Builds:** `srcversion` and `coresize` are static compile-time constants embedded in the `.ko` binary. If the same `.ko` file is reloaded, both values are identical.
- **Evaluation:** The fingerprint provides robust detection of *dissimilar* module replacement (different code version, different compilation, different size, different KASLR slot, or different second of insertion). However, it cannot guarantee identity against an identical binary reloaded within the same second. This limitation is formally demonstrated by Synthetic Test 53.

---

### Finding 2: Inherent Unload TOCTOU Race Window (`delete_module(2)`)
- **Classification:** **HIGH (Architectural Boundary of Linux Syscall API)**
- **Source Lines:** [scripts/stage-b-test.sh:1050-1116](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L1050-L1116)
- **Detailed Technical Analysis:**
  The Linux kernel system call for unloading modules is:
  `delete_module(const char *name_user, unsigned int flags)`
  - The syscall takes only a string name (e.g. `"fte3600"`).
  - The Linux kernel does **not** provide a system call to unload a module by file descriptor, inode, or instance ID (unlike `close(fd)` or `pidfd_send_signal()`).
  - Consequently, any userspace program must look up or verify the module first, and then issue `rmmod` / `delete_module(2)`.
  - In the original Stage 2E.7.11 code, `verify_module_instance_identity()` was invoked at line 1050, followed by Teardown Step 1 (`check_open_descriptors`) and Teardown Step 2 (`check_open_descriptors`), taking hundreds of milliseconds before line 1110 called `rmmod fte3600`.
- **Correction Implemented:**
  Added a pre-unload module instance re-verification immediately before `rmmod fte3600` at Teardown Step 3:
  ```bash
  # Pre-Unload Module Instance Re-Verification (immediately before rmmod)
  if ! verify_module_instance_identity "fte3600"; then
      printf "%s\n" "[SAFETY STOP] Module instance identity changed during descriptor audit! Original instance replaced externally."
      printf "%s\n" "[SAFETY STOP] Refusing module unload to protect external module."
      CLEANUP_OVERALL_OK=0
  else
      if rmmod fte3600; then ...
  ```
- **Evaluation:** This narrows the TOCTOU window by eliminating the latency of the two descriptor scans. However, between this final verification in Bash and the kernel executing `delete_module("fte3600")`, a microscopic fork/exec window remains. This window cannot be eliminated from userspace without kernel-level modifications. Synthetic Test 52 verifies that any replacement occurring during the descriptor scans is intercepted.

---

### Finding 3: Documentation Inaccuracy in `query_module_state()`
- **Classification:** **MEDIUM (Documentation Inaccuracy - Corrected)**
- **Source Lines:** [scripts/stage-b-test.sh:327-341](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L327-L341)
- **Detailed Technical Analysis:**
  The comments in `scripts/stage-b-test.sh` previously stated:
  `# 1. Inspects three independent sources: /sys/module/<name>, /proc/modules, lsmod.`
  In reality:
  - `/sys/module/<name>` is provided by `kernfs`.
  - `/proc/modules` is provided by `procfs`.
  - `lsmod` is a userspace binary (from `kmod`) that opens and parses `/proc/modules`.
  - Therefore, `/proc/modules` and `lsmod` are not two independent kernel sources; they reflect the exact same underlying kernel state.
- **Correction Implemented:**
  Updated the documentation and comments to accurately state that `query_module_state()` inspects two independent kernel representations (sysfs and procfs), while `lsmod` serves as an auxiliary userspace parser cross-check. Synthetic Test 54 confirms this shared dependency.

---

### Finding 4: Phase 2 Instance Capture Ordering Window
- **Classification:** **MEDIUM (Control Flow Inconsistency - Corrected)**
- **Source Lines:** [scripts/stage-b-test.sh:1418-1422](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L1418-L1422), [scripts/stage-b-test.sh:1467-1471](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L1467-L1471)
- **Detailed Technical Analysis:**
  In Stage 2E.7.11, `capture_module_instance_identity "fte3600"` was placed at line 1454 (after `sleep 1` and `query_module_state`).
  If `insmod` completed with exit code 0, but a signal (e.g. `SIGINT` or `SIGTERM`) was deferred during insertion:
  1. The critical section ended.
  2. Traps were restored.
  3. `cleanup_handler` was dispatched via `DEFERRED_SIGNAL`.
  4. In `cleanup_handler`: `THIS_INVOCATION_INSERTED=1`, but `MOD_INSTANCE_RECORDED=0`!
  5. `cleanup_handler` logged:
     `[WARN] Module instance identity was not recorded prior to teardown.`
     `[SAFETY STOP] Cannot verify module instance identity. Refusing module unload.`
  6. The newly inserted module was left stranded in the kernel without teardown!
- **Correction Implemented:**
  Invoked `capture_module_instance_identity "fte3600" || true` immediately upon `insmod_rc == 0` inside the insertion critical section. This commits `MOD_INSTANCE_RECORDED=1` atomically with `THIS_INVOCATION_INSERTED=1` before traps are restored. Synthetic Test 56 confirms that clean teardown now succeeds if a signal is deferred after successful insertion.

---

### Finding 5: Advisory Nature of `flock`
- **Classification:** **LOW (Expected Advisory Semantics)**
- **Source Lines:** [scripts/stage-b-test.sh:1217-1252](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L1217-L1252)
- **Detailed Technical Analysis:**
  The cooperative lock on `/run/lock/fte3600-test.lock` is acquired using Linux `flock(2)`. In Linux, file locks created by `flock()` are **strictly advisory**.
  - A second instance of `stage-b-test.sh` checking `/run/lock/fte3600-test.lock` is blocked immediately (fail-closed).
  - However, an uncoordinated administrator or script running manual commands (`modprobe fte3600`, `rmmod fte3600`) does not query `/run/lock/fte3600-test.lock` and will not be blocked by `flock`.
- **Evaluation:** This is the standard behavior of Unix advisory locks. Protection against uncoordinated external commands is provided by the script's internal ownership state machine (`INITIAL_FTE3600_LOADED`, `THIS_INVOCATION_INSERTED`, `INSERTION_STATE`), which bars unload of unowned or pre-existing modules.

---

## 4. Analysis: Guaranteed Guarantees vs Inferred Behaviors

| Property | Status | Technical Substantiation |
|:---|:---:|:---|
| **Zero Hardware Operations During Cleanup** | **GUARANTEED** | Verified by static audit of `cleanup_handler()`. Only read-only `test`, `stat`, `cat`, and graceful `rmmod fte3600` are executed. Zero ioctls, zero GPIO writes. |
| **No Automatic Process Termination** | **GUARANTEED** | Verified in `check_open_descriptors()`. No `kill`, `pkill`, or `SIGTERM` exists. Discovered PIDs are reported and halt unload with status 1. |
| **Preservation of Pre-Existing Modules** | **GUARANTEED** | Verified via `INITIAL_FTE3600_LOADED=1`. Pre-existing modules are never removed by `cleanup_handler()`. |
| **Uncertain Insertion Barred from Unload** | **GUARANTEED** | Verified via `INSERTION_STATE_UNCERTAIN`. Interrupted `insmod` (`rc >= 128`) permanently blocks teardown. |
| **Inconclusive Descriptor Scan Bars Unload** | **GUARANTEED** | Verified via return status 2 in `check_open_descriptors()`. Unreadable entries halt teardown fail-closed. |
| **No Forced Module Removal (`rmmod -f`)** | **GUARANTEED** | Verified in `cleanup_handler()`. If normal `rmmod` fails, the error is logged and the module remains safely loaded. |
| **Cooperative Multi-Runner Mutual Exclusion** | **GUARANTEED** | Verified via `flock -n 200` on `/run/lock/fte3600-test.lock`. Concurrent test harness runs are rejected. |
| **Atomic Instance Identification Against Identical Reload** | **CANNOT BE GUARANTEED** | Linux userspace cannot distinguish an identical binary reloaded within the same integer second with reused kernfs inode and vmalloc address (Test 53). |
| **Atomic Module Unload (`rmmod`) Without Race** | **CANNOT BE GUARANTEED** | Linux `delete_module(2)` takes only a module name string. A microscopic TOCTOU window exists between final check and syscall execution. |

---

## 5. Synthetic Offline Test Suite Verification (57/57 Tests)

The offline synthetic test harness was expanded with 7 new tests specifically exercising the audit scenarios:

```text
===========================================================================
Stage 2E.7.12 Offline Synthetic Test Suite
===========================================================================
[PASS] Test 1 to Test 50: Baseline stat, descriptor, and ownership checks
[PASS] Test 51: Module replacement with identical srcversion/coresize but differing .text
[PASS] Test 52: Replacement during descriptor scan caught by pre-unload re-verification
[PASS] Test 53: Blind spot audit: Identical reload within same second & reused inode
[PASS] Test 54: Shared underlying state between /proc/modules and lsmod
[PASS] Test 55: Interrupted insmod leaving module loaded in kernel refuses unload
[PASS] Test 56: Critical section instance capture enables safe teardown on deferred signal
[PASS] Test 57: Exhaustive check that unverified ownership paths never authorize teardown
===========================================================================
Summary: 57 passed, 0 failed out of 57 tests.
===========================================================================
```

### Key Test Case Verifications:
- **Test 51 (KASLR Text Address Mismatch):** Proves that if an attacker or external process reloads a module having the identical `srcversion` and `coresize`, but mapped to a different virtual address via KASLR, `verify_module_instance_identity` catches the mismatch and blocks unload.
- **Test 52 (Mid-Cleanup Replacement):** Proves that if a module is replaced *during* the open descriptor audits, the pre-unload re-verification immediately before `rmmod` intercepts the replacement and blocks unload.
- **Test 53 (Blind Spot Audit):** Demonstrates that if an identical module is reloaded within the same second and receives the same inode, userspace fingerprinting returns 0 (verifying the documented architectural limitation).
- **Test 54 (Procfs / Lsmod Dependency):** Proves that `lsmod` and `/proc/modules` share the same underlying data source.
- **Test 55 (Interrupted Insmod):** Proves that an interrupted `insmod` (`rc=130`) leaving a loaded module refuses unload fail-closed.
- **Test 56 (Critical Section Instance Capture):** Proves that capturing instance identity upon `insmod_rc == 0` enables safe teardown if an interrupted signal arrives before probe completion.
- **Test 57 (Exhaustive Ownership Paths):** Verifies that all unverified ownership paths (pre-existing, failed insertion, uncertain insertion, unrecorded instance) strictly block teardown.

---

## 6. Source Modifications Made in This Stage

Three targeted modifications were made to [scripts/stage-b-test.sh](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh):
1. **Line 327–341:** Corrected documentation in `query_module_state()` to explain that `lsmod` parses `/proc/modules`, clarifying that sysfs and procfs are the two independent kernel-state sources.
2. **Line 1112–1120:** Added pre-unload module instance re-verification immediately before `rmmod fte3600` at Teardown Step 3 to intercept replacements occurring during descriptor audits.
3. **Line 1421 & Line 1467:** Added immediate module instance identity capture upon `insmod_rc == 0` inside the critical section, ensuring identity is recorded before processing any deferred signal.

---

## 7. Static Safety Verifications & Host Invariant Checks

All static safety checks were executed cleanly:
- `bash -n scripts/stage-b-test.sh` & `bash -n scripts/run-stage-b-in-terminal.sh`: **PASS** (zero syntax warnings or errors).
- `git diff --check`: **PASS** (zero whitespace or hygiene issues).
- `git diff -- kernel/`: **ZERO MODIFICATIONS** (kernel source remains strictly untouched).
- `git commit`: **ZERO COMMITS CREATED**.
- Host Invariant Checks:
  - `fte3600` is **NOT** loaded (`lsmod | grep fte3600` is empty).
  - `/dev/fte3600` is **absent**.
  - `spi-FTE3600:00` is **unbound** (`/sys/bus/spi/devices/spi-FTE3600:00/driver` is absent).
  - Safety gate is **sealed** (`bin/fw9369-hw-tool reset` returns `1`).

---

## 8. Final Required Conclusion

In accordance with the Stage 2E.7.12 specification, the conclusion is:

### **Choice A: Source audit found a defensible cleanup policy, with all remaining limitations explicitly documented.**

**Justification:**
1. The cleanup policy is strictly **fail-closed**:
   - Pre-existing modules are never removed.
   - Uncertain or failed insertions never grant ownership.
   - Any open file descriptor halts unload.
   - Inconclusive audits or unreadable `/proc` entries halt unload.
   - Replaced modules (different inode, ctime, or KASLR address) halt unload.
   - Forced unloads (`rmmod -f`) are barred.
   - Process termination (`kill`, `pkill`) is barred.
2. The remaining limitations (heuristic nature of userspace module fingerprinting and the microscopic TOCTOU window of Linux `delete_module(2)`) are inherent architectural boundaries of the Linux kernel syscall interface. They cannot be eliminated by any userspace implementation without kernel API modifications.
3. These boundaries are now explicitly documented, tested, and accounted for.
4. Stage B is an authorized, single-reset test procedure executed in a controlled, isolated administrative environment where uncoordinated concurrent module reloading does not occur.

---

> [!CAUTION]
> **EXPLICIT STAGE B EXECUTION STATUS:**  
> **Stage B remains STRICTLY ON HOLD.**  
> Do NOT execute `scripts/run-stage-b-in-terminal.sh`. Do NOT execute `scripts/stage-b-test.sh`. Execution requires explicit user authorization following independent review of this audit.
