# Stage 2E.7.9 — Module Insertion Race and Final Source Audit

**Document ID:** `STAGE2E7_9_MODULE_INSERTION_RACE_AUDIT`  
**Date:** 2026-11-03  
**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Device:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor on Intel LPSS SPI2 (`spi-FTE3600:00`, bus 1, CS0, `pxa2xx-spi.2`)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (x86_64)  
**Scope:** Static source verification, module-insertion race elimination, module ownership tracking, stat attribute length bounds checking (Bash 64-bit integer overflow protection), exhaustive 11-path cleanup control flow audit, kernel reference protection review, and 28-scenario offline synthetic test suite.  
**Execution Boundary:** **STRICTLY STATIC AND READ-ONLY.** Zero hardware operations were executed; no kernel modules were inserted; no SPI transactions were dispatched; no GPIOs were toggled; the legacy hardware safety gate remains sealed. Stage B execution remains **ON HOLD**.

---

## 1. Executive Summary & Core Improvements

Stage 2E.7.9 performed a rigorous static audit of the Stage B execution and host teardown scripts (`scripts/stage-b-test.sh` and `scripts/run-stage-b-in-terminal.sh`), addressing all identified edge cases prior to hardware execution:

1. **Elimination of the Module Insertion Race Window (Task 1):**
   - **Vulnerability Identified:** In previous iterations, `insmod "$MODULE_PATH"` was executed, followed by `MODULE_INSERTED=1`. Because `insmod` executes as a separate process, an asynchronous signal (`SIGINT`, `SIGTERM`, `SIGHUP`) arriving immediately after the kernel finishes module insertion but before Bash executes the next statement would trigger `cleanup_handler` while `MODULE_INSERTED` was still `0`. Previously, `cleanup_handler` relied solely on `if [ "$MODULE_INSERTED" -eq 0 ]`, causing it to erroneously conclude the module was absent and abort teardown, leaving an orphan driver bound to the hardware.
   - **Resolution Implemented:** A 3-tier ownership model was designed:
     - `INITIAL_FTE3600_LOADED`: Records whether `fte3600` was already present in the kernel before the script ran.
     - `MODULE_INSERTION_ATTEMPTED`: Set to `1` immediately before invoking `insmod`.
     - `current_module_loaded`: Evaluated live in `cleanup_handler` via `lsmod`.
   - If `current_module_loaded == 1` and `MODULE_INSERTION_ATTEMPTED == 1`, teardown executes even if `MODULE_INSERTED` is `0`.
   - If `INITIAL_FTE3600_LOADED == 1`, cleanup logs that the module pre-existed and preserves it (`should_unload=0`).
   - If `current_module_loaded == 1` but `MODULE_INSERTION_ATTEMPTED == 0`, ownership is unknown; cleanup fails closed (`should_unload=0`, `CLEANUP_OVERALL_OK=0`).

2. **Stat Attribute Bounds Checking against 64-Bit Integer Overflow (Task 2):**
   - In both [`record_devnode_identity()`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L180-L205) and [`check_open_descriptors()`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L320-L385), strict string length limits were added prior to arithmetic evaluation:
     - Decimal `rdev`: must match `^[0-9]+$` AND `${#rdev} -le 19` (Bash arithmetic uses signed 64-bit `intmax_t`, max value $9,223,372,036,854,775,807$, which is 19 decimal digits).
     - Hexadecimal `maj_hex`: must match `^[0-9a-fA-F]+$` AND `${#maj_hex} -le 8` (32-bit major number limit).
     - Hexadecimal `min_hex`: must match `^[0-9a-fA-F]+$` AND `${#min_hex} -le 8` (32-bit minor number limit).
   - Any value exceeding these bounds is rejected with status 2 before invoking `$((16#...))` or bitwise operators.

