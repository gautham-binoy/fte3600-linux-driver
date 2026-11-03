# Stage 2E.12 — Fix Expected Nonzero Return Handling in Stage B Preflight Report

**Target System:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Target SPI Device:** `spi-FTE3600:00` on Intel LPSS SPI2 (`pxa2xx-spi.2`, CS0)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (x86_64, SMP PREEMPT_DYNAMIC)  
**Task Type:** Bug Fix, Audit of Nonzero Return Codes & Offline Synthetic Verification  
**Status:** **PREFLIGHT ERREXIT BUG FIXED — ALL 38 SYNTHETIC TESTS PASSED — HARDWARE OPERATIONS NONE**

---

## 1. Why the Original Code Failed

### 1.1 Root Cause Analysis
During Stage 2E.11 execution, the test script [`scripts/stage-b-test.sh`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh) halted unexpectedly during Phase 1 Pre-Flight Check 3 (`Checking existing module loading ...`).

In [`scripts/stage-b-test.sh`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh), line 29 enables Bash strict error handling:
```bash
set -euo pipefail
```
At line 1116 (original line 1108), the script executed:
```bash
# 3. Existing module presence
printf "Checking existing module loading ... "
initial_mod_state=0
query_module_state "fte3600"
initial_mod_state=$?
```
The helper function `query_module_state()` is explicitly documented with three distinct return codes:
- **`0`**: Module is confirmed **LOADED** in the kernel.
- **`1`**: Module is confirmed **ABSENT** from the kernel.
- **`2`**: Module state inspection **INCONCLUSIVE** (error or contradictory sysfs/proc data).

On a clean, pristine system prior to test execution, `fte3600` is **absent** from the kernel. Therefore, `query_module_state "fte3600"` correctly and intentionally returned **`1`**.

However, because `query_module_state "fte3600"` was executed as an unguarded standalone command while `set -e` (`errexit`) was active, Bash interpreted the expected nonzero return status (`1`) as an unhandled command failure. Bash immediately aborted execution with status 1 and triggered `trap 'cleanup_handler $? "EXIT"' EXIT`, terminating before attempting module insertion or reaching the RESET ioctl.

A second instance of this bug was present immediately below at line 1129 for `query_module_state "spidev"`.

---

## 2. Exact Source Changes

The script [`scripts/stage-b-test.sh`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh) was modified to explicitly shield all expected nonzero return codes using standard Bash `shopt -o -q errexit` / `set +e` / `set -e` guard blocks.

### 2.1 Primary Preflight Fix (Lines 1114–1148)
```diff
--- a/scripts/stage-b-test.sh
+++ b/scripts/stage-b-test.sh
@@ -1114,8 +1114,13 @@ fi
 # 3. Existing module presence
 printf "Checking existing module loading ... "
 initial_mod_state=0
+was_errexit_initial_mod=0
+if shopt -o -q errexit; then was_errexit_initial_mod=1; fi
+set +e
 query_module_state "fte3600"
 initial_mod_state=$?
+if [ "$was_errexit_initial_mod" -eq 1 ]; then set -e; fi
+
 if [ "$initial_mod_state" -eq 0 ]; then
     INITIAL_FTE3600_LOADED=1
     printf "%s\n" "[FAIL] Conflicting module already loaded: fte3600"
@@ -1126,8 +1131,18 @@ fi
 
 spidev_mod_state=0
+was_errexit_spidev=0
+if shopt -o -q errexit; then was_errexit_spidev=1; fi
+set +e
 query_module_state "spidev"
 spidev_mod_state=$?
+if [ "$was_errexit_spidev" -eq 1 ]; then set -e; fi
+
 if [ "$spidev_mod_state" -eq 0 ]; then
     printf "%s\n" "[FAIL] Conflicting module already loaded: spidev"
     exit 1
+elif [ "$spidev_mod_state" -eq 2 ]; then
+    printf "%s\n" "[FAIL] Unable to determine spidev module state in kernel (inspection inconclusive)."
+    exit 1
 fi
 printf "%s\n" "[PASS] (Neither fte3600 nor spidev loaded)"
```

