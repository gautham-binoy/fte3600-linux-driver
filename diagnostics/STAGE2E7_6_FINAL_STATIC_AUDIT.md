# Stage 2E.7.6 — Final Device Identity and Descriptor Audit Review

**Document ID:** `STAGE2E7_6_FINAL_STATIC_AUDIT`  
**Date:** 2026-11-03  
**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Device:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor on Intel LPSS SPI2 (`spi-FTE3600:00`, bus 1, CS0)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (x86_64)  
**Task Type:** Static Code Inspection, Correctness Hardening, and Offline Synthetic Test Suite.  
**Execution Boundary:** **STRICTLY STATIC AND READ-ONLY.** Zero hardware operations performed; no kernel modules inserted; no SPI transactions executed; no GPIO toggled; legacy safety gate remains sealed.

---

## 1. Executive Summary & Exact Changes Made

In Stage 2E.7.6, the Stage B execution script (`scripts/stage-b-test.sh`) was reviewed and hardened against device identity spoofing, race conditions during `/proc` descriptor auditing, and teardown failure propagation.

### Summary of Exact Changes:
1. **Removed Stat-Only Identity Fallback in `record_devnode_identity()`:**
   - Previous revisions permitted falling back to recording `DEV_IDENTITY_RECORDED=1` using `stat` attributes alone if sysfs was unavailable.
   - This fallback has been completely removed.
   - The script now requires both primary (`/sys/class/fte3600/fte3600/dev`) or secondary (`/sys/bus/spi/devices/spi-FTE3600:00/fte3600/dev`) sysfs device attributes to exist, be readable, match numeric range constraints ($0 \le \text{major} \le 4095, 0 \le \text{minor} \le 1048575$), and cross-check exactly against `stat -c "%r"` and `stat -c "%t:%T"`.
   - If either attribute is missing, unreadable, malformed, or inconsistent, the function immediately fails closed (`DEV_IDENTITY_RECORDED=0; return 1;`), terminating Stage B before any hardware operation can take place.

2. **Immutable Reference Identity & Devnode Replacement Detection in `check_open_descriptors()`:**
   - The probe-time recorded identity (`RECORDED_DEV_RDEV`, `RECORDED_DEV_MAJOR_HEX`, `RECORDED_DEV_MINOR_HEX`, `RECORDED_DEV_MAJ_MIN_DEC`) is enforced as the immutable reference identity.
   - `check_open_descriptors()` strictly asserts `DEV_IDENTITY_RECORDED == 1` before auditing.
   - If `/dev/fte3600` exists on disk during teardown, it verifies `test -c`, confirms internal `stat` consistency, and checks `curr_stat_rdev == RECORDED_DEV_RDEV`. If an unexpected device or file has replaced `/dev/fte3600`, it logs a safety stop and returns code 2 (inconclusive / refusal to unload).
   - If `/dev/fte3600` has disappeared, it relies on the recorded reference identity and any available sysfs attributes to identify candidate descriptors in `/proc`.

3. **Robust Direct `/proc` Enumeration with Errexit Isolation:**
   - Unmasked real return codes for `lsof`, `fuser`, `ls /proc`, and `ls /proc/<pid>/fd`.
   - All diagnostic tools and directory listings run under temporary `set +e` with status capture and safe `shopt -o -q errexit` restoration.
   - When invoked from `cleanup_handler` (which operates under `set +e`), errexit is never re-enabled, guaranteeing that reporting or listing errors cannot abort host teardown midway.
   - Enumeration explicitly distinguishes between:
     - Empty FD directory on living process (`rc=0`, zero stdout) $\rightarrow$ process examined, no open fds.
     - Populated FD directory (`rc=0`, non-empty stdout) $\rightarrow$ inspect each fd link.
     - Process exited during enumeration (`rc!=0` or missing directory) $\rightarrow$ cleanly ignored.
     - Active process with unreadable descriptor directory (`rc!=0`, `[ -d "$proc_dir" ]` true) $\rightarrow$ counted as `proc_read_errors`, halting module unload with status 2.
     - Active process with unreadable fd link $\rightarrow$ counted as `proc_read_errors`, halting module unload with status 2.