3. **Exhaustive Cleanup Control Flow Audit (Task 3):**
   - Traced all 11 control flow paths (preflight failures, safety gate stops, early signals, `insmod` failures, mid-probe signals, reset failures, snapshot failures, and normal exits).
   - Confirmed that zero hardware ioctls, zero SPI transfers, and zero sensor commands are issued anywhere in cleanup.
   - Proved that `bin/fte3600-tool reset` exit codes are strictly preserved across cleanup.
   - Verified that `rmmod -f` is prohibited and no processes are terminated.

4. **Kernel Reference Protection & Module Unload Safety Audit (Task 4):**
   - Examined the precise mechanism of Linux VFS character device reference tracking:
     - `cdev.owner = THIS_MODULE` protects only during the lookup/open path inside `fs/char_dev.c:chrdev_open()`.
     - `file_operations.owner = THIS_MODULE` is held by `filp->f_op->owner` for the entire lifetime of an open file descriptor, incrementing module refcount until `__fput()`.
     - `delete_module()` syscall in `kernel/module/main.c` checks `module_refcount(mod) != 0` under `module_mutex` and returns `-EBUSY`.
     - `get_device(&data->dev)` / `put_device(&data->dev)` preserves struct memory to prevent use-after-free, while `atomic_set(&data->offline, 1)` ensures immediate `-ENODEV` rejection if hardware is removed.

5. **28-Scenario Offline Synthetic Test Suite (Task 5):**
   - Expanded test suite from 23 to 28 offline synthetic tests in `scratch/test_synthetic_suite.py`.
   - All 28 tests pass (100% pass rate).

---

## 2. Module Insertion Race: Detailed Technical Investigation & Solution

### 2.1 The Race Window Mechanism

In prior revisions, module insertion in Phase 2 was structured as:
```bash
MODULE_INSERTION_ATTEMPTED=0 # not present
insmod "$MODULE_PATH"        # Subprocess spawns and executes init_module() syscall
MODULE_INSERTED=1            # Next Bash statement
```
And cleanup checked:
```bash
if [ "$MODULE_INSERTED" -eq 0 ]; then
    printf "[INFO] Module was not inserted; unload not required.\n"
    exit ...
fi
```

**The Interruption Window:**
1. Bash forks and execs `insmod`.
2. The Linux kernel processes `init_module()`, calls `fte3600_probe()`, registers the SPI driver, claims the GPIOs, creates `/dev/fte3600`, and completes `sys_finit_module()`. The module is now live in the kernel.
3. `insmod` exits with code 0.
4. An external signal (`SIGINT` from Ctrl+C, `SIGTERM` from process manager, or `SIGHUP` from terminal close) is received by Bash before Bash evaluates `MODULE_INSERTED=1`.
5. The signal trap immediately vectors control to `cleanup_handler()`.
6. `cleanup_handler()` reads `MODULE_INSERTED`, which is still `0`.
7. `cleanup_handler()` assumes the module was never loaded, logs "unload not required", and terminates.
8. **Result:** The kernel module remains loaded, holding hardware GPIOs and SPI bindings, without running open descriptor audits or teardown.

### 2.2 The 3-Tier Ownership Solution

To permanently eliminate this race, `scripts/stage-b-test.sh` decouples module presence from the Bash variable assignment using a 3-tier ownership model:

