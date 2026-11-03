# Stage 2E.7.7 — Final Fail-Closed Descriptor Audit Corrections

**Document ID:** `STAGE2E7_7_FINAL_DESCRIPTOR_AUDIT`  
**Date:** 2026-11-03  
**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Device:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor on Intel LPSS SPI2 (`spi-FTE3600:00`, bus 1, CS0)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (x86_64)  
**Scope:** Static source verification, fail-closed FD identity handling, dangling symlink detection, octal-safe device identity conversion, complete cleanup path audit, and expanded offline synthetic testing (18 scenarios).  
**Execution Boundary:** **STRICTLY STATIC AND READ-ONLY.** Zero hardware operations executed; no module insertion; no SPI transactions; no GPIO toggled; legacy hardware safety gate remains sealed.

---

## 1. Executive Summary & Exact Changes Made

Stage 2E.7.7 addressed the remaining safety and fail-closed edge cases identified in `scripts/stage-b-test.sh`:

1. **Inconclusive FD Identity Handling (Task 1):**
   - In [`check_open_descriptors()`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L555-L605), the FD inspection loop was revised so that `stat -L` failure on any existing FD of a living process immediately increments `proc_read_errors`.
   - A successful `readlink` pointing to an unrelated path is **never** permitted to override an inconclusive identity inspection. Pathname matching is strictly supplementary.
   - If `cand_stat` is empty, the code re-verifies whether the process exited (`[ ! -d "$proc_dir" ]`) or the FD closed (`[ ! -e "$fd_link" ] && [ ! -L "$fd_link" ]`). Only confirmed benign disappearances are skipped; otherwise, the inspection is flagged inconclusive and forces return code 2 (Safety Stop: module unload refused).

2. **Dangling Symlink & Devnode Replacement Detection (Task 2):**
   - The existence test for `/dev/fte3600` was rewritten to handle symlinks explicitly via `[ -L "$target_node" ]`.
   - **Dangling symlink:** If `[ -L "$target_node" ]` is true but `[ ! -e "$target_node" ]` is true, the script detects a dangling symlink, logs a safety stop, and returns code 2.
   - **Symlink to non-character device:** If `stat -L -c "%F"` is not `character special file` (e.g. symlink to regular file or directory), returns code 2.
   - **Symlink to different character device:** If resolved `stat -L -c "%r"` does not match `target_rdev`, returns code 2.
   - **Symlink to expected character device:** Verified against `target_rdev` and logged cleanly.
   - **Non-character file or directory:** If `[ -e "$target_node" ]` is true but `[ ! -c "$target_node" ]`, returns code 2.
   - **Missing devnode:** Detected when `[ ! -e "$target_node" ] && [ ! -L "$target_node" ]`, proceeding with audit using immutable recorded probe identity (`RECORDED_DEV_RDEV`).

3. **Octal-Safe Identity Conversion & Nameref Isolation (Task 3):**
   - In [`convert_sysfs_dev_to_identity()`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L75-L135), all internal local variables and namerefs now use the `_csd_` prefix to guarantee zero collision with caller variable names.
   - Leading zeros are normalized (`${val#"${val%%[!0]*}"}`) and parsed explicitly in base 10 (`$(( 10#$_csd_maj_norm ))`), completely eliminating Bash octal traps (such as `010` becoming 8 or `08` causing `value too great for base` syntax errors).
   - String length checks (`${#_csd_maj_norm} -gt 4` and `${#_csd_min_norm} -gt 7`) prevent 64-bit integer overflow before arithmetic evaluation.
   - Numeric range checks strictly enforce $0 \le \text{major} \le 4095$ and $0 \le \text{minor} \le 1048575$.

4. **Complete Cleanup Path Audit (Task 4):**
   - Full read of `scripts/stage-b-test.sh` verified the cleanup sequence: error handling safety, zero process termination, immutable reference identity, two independent open descriptor audits, graceful `rmmod fte3600` only, zero forced unloading, and absolute preservation of `RESET_RC` exit code.
   - Documented kernel `.owner = THIS_MODULE` atomic VFS protection.

5. **Stage B Boundary Verification (Task 5):**
   - Verified that the execution script contains exactly one invocation of `"$TOOL_BIN" reset`, zero retries, zero INIT, zero CAPTURE, zero SPI transfers, and zero manual register/GPIO writes.

6. **Expanded Offline Synthetic Testing (Task 6):**
   - 18 offline test cases executed with 100% pass rate (18 passed, 0 failed).

---

## 2. Updated Helper Functions