4. **Absolute Prohibition on Process Termination:**
   - No `kill`, `pkill`, or `kill -9` commands exist anywhere in the cleanup pipeline.
   - Active holders of `/dev/fte3600` are diagnosed and reported with command-line information, and module unload is refused to protect system stability.

5. **Locking of Primary RESET Failure Exit Code:**
   - A non-zero return code from `"$TOOL_BIN" reset` is permanently locked into `TEST_EXIT_STATUS`.
   - Subsequent Snapshot 2 or dmesg diagnostics cannot alter or mask `RESET_RC`.

---

## 2. Complete Source: `record_devnode_identity()`

From [`scripts/stage-b-test.sh`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh) lines 132–230:

```bash
# ------------------------------------------------------------------------------
# Character Device Node Identity Verification & Recording
# ------------------------------------------------------------------------------
# Validates /dev/fte3600 character device identity against sysfs.
# Sysfs paths verified from kernel/fte3600.c:
#   Primary:   /sys/class/fte3600/fte3600/dev (via fte3600_class + dev_set_name)
#   Secondary: /sys/bus/spi/devices/spi-FTE3600:00/fte3600/dev (via data->dev.parent)
# Requirements:
#   1. Verify /dev/fte3600 is a character device (test -c).
#   2. Collect and validate stat identity (%r, %t, %T).
#   3. Read expected sysfs major:minor attribute.
#   4. Validate sysfs format and numeric ranges via convert_sysfs_dev_to_identity.
#   5. Cross-check stat and sysfs identities.
#   6. Set DEV_IDENTITY_RECORDED=1 ONLY after ALL checks succeed.
#   7. If neither sysfs location is readable, fail closed.
#   8. If either identity is malformed or inconsistent, fail closed.
# Sets globals:
#   RECORDED_DEV_RDEV
#   RECORDED_DEV_MAJOR_HEX
#   RECORDED_DEV_MINOR_HEX
#   RECORDED_DEV_MAJ_MIN_DEC
#   DEV_IDENTITY_RECORDED=1
# Returns 0 on success, 1 on failure.
# ------------------------------------------------------------------------------
record_devnode_identity() {
    local target_node="/dev/fte3600"
    local sysfs_dev_path="/sys/class/fte3600/fte3600/dev"
    if [ ! -r "$sysfs_dev_path" ] && [ -r "/sys/bus/spi/devices/spi-FTE3600:00/fte3600/dev" ]; then
        sysfs_dev_path="/sys/bus/spi/devices/spi-FTE3600:00/fte3600/dev"
    fi

    # 1. Validate character device using test -c
    if [ ! -c "$target_node" ]; then
        printf "%s\n" "[FAIL] Character device node $target_node was not created or is not a character special file."
        DEV_IDENTITY_RECORDED=0
        return 1
    fi

    printf "%s\n" "[PASS] (Character device node present)"
    ls -l "$target_node"

    # 2. Collect and validate stat attributes
    local stat_type=""
    local stat_rdev=""
    local stat_maj_hex=""
    local stat_min_hex=""
    stat_type=$(stat -c "%F" "$target_node" 2>/dev/null || true)
    stat_rdev=$(stat -c "%r" "$target_node" 2>/dev/null || true)
    stat_maj_hex=$(stat -c "%t" "$target_node" 2>/dev/null || true)
    stat_min_hex=$(stat -c "%T" "$target_node" 2>/dev/null || true)

    local stat_valid=0
    if [ "$stat_type" = "character special file" ] && \
       [[ "$stat_rdev" =~ ^[0-9]+$ ]] && [ "$stat_rdev" -gt 0 ] && \
       [[ "$stat_maj_hex" =~ ^[0-9a-fA-F]+$ ]] && \
       [[ "$stat_min_hex" =~ ^[0-9a-fA-F]+$ ]]; then
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

    if [ "$stat_valid" -ne 1 ]; then
        printf "%s\n" "[FAIL] Character device stat validation failed."
        DEV_IDENTITY_RECORDED=0
        return 1
    fi

    # 3. Read and validate expected sysfs device attribute (fail-closed: MUST be readable)
    if [ ! -r "$sysfs_dev_path" ]; then
        printf "%s\n" "[FAIL] Expected sysfs device attribute is unreadable (checked primary and secondary paths)."
        printf "%s\n" "[SAFETY STOP] Character device identity cannot be verified against sysfs. Refusing to proceed."
        DEV_IDENTITY_RECORDED=0
        return 1
    fi

    local sysfs_dev_str=""
    local sysfs_valid=0
    local sysfs_rdev=""
    local sysfs_maj_hex=""
    local sysfs_min_hex=""
    local sysfs_majmin_hex=""
    sysfs_dev_str=$(cat "$sysfs_dev_path" 2>/dev/null || true)
    if convert_sysfs_dev_to_identity "$sysfs_dev_str" sysfs_rdev sysfs_maj_hex sysfs_min_hex sysfs_majmin_hex; then
        sysfs_valid=1
    fi

    if [ "$sysfs_valid" -ne 1 ]; then
        printf "[FAIL] Expected sysfs device attribute is malformed or out of range: '%s'\n" "$sysfs_dev_str"
        printf "%s\n" "[SAFETY STOP] Malformed sysfs identity. Refusing to proceed."
        DEV_IDENTITY_RECORDED=0
        return 1
    fi

    # 4. Cross-check stat and sysfs identities
    if [ "$stat_rdev" = "$sysfs_rdev" ] && [ "$stat_maj_hex:$stat_min_hex" = "$sysfs_majmin_hex" ]; then
        RECORDED_DEV_RDEV="$stat_rdev"
        RECORDED_DEV_MAJOR_HEX="$stat_maj_hex"
        RECORDED_DEV_MINOR_HEX="$stat_min_hex"
        RECORDED_DEV_MAJ_MIN_DEC="$sysfs_dev_str"
        DEV_IDENTITY_RECORDED=1
    else
        printf "%s\n" "[FAIL] Device identity mismatch between stat ($stat_rdev, $stat_maj_hex:$stat_min_hex) and sysfs ($sysfs_rdev, $sysfs_majmin_hex)!"
        DEV_IDENTITY_RECORDED=0
        return 1
    fi

    printf "[INFO] Character device identity validated & recorded: rdev=%s (hex: %s:%s, sysfs: %s)\n" \
        "$RECORDED_DEV_RDEV" "$RECORDED_DEV_MAJOR_HEX" "$RECORDED_DEV_MINOR_HEX" "$RECORDED_DEV_MAJ_MIN_DEC"
    return 0
}
```