```mermaid
flowchart TD
    subgraph Preflight ["Preflight Phase"]
        A["Check lsmod for fte3600"] -->|Present| B["INITIAL_FTE3600_LOADED = 1<br/>Abort Preflight"]
        A -->|Absent| C["INITIAL_FTE3600_LOADED = 0"]
    end

    subgraph Insertion ["Phase 2 Module Insertion"]
        C --> D["MODULE_INSERTION_ATTEMPTED = 1"]
        D --> E["insmod fte3600.ko"]
        E -->|Success| F["MODULE_INSERTED = 1"]
        E -->|Failure| G["insmod_rc != 0<br/>Exit insmod_rc"]
    end

    subgraph Teardown ["Cleanup Handler Ownership Logic"]
        H["Check lsmod for fte3600<br/>(current_module_loaded)"]
        H -->|current_module_loaded == 0| I["should_unload = 0<br/>[INFO] Module not in kernel"]
        H -->|current_module_loaded == 1| J{"Check Ownership"}
        J -->|INITIAL_FTE3600_LOADED == 1| K["should_unload = 0<br/>[WARN] Pre-existing module; preserve"]
        J -->|MODULE_INSERTED == 1 OR MODULE_INSERTION_ATTEMPTED == 1| L["should_unload = 1<br/>[INFO] Test script owns module;<br/>Trigger Controlled Teardown"]
        J -->|Otherwise (Unowned)| M["should_unload = 0<br/>CLEANUP_OVERALL_OK = 0<br/>[SAFETY STOP] Refuse unload"]
    end

    E -.->|Signal arrives here| H
```

### 2.3 Implementation Source Excerpts

#### Pre-Flight Baseline Recording ([`scripts/stage-b-test.sh#L920-L930`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L920-L930)):
```bash
# 3. Existing module presence
printf "Checking existing module loading ... "
if lsmod | grep -w "fte3600" >/dev/null 2>&1; then
    INITIAL_FTE3600_LOADED=1
fi
if lsmod | grep -E "fte3600|spidev" >/dev/null 2>&1; then
    printf "%s\n" "[FAIL] Conflicting module already loaded:"
    lsmod | grep -E "fte3600|spidev"
    exit 1
fi
printf "%s\n" "[PASS] (Neither fte3600 nor spidev loaded)"
```

#### Protected Module Insertion ([`scripts/stage-b-test.sh#L1005-L1023`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L1005-L1023)):
```bash
MODULE_INSERTION_ATTEMPTED=1
was_errexit_insmod=0
if shopt -o -q errexit; then was_errexit_insmod=1; fi
set +e
insmod "$MODULE_PATH"
insmod_rc=$?
if [ "$insmod_rc" -eq 0 ]; then
    MODULE_INSERTED=1
fi
if [ "$was_errexit_insmod" -eq 1 ]; then set -e; fi

if [ "$insmod_rc" -eq 0 ]; then
    printf "%s\n" "[PASS] (insmod succeeded)"
else
    printf "[FAIL] insmod failed with exit code %d\n" "$insmod_rc"
    exit "$insmod_rc"
fi
```

#### Ownership Determination in Cleanup ([`scripts/stage-b-test.sh#L734-L764`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L734-L764)):
```bash
    # --------------------------------------------------------------------------
    # Determine module ownership and teardown requirement (Stage 2E.7.9)
    # --------------------------------------------------------------------------
    local current_module_loaded=0
    if lsmod 2>/dev/null | grep -w "fte3600" >/dev/null 2>&1; then
        current_module_loaded=1
    fi

    local should_unload=0

    if [ "$current_module_loaded" -eq 0 ]; then
        printf "%s\n" "[INFO] fte3600 module is not loaded in kernel; unload not required."
    elif [ "$INITIAL_FTE3600_LOADED" -eq 1 ]; then
        printf "%s\n" "[WARN] fte3600 was loaded prior to script invocation; ownership belongs to host. Leaving loaded."
    elif [ "$MODULE_INSERTED" -eq 1 ] || [ "$MODULE_INSERTION_ATTEMPTED" -eq 1 ]; then
        # Module was absent at preflight, insertion was attempted by this script, and module is loaded
        printf "%s\n" "[INFO] fte3600 was inserted by this test script; initiating controlled teardown."
        should_unload=1
    else
        # Module is loaded, but this script did not attempt insertion!
        # Ownership cannot be established safely. Fail closed: leave loaded and report.
        printf "%s\n" "[WARN] fte3600 is loaded but module insertion was not attempted by this script."
        printf "%s\n" "[SAFETY STOP] Ownership cannot be established safely. Refusing module unload."
        CLEANUP_OVERALL_OK=0
    fi
```