### 2.2 Descriptor Audit Module Check Fix (Line 590)
In `check_open_descriptors()`, `query_module_state "fte3600"` was previously unguarded. If called when `errexit` was active, return code 1 would abort the script:
```bash
    local module_loaded=0
    local cur_mod_state=0
    local was_errexit_cur_mod=0
    if shopt -o -q errexit; then was_errexit_cur_mod=1; fi
    set +e
    query_module_state "fte3600"
    cur_mod_state=$?
    if [ "$was_errexit_cur_mod" -eq 1 ]; then set -e; fi

    if [ "$cur_mod_state" -eq 0 ] || [ "${THIS_INVOCATION_INSERTED:-0}" -eq 1 ] || [ "${MODULE_INSERTED:-0}" -eq 1 ]; then
        module_loaded=1
    fi
```

### 2.3 Post-Probe Module Check Fix (Line 1292)
Phase 2 Post-Probe verification previously used `if query_module_state "fte3600"; then`, which did not distinguish between return code 1 (absent) and return code 2 (inconclusive). It now captures the exact status safely:
```bash
# 1. Verify module is loaded
printf "Checking module loading state ... "
post_mod_state=0
was_errexit_post_mod=0
if shopt -o -q errexit; then was_errexit_post_mod=1; fi
set +e
query_module_state "fte3600"
post_mod_state=$?
if [ "$was_errexit_post_mod" -eq 1 ]; then set -e; fi

if [ "$post_mod_state" -eq 0 ]; then
    printf "%s\n" "[PASS]"
    lsmod | grep fte3600 || true
elif [ "$post_mod_state" -eq 2 ]; then
    printf "%s\n" "[FAIL] Unable to determine fte3600 module state in kernel (inspection inconclusive)."
    exit 1
else
    printf "%s\n" "[FAIL] fte3600 module not confirmed loaded (absent)."
    exit 1
fi
```

### 2.4 Pre-Reset Snapshot 1 Telemetry Capture (Line 1341)
`"$TOOL_BIN" get-state` was previously invoked as a standalone command without an explicit failure banner. It now captures `$SNAP1_RC` with a dedicated guard, printing an explicit failure message before cleanly exiting:
```bash
# 6. Query initial driver state via GET_STATE ioctl (Snapshot 1)
printf "\n%s\n" "--- [PHASE 3] Telemetry Snapshot 1: Pre-Reset State ---"
SNAP1_RC=0
was_errexit_snap1=0
if shopt -o -q errexit; then was_errexit_snap1=1; fi
set +e
"$TOOL_BIN" get-state
SNAP1_RC=$?
if [ "$was_errexit_snap1" -eq 1 ]; then set -e; fi
if [ "$SNAP1_RC" -ne 0 ]; then
    printf "[FAIL] Initial GET_STATE ioctl failed with exit code %d\n" "$SNAP1_RC"
    exit "$SNAP1_RC"
fi
```

---

## 3. Comprehensive Audit of Nonzero Return Handling

Every function, command, and validation helper in [`scripts/stage-b-test.sh`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh) was inspected for `set -e` behavior:

