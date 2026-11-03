# Stage 2E.8 Implementation Report: Fail-Closed No-Auto-Unload Policy (Option 1)

**Target System:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Target SPI Device:** `spi-FTE3600:00` on Intel LPSS SPI2 (`pxa2xx-spi.2`, CS0)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (x86_64)  
**Stage:** Stage 2E.8 (Static Implementation & Verification)  
**Safety Status:** **STAGE B ON HOLD — ZERO HARDWARE ACCESS PERFORMED**

---

## 1. Executive Summary & Authorization Boundary

In Stages 2E.7.9 through 2E.7.13, exhaustive analysis established that userspace-only heuristics cannot reliably maintain proof of kernel module ownership across all asynchronous boundaries, signals, and multi-actor races. In particular, any automated userspace module unloading (`rmmod`) suffers from fundamental TOCTOU windows and cannot distinguish identical reloads or atomic module identity transfers without dedicated kernel-level instance handles (such as an instance file descriptor).

Consequently, Stage 2E.8 permanently implements **Option 1: Never Automatically Unload the Fingerprint Driver**. Under Option 1:
- The Stage B test harness (`scripts/stage-b-test.sh`) **never** automatically unloads `fte3600.ko` under any condition (whether successful completion, reset failure, signal interruption, uncertain insertion, devnode mismatch, or descriptor audit failure).
- Automated module unloading (`rmmod`, `modprobe -r`, `delete_module`) is permanently and completely eliminated from all script execution and cleanup paths.
- SPI driver unbinding (`spi-FTE3600:00`) is never performed automatically.
- Processes holding open file descriptors are never automatically terminated.
- The test harness transitions cleanup from an active teardown agent into a **read-only diagnostic reporter & script-resource releaser**.
- Responsibility for module unloading is transferred strictly and exclusively to the human system administrator via explicit, manual commands after review of diagnostic logs.

**Execution Boundary Confirmation:**
- **Zero** hardware operations were performed.
- Neither `scripts/run-stage-b-in-terminal.sh` nor `scripts/stage-b-test.sh` was executed against host hardware.
- `fte3600.ko` was **not** inserted, loaded, or unloaded on the host system.
- `spi-FTE3600:00` remains unbound.
- The legacy safety gate (`bin/fw9369-hw-tool`) remains sealed (status 1).

---

## 2. Option 1 Architectural Simplification & Justification

### 2.1 The Problem Solved by Option 1
Between Stages 2E.7.9 and 2E.7.13, the following unsolvable races and limitations of automated userspace unloading were uncovered:
1. **Insertion-to-Capture TOCTOU Window:** An external process could unload and replace the module between `insmod` returning and the script capturing sysfs attributes (`stat` on `/sys/module/fte3600`), causing the script to record and later unload an externally substituted module.
2. **Pre-Unload-to-Execution TOCTOU Window:** Even with multi-attribute pre-unload verification (`stat -c %i,%Z`, `coresize`, `srcversion`, `.text`), an external unload and reload occurring between userspace attribute check and `rmmod` cannot be prevented in userspace because `delete_module(2)` takes only a string name (`"fte3600"`), not an inode or module pointer.
3. **Reference Count Decoupling:** `device_initialize()` / `get_device()` increments device object references, but does **not** increment `struct module *owner` refcounts. A kernel module with open file descriptors has `module_refcount > 0` (causing `delete_module` to fail with `-EBUSY`), but uncoordinated unloading attempts create unnecessary kernel friction.
4. **Attribute Indistinguishability:** If an external process reloads an identical module build with identical ctime, inode, and section placement, userspace heuristics cannot distinguish the new instance from the original.

### 2.2 The Solution: Elimination of Automated Unload
By enforcing Option 1:
- All complex, heuristic, and racy userspace instance-fingerprinting logic is rendered completely obsolete and safely deleted.
- The risk of inadvertently unloading an external or administrator-managed module is reduced to **zero**.
- The driver remains stably loaded and bound to the hardware device following the single authorized `FTE3600_IOC_RESET` pulse, allowing telemetry, hardware pad registers, and device-node identity to be inspected calmly without teardown races.
- If a test run fails, crashes, or is interrupted by SIGINT/SIGTERM, the exact state of the hardware and driver is preserved in place for administrative analysis.
- Subsequent runs of `scripts/stage-b-test.sh` safely fail Pre-Flight Check 3 (`INITIAL_FTE3600_LOADED=1`) until the operator reviews the logs and manually executes `sudo rmmod fte3600`.

---

## 3. Detailed Source Code Modifications

### 3.1 Modifications to `scripts/stage-b-test.sh`