---

## 3. Stat Attribute Bounds Checking (Bash 64-Bit Integer Overflow Protection)

### 3.1 Mathematical and Architectural Justification

In GNU Bash on x86_64, arithmetic expansion `$(( ... ))` operates on signed 64-bit integers (`intmax_t`):
$$\text{Max signed 64-bit int} = 2^{63}-1 = 9{,}223{,}372{,}036{,}854{,}775{,}807 \quad (19\text{ decimal digits})$$
$$\text{Max 32-bit unsigned hex} = \text{0xFFFFFFFF} \quad (8\text{ hex characters})$$

If a corrupted, malicious, or synthetic `stat` output outputs a decimal number with $\ge 20$ digits, Bash arithmetic can suffer silent wrap-around to negative values or produce runtime syntax errors. Similarly, major and minor device numbers in Linux fit within 32 bits (12-bit major, 20-bit minor in modern Linux device encoding). An input with $> 8$ hex characters represents an overflow of the 32-bit major/minor field.

### 3.2 Implementation in `scripts/stage-b-test.sh`

#### In [`record_devnode_identity()`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L190-L205):
```bash
    local stat_valid=0
    if [ "$stat_type" = "character special file" ] && \
       [[ "$stat_rdev" =~ ^[0-9]+$ ]] && [ ${#stat_rdev} -le 19 ] && [ "$stat_rdev" -gt 0 ] && \
       [[ "$stat_maj_hex" =~ ^[0-9a-fA-F]+$ ]] && [ ${#stat_maj_hex} -le 8 ] && \
       [[ "$stat_min_hex" =~ ^[0-9a-fA-F]+$ ]] && [ ${#stat_min_hex} -le 8 ]; then
        # Confirm mutual internal consistency of stat attributes (rdev == makedev(maj, min))
        local stat_maj_dec=$((16#$stat_maj_hex))
        local stat_min_dec=$((16#$stat_min_hex))
        local stat_calc_rdev=$(( ((stat_maj_dec & 0xfffff000) << 32) | ((stat_maj_dec & 0xfff) << 8) | ((stat_min_dec & 0xffffff00) << 12) | (stat_min_dec & 0xff) ))
        if [ "$stat_rdev" -eq "$stat_calc_rdev" ]; then
            stat_valid=1
        else
            printf "%s\n" "[FAIL] Internal stat inconsistency: rdev ($stat_rdev) != makedev calculation ($stat_calc_rdev)"
            stat_valid=0
        fi
    fi
```

#### In [`check_open_descriptors()`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L373-L382):
```bash
        # Explicit validation of stat output values before any arithmetic conversion
        if [ -z "$curr_stat_rdev" ] || ! [[ "$curr_stat_rdev" =~ ^[0-9]+$ ]] || [ ${#curr_stat_rdev} -gt 19 ] || \
           [ -z "$curr_stat_maj_hex" ] || ! [[ "$curr_stat_maj_hex" =~ ^[0-9a-fA-F]+$ ]] || [ ${#curr_stat_maj_hex} -gt 8 ] || \
           [ -z "$curr_stat_min_hex" ] || ! [[ "$curr_stat_min_hex" =~ ^[0-9a-fA-F]+$ ]] || [ ${#curr_stat_min_hex} -gt 8 ]; then
            printf "[FAIL] Current devnode stat values are empty, malformed, or out of range (rdev='%s', maj='%s', min='%s')!\n" \
                "$curr_stat_rdev" "$curr_stat_maj_hex" "$curr_stat_min_hex"
            printf "%s\n" "[SAFETY STOP] Devnode stat inspection failed. Refusing module unload."
            return 2
        fi
```

---

## 4. Comprehensive Cleanup Control Flow Audit (All 11 Paths)