---

## 3. Complete Source: `check_open_descriptors()`

From [`scripts/stage-b-test.sh`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh) lines 250–585:

```bash
# ------------------------------------------------------------------------------
# Robust Fail-Closed Open Descriptor Audit Helper
# ------------------------------------------------------------------------------
# Invariants:
# 1. lsof and fuser exit code 1 is NEVER assumed to prove that zero descriptors are open.
# 2. Real return codes from lsof and fuser are captured directly without masking.
# 3. lsof and fuser are treated as supplementary diagnostic discovery tools only.
# 4. Direct /proc inspection is ALWAYS executed as an independent primary verification.
# 5. /proc directory enumeration distinguishes empty, populated, unreadable, and exited.
# 6. /proc candidate matching uses device identity (character device st_rdev and major/minor)
#    in addition to pathname matching, accounting for symlinks, alternate paths, and unlinked nodes.
# 7. Inaccessible or unreadable /proc entries on active processes fail closed.
# 8. Automatic process termination is strictly barred. Open descriptors are reported and halt unload.
# 9. Returns: 0 = confirmed zero open descriptors; 1 = open descriptors detected; 2 = inconclusive / error.
# ------------------------------------------------------------------------------
check_open_descriptors() {
    local target_node="/dev/fte3600"
    local target_rdev=""
    local target_major_hex=""
    local target_minor_hex=""
    local target_majmin_dec=""

    printf "Performing fail-closed descriptor inspection on %s ... \n" "$target_node"

    # Step A: Validate reference character device identity against recorded probe identity
    if [ "$DEV_IDENTITY_RECORDED" -ne 1 ]; then
        printf "%s\n" "[FAIL] Device identity was never recorded during driver probe."
        printf "%s\n" "[SAFETY STOP] Cannot establish reference device identity to audit open descriptors. Refusing module unload."
        return 2
    fi

    # Reference identity established from recorded probe verification
    target_rdev="$RECORDED_DEV_RDEV"
    target_major_hex="$RECORDED_DEV_MAJOR_HEX"
    target_minor_hex="$RECORDED_DEV_MINOR_HEX"
    target_majmin_dec="$RECORDED_DEV_MAJ_MIN_DEC"

    # If the devnode currently exists, verify its identity has not been replaced or altered
    if [ -e "$target_node" ]; then
        if [ ! -c "$target_node" ]; then
            printf "%s\n" "[FAIL] Target $target_node exists but is NOT a character device special file!"
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
    fi

    # Where sysfs is available, cross-check it against the recorded reference identity
    local sysfs_dev_path="/sys/class/fte3600/fte3600/dev"
    if [ ! -r "$sysfs_dev_path" ] && [ -r "/sys/bus/spi/devices/spi-FTE3600:00/fte3600/dev" ]; then
        sysfs_dev_path="/sys/bus/spi/devices/spi-FTE3600:00/fte3600/dev"
    fi
    if [ -r "$sysfs_dev_path" ]; then
        local sysfs_dev_str=""
        local s_rdev="" s_maj_hex="" s_min_hex="" s_majmin=""
        sysfs_dev_str=$(cat "$sysfs_dev_path" 2>/dev/null || true)
        if convert_sysfs_dev_to_identity "$sysfs_dev_str" s_rdev s_maj_hex s_min_hex s_majmin; then
            if [ "$s_rdev" != "$target_rdev" ] || [ "$s_maj_hex:$s_min_hex" != "$target_major_hex:$target_minor_hex" ]; then
                printf "[FAIL] Sysfs identity mismatch: sysfs (rdev=%s, hex=%s) != recorded (rdev=%s, hex=%s:%s)!\n" \
                    "$s_rdev" "$s_majmin" "$target_rdev" "$target_major_hex" "$target_minor_hex"
                printf "%s\n" "[SAFETY STOP] Sysfs identity does not match recorded identity. Refusing module unload."
                return 2
            fi
        else
            printf "%s\n" "[FAIL] Sysfs device attribute is malformed ($sysfs_dev_str)!"
            return 2
        fi
    fi

    # Create temporary scratch directory for diagnostic logs
    local tmp_dir=""
    if ! tmp_dir=$(mktemp -d 2>/dev/null) || [ -z "$tmp_dir" ] || [ ! -d "$tmp_dir" ]; then
        printf "%s\n" "[FAIL] Failed to create temporary audit directory."
        printf "%s\n" "[SAFETY STOP] Cannot execute fail-closed descriptor inspection. Refusing module unload."
        return 2
    fi

    local open_pids_detected=""
    local open_details_log=""

    # Step B: Supplementary Diagnostic Tool 1 — lsof (Unmasked real return code)
    if command -v lsof >/dev/null 2>&1; then
        local lsof_err_file="$tmp_dir/lsof.err"
        local lsof_out_file="$tmp_dir/lsof.out"
        local was_errexit_lsof=0
        if shopt -o -q errexit; then was_errexit_lsof=1; fi
        set +e
        lsof -t "$target_node" >"$lsof_out_file" 2>"$lsof_err_file"
        local lsof_rc=$?
        if [ "$was_errexit_lsof" -eq 1 ]; then set -e; fi

        local lsof_out=""
        local lsof_err=""
        lsof_out=$(cat "$lsof_out_file" 2>/dev/null || true)
        lsof_err=$(cat "$lsof_err_file" 2>/dev/null || true)

        if [ "$lsof_rc" -eq 0 ] && [ -n "$lsof_out" ]; then
            open_pids_detected="$open_pids_detected $lsof_out"
            open_details_log="${open_details_log} [lsof match: PIDs $lsof_out]"
        elif [ "$lsof_rc" -eq 1 ] && [ -z "$lsof_out" ] && [ -z "$lsof_err" ]; then
            printf "  [DIAG] lsof: reported no match (rc=1, zero output).\n"
        else
            printf "  [DIAG] lsof: inconclusive / non-zero return code %d (stderr: %s).\n" "$lsof_rc" "$lsof_err"
        fi
    else
        printf "  [DIAG] lsof: utility not installed; relying on independent /proc audit.\n"
    fi

    # Step C: Supplementary Diagnostic Tool 2 — fuser (Unmasked real return code)
    if command -v fuser >/dev/null 2>&1; then
        local fuser_err_file="$tmp_dir/fuser.err"
        local fuser_out_file="$tmp_dir/fuser.out"
        local was_errexit_fuser=0
        if shopt -o -q errexit; then was_errexit_fuser=1; fi
        set +e
        fuser "$target_node" >"$fuser_out_file" 2>"$fuser_err_file"
        local fuser_rc=$?
        if [ "$was_errexit_fuser" -eq 1 ]; then set -e; fi

        local fuser_out=""
        local fuser_err=""
        fuser_out=$(cat "$fuser_out_file" 2>/dev/null || true)
        fuser_err=$(cat "$fuser_err_file" 2>/dev/null || true)

        if [ "$fuser_rc" -eq 0 ] && [ -n "$fuser_out" ]; then
            open_pids_detected="$open_pids_detected $fuser_out"
            open_details_log="${open_details_log} [fuser match: PIDs $fuser_out]"
        elif [ "$fuser_rc" -eq 1 ] && [ -z "$fuser_out" ] && [ -z "$fuser_err" ]; then
            printf "  [DIAG] fuser: reported no match (rc=1, zero output).\n"
        else
            printf "  [DIAG] fuser: inconclusive / non-zero return code %d (stderr: %s).\n" "$fuser_rc" "$fuser_err"
        fi
    else
        printf "  [DIAG] fuser: utility not installed; relying on independent /proc audit.\n"
    fi

    # Step D: Primary Independent /proc Descriptor Audit (Always Executed)
    # This inspects all active processes, matching device identity (st_rdev) and canonical path.
    local procs_examined=0
    local fds_examined=0
    local proc_read_errors=0
    local proc_error_details=""
    local proc_matched_pids=""

    # 1. Enumerate active processes in /proc
    local proc_ls_err_file="$tmp_dir/proc_ls.err"
    local proc_ls_out_file="$tmp_dir/proc_ls.out"
    local was_errexit_proc=0
    if shopt -o -q errexit; then was_errexit_proc=1; fi
    set +e
    ls -1d /proc/[0-9]* >"$proc_ls_out_file" 2>"$proc_ls_err_file"
    local proc_ls_rc=$?
    if [ "$was_errexit_proc" -eq 1 ]; then set -e; fi

    local proc_dir_list=""
    local proc_ls_err=""
    proc_dir_list=$(cat "$proc_ls_out_file" 2>/dev/null || true)
    proc_ls_err=$(cat "$proc_ls_err_file" 2>/dev/null || true)

    if [ "$proc_ls_rc" -ne 0 ] || [ -z "$proc_dir_list" ]; then
        printf "%s\n" "[FAIL] Failed to enumerate /proc directory: rc=$proc_ls_rc, err=$proc_ls_err"
        printf "%s\n" "[SAFETY STOP] Cannot enumerate active processes. Refusing module unload."
        rm -rf "$tmp_dir"
        return 2
    fi

    # 2. Iterate through each process directory
    local fd_ls_err_file="$tmp_dir/fd_ls.err"
    local fd_ls_out_file="$tmp_dir/fd_ls.out"
    for proc_dir in $proc_dir_list; do
        if [ ! -d "$proc_dir" ]; then
            # Process exited between directory listing and inspection
            continue
        fi

        local pid
        pid=$(basename "$proc_dir")
        local fd_dir="$proc_dir/fd"

        # Explicit enumeration of /proc/$pid/fd capturing return code and stderr directly
        local was_errexit_fd=0
        if shopt -o -q errexit; then was_errexit_fd=1; fi
        set +e
        ls -1A "$fd_dir" >"$fd_ls_out_file" 2>"$fd_ls_err_file"
        local fd_ls_rc=$?
        if [ "$was_errexit_fd" -eq 1 ]; then set -e; fi

        local fd_entries=""
        local fd_ls_err=""
        fd_entries=$(cat "$fd_ls_out_file" 2>/dev/null || true)
        fd_ls_err=$(cat "$fd_ls_err_file" 2>/dev/null || true)

        # Distinction: Error vs Empty vs Exited
        if [ "$fd_ls_rc" -ne 0 ]; then
            if [ ! -d "$proc_dir" ]; then
                # Process exited during enumeration; kernel released descriptors cleanly
                continue
            else
                # Process is STILL ALIVE, but its fd directory could not be enumerated!
                proc_read_errors=$((proc_read_errors + 1))
                proc_error_details="${proc_error_details} PID $pid /fd enumeration failed (rc=$fd_ls_rc: $fd_ls_err);"
                continue
            fi
        fi

        # Empty FD directory on active process (e.g. kernel thread or closed descriptors)
        if [ -z "$fd_entries" ]; then
            procs_examined=$((procs_examined + 1))
            continue
        fi

        # Populated FD directory on active process
        procs_examined=$((procs_examined + 1))

        for fd_num in $fd_entries; do
            local fd_link="$fd_dir/$fd_num"

            # Check if descriptor link still exists
            if [ ! -e "$fd_link" ] && [ ! -L "$fd_link" ]; then
                if [ ! -d "$proc_dir" ]; then
                    # Process exited
                    break
                fi
                # Descriptor was closed by active process during scan
                continue
            fi

            fds_examined=$((fds_examined + 1))
            local matched=0

            # 1. Device identity check via stat -L (follows symlink to underlying inode)
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
            fi

            # 2. Canonical target pathname check via readlink
            local cand_target=""
            cand_target=$(readlink "$fd_link" 2>/dev/null || true)

            if [ -n "$cand_target" ]; then
                if [ "$cand_target" = "$target_node" ] || [[ "$cand_target" == "$target_node "* ]] || [[ "$cand_target" == *"/dev/fte3600"* ]]; then
                    matched=1
                fi
            fi

            # 3. Detection of unreadable candidate entry on active process
            if [ -z "$cand_stat" ] && [ -z "$cand_target" ]; then
                # Check if process or fd closed during read
                if [ -d "$proc_dir" ] && ([ -e "$fd_link" ] || [ -L "$fd_link" ]); then
                    proc_read_errors=$((proc_read_errors + 1))
                    proc_error_details="${proc_error_details} PID $pid fd $fd_num stat/readlink failed;"
                fi
            fi

            if [ "$matched" -eq 1 ]; then
                proc_matched_pids="$proc_matched_pids $pid"
                open_details_log="${open_details_log} [/proc match: PID $pid fd $fd_num]"
            fi
        done
    done

    # Clean up temporary scratch directory
    rm -rf "$tmp_dir"

    # Merge matched PIDs
    open_pids_detected="$open_pids_detected $proc_matched_pids"
    # Deduplicate open PIDs list
    local unique_pids=""
    for p in $open_pids_detected; do
        if [[ ! " $unique_pids " =~ [[:space:]]$p[[:space:]] ]]; then
            unique_pids="$unique_pids $p"
        fi
    done
    unique_pids=$(echo "$unique_pids" | xargs || true)

    # Step E: Handle open descriptors (strictly fail-closed, NO process termination)
    if [ -n "$unique_pids" ]; then
        printf "%s\n" "[WARN] Open descriptor(s) detected holding character device!"
        printf "  Details: %s\n" "$open_details_log"
        for pid in $unique_pids; do
            if [ -d "/proc/$pid" ]; then
                local cmdline
                cmdline=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null || true)
                printf "  -> ACTIVE HOLDER: PID %s holding node: cmdline='%s'\n" "$pid" "$cmdline"
            else
                printf "  -> ACTIVE HOLDER: PID %s holding node (process info unreadable)\n" "$pid"
            fi
        done
        printf "%s\n" "[SAFETY STOP] Automatic process termination is strictly disabled."
        printf "%s\n" "[SAFETY STOP] Refusing module unload while descriptors remain open."
        return 1
    fi

    # Step F: Validate completeness of /proc scan
    if [ "$proc_read_errors" -ne 0 ]; then
        printf "%s\n" "[FAIL] Incomplete /proc descriptor scan: $proc_read_errors unreadable entries on active processes."
        printf "  Error details: %s\n" "$proc_error_details"
        printf "%s\n" "[SAFETY STOP] Cannot definitively substantiate that zero descriptors are open. Refusing module unload."
        return 2
    fi

    if [ "$procs_examined" -eq 0 ]; then
        printf "%s\n" "[FAIL] Incomplete /proc descriptor scan: zero processes examined."
        printf "%s\n" "[SAFETY STOP] /proc scan was empty or unreadable. Refusing module unload."
        return 2
    fi

    printf "[PASS] Zero open descriptors confirmed across %d active processes (%d fds inspected).\n" \
        "$procs_examined" "$fds_examined"
    return 0
}
```