#### A. Scope and Policy Documentation
The script header comments were updated to clearly state the mandatory Option 1 policy:
```bash
# Scope: STAGE B ONLY (Option 1: Fail-Closed No-Auto-Unload Policy)
#   - Pre-flight checks (kernel, vermagic, modules, sysfs, devnodes, safety gate)
#   - insmod fte3600.ko
#   - Verify probe binding to spi-FTE3600:00
#   - Verify /dev/fte3600 creation and validate character device identity (major/minor)
#   - Query initial state via GET_STATE (Snapshot 1)
#   - Issue exactly ONE FTE3600_IOC_RESET (40 ms active-low reset pulse sequence)
#   - Query post-reset state via GET_STATE (Snapshot 2)
#   - Zero SPI transfers; zero INIT ioctls; zero CAPTURE ioctls
#   - Guaranteed non-recursive cleanup trap handling EXIT, INT, TERM, HUP
#   - Genuinely fail-closed descriptor audit: independent /proc device-identity matching
#   - Controlled error-state handling without shell-wide errexit pollution
#   - Zero automatic process termination (no SIGTERM, no SIGKILL, no pkill)
#   - Absolute preservation of primary reset failure status across subsequent diagnostics
#   - OPTION 1 MANDATORY POLICY: NEVER automatically unload fte3600 kernel module
#   - Zero automatic rmmod, zero modprobe -r, zero sysfs SPI unbinds
#   - Module is intentionally left loaded and bound upon exit for manual administrative teardown
```

#### B. Elimination of Obsolete Instance Identity Tracking
The following variables and functions were deleted:
- Variables removed:
  * `RECORDED_MOD_SYSFS_INODE`
  * `RECORDED_MOD_CTIME`
  * `RECORDED_MOD_SRCVERSION`
  * `RECORDED_MOD_CORESIZE`
  * `RECORDED_MOD_TEXT_ADDR`
  * `MOD_INSTANCE_RECORDED`
- Functions removed:
  * `capture_module_instance_identity()`
  * `verify_module_instance_identity()`
- Call sites removed:
  * Phase 2 `insmod` success block call to `capture_module_instance_identity`.
  * Phase 2 post-probe verification call to `capture_module_instance_identity`.

#### C. Redesigned Fail-Closed `cleanup_handler()`
`cleanup_handler()` was completely re-architected into a non-destructive diagnostic reporter:
1. **Re-entrancy Protection:** Guarded by `CLEANUP_IN_PROGRESS=1`. Any secondary invocation returns 0 immediately.
2. **Robust Diagnostic Execution:** Sets `set +e` so errors in diagnostic tools do not prematurely abort diagnostic reporting.
3. **Read-Only Kernel Module Audit:** Queries `query_module_state "fte3600"` and logs whether the module is loaded, absent, or inconclusive.
4. **Read-Only Character Device Audit:** Checks presence/absence of `/dev/fte3600` (supporting `MOCK_TARGET_NODE`).
5. **Read-Only SPI Driver Binding Audit:** Checks `/sys/bus/spi/devices/spi-FTE3600:00/driver` link target without modifying it.
6. **Informative Open-Descriptor Audit:** Executes `check_open_descriptors` as a purely informative check to log any processes holding `/dev/fte3600`. Zero process termination is performed.
7. **Read-Only Hardware State Inspection:** Logs PCH Pad 14, Pad 229, debugfs GPIO claims, and the tail of `dmesg`.
8. **Option 1 Mandatory Policy Banners:**
   - On successful insertion:
     ```
     [POLICY] MODULE LEFT LOADED BY POLICY — MANUAL CLEANUP REQUIRED
     [POLICY] To manually unload after inspection: sudo rmmod fte3600
     ```
   - On uncertain insertion outcome:
     ```
     [POLICY] MODULE STATE MAY BE PRESENT — INSERTION OUTCOME UNCERTAIN; DO NOT AUTOMATICALLY REMOVE
     [POLICY] Manual inspection required to verify kernel module status.
     ```
   - On definitely failed insertion:
     ```
     [POLICY] Module insertion definitely failed. Script does not own any loaded module.
     ```
   - On pre-existing module:
     ```
     [POLICY] Module was pre-existing before script started. Script does not own module.
     [POLICY] PRE-EXISTING MODULE PRESERVED IN KERNEL — NO UNLOAD PERFORMED
     ```
9. **Script-Owned Resource Release:** Unlocks and closes the cooperative lock on FD 200 *only after all diagnostic logging and policy banners have completed*.
10. **Exit Status Preservation:** Strictly preserves the incoming exit code (`$incoming_exit`), whether from signal traps (130, 143, 129), reset ioctl failure, or success (0).
11. **Trap Unhooking:** Clears traps (`trap - EXIT INT TERM HUP`) before exiting with `$final_exit`.

---

### 3.2 Modifications to `scripts/run-stage-b-in-terminal.sh`