| Command / Helper | Location | Expected Nonzero Status | How Handled under `set -e` | Audit Verdict |
| :--- | :--- | :--- | :--- | :---: |
| `query_module_state "fte3600"` | Line 1118 (Preflight) | `1` (absent), `2` (inconclusive) | Guarded with `set +e` / `set -e`; explicit branching for `0`, `1`, `2` | **SAFE & EXPLICIT** |
| `query_module_state "spidev"` | Line 1133 (Preflight) | `1` (absent), `2` (inconclusive) | Guarded with `set +e` / `set -e`; explicit branching for `0`, `1`, `2` | **SAFE & EXPLICIT** |
| `query_module_state "fte3600"` | Line 590 (`check_open_descriptors`) | `1` (absent), `2` (inconclusive) | Guarded with `set +e` / `set -e`; captures `$?` without premature abort | **SAFE & EXPLICIT** |
| `query_module_state "fte3600"` | Line 926 (`cleanup_handler`) | `1` (absent), `2` (inconclusive) | Guarded with `set +e` / `set -e`; logs informative state banner | **SAFE & EXPLICIT** |
| `query_module_state "fte3600"` | Line 1297 (Post-probe) | `1` (absent), `2` (inconclusive) | Guarded with `set +e` / `set -e`; distinguishes absent from inconclusive | **SAFE & EXPLICIT** |
| `validate_decimal_rdev` | Lines 255, 499, 545 | `1` (invalid/overflow) | Executed inside `if` conditionals (`if validate_...; then`); exempt from errexit | **SAFE** |
| `validate_hex_majmin` | Lines 256, 500, 546 | `1` (invalid hex) | Executed inside `if` conditionals; exempt from errexit | **SAFE** |
| `convert_sysfs_dev_to_identity` | Lines 291, 611 | `1` (malformed sysfs string) | Executed inside `if convert_...; then`; exempt from errexit | **SAFE** |
| `record_devnode_identity` | Line 1324 | `1` (missing or mismatched devnode) | Executed inside `if ! record_...; then exit 1; fi`; exempt from errexit | **SAFE** |
| `flock -n 200` | Line 1073 | `1` (lock held) | Executed inside `if ! flock -n 200; then exit 1; fi`; exempt from errexit | **SAFE** |
| `exec 200>"$LOCK_FILE"` | Line 1065 | `1` (cannot create lock file) | Executed inside `if ! exec 200>...; then exit 1; fi`; exempt from errexit | **SAFE** |
| `"$SAFETY_GATE_BIN"` | Line 1172 | `1` (sealed safety gate) | Guarded with `was_errexit_gate` and `set +e`; explicit check for `GATE_RC=1` | **SAFE & EXPLICIT** |
| `insmod "$MODULE_PATH"` | Line 1231 | `>= 128` (signal), `1` (fail) | Guarded with `was_errexit_insmod` and `set +e`; tri-state state machine | **SAFE & EXPLICIT** |
| `"$TOOL_BIN" get-state` (Snap 1) | Line 1345 | `>= 1` (ioctl failure) | Guarded with `was_errexit_snap1` and `set +e`; fails closed before reset | **SAFE & EXPLICIT** |
| `"$TOOL_BIN" reset` | Line 1362 | `>= 1` (reset ioctl failure) | Guarded with `was_errexit_reset` and `set +e`; zero retries, preserves RC | **SAFE & EXPLICIT** |
| `"$TOOL_BIN" get-state` (Snap 2) | Line 1378 | `>= 1` (ioctl failure) | Guarded with `was_errexit_snap2` and `set +e`; logs warning, preserves primary RC | **SAFE & EXPLICIT** |
| `lsof -t "$target_node"` | Line 641 | `1` (no match) | Guarded with `was_errexit_lsof` and `set +e`; captures real exit code | **SAFE & EXPLICIT** |
| `fuser "$target_node"` | Line 669 | `1` (no match) | Guarded with `was_errexit_fuser` and `set +e`; captures real exit code | **SAFE & EXPLICIT** |
| `ls -1d /proc/[0-9]*` | Line 707 | `1` (empty/error) | Guarded with `was_errexit_proc` and `set +e`; fails closed on non-zero | **SAFE & EXPLICIT** |
| `ls -1A "$fd_dir"` | Line 740 | `1` (process exited) | Guarded with `was_errexit_fd` and `set +e`; handles process exit cleanly | **SAFE & EXPLICIT** |
| `dmesg` | Line 1398 | `>= 1` (unprivileged buffer read) | Guarded with `was_errexit_dmesg` and `set +e`; logs warning, preserves status | **SAFE & EXPLICIT** |

---

## 4. Return-Code Meaning Preservation Matrix

The fix preserves all three documented semantic states of `query_module_state`:

| Return Code | Semantic Meaning | Script Preflight Action | Test Verification |
| :---: | :--- | :--- | :--- |
| **`0`** | Module is confirmed **LOADED** in kernel | **ABORT** with exit code 1 (`[FAIL] Conflicting module already loaded`). Prevents claiming pre-existing module instances. | Verified in `Stage 2E.12.2` |
| **`1`** | Module is confirmed **ABSENT** from kernel | **PROCEED** to next check (`[PASS] confirmed absent`). The expected clean preflight condition. | Verified in `Stage 2E.12.1` |
| **`2`** | Inspection **INCONCLUSIVE** or contradictory | **ABORT** with exit code 1 (`[FAIL] Unable to determine module state`). Fail closed. | Verified in `Stage 2E.12.3` |