---

## 4. Kernel Module Reference Protection Analysis

The safety architecture relies on hardware-level and kernel-level invariants rather than userspace alone. The kernel VFS and module subsystem enforce strict reference protection against premature unloading:

### 4.1 Module Ownership Invariants in `kernel/fte3600.c`
1. **File Operations Owner (`.owner = THIS_MODULE`):**
   In [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L544-L546):
   ```c
   static const struct file_operations fte3600_fops = {
       .owner          = THIS_MODULE,
       .open           = fte3600_cdev_open,
       .release        = fte3600_cdev_release,
       ...
   };
   ```
2. **CDEV Struct Owner (`data->cdev.owner = THIS_MODULE`):**
   In [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L622-L624):
   ```c
   cdev_init(&data->cdev, &fte3600_fops);
   data->cdev.owner = THIS_MODULE;
   ret = cdev_device_add(&data->cdev, &data->dev);
   ```

### 4.2 Kernel Open/Release Mechanism
- When any userspace process calls `open("/dev/fte3600", ...)`, the Linux VFS character device layer invokes `chrdev_open()` (in `fs/char_dev.c`).
- `chrdev_open()` executes `try_module_get(p->owner)`. If successful, the module refcount (`module_refcount(mod)`) is atomically incremented by 1.
- `filp->f_op->open()` (`fte3600_cdev_open`) is then called.
  - In `fte3600_cdev_open()` ([lines 368–385](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L368-L385)), `atomic_cmpxchg(&data->is_open, 0, 1)` establishes an exclusive single-client lease. No secondary open can succeed while this lease is held.
  - `get_device(&data->dev)` increments the device kobject reference count, guaranteeing the underlying memory struct `fte3600_data` cannot be freed while the fd is held.