### 2.1 `convert_sysfs_dev_to_identity()`
From [`scripts/stage-b-test.sh`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh) lines 75–135:
```bash
convert_sysfs_dev_to_identity() {
    local _csd_raw="$1"
    local -n _csd_out_rdev=$2
    local -n _csd_out_maj_hex=$3
    local -n _csd_out_min_hex=$4
    local -n _csd_out_majmin_hex=$5

    _csd_out_rdev=""
    _csd_out_maj_hex=""
    _csd_out_min_hex=""
    _csd_out_majmin_hex=""

    # 1. Reject empty
    if [ -z "$_csd_raw" ]; then
        return 1
    fi

    # 2. Strict format check: digits:digits only (rejects negative numbers, non-digits, etc.)
    if ! [[ "$_csd_raw" =~ ^[0-9]+:[0-9]+$ ]]; then
        return 1
    fi

    local _csd_maj_raw
    local _csd_min_raw
    _csd_maj_raw=$(echo "$_csd_raw" | cut -d: -f1)
    _csd_min_raw=$(echo "$_csd_raw" | cut -d: -f2)

    # 3. Strip any leading zeros to eliminate accidental octal interpretation in bash, preserving single "0"
    local _csd_maj_norm="${_csd_maj_raw#"${_csd_maj_raw%%[!0]*}"}"
    _csd_maj_norm="${_csd_maj_norm:-0}"
    local _csd_min_norm="${_csd_min_raw#"${_csd_min_raw%%[!0]*}"}"
    _csd_min_norm="${_csd_min_norm:-0}"

    # 4. Range validation: check digit length to prevent 64-bit integer overflow before arithmetic evaluation
    # Major max 4095 (4 digits); Minor max 1048575 (7 digits)
    if [ ${#_csd_maj_norm} -gt 4 ] || [ ${#_csd_min_norm} -gt 7 ]; then
        return 1
    fi

    # Parse explicitly in base-10
    local _csd_maj_dec=$(( 10#$_csd_maj_norm ))
    local _csd_min_dec=$(( 10#$_csd_min_norm ))

    # Linux device number range validation (major: 0..4095, minor: 0..1048575)
    if [ "$_csd_maj_dec" -lt 0 ] || [ "$_csd_maj_dec" -gt 4095 ] || \
       [ "$_csd_min_dec" -lt 0 ] || [ "$_csd_min_dec" -gt 1048575 ]; then
        return 1
    fi

    # 5. Convert to lowercase hex matching GNU stat -c "%t" and "%T"
    _csd_out_maj_hex=$(printf "%x" "$_csd_maj_dec")
    _csd_out_min_hex=$(printf "%x" "$_csd_min_dec")
    _csd_out_majmin_hex="$_csd_out_maj_hex:$_csd_out_min_hex"

    # 6. Calculate 64-bit Linux makedev matching GNU stat -c "%r"
    # Linux glibc sysmacros.h:
    # (((maj & 0xfffff000ULL) << 32) | ((maj & 0xfffULL) << 8) | ((min & 0xffffff00ULL) << 12) | (min & 0xffULL))
    _csd_out_rdev=$(( ((_csd_maj_dec & 0xfffff000) << 32) | ((_csd_maj_dec & 0xfff) << 8) | ((_csd_min_dec & 0xffffff00) << 12) | (_csd_min_dec & 0xff) ))

    return 0
}
```

### 2.2 `check_open_descriptors()` Devnode & FD Inspection Excerpts
From [`scripts/stage-b-test.sh`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh):

