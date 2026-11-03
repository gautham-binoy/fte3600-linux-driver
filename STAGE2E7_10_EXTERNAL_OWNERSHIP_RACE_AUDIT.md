# Stage 2E.7.10 — External Module Ownership Race and Signal-Safe Insertion Audit

**Document ID:** `STAGE2E7_10_EXTERNAL_OWNERSHIP_RACE_AUDIT`  
**Date:** 2026-11-03  
**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Device:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor on Intel LPSS SPI2 (`spi-FTE3600:00`, bus 1, CS0, `pxa2xx-spi.2`)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (x86_64)  
**Scope:** Static source verification, external module ownership race elimination, signal-safe insertion critical section design, multi-tier module state inspection, cooperative test runner locking, re-audit of cleanup handler integration, and 36-scenario offline synthetic test suite.  
**Execution Boundary:** **STRICTLY STATIC AND READ-ONLY.** Zero hardware operations were executed; no kernel modules were inserted; no SPI transfers were dispatched; no GPIOs were toggled; the legacy hardware safety gate remains sealed. Stage B remains **ON HOLD**.

---

## 1. Executive Summary & Root Cause Analysis

### 1.1 The External Ownership Race Defect in Prior Implementation
In Stage 2E.7.9, the ownership logic in [`cleanup_handler()`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh) evaluated:
```bash
elif [ "$MODULE_INSERTED" -eq 1 ] || [ "$MODULE_INSERTION_ATTEMPTED" -eq 1 ]; then
    should_unload=1
```
This assumed that setting `MODULE_INSERTION_ATTEMPTED=1` immediately before `insmod`, combined with observing the module loaded in the kernel during cleanup, constituted proof that this script owned the module.

**The Failure Scenario:**
1. Pre-flight Check 3 runs and observes `fte3600` is absent (`INITIAL_FTE3600_LOADED=0`).
2. An external administrator or system process concurrently loads `fte3600` (e.g. via `modprobe fte3600`).
3. This script enters Phase 2 and sets `MODULE_INSERTION_ATTEMPTED=1`.
4. `insmod "$MODULE_PATH"` runs and fails with exit code 1 (`-EEXIST`, "File exists") because the module is already loaded in the kernel.
5. The script terminates with exit code 1, triggering `cleanup_handler 1 "EXIT"`.
6. `cleanup_handler` inspects the kernel and observes `fte3600` is loaded. Because `MODULE_INSERTION_ATTEMPTED=1` was set, the previous logic concluded that *this* script owned the module, and dispatched `rmmod fte3600`, erroneously unloading the external administrator's module!

### 1.2 The Corrected Invariant
Setting `MODULE_INSERTION_ATTEMPTED=1` before `insmod` proves **intent**, not **ownership**.  
Under no circumstances may a failed `insmod` grant ownership. The script must establish that **this particular invocation successfully inserted the module** (`THIS_INVOCATION_INSERTED=1`) before it can ever be eligible for teardown.

---

## 2. Signal-Safe Critical Section Design

### 2.1 The Interruption Problem
If standard signal traps (`INT`, `TERM`, `HUP`) are active during foreground `insmod` execution:
- If a signal arrives while `insmod` is running or immediately after `insmod` returns 0, the trap handler immediately interrupts execution and jumps to `cleanup_handler`.
- If the script relies solely on `MODULE_INSERTED=1` set *after* `insmod`, an interruption in the window between `insmod` returning and Bash scheduling the variable assignment leaves `MODULE_INSERTED=0`.
- Conversely, if the script prematurely claims ownership before `insmod`, a failed insertion followed by cleanup unloads external modules.

### 2.2 The Critical Section Solution
To make insertion completely signal-safe, Phase 2 implements a critical section with temporary signal deferral:

```mermaid
sequenceDiagram
    autonumber
    participant Trap as Bash Trap Subsystem
    participant Script as stage-b-test.sh
    participant Kernel as Linux Kernel (insmod)
    participant Cleanup as cleanup_handler()

    Note over Script,Trap: Enter Critical Section
    Script->>Script: THIS_INVOCATION_INSERTED = 0
    Script->>Trap: trap 'defer_signal' INT TERM HUP
    Script->>Script: set +e (disable errexit)

    Script->>Kernel: insmod kernel/fte3600.ko
    Note over Kernel: Kernel loads module
    alt Signal arrives during insmod or return
        Trap-->>Script: Signal delivered -> defer_signal records DEFERRED_SIGNAL
    end
    Kernel-->>Script: Returns insmod_rc (0 or non-zero)

    Note over Script: Atomic State Commit
    Script->>Script: Capture insmod_rc=$?
    opt insmod_rc == 0
        Script->>Script: THIS_INVOCATION_INSERTED = 1
    end

    Note over Script,Trap: Exit Critical Section
    Script->>Trap: Restore trap 'cleanup_handler' INT TERM HUP
    Script->>Script: Restore errexit if previously enabled

    alt DEFERRED_SIGNAL is set
        Script->>Cleanup: Dispatch deferred signal (cleanup_handler DEFERRED_SIGNAL)
        Note over Cleanup: Cleanup evaluates THIS_INVOCATION_INSERTED with 100% accurate state
    else insmod_rc != 0
        Script->>Cleanup: Exit insmod_rc -> EXIT trap -> cleanup_handler
        Note over Cleanup: THIS_INVOCATION_INSERTED == 0 -> Refuses unload; Preserves module
    else Normal flow
        Script->>Script: Proceed to post-probe verification
    end
```

### 2.3 Implementation Source Excerpts

#### Signal Deferral Helper ([`scripts/stage-b-test.sh#L313-L325`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L313-L325)):
```bash
defer_signal() {
    local sig_code="$1"
    local sig_name="$2"
    if [ -z "$DEFERRED_SIGNAL" ] || [ "$sig_code" -eq 143 ]; then
        DEFERRED_SIGNAL="$sig_code"
        DEFERRED_SIGNAL_NAME="$sig_name"
    fi
    printf "\n[WARN] Signal %s (%d) received during insertion critical section; deferred until ownership state is committed.\n" \
        "$sig_name" "$sig_code" >&2
}
```

#### Insertion Critical Section ([`scripts/stage-b-test.sh#L1145-L1180`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L1145-L1180)):
```bash
# Critical Section: Signal-Safe Module Insertion (Stage 2E.7.10)
MODULE_INSERTION_ATTEMPTED=1
THIS_INVOCATION_INSERTED=0
DEFERRED_SIGNAL=""
DEFERRED_SIGNAL_NAME=""

trap 'defer_signal 130 "SIGINT"' INT
trap 'defer_signal 143 "SIGTERM"' TERM
trap 'defer_signal 129 "SIGHUP"' HUP

was_errexit_insmod=0
if shopt -o -q errexit; then was_errexit_insmod=1; fi
set +e
insmod "$MODULE_PATH"
insmod_rc=$?

if [ "$insmod_rc" -eq 0 ]; then
    THIS_INVOCATION_INSERTED=1
    MODULE_INSERTED=1
fi

# Critical state committed. Restore standard trap handlers.
trap 'cleanup_handler 130 "SIGINT"' INT
trap 'cleanup_handler 143 "SIGTERM"' TERM
trap 'cleanup_handler 129 "SIGHUP"' HUP

if [ "$was_errexit_insmod" -eq 1 ]; then set -e; fi

# Process any signal received during the critical section
if [ -n "$DEFERRED_SIGNAL" ]; then
    printf "[INFO] Processing deferred signal %s (%d) after critical section (THIS_INVOCATION_INSERTED=%d).\n" \
        "$DEFERRED_SIGNAL_NAME" "$DEFERRED_SIGNAL" "$THIS_INVOCATION_INSERTED"
    cleanup_handler "$DEFERRED_SIGNAL" "$DEFERRED_SIGNAL_NAME"
fi

if [ "$insmod_rc" -eq 0 ]; then
    printf "%s\n" "[PASS] (insmod succeeded; ownership verified and committed)"
else
    printf "[FAIL] insmod failed with exit code %d\n" "$insmod_rc"
    exit "$insmod_rc"
fi
```