The launcher script was updated to match the Option 1 contract:
```bash
printf "===============================================================================\n"
printf " FTE3600 Stage 2E.8 — Stage B Controlled Hardware Reset Test\n"
printf " This script runs the strictly bounded, single-reset execution sequence:\n"
printf "   1. Loads fte3600.ko\n"
printf "   2. Verifies driver probe and /dev/fte3600\n"
printf "   3. Takes Snapshot 1 via GET_STATE (initial state)\n"
printf "   4. Issues exactly ONE FTE3600_IOC_RESET (40 ms reset pulse sequence)\n"
printf "   5. Takes Snapshot 2 via GET_STATE (post-reset state)\n"
printf "   6. Releases cooperative lock; leaves module loaded and bound by policy\n"
printf "   * ZERO automatic module unloads (rmmod permanently disabled in harness)\n"
printf "   * ZERO SPI transactions; ZERO INIT ioctls; ZERO CAPTURE ioctls\n"
printf "===============================================================================\n\n"
```
The exit message informs the user:
```bash
if [ "$status" -eq 0 ]; then
    printf "Stage B reset test completed successfully (Status 0).\n"
    printf "NOTE: Module fte3600 remains loaded by Option 1 policy. Manual rmmod required.\n"
else
    printf "Stage B script exited with status %d.\n" "$status"
    printf "NOTE: Driver/module state preserved in kernel. Manual inspection required.\n"
fi
```
The launcher contains zero background processes, zero auto-retries, and zero unload commands.

---

## 4. Verification & Synthetic Test Results

The offline synthetic test suite (`scratch/test_synthetic_suite.py`) was updated to cover foundational validation as well as all 14 mandatory Stage 2E.8 Option 1 execution and failure paths.

### 4.1 Test Suite Execution Output
```
===========================================================================
Stage 2E.8 Offline Synthetic Test Suite (Option 1 No-Auto-Unload Policy)
===========================================================================
[PASS] Test 1: Existing FD where stat fails but readlink succeeds
[PASS] Test 2: Process exits during inspection handled cleanly
[PASS] Test 3: Symlink to different character device or file rejected
[PASS] Test 4: Malformed sysfs major:minor rejection
[PASS] Test 5: Negative and oversized value rejection
[PASS] Test 6: Numeric parsing avoiding octal traps
[PASS] Test 7: Preservation of RESET failure status
[PASS] Test 8: Baseline valid matching stat and sysfs identities
[PASS] Test 9: Identity mismatch between stat and sysfs
[PASS] Test 10: Missing sysfs identity fails closed
[PASS] Test 11: Bounds validation (INT64_MAX vs overflow values)
[PASS] Test 12: Cooperative lock fail-closed when already held
[PASS] Test 13: query_module_state reports absent (status 1) for absent module
[PASS] Path 1: Successful insertion + successful test -> module preserved, banner logged, exit 0
[PASS] Path 2: Successful insertion + failed RESET ioctl -> primary status (rc=5) preserved, module preserved
[PASS] Path 3: Successful insertion + SIGINT -> signal status (rc=130) preserved, module preserved
[PASS] Path 4: Interrupted insmod (UNCERTAIN outcome) -> module preserved, uncertain banner logged
[PASS] Path 5: Definitely failed insmod -> failure status (rc=1) preserved, definitely failed banner logged
[PASS] Path 6: Device-node creation failure -> exit status (rc=1) preserved, module preserved
[PASS] Path 7: Open descriptors detected during audit -> zero termination, module preserved
[PASS] Path 8: Sysfs /proc enumeration error -> informative audit reports issue, module preserved
[PASS] Path 9: Module-state query inconclusive -> warning logged, module preserved
[PASS] Path 10: Multiple cleanup invocations (re-entrancy) -> idempotent, returns cleanly
[PASS] Path 11: Pre-existing module -> preserved in kernel, pre-existing banner logged
[PASS] Path 12: Script closes only owned lock FD (FD 200) and releases flock
[PASS] Path 13: Never unbinds SPI device link
[PASS] Path 14: Static code inspection (zero executable unload/unbind commands in test scripts)
===========================================================================
Summary: 27 passed, 0 failed out of 27 tests.
===========================================================================
```