**Devnode Status Inspection (Lines 292–375):**
```bash
    # Devnode status inspection:
    # Must distinguish:
    # 1. Expected character device with recorded identity
    # 2. Different character device
    # 3. Regular file
    # 4. Directory
    # 5. Valid symlink to expected character device
    # 6. Symlink to different device
    # 7. Dangling symlink
    # 8. Missing device node
    if [ -L "$target_node" ]; then
        if [ ! -e "$target_node" ]; then
            printf "[FAIL] Target %s is a dangling symlink (points to: %s)!\n" \
                "$target_node" "$(readlink "$target_node" 2>/dev/null || echo "unknown")"
            printf "%s\n" "[SAFETY STOP] Dangling symlink replacing devnode path. Refusing module unload."
            return 2
        fi

        # Symlink target exists; verify resolved target is character device matching recorded identity
        local sym_target_type=""
        sym_target_type=$(stat -L -c "%F" "$target_node" 2>/dev/null || true)
        if [ "$sym_target_type" != "character special file" ]; then
            printf "[FAIL] Target %s is a symlink to non-character-device '%s' (type: %s)!\n" \
                "$target_node" "$(readlink "$target_node" 2>/dev/null || echo "unknown")" "$sym_target_type"
            printf "%s\n" "[SAFETY STOP] Unexpected file type replacing devnode path. Refusing module unload."
            return 2
        fi

        local sym_rdev="" sym_maj_hex="" sym_min_hex=""
        sym_rdev=$(stat -L -c "%r" "$target_node" 2>/dev/null || true)
        sym_maj_hex=$(stat -L -c "%t" "$target_node" 2>/dev/null || true)
        sym_min_hex=$(stat -L -c "%T" "$target_node" 2>/dev/null || true)

        if [ "$sym_rdev" != "$target_rdev" ] || \
           [ "$sym_maj_hex" != "$target_major_hex" ] || \
           [ "$sym_min_hex" != "$target_minor_hex" ]; then
            printf "[FAIL] Target %s symlink points to a different character device (rdev=%s, hex=%s:%s; expected rdev=%s, hex=%s:%s)!\n" \
                "$target_node" "$sym_rdev" "$sym_maj_hex" "$sym_min_hex" \
                "$target_rdev" "$target_major_hex" "$target_minor_hex"
            printf "%s\n" "[SAFETY STOP] Devnode replaced with a symlink to an unexpected device. Refusing module unload."
            return 2
        fi
        printf "[INFO] Target %s is a verified symlink to expected character device (rdev=%s).\n" \
            "$target_node" "$sym_rdev"

    elif [ -e "$target_node" ]; then
        # Node exists and is not a symlink
        if [ ! -c "$target_node" ]; then
            local file_type=""
            file_type=$(stat -c "%F" "$target_node" 2>/dev/null || echo "unknown")
            printf "[FAIL] Target %s exists but is NOT a character device special file (type: %s)!\n" \
                "$target_node" "$file_type"
            printf "%s\n" "[SAFETY STOP] Unexpected file replacing devnode path. Refusing module unload."
            return 2
        fi

        local curr_stat_rdev="" curr_stat_maj_hex="" curr_stat_min_hex=""
        curr_stat_rdev=$(stat -c "%r" "$target_node" 2>/dev/null || true)
        curr_stat_maj_hex=$(stat -c "%t" "$target_node" 2>/dev/null || true)
        curr_stat_min_hex=$(stat -c "%T" "$target_node" 2>/dev/null || true)

        # Confirm internal consistency of current devnode
        local curr_maj_dec=$((16#$curr_stat_maj_hex))
        local curr_min_dec=$((16#$curr_stat_min_hex))
        local curr_calc_rdev=$(( ((curr_maj_dec & 0xfffff000) << 32) | ((curr_maj_dec & 0xfff) << 8) | ((curr_min_dec & 0xffffff00) << 12) | (curr_min_dec & 0xff) ))
        if [ "$curr_stat_rdev" -ne "$curr_calc_rdev" ]; then
            printf "%s\n" "[FAIL] Current devnode stat is internally inconsistent ($curr_stat_rdev vs $curr_calc_rdev)!"
            return 2
        fi

        # Compare current devnode against recorded reference identity
        if [ "$curr_stat_rdev" != "$target_rdev" ] || \
           [ "$curr_stat_maj_hex" != "$target_major_hex" ] || \
           [ "$curr_stat_min_hex" != "$target_minor_hex" ]; then
            printf "[FAIL] Devnode identity mismatch: current (rdev=%s, hex=%s:%s) != recorded (rdev=%s, hex=%s:%s)!\n" \
                "$curr_stat_rdev" "$curr_stat_maj_hex" "$curr_stat_min_hex" \
                "$target_rdev" "$target_major_hex" "$target_minor_hex"
            printf "%s\n" "[SAFETY STOP] Devnode was replaced with an unexpected device. Refusing module unload."
            return 2
        fi
    else
        # Node does not exist ([ ! -e "$target_node" ] && [ ! -L "$target_node" ])
        printf "[INFO] Devnode %s is absent; proceeding with audit using recorded reference identity (rdev=%s, hex=%s:%s).\n" \
            "$target_node" "$target_rdev" "$target_major_hex" "$target_minor_hex"
    fi
```