---

## 3. Cooperative Locking (`flock`) vs External Loaders

### 3.1 Cooperative Lock Implementation
A cooperative file lock is acquired in Pre-Flight Check 0 ([`scripts/stage-b-test.sh#L1000-L1015`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L1000-L1015)) using `/run/lock/fte3600-test.lock`:
```bash
LOCK_FILE="/run/lock/fte3600-test.lock"
printf "Checking cooperative test runner lock ... "
if [ -d "/run/lock" ]; then
    if ! exec 200>"$LOCK_FILE" 2>/dev/null; then
        printf "%s\n" "[WARN] Unable to open cooperative lock file $LOCK_FILE; continuing without lock."
    elif ! flock -n 200; then
        printf "%s\n" "[FAIL] Another instance of the Stage B test runner is currently holding $LOCK_FILE!"
        exit 1
    else
        printf "%s\n" "[PASS] (Cooperative lock acquired on $LOCK_FILE)"
    fi
else
    printf "%s\n" "[WARN] /run/lock directory absent; continuing without lock."
fi
```
The lock is released cleanly upon exit in `cleanup_handler()`:
```bash
if { true >&200; } 2>/dev/null; then
    flock -u 200 2>/dev/null || true
    exec 200>&- 2>/dev/null || true
fi
```

### 3.2 Security and Scope Limitations of `flock`
- **What `flock` protects:** Multiple invocations of `scripts/stage-b-test.sh` or `scripts/run-stage-b-in-terminal.sh` running simultaneously on the same host will detect each other and fail immediately with exit code 1.
- **What `flock` CANNOT protect:** Uncoordinated external loaders (such as an administrator running `modprobe fte3600`, udev rules, systemd services, or DKMS rebuild scripts) do not participate in `/run/lock/fte3600-test.lock`.
- **Architectural Guarantee:** Because cooperative locks cannot govern external entities, the in-script ownership verification (`THIS_INVOCATION_INSERTED=1` committed only on `insmod_rc == 0`) remains the primary, authoritative defense against touching external modules.

---

## 4. Multi-Tier Module State Inspection (`query_module_state`)

### 4.1 Inconclusive Inspection Handling
A key defect addressed in Stage 2E.7.10 is that failures in `lsmod` or `/proc` inspection must **never** be interpreted as proof that a module is absent.

[`query_module_state()`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L265-L310) establishes a 3-tier inspection protocol:
1. **Sysfs Directory (`/sys/module/<mod_name>`):** Primary kernel kobject hierarchy. If present, module is confirmed LOADED (status 0).
2. **Direct `/proc/modules` Read:** Kernel module list. If `^mod_name\b` matches, confirmed LOADED (status 0).
3. **`lsmod` Execution:** Fallback userspace utility.
4. **Absence Determination:** A module is confirmed ABSENT (status 1) **only** if at least one inspection source succeeded without errors and confirmed the module is not present.
5. **Inspection Failure:** If sysfs, `/proc/modules`, and `lsmod` all fail or are unreadable, the function returns **status 2 (INCONCLUSIVE / ERROR)**.

### 4.2 Integration in `cleanup_handler`
When `query_module_state` returns status 2:
```bash
    elif [ "$mod_state" -eq 2 ]; then
        # Inspection error / inconclusive
        printf "%s\n" "[FAIL] Unable to determine fte3600 module state in kernel (inspection inconclusive)."
        if [ "$THIS_INVOCATION_INSERTED" -eq 1 ]; then
            printf "%s\n" "[WARN] This invocation successfully inserted the module, but module state inspection failed."
            printf "%s\n" "[SAFETY STOP] Cannot safely audit module state. Refusing unload to prevent kernel instability."
            CLEANUP_OVERALL_OK=0
        else
            printf "%s\n" "[SAFETY STOP] Cannot verify module state and this invocation did not insert it. Refusing unload."
            CLEANUP_OVERALL_OK=0
        fi
```
The script immediately halts with a Safety Stop, refuses to call `rmmod`, and marks teardown failed (`CLEANUP_OVERALL_OK=0`).