### 4.2 Detailed Analysis of the 14 Required Paths
| Path # | Scenario | Outcome | Verification Method | Result |
|---|---|---|---|---|
| **Path 1** | Successful insertion + full test success | Module preserved, banner logged, exit 0 | Mock test run through `cleanup_handler 0 "EXIT"` | **PASS** |
| **Path 2** | Successful insertion + failed reset ioctl (rc=5) | Module preserved, primary exit code 5 preserved | Mock reset failure passed to `cleanup_handler 5 "EXIT"` | **PASS** |
| **Path 3** | Interrupted execution (SIGINT / SIGTERM / SIGHUP) | Module preserved, signal exit code (130) preserved | Invoked via `cleanup_handler 130 "SIGINT"` | **PASS** |
| **Path 4** | Interrupted insmod (UNCERTAIN insertion state) | Module preserved, UNCERTAIN banner logged | `INSERTION_STATE=UNCERTAIN`, verified no rmmod called | **PASS** |
| **Path 5** | Definitely failed insmod (rc=1) | Script reports failure (rc=1) without claiming ownership | `INSERTION_STATE=DEFINITELY_FAILED`, verified banner | **PASS** |
| **Path 6** | Devnode creation / validation failure | Failure exit status (rc=1) preserved, module preserved | Verified cleanup preserves loaded module | **PASS** |
| **Path 7** | Open descriptors detected during audit | Zero process termination, module preserved | Mock active process holding target node, verified `kill` never called | **PASS** |
| **Path 8** | Sysfs /proc scan enumeration error | Error logged, module preserved | Mock unreadable `/proc` directory, verified fail-closed behavior | **PASS** |
| **Path 9** | Module-state query inconclusive | Warning logged, module preserved | `query_module_state` returning 2, verified no unload | **PASS** |
| **Path 10**| Multiple cleanup invocations (re-entrancy) | Idempotent, zero crash or recursion | Direct double-invocation with `CLEANUP_IN_PROGRESS=1` | **PASS** |
| **Path 11**| Pre-existing module (`INITIAL_FTE3600_LOADED=1`) | Preserved in kernel, pre-existing banner logged | Verified pre-existing banner and zero unload | **PASS** |
| **Path 12**| Cooperative lock FD release | Script releases FD 200 and flock | Subshell lock release tested via non-blocking secondary lock | **PASS** |
| **Path 13**| SPI driver binding link preservation | SPI link left intact, zero unbind | Target `$SPI_PATH/driver` checked; unbind write never executed | **PASS** |
| **Path 14**| Static code inspection for forbidden commands | Zero executable `rmmod`, `modprobe -r`, `delete_module`, `unbind` | AST/Regex parsing of test and launcher scripts | **PASS** |

---

## 5. Operator Manual Cleanup & Diagnostic Inspection Guide

When Stage B is eventually authorized and executed, the test harness will exit leaving `fte3600.ko` loaded and bound to `spi-FTE3600:00`. The operator must perform the following manual procedure:

### Step 1: Review Test Execution Results
Inspect the complete test log:
```bash
cat diagnostics/stage-b-results.txt
```
Verify the exit code and check whether the single `FTE3600_IOC_RESET` pulse was successfully executed.

### Step 2: Check Driver and Hardware State
Confirm the driver is currently loaded:
```bash
lsmod | grep fte3600
```
Inspect the character device node:
```bash
ls -l /dev/fte3600
```
Inspect the SPI driver binding:
```bash
ls -l /sys/bus/spi/devices/spi-FTE3600:00/driver
```
Inspect kernel ring buffer messages:
```bash
sudo dmesg | grep fte3600
```

### Step 3: Inspect Open Descriptors
Before unloading the module, check if any lingering process holds the device node open:
```bash
sudo lsof /dev/fte3600
# or
sudo fuser -v /dev/fte3600
```
If any process holds the node open, allow it to finish or terminate it gracefully.

### Step 4: Perform Manual Driver Unload
Execute the standard module removal:
```bash
sudo rmmod fte3600
```
*(Do NOT use `rmmod -f` under any circumstances).*

### Step 5: Verify Final System State
Confirm module removal and devnode destruction:
```bash
lsmod | grep fte3600              # Must return nothing
ls -l /dev/fte3600                 # Must report "No such file or directory"
ls -l /sys/bus/spi/devices/spi-FTE3600:00/driver  # Must report "No such file or directory"
```
The system is now fully clean and ready for subsequent evaluations.

---

## 6. Pre-Execution Safety Invariant Verification

Host invariant verification performed during this audit confirms:
1. **fte3600 Module:** Absent from kernel (`lsmod | grep fte3600` returns empty).
2. **Device Node:** Absent (`/dev/fte3600` does not exist).
3. **SPI Device Binding:** Unbound (`/sys/bus/spi/devices/spi-FTE3600:00/driver` does not exist).
4. **Kernel Codebase:** Unmodified (`git diff -- kernel/` returns 0 changes).
5. **Legacy Safety Gate:** Sealed (`bin/fw9369-hw-tool` exits with status 1, policy banner verified).
6. **Execution Scripts:** Clean syntax (`bash -n` passes with status 0 on both scripts).

---

## 7. Mandatory Status Declarations

```
AUTOMATIC MODULE UNLOAD: PERMANENTLY DISABLED IN STAGE B HARNESS
STAGE B HARDWARE EXECUTION: NOT PERFORMED
STAGE B AUTHORIZATION: STILL ON HOLD PENDING INDEPENDENT REVIEW
```