---

## 5. Offline Synthetic Test Suite Results

The offline synthetic test suite ([`scratch/test_synthetic_suite.py`](file:///home/titan/fte3600-linux-driver/scratch/test_synthetic_suite.py)) was expanded with 11 dedicated tests covering all prompt requirements.

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
[PASS] Stage 2E.12.1: Module absent (return 1) proceeds past preflight under set -e
[PASS] Stage 2E.12.2: Module loaded (return 0) aborts preflight fail-closed
[PASS] Stage 2E.12.3: Module state inconclusive (return 2) aborts preflight fail-closed
[PASS] Stage 2E.12.4: query_module_state internal disagreement returns 2 (inconclusive)
[PASS] Stage 2E.12.5: Lock already held fails closed with non-zero exit
[PASS] Stage 2E.12.6: Lock unavailable (unwritable path) aborts fail-closed
[PASS] Stage 2E.12.7: Missing /dev/fte3600 node aborts fail-closed
[PASS] Stage 2E.12.8: Failed initial GET_STATE aborts before RESET with reset=0
[PASS] Stage 2E.12.9: RESET failure preserves rc=5 with exactly one attempt and zero retries
[PASS] Stage 2E.12.10: Signal interruption preserves rc=130 without unloading module
[PASS] Stage 2E.12.11: Cleanup re-entry is idempotent with zero recursion
[PASS] Path 14: Static code inspection (zero executable unload/unbind commands in test scripts)
===========================================================================
Summary: 38 passed, 0 failed out of 38 tests.
===========================================================================
```

**Verification Highlights:**
- **Stage 2E.12.1:** Proves that when `query_module_state` returns 1 (module absent), preflight execution proceeds without premature Bash `errexit` termination.
- **Stage 2E.12.2 & 2E.12.3:** Proves that loaded (0) and inconclusive (2) states abort preflight fail-closed.
- **Stage 2E.12.8 & 2E.12.9:** Proves that initial GET_STATE failure aborts before reset, and reset failure executes exactly one attempt with zero retries.

---

## 6. Diagnostic Description Correction for Pad 14

In [`diagnostics/STAGE_B_HARDWARE_EXECUTION_REPORT.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE_B_HARDWARE_EXECUTION_REPORT.md) and its mirrored artifact, the description of Pad 14 has been revised in accordance with Section 5:

- **Original Wording:**  
  `- Bit 0 (PADCFG0_GPIOTXSTATE): 1 (Output value HIGH / 3.3V).`
- **Corrected Wording:**  
  `- Bit 0 (PADCFG0_GPIOTXSTATE): 1 (Output TX bit configured to logical 1 / inactive deasserted state; nominal HIGH logic level, physical voltage unmeasured).`

This strictly bounds claims to observable register configuration and intended logical state without asserting unmeasured physical voltage.

---

## 7. Static Code Audit & Git Diff Summary

- **Syntax Validation:**
  - `bash -n scripts/stage-b-test.sh` $\rightarrow$ **PASS (0 syntax errors)**
  - `bash -n scripts/run-stage-b-in-terminal.sh` $\rightarrow$ **PASS (0 syntax errors)**
- **Static Invariant Search:**
  - `rmmod` / `modprobe -r` / `delete_module`: **0 executable occurrences**
  - sysfs driver unbind (`> ... unbind`): **0 executable occurrences**
  - Reset retry loops: **0 occurrences**
  - `FTE3600_IOC_RESET` invocations: **Exactly 1 reachable occurrence** ([`scripts/stage-b-test.sh:1362`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L1362))
- **`git diff --check`:** **0 formatting or whitespace errors**
- **Git Commit Policy:** **0 git commits created**

---

## 8. Final Status Declarations

```
PREFLIGHT BUG: FIXED
HARDWARE OPERATIONS: NONE
RESET ATTEMPTS: 0
AUTOMATIC MODULE UNLOAD: DISABLED
AUTOMATIC SPI UNBIND: DISABLED
```