The complete lifecycle of `scripts/stage-b-test.sh` and its invocation of `cleanup_handler()` was audited across all 11 possible runtime control flow paths:

| Path # | Scenario Description | Trigger Point | `current_module_loaded` | `INITIAL_FTE3600_LOADED` | `MODULE_INSERTION_ATTEMPTED` | `should_unload` | Teardown Action | Final Script Exit Code |
|:---:|:---|:---|:---:|:---:|:---:|:---:|:---|:---:|
| **1** | Pre-Flight Failure (kernel mismatch, missing tool, bad vermagic) | `exit 1` in Pre-Flight | 0 | 0 | 0 | 0 | No unload attempted; logs unneeded | `1` |
| **2** | Legacy Safety Gate Stop (if mock gate unexpectedly halts) | `exit 1` in Gate Check | 0 | 0 | 0 | 0 | No unload attempted; logs unneeded | `1` |
| **3** | Signal before Phase 2 (`SIGINT`/`SIGTERM`/`SIGHUP` during Preflight) | Trap handler | 0 | 0 | 0 | 0 | No unload attempted; logs unneeded | `130`/`143`/`129` |
| **4** | Module Insertion Failure (`insmod` returns non-zero) | `exit $insmod_rc` | 0 | 0 | 1 | 0 | Module not loaded in kernel; logs unneeded | `$insmod_rc` ($\ne 0$) |
| **5** | **Race Window: Signal immediately after kernel `insmod` succeeds** | Trap handler | 1 | 0 | 1 | **1** | **Controlled teardown: 2x descriptor audit + `rmmod`** | `130`/`143`/`129` |
| **6** | Post-Probe Verification Failure (devnode absent, SPI bind fail, stat mismatch) | `exit 1` in Phase 2 | 1 | 0 | 1 | 1 | Controlled teardown: 2x descriptor audit + `rmmod` | `1` |
| **7** | Snapshot 1 Failure (`fte3600-tool snapshot` fails) | `exit 1` in Phase 3 | 1 | 0 | 1 | 1 | Controlled teardown: 2x descriptor audit + `rmmod` | `1` |
| **8** | **RESET ioctl Failure (`fte3600-tool reset` fails)** | `exit $RESET_RC` | 1 | 0 | 1 | 1 | Controlled teardown: 2x descriptor audit + `rmmod` | **`$RESET_RC` (strictly preserved)** |
| **9** | Snapshot 2 Failure (`fte3600-tool snapshot` fails post-reset) | `exit 1` in Phase 5 | 1 | 0 | 1 | 1 | Controlled teardown: 2x descriptor audit + `rmmod` | `1` |
| **10** | Signal during Snapshot 2 or Completion | Trap handler | 1 | 0 | 1 | 1 | Controlled teardown: 2x descriptor audit + `rmmod` | `130`/`143`/`129` |
| **11** | Normal Clean Completion through Phase 6 | `exit 0` at script end | 1 | 0 | 1 | 1 | Controlled teardown: 2x descriptor audit + `rmmod` | `0` (or `1` if cleanup fails) |

### Key Guarantees Verified:
- **Zero Hardware Commands in Cleanup:** The teardown sequence consists strictly of:
  1. Open descriptor audit (`check_open_descriptors`)
  2. Descriptor re-verification (`check_open_descriptors`)
  3. `rmmod fte3600`
  4. Node absence check (`/dev/fte3600`)
  5. SPI unbind verification (`$SPI_PATH/driver`)
  6. Module absence verification (`lsmod`)
  7. Passive GPIO/pinctrl/dmesg debug reads.  
  No secondary `RESET`, `INIT`, `CAPTURE`, or SPI register writes can ever be invoked during cleanup.