- When the process closes the file descriptor (or terminates), the VFS executes `filp->f_op->release()` (`fte3600_cdev_release`), which clears `data->is_open` and calls `put_device(&data->dev)`. Finally, the VFS invokes `module_put(filp->f_op->owner)`, atomically decrementing the module refcount back to 0.

### 4.3 Syscall `delete_module()` Atomic Protection
- When userspace runs `rmmod fte3600`, the kernel executes `sys_delete_module()` in `kernel/module/main.c`.
- `delete_module()` explicitly inspects:
  ```c
  if (module_refcount(mod) != 0)
      return -EBUSY;
  ```
- If even a single file descriptor remains open anywhere on the host, the kernel immediately aborts module unloading with `-EBUSY` (`Device or resource busy`).
- Because `scripts/stage-b-test.sh` strictly uses `rmmod` and NEVER uses `rmmod -f` (and forced unload is disabled in production kernels), the kernel will never unload code or unmap memory while a descriptor is active.

---

## 5. Offline Synthetic Test Results (14 Scenarios)

All 14 test cases specified in Stage 2E.7.6 Section 6 were executed offline using the synthetic test runner `scratch/test_synthetic_suite.py`. Zero hardware was touched.

| # | Test Scenario | Expected Result | Actual Result | Status |
|---|---|---|---|---|
| **1** | Valid matching stat and sysfs identities | Status 0; identity recorded (`rdev`, `maj:min`) | Recorded `1:3` (rdev 259) | **PASS** |
| **2** | Mismatched stat and sysfs identities | Status 1; logs mismatch, no record | Logs mismatch (`1:3` vs `1:5`), `DEV_IDENTITY_RECORDED=0` | **PASS** |
| **3** | Missing sysfs identity | Status 1; fails closed, no fallback | Logs sysfs unreadable, refuses to proceed | **PASS** |
| **4** | Malformed sysfs identity (`invalid_maj_min`) | Status 1; fails closed, no fallback | Logs malformed attribute, refuses to proceed | **PASS** |
| **5** | Missing character device node | Status 1; fails closed | Logs character special file missing | **PASS** |
| **6** | Unexpected replacement character device | Status 2; safety stop, unload refused | Detects identity change from recorded probe | **PASS** |
| **7** | Open descriptor detection (`/dev/null` held by mock PID) | Status 1; active holder reported, unload halted | Detected PID 1234 holding node; termination barred | **PASS** |
| **8** | Deleted device node descriptor (`/dev/fte3600 (deleted)`) | Status 1; active holder reported via readlink | Detected PID 1234 holding unlinked node | **PASS** |
| **9** | Inaccessible active descriptor directory (`chmod 000`) | Status 2; safety stop, unload refused | Logs incomplete scan (PID 9999 /fd unreadable) | **PASS** |
| **10** | Process disappearing during enumeration | Status 0; cleanly ignored | Ignored cleanly; confirmed zero open descriptors | **PASS** |
| **11** | Descriptor disappearing during inspection | Status 0; cleanly ignored | Broken link ignored; zero open descriptors confirmed | **PASS** |
| **12** | Temporary directory creation failure (`mktemp -d` fails) | Status 2; safety stop, unload refused | Logs temporary directory failure; halts unload | **PASS** |
| **13** | Normal unload refusal on inconclusive audit (status 2) | Refuses unload; `rmmod` NOT called; `CLEANUP_OVERALL_OK=0` | `rmmod` was never executed; module left safely loaded | **PASS** |
| **14** | RESET error preservation in control-flow logic | Returns primary `RESET_RC` (5) even if Snapshot 2 fails | Exit code 5 strictly preserved over secondary failures | **PASS** |