---

## 5. Strict Ownership Rules Matrix

The revised ownership rules in `cleanup_handler()` guarantee:

| Scenario | `INITIAL_FTE3600_LOADED` | `THIS_INVOCATION_INSERTED` | `mod_state` | Cleanup Action | Calls `rmmod`? | Exit Code |
|:---|:---:|:---:|:---:|:---|:---:|:---:|
| **Pre-existing module** | 1 | 0 | 0 (loaded) | Logs pre-existing; preserves module | **NO** | 1 |
| **External module loaded after preflight** | 0 | 0 | 0 (loaded) | Logs external ownership; Safety Stop | **NO** | `$insmod_rc` (1) |
| **`insmod` failed (EEXIST / ENODEV)** | 0 | 0 | 0 or 1 | Failed insertion never grants ownership | **NO** | `$insmod_rc` |
| **Successful insertion + normal finish** | 0 | 1 | 0 (loaded) | Controlled teardown: audits + `rmmod` | **YES** | 0 |
| **Successful insertion + pending signal** | 0 | 1 | 0 (loaded) | Controlled teardown: audits + `rmmod` | **YES** | 130 / 143 / 129 |
| **Signal during insertion critical section** | 0 | 1 | 0 (loaded) | Controlled teardown: audits + `rmmod` | **YES** | 130 / 143 / 129 |
| **Inconclusive ownership (unowned load)** | 0 | 0 | 0 (loaded) | Safety Stop: refuses unload | **NO** | 1 |
| **Inconclusive module inspection** | 0 | 1 or 0 | 2 (error) | Safety Stop: refuses unload | **NO** | 1 |

---

## 6. Offline Synthetic Test Suite Results (36/36 PASS)