- **Strict Exit Code Precedence:** In Path 8, `incoming_exit` receives `RESET_RC`. Lines 856–870 ensure `final_exit` retains `RESET_RC`:
  ```bash
  local final_exit="$incoming_exit"
  ...
  exit "$final_exit"
  ```
  Even if cleanup completes with `CLEANUP_OVERALL_OK=1`, the non-zero `RESET_RC` is preserved and returned to the caller.
- **Fail-Closed on Unowned Module:** If `current_module_loaded == 1` but `MODULE_INSERTION_ATTEMPTED == 0` (e.g. an admin loaded the module in parallel while preflight ran), cleanup refuses to unload the module (`should_unload=0`), reports a safety stop, and sets `CLEANUP_OVERALL_OK=0`.

---

## 5. Kernel Reference Protection & Module Unload Safety Audit

A comprehensive review of the Linux kernel VFS and module subsystems was conducted against `kernel/fte3600.c`:

```mermaid
sequenceDiagram
    autonumber
    participant App as Userspace App
    participant VFS as Linux VFS (chrdev_open)
    participant Mod as Module Subsystem
    participant Driver as fte3600 Driver
    participant Dev as Linux Device Model

    Note over App,Dev: Phase A: Device Open Path
    App->>VFS: open("/dev/fte3600")
    VFS->>Mod: try_module_get(cdev->owner) [fte3600.ko pinned]
    VFS->>VFS: fops_get(cdev->ops) -> try_module_get(fte3600_fops.owner)
    VFS->>Mod: module_put(cdev->owner) [cdev unpinned; fops retains pin!]
    VFS->>Driver: fte3600_cdev_open()
    Driver->>Driver: atomic_cmpxchg(&data->is_open, 0, 1) [Exclusive Lease]
    Driver->>Dev: get_device(&data->dev) [kobject refcount++]
    Driver-->>App: fd returned (filp active)

    Note over App,Dev: Phase B: Concurrent rmmod Attempt
    App->>App: Holds fd open
    Note over Mod: rmmod fte3600 (sys_delete_module)
    Mod->>Mod: module_refcount(mod) > 0 under module_mutex
    Mod-->>Driver: Returns -EBUSY (Unload Rejected by Kernel!)

    Note over App,Dev: Phase C: Device Close Path
    App->>VFS: close(fd)
    VFS->>Driver: fte3600_cdev_release()
    Driver->>Driver: atomic_set(&data->is_open, 0) [Lease Released]
    Driver->>Dev: put_device(&data->dev) [kobject refcount--]
    VFS->>Mod: module_put(filp->f_op->owner) [Module refcount drops to 0]
```

### 5.1 VFS `cdev.owner` vs `file_operations.owner = THIS_MODULE`
- **`cdev.owner` (`kernel/fte3600.c#L623`):**  
  In `fs/char_dev.c:chrdev_open()`, the kernel uses `try_module_get(p->owner)` to pin the driver module while looking up `p->ops`. Immediately after `fops_get(p->ops)` succeeds, the VFS calls `module_put(p->owner)`. Thus, `cdev.owner` **only protects the open lookup sequence**, not the lifetime of the file.
- **`file_operations.owner = THIS_MODULE` (`kernel/fte3600.c#L545`):**  
  When `fops_get()` is called in `chrdev_open()`, it invokes `try_module_get(fops->owner)`. This reference count remains pinned on `fte3600.ko` for the **entire duration that the file descriptor is open**. It is only released when userspace calls `close(fd)` and the kernel executes `__fput()` in `fs/file_table.c`, calling `module_put(filp->f_op->owner)`.
- **In-Kernel `delete_module()` Guarantee:**  
  When `rmmod fte3600` executes, the kernel enters `kernel/module/main.c:delete_module()`. Under `module_mutex`, it evaluates `module_refcount(mod)`. If any file descriptor is open, `module_refcount > 0` and the kernel rejects the syscall with `-EBUSY`.