**FD Inspection Loop (Lines 555–605):**
```bash
            fds_examined=$((fds_examined + 1))
            local matched=0

            # 1. Device identity check via stat -L (follows magic symlink to underlying inode)
            local cand_stat=""
            cand_stat=$(stat -L -c "%F|%r|%t:%T" "$fd_link" 2>/dev/null || true)

            if [ -n "$cand_stat" ]; then
                local cand_type=""
                local cand_rdev=""
                local cand_majmin_hex=""
                cand_type=$(echo "$cand_stat" | cut -d'|' -f1)
                cand_rdev=$(echo "$cand_stat" | cut -d'|' -f2)
                cand_majmin_hex=$(echo "$cand_stat" | cut -d'|' -f3)

                if [ "$cand_type" = "character special file" ]; then
                    if [ -n "$target_rdev" ] && [ "$cand_rdev" = "$target_rdev" ]; then
                        matched=1
                    elif [ -n "$target_major_hex" ] && [ "$cand_majmin_hex" = "$target_major_hex:$target_minor_hex" ]; then
                        matched=1
                    fi
                fi
                # If stat -L succeeds and establishes cand_type != "character special file"
                # or rdev != target_rdev, this descriptor is unrelated, subject to pathname check.
            else
                # stat -L failed on candidate entry!
                # Handle benign races: process exited or descriptor closed during scan
                if [ ! -d "$proc_dir" ]; then
                    # Process exited
                    break
                elif [ ! -e "$fd_link" ] && [ ! -L "$fd_link" ]; then
                    # Descriptor closed by active process
                    continue
                else
                    # Process is still alive and FD entry still exists, but stat -L failed!
                    # Inconclusive identity inspection: increment proc_read_errors
                    proc_read_errors=$((proc_read_errors + 1))
                    proc_error_details="${proc_error_details} PID $pid fd $fd_num stat -L failed on active FD;"
                fi
            fi

            # 2. Canonical target pathname check via readlink (supplementary path match only)
            local cand_target=""
            cand_target=$(readlink "$fd_link" 2>/dev/null || true)

            if [ -n "$cand_target" ]; then
                if [ "$cand_target" = "$target_node" ] || [[ "$cand_target" == "$target_node "* ]] || [[ "$cand_target" == *"/dev/fte3600"* ]]; then
                    matched=1
                fi
            fi

            if [ "$matched" -eq 1 ]; then
                proc_matched_pids="$proc_matched_pids $pid"
                open_details_log="${open_details_log} [/proc match: PID $pid fd $fd_num]"
            fi
```

---

## 3. Kernel Module Reference Protection vs Userspace Auditing

Userspace `/proc` scanning cannot provide absolute concurrency guarantees by itself because processes can spawn or open files between enumeration steps.

The safety architecture therefore treats userspace `/proc` scanning as a **pre-flight diagnostic verification** and relies on the kernel VFS and module subsystem for **atomic synchronization**:

1. **`fte3600_fops.owner = THIS_MODULE` ([L545](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L545)) & `data->cdev.owner = THIS_MODULE` ([L623](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L623)):**
   Every time an open syscall resolves to `/dev/fte3600`, the kernel VFS `chrdev_open()` executes `try_module_get(data->cdev.owner)`, atomically bumping `module_refcount(mod)`.
2. **Exclusive Single-Client Lease ([L377](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L377)):**
   `atomic_cmpxchg(&data->is_open, 0, 1)` ensures that even if multiple processes race to open `/dev/fte3600`, exactly one client is granted access; all others immediately fail with `-EBUSY`.
3. **Atomic `delete_module()` Syscall Check:**
   When `rmmod fte3600` is executed, the kernel `delete_module()` implementation in `kernel/module/main.c` executes:
   ```c
   if (module_refcount(mod) != 0)
       return -EBUSY;
   ```
   If any file descriptor remains open anywhere on the system, the kernel immediately rejects module unloading with `-EBUSY`.
4. **Prohibition of Forced Unload:**
   `scripts/stage-b-test.sh` never invokes `rmmod -f`. If normal `rmmod` fails, the module remains safely loaded in memory, preventing kernel panics or use-after-free bugs.

---

## 4. Complete Offline Synthetic Test Results (18 Scenarios)

The offline synthetic test harness ([`scratch/test_synthetic_suite.py`](file:///home/titan/.gemini/antigravity/brain/b45cb0fc-a222-4605-b5ac-d0240eac7c36/scratch/test_synthetic_suite.py)) was executed without accessing hardware or loading modules:

```text
===========================================================================
Stage 2E.7.7 Offline Synthetic Test Suite
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
===========================================================================
Summary: 18 passed, 0 failed out of 18 tests.
===========================================================================
```

---

## 5. Static Source & System Invariant Checks

1. **Bash Syntax Validation:**
   - `bash -n scripts/stage-b-test.sh` $\rightarrow$ Exit code 0 (PASS).
   - `bash -n scripts/run-stage-b-in-terminal.sh` $\rightarrow$ Exit code 0 (PASS).
2. **Git Diff Check:**
   - `git diff kernel/` is completely empty. The kernel module source was untouched.
3. **Read-Only System State:**
   - `lsmod | grep -E "fte3600|spidev"` $\rightarrow$ Clean (no modules loaded).
   - `/dev/fte3600` $\rightarrow$ Absent.
   - `spi-FTE3600:00` $\rightarrow$ Unbound.
   - Legacy safety gate (`bin/fw9369-hw-tool`) $\rightarrow$ Sealed (exits with code 1).

---

## 6. Safety Notice & Final Boundary

- **NO CLAIM** is made that the fingerprint sensor is operational or communicating over SPI.
- **NO CLAIM** is made that the hardware reset waveform has been physically measured or verified on the board.
- **Stage B remains ON HOLD pending independent final review.**
- Stage C is strictly prohibited.