The test suite in [`scratch/test_synthetic_suite.py`](file:///home/titan/.gemini/antigravity/brain/b45cb0fc-a222-4605-b5ac-d0240eac7c36/scratch/test_synthetic_suite.py) was executed. All 36 tests passed offline:

```
===========================================================================
Stage 2E.7.10 Offline Synthetic Test Suite
===========================================================================
[PASS] Test 1: Existing FD where stat fails but readlink succeeds
[PASS] Test 2: Existing FD where stat fails and readlink fails
[PASS] Test 3: FD disappears / closes during inspection
[PASS] Test 4: Process exits during inspection
[PASS] Test 5: Dangling devnode symlink detection
[PASS] Test 6: Symlink to different character device
[PASS] Test 7: Symlink to expected character device
[PASS] Test 8: Malformed sysfs major:minor rejection
[PASS] Test 9: Negative and oversized value rejection
[PASS] Test 10: Numeric parsing avoiding octal traps
[PASS] Test 11: Normal unload refusal after inconclusive audit
[PASS] Test 12: Preservation of RESET failure status
[PASS] Test 13: Baseline valid matching stat and sysfs identities
[PASS] Test 14: Identity mismatch between stat and sysfs
[PASS] Test 15: Missing sysfs identity fails closed
[PASS] Test 16: Open descriptor detection holding character device
[PASS] Test 17: Inaccessible active process descriptor directory
[PASS] Test 18: Temporary-file creation failure
[PASS] Test 19: Failed stat command on devnode (empty rdev)
[PASS] Test 20: Malformed non-numeric rdev in stat output
[PASS] Test 21: Malformed non-hex major in stat output
[PASS] Test 22: Missing sysfs attribute during cleanup while module is loaded
[PASS] Test 23: Stat internal calculation inconsistency
[PASS] Test 24: Insertion race: signal before MODULE_INSERTED=1
[PASS] Test 25: Pre-existing module protection
[PASS] Test 26: Unowned loaded module fails closed
[PASS] Test 27: Oversized decimal rdev bounds check
[PASS] Test 28: Oversized hex major bounds check
[PASS] Test 29: External module load after preflight before insmod
[PASS] Test 30: insmod failure because module is already loaded
[PASS] Test 31: Successful insmod followed by pending signal
[PASS] Test 32: Signal during insertion critical section
[PASS] Test 33: Successful insertion with normal cleanup
[PASS] Test 34: Pre-existing module protection
[PASS] Test 35: Inconclusive ownership state refusing unload
[PASS] Test 36: Inconclusive module-state inspection refusing unload
===========================================================================
Summary: 36 passed, 0 failed out of 36 tests.
===========================================================================
```

### Detailed Verification of Tests 29–36:
- **Test 29 (External module load after preflight before insmod):** Verified that when `insmod` fails with `EEXIST` due to an external module load, `THIS_INVOCATION_INSERTED` remains 0, `cleanup_handler` refuses unload with a Safety Stop, `rmmod` is **NOT called**, and script exits with 1.
- **Test 30 (insmod failure because module is already loaded):** Verified that a failed `insmod` never grants ownership; `rmmod` is **NOT called**, and script exits with 17.
- **Test 31 (Successful insmod followed by pending signal):** Verified that signal deferral preserves `THIS_INVOCATION_INSERTED=1`; teardown is dispatched cleanly, `rmmod` **IS called**, and final exit status is 130.
- **Test 32 (Signal during insertion critical section):** Verified that an active `SIGINT` delivered while foreground `insmod` is executing is caught by `defer_signal` without interrupting `insmod_rc` capture; `THIS_INVOCATION_INSERTED=1` is committed, `rmmod` **IS called**, and final exit status is 130.
- **Test 33 (Successful insertion with normal cleanup):** Verified normal clean path; `THIS_INVOCATION_INSERTED=1` triggers full controlled teardown, `rmmod` **IS called**, and final exit status is 0.
- **Test 34 (Pre-existing module protection):** Verified that `INITIAL_FTE3600_LOADED=1` completely blocks unload; `rmmod` is **NOT called**, pre-existing module is preserved, and final exit status is 1.
- **Test 35 (Inconclusive ownership state refusing unload):** Verified that if a module is loaded without verified insertion by this script, `cleanup_handler` fails closed; `rmmod` is **NOT called**, and script exits with 1 (`CLEANUP_OVERALL_OK=0`).
- **Test 36 (Inconclusive module-state inspection refusing unload):** Verified that when `query_module_state` returns 2 (inspection error), cleanup refuses to assume module absence; `rmmod` is **NOT called**, Safety Stop is logged, and script exits with 1 (`CLEANUP_OVERALL_OK=0`).

---

## 7. Hardware & Host Invariants Verification

Host state was queried via non-invasive commands:
- **Module state:** `lsmod | grep -i fte` is empty (`fte3600` is NOT loaded).
- **Device node:** `/dev/fte3600` is absent.
- **SPI binding:** `/sys/bus/spi/devices/spi-FTE3600:00/driver` is absent (unbound).
- **Legacy Safety Gate:** `bin/fw9369-hw-tool` executed offline, verified physical access is blocked by policy, and exited 1.
- **Syntax & Worktree:** `bash -n scripts/stage-b-test.sh`, `bash -n scripts/run-stage-b-in-terminal.sh`, and `git diff --check` passed cleanly. Kernel sources in `kernel/` remain unmodified.

---

## 8. Conclusion and Stop Condition

All requirements for Stage 2E.7.10 have been met:
- The external module ownership race has been eliminated.
- Signal deferral around `insmod` provides atomic ownership state commitment.
- Cooperative locking coordinates test runs while in-script ownership verification protects against external entities.
- Module state inspection is genuinely fail-closed.
- All 36 offline synthetic tests pass.

> [!IMPORTANT]
> **Stage B remains ON HOLD.**  
> Physical hardware execution has **NOT** taken place. Execution remains suspended pending user review of this audit.