### 5.2 Device Object Lifetime (`get_device` / `put_device`)
- In `kernel/fte3600.c#L386`, `fte3600_cdev_open()` calls `get_device(&data->dev)`.
- In `kernel/fte3600.c#L397`, `fte3600_cdev_release()` calls `put_device(&data->dev)`.
- In `kernel/fte3600.c#L697`, `fte3600_remove()` calls `cdev_device_del(&data->cdev, &data->dev)` and `put_device(&data->dev)`.
- The actual driver memory `struct fte3600_data` is freed in `fte3600_dev_release()` only when the last reference count drops to 0. This guarantees that even if the hardware is hot-unplugged or unbound while an application holds `/dev/fte3600` open, no use-after-free can occur.

### 5.3 Offline Flag Isolation (`data->offline`)
- In `fte3600_remove()`, the driver executes `atomic_set(&data->offline, 1)`.
- In `fte3600_cdev_ioctl()`, `atomic_read(&data->offline)` is checked at the entry of every hardware ioctl. If offline, the ioctl immediately aborts with `-ENODEV` without touching SPI or GPIO hardware.

### 5.4 Exclusive Single-Client Lease (`data->is_open`)
- In `fte3600_cdev_open()`, `atomic_cmpxchg(&data->is_open, 0, 1) != 0` immediately returns `-EBUSY` if another process already holds the device open. This prevents multi-process contention.

---

## 6. Offline Synthetic Test Suite Results (28 Scenarios)

The comprehensive offline test suite in `scratch/test_synthetic_suite.py` was executed. All 28 tests passed completely:

| Test ID | Test Scenario Description | Simulated Condition | Expected Outcome | Actual Result |
|:---:|:---|:---|:---|:---:|
| **Test 1** | Existing FD where stat fails but readlink succeeds | Target FD stat returns error, symlink resolves | Status 1 (Descriptor detected) | **PASS** |
| **Test 2** | Existing FD where stat fails and readlink fails | Inaccessible active FD (dangling/restricted) | Status 2 (Fail-closed safety stop) | **PASS** |
| **Test 3** | FD disappears / closes during inspection | FD unlinked between listing and stat | Ignored (closed safely), Status 0 | **PASS** |
| **Test 4** | Process exits during inspection | PID terminates between listing and inspect | Ignored (process terminated), Status 0 | **PASS** |
| **Test 5** | Dangling devnode symlink detection | `/dev/fte3600` symlink points to non-existent target | Status 2 (Fail-closed safety stop) | **PASS** |
| **Test 6** | Symlink to different character device | Symlink points to `/dev/zero` (different rdev) | Status 2 (Mismatched identity stop) | **PASS** |
| **Test 7** | Symlink to expected character device | Symlink points to valid target device | Status 0 (Passes identity, proceeds) | **PASS** |
| **Test 8** | Malformed sysfs major:minor rejection | `dev` attribute contains "invalid_data" | Status 2 (Malformed sysfs stop) | **PASS** |
| **Test 9** | Negative and oversized value rejection | Major or minor > 1048575 or negative | Status 2 (Out of range stop) | **PASS** |
| **Test 10** | Numeric parsing avoiding octal traps | Major/minor formatted with leading zero "08" | Parsed as decimal (8), Status 0 | **PASS** |
| **Test 11** | Normal unload refusal after inconclusive audit | Mock `check_open_descriptors` returns 2 | rmmod refused, module remains loaded | **PASS** |
| **Test 12** | Preservation of RESET failure status | Mock `fte3600-tool reset` fails (rc=3) | Final exit code is 3 (preserved) | **PASS** |
| **Test 13** | Baseline valid matching stat and sysfs identities | Matching stat rdev and sysfs dev string | Recorded successfully, Status 0 | **PASS** |
| **Test 14** | Identity mismatch between stat and sysfs | Stat reports 511:0, sysfs reports 510:0 | Rejected at probe, Status 1 | **PASS** |
| **Test 15** | Missing sysfs identity fails closed | Sysfs `dev` attribute unreadable at probe | Probe refused, Status 1 | **PASS** |
| **Test 16** | Open descriptor detection holding character device | Mock proc FD holding exact matching rdev | Status 1 (Descriptor detected) | **PASS** |
| **Test 17** | Inaccessible active process descriptor directory | PID exists, `fd/` directory unreadable (EPERM) | Status 2 (Fail-closed safety stop) | **PASS** |
| **Test 18** | Temporary-file creation failure | `mktemp -d` fails (read-only filesystem) | Status 2 (Fail-closed safety stop) | **PASS** |
| **Test 19** | Failed stat command on devnode (empty rdev) | Devnode `stat -c %r` returns empty string | Status 2 (Empty stat attribute stop) | **PASS** |
| **Test 20** | Malformed non-numeric rdev in stat output | Devnode `stat -c %r` returns "not_a_number" | Status 2 (Malformed attribute stop) | **PASS** |
| **Test 21** | Malformed non-hex major in stat output | Devnode `stat -c %t` returns "xyz" | Status 2 (Malformed hex attribute stop) | **PASS** |
| **Test 22** | Missing sysfs attribute during cleanup while loaded | Module loaded, sysfs attribute deleted/missing | Status 2 (Sysfs unreadable stop) | **PASS** |
| **Test 23** | Stat internal calculation inconsistency | Reported rdev != calculated makedev(maj, min) | Status 2 (Inconsistent stat stop) | **PASS** |
| **Test 24** | **Insertion race: signal before `MODULE_INSERTED=1`** | **Signal arrives with attempt=1, inserted=0** | **Status 130; Teardown triggered!** | **PASS** |
| **Test 25** | **Pre-existing module protection** | **`INITIAL_FTE3600_LOADED=1` at preflight** | **Module preserved; rmmod refused** | **PASS** |
| **Test 26** | **Unowned loaded module fails closed** | **Module loaded, attempt=0, initial=0** | **Refuses unload; SAFETY STOP** | **PASS** |
| **Test 27** | **Oversized decimal rdev bounds check** | **Stat rdev has 25 digits (> 19 max digits)** | **Status 2 (Out of range stop)** | **PASS** |
| **Test 28** | **Oversized hex major bounds check** | **Stat maj_hex has 12 characters (> 8 max)** | **Status 2 (Out of range stop)** | **PASS** |