**Summary: 14 passed, 0 failed out of 14 tests.**

---

## 6. Bash Syntax Verification

Both Stage B runner scripts were validated using `bash -n`:
```bash
bash -n scripts/stage-b-test.sh
# Exit code: 0 (No syntax errors detected)

bash -n scripts/run-stage-b-in-terminal.sh
# Exit code: 0 (No syntax errors detected)
```

---

## 7. System Invariants & Hardware Isolation Confirmation

At the conclusion of this static audit and synthetic testing:
1. **Zero Hardware Operations:** No SPI transactions, no GPIO pulses, no register reads or writes, no mmio operations were executed.
2. **Legacy Safety Gate Sealed:** Direct execution of `bin/fw9369-hw-tool` confirmed that hardware access remains blocked by policy and exits with code 1.
3. **Host Cleanliness:**
   - Kernel module `fte3600` is **NOT loaded** (`lsmod` empty).
   - `/dev/fte3600` character device node is **absent**.
   - `spi-FTE3600:00` SPI device is **unbound**.
4. **Driver Code Untouched:** `git diff kernel/` is completely clean; no driver files were modified.

---

## 8. Explicit Non-Claims & Safety Notice

- **NO CLAIM** is made that the physical sensor hardware reset has been verified.
- **NO CLAIM** is made that the fingerprint sensor is operational or communicating over SPI.
- Stage B is **ON HOLD** awaiting explicit user review and authorization.
- Stage C is strictly prohibited.