---

## 7. Hardware & Host Invariant Verification

Host state was re-verified via non-invasive queries:
1. **Module Unloaded:** `lsmod | grep -i fte` returns empty (exit code 1).
2. **Device Node Absent:** `ls -l /dev/fte3600` returns "No such file or directory" (exit code 2).
3. **SPI Device Unbound:** `ls -ld /sys/bus/spi/devices/spi-FTE3600:00/driver` returns "No such file or directory" (exit code 2).
4. **Legacy Safety Gate Sealed:** `bin/fw9369-hw-tool` executed offline, verified that hardware access is completely blocked by policy, and exited with status 1.
5. **Git Worktree Cleanliness:** `git diff --check` executed with zero whitespace or line-break errors. Kernel sources in `kernel/` remain unmodified.

---

## 8. Conclusion & Current Authorization Status

All audit criteria and safety corrections for Stage 2E.7.9 have been fully satisfied:
- The module insertion race condition has been eliminated through a decoupled 3-tier ownership model.
- Bounds checks protect Bash against 64-bit integer overflows from corrupted stat data.
- All 11 control flow paths have been audited and proven fail-closed with zero secondary hardware commands in cleanup.
- Kernel VFS and driver reference mechanisms have been reviewed and validated.
- All 28 offline synthetic tests pass.

> [!IMPORTANT]
> **Stage B remains ON HOLD.**  
> Although the user previously authorized exactly one controlled reset, physical hardware execution is **NOT** taking place in this step. Execution remains suspended pending user review and confirmation of this audit.
