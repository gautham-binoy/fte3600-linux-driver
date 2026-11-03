#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Stage 2E.8 Stage B Controlled Hardware Reset Test Script
# Dynabook Satellite Pro C40-J (PYS46G-06J02D)
# Target Silicon: FocalTech FT9362 / FW9369
# Target SPI Device: spi-FTE3600:00 on pxa2xx-spi.2 (spi1.0)
#
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
#   - Real return code capture for supplementary diagnostic tools (no || true masking)
#   - Controlled error-state handling without shell-wide errexit pollution
#   - Rigorous /proc directory enumeration distinguishing empty, populated, unreadable, and exited
#   - Zero automatic process termination (no SIGTERM, no SIGKILL, no pkill)
#   - Absolute preservation of primary reset failure status across subsequent diagnostics
#   - OPTION 1 MANDATORY POLICY: NEVER automatically unload fte3600 kernel module
#   - Zero automatic rmmod, zero modprobe -r, zero sysfs SPI unbinds
#   - Module is intentionally left loaded and bound upon exit for manual administrative teardown
#
set -euo pipefail
export LC_ALL=C

PROJECT_DIR=$(cd "$(dirname "$0")/.." && pwd)
DIAG_DIR="$PROJECT_DIR/diagnostics"
STAGE_B_LOG="$DIAG_DIR/stage-b-results.txt"
MODULE_PATH="$PROJECT_DIR/kernel/fte3600.ko"
TOOL_BIN="$PROJECT_DIR/bin/fte3600-tool"
SAFETY_GATE_BIN="$PROJECT_DIR/bin/fw9369-hw-tool"
SPI_DEV="spi-FTE3600:00"
SPI_PATH="/sys/bus/spi/devices/$SPI_DEV"
PINCTRL_PINS="/sys/kernel/debug/pinctrl/INT34C5:00/pins"
DEBUGFS_GPIO="/sys/kernel/debug/gpio"

# State tracking flags for robust cleanup & ownership (Stage 2E.7.10/2E.7.11/2E.8)
INITIAL_FTE3600_LOADED=0
MODULE_INSERTION_ATTEMPTED=0
THIS_INVOCATION_INSERTED=0
MODULE_INSERTED=0
CLEANUP_IN_PROGRESS=0
CLEANUP_ATTEMPTED=0
CLEANUP_OVERALL_OK=1

# Conservative module insertion state machine (Stage 2E.7.11)
INSERTION_STATE_NOT_ATTEMPTED="NOT_ATTEMPTED"
INSERTION_STATE_DEFINITELY_FAILED="DEFINITELY_FAILED"
INSERTION_STATE_DEFINITELY_SUCCEEDED="DEFINITELY_SUCCEEDED"
INSERTION_STATE_UNCERTAIN="UNCERTAIN"
INSERTION_STATE="$INSERTION_STATE_NOT_ATTEMPTED"

# Deferred signal tracking during insertion critical section
DEFERRED_SIGNAL=""
DEFERRED_SIGNAL_NAME=""

# Character device identity tracking (established and validated post-probe)
RECORDED_DEV_RDEV=""
RECORDED_DEV_MAJOR_HEX=""
RECORDED_DEV_MINOR_HEX=""
RECORDED_DEV_MAJ_MIN_DEC=""
DEV_IDENTITY_RECORDED=0

mkdir -p "$DIAG_DIR"
exec > >(tee "$STAGE_B_LOG") 2>&1

printf "%s\n" "==============================================================================="
printf "%s\n" " Stage 2E.8 — Stage B Controlled Hardware Reset Test & Verification"
printf "%s\n" " Option 1 Policy: Never Automatically Unload the Fingerprint Driver"
printf "%s\n" " Host: Dynabook Satellite Pro C40-J (PYS46G-06J02D)"
printf "%s\n" " Target: spi-FTE3600:00 (FocalTech FT9362 / FW9369)"
printf " Kernel: %s\n" "$(uname -r)"
printf " Timestamp: %s\n" "$(date -Iseconds)"
printf "%s\n\n" "==============================================================================="

# ------------------------------------------------------------------------------
# Safe Decimal Device Number Validation Helper (Stage 2E.7.11)
# ------------------------------------------------------------------------------
# Validates that a raw decimal device number string is:
# 1. Non-empty and contains only ASCII digits.
# 2. Strictly <= 19 digits.
# 3. If exactly 19 digits, strictly <= 9223372036854775807 (INT64_MAX) to prevent
#    silent signed 64-bit wrap-around in Bash arithmetic ($(( ... ))).
# 4. Strictly > 0 (valid Linux device special files cannot have zero raw rdev).
# Returns 0 on valid; 1 on invalid or arithmetic overflow.
# ------------------------------------------------------------------------------
validate_decimal_rdev() {
    local val="$1"
    if [ -z "$val" ] || ! [[ "$val" =~ ^[0-9]+$ ]]; then
        return 1
    fi
    if [ ${#val} -gt 19 ]; then
        return 1
    fi
    if [ ${#val} -eq 19 ] && [ "$val" \> "9223372036854775807" ]; then
        return 1
    fi
    local stripped="${val#"${val%%[!0]*}"}"
    if [ -z "$stripped" ]; then
        return 1
    fi
    return 0
}

# ------------------------------------------------------------------------------
# Safe Hexadecimal Major/Minor Validation Helper (Stage 2E.7.11)
# ------------------------------------------------------------------------------
# Validates that a hexadecimal major/minor string is:
# 1. Non-empty and contains only hex characters.
# 2. String length <= max_len characters (default 8, at most 32 bits, safely fitting within 64-bit int).
# Returns 0 on valid; 1 on invalid or out-of-range.
# ------------------------------------------------------------------------------
validate_hex_majmin() {
    local val="$1"
    local max_len="${2:-8}"
    if [ -z "$val" ] || ! [[ "$val" =~ ^[0-9a-fA-F]+$ ]]; then
        return 1
    fi
    if [ ${#val} -gt "$max_len" ]; then
        return 1
    fi
    return 0
}

# ------------------------------------------------------------------------------
# Character Device Identity Conversion & Validation Helper
# ------------------------------------------------------------------------------
# Converts a sysfs major:minor string (e.g., "240:0") into:
#   1. Clean hex major and hex minor (e.g., "f0" and "0")
#   2. Canonical hex pair "f0:0" matching stat -c "%t:%T"
#   3. Raw 64-bit decimal rdev matching stat -c "%r"
# Returns 0 on successful validation and conversion; 1 on invalid/malformed input.
# ------------------------------------------------------------------------------
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
       validate_decimal_rdev "$stat_rdev" && \
       validate_hex_majmin "$stat_maj_hex" 8 && \
       validate_hex_majmin "$stat_min_hex" 8; then
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

# ------------------------------------------------------------------------------
# Robust Kernel Module State Query Helper (Stage 2E.7.10/2E.7.11/2E.7.12)
# ------------------------------------------------------------------------------
# Queries whether a given kernel module is currently present in the kernel.
# Invariants:
# 1. Inspects kernel state across two independent kernel interfaces:
#    - Sysfs: /sys/module/<name> (kernfs kobject hierarchy)
#    - Procfs: /proc/modules direct read (kernel module list)
#    Plus userspace parser: lsmod (utility reading and formatting /proc/modules).
#    Note: lsmod is NOT a third independent kernel-state source; it is a userspace
#    parser of /proc/modules, used here as an auxiliary formatting cross-check.
# 2. Tracks per-source states: 0 = present, 1 = absent, 2 = inspection error / inaccessible.
# 3. Absence is confirmed ONLY if all sources succeed without error and agree on absence.
# 4. A failed lsmod, unreadable /proc/modules, or inaccessible /sys/module is NEVER
#    interpreted as proof of absence (returns 2 = INCONCLUSIVE / ERROR).
# 5. Contradictory observations (e.g. one reports present, another absent) fail closed (status 2).
# Returns:
#   0 = module is confirmed LOADED
#   1 = module is confirmed ABSENT
#   2 = inspection error / INCONCLUSIVE (cannot safely verify)
# ------------------------------------------------------------------------------
query_module_state() {
    local mod_name="${1:-fte3600}"
    local sysfs_dir="/sys/module/$mod_name"
    local sysfs_state=2
    local proc_state=2
    local lsmod_state=2

    # 1. Source 1: Sysfs /sys/module
    if [ -d "/sys/module" ]; then
        if [ -d "$sysfs_dir" ]; then
            sysfs_state=0
        elif [ ! -e "$sysfs_dir" ] && [ ! -L "$sysfs_dir" ]; then
            sysfs_state=1
        else
            sysfs_state=2
        fi
    else
        sysfs_state=2
    fi

    # 2. Source 2: /proc/modules direct read
    if [ -r "/proc/modules" ]; then
        if grep -w "^$mod_name" /proc/modules >/dev/null 2>&1; then
            proc_state=0
        else
            proc_state=1
        fi
    else
        proc_state=2
    fi

    # 3. Source 3: lsmod utility
    if command -v lsmod >/dev/null 2>&1; then
        local lsmod_out=""
        local lsmod_rc=0
        lsmod_out=$(lsmod 2>&1)
        lsmod_rc=$?
        if [ "$lsmod_rc" -eq 0 ]; then
            if echo "$lsmod_out" | grep -w "^$mod_name" >/dev/null 2>&1; then
                lsmod_state=0
            else
                lsmod_state=1
            fi
        else
            lsmod_state=2
        fi
    else
        lsmod_state=2
    fi

    # Evaluation: Fail closed if ANY inspection source errored
    if [ "$sysfs_state" -eq 2 ] || [ "$proc_state" -eq 2 ] || [ "$lsmod_state" -eq 2 ]; then
        return 2
    fi

    # Fail closed on contradictory observations
    if [ "$sysfs_state" -ne "$proc_state" ] || [ "$proc_state" -ne "$lsmod_state" ]; then
        return 2
    fi

    # All three sources agree
    if [ "$sysfs_state" -eq 0 ]; then
        return 0
    elif [ "$sysfs_state" -eq 1 ]; then
        return 1
    fi

    return 2
}

# ------------------------------------------------------------------------------
# Critical Section Signal Deferral Helper (Stage 2E.7.10)
# ------------------------------------------------------------------------------
# Defers asynchronous signals during module insertion until exact ownership state
# (THIS_INVOCATION_INSERTED) is committed in Bash.
# ------------------------------------------------------------------------------
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

        # Validate stat attributes before arithmetic conversion
        if ! validate_decimal_rdev "$sym_rdev" || \
           ! validate_hex_majmin "$sym_maj_hex" 8 || \
           ! validate_hex_majmin "$sym_min_hex" 8; then
            printf "[FAIL] Symlink target stat values are empty, malformed, or out of range (rdev='%s', maj='%s', min='%s')!\n" \
                "$sym_rdev" "$sym_maj_hex" "$sym_min_hex"
            printf "%s\n" "[SAFETY STOP] Symlink target stat validation failed. Refusing module unload."
            return 2
        fi

        local sym_maj_dec=$((16#$sym_maj_hex))
        local sym_min_dec=$((16#$sym_min_hex))
        local sym_calc_rdev=$(( ((sym_maj_dec & 0xfffff000) << 32) | ((sym_maj_dec & 0xfff) << 8) | ((sym_min_dec & 0xffffff00) << 12) | (sym_min_dec & 0xff) ))
        if [ "$sym_rdev" -ne "$sym_calc_rdev" ]; then
            printf "%s\n" "[FAIL] Symlink target stat is internally inconsistent ($sym_rdev vs $sym_calc_rdev)!"
            return 2
        fi

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

        # Explicit validation of stat output values before any arithmetic conversion
        if ! validate_decimal_rdev "$curr_stat_rdev" || \
           ! validate_hex_majmin "$curr_stat_maj_hex" 8 || \
           ! validate_hex_majmin "$curr_stat_min_hex" 8; then
            printf "[FAIL] Current devnode stat values are empty, malformed, or out of range (rdev='%s', maj='%s', min='%s')!\n" \
                "$curr_stat_rdev" "$curr_stat_maj_hex" "$curr_stat_min_hex"
            printf "%s\n" "[SAFETY STOP] Devnode stat inspection failed. Refusing module unload."
            return 2
        fi

        # Confirm internal consistency of current devnode
        local curr_maj_dec=$((16#$curr_stat_maj_hex))
        local curr_min_dec=$((16#$curr_stat_min_hex))
        local curr_calc_rdev=$(( ((curr_maj_dec & 0xfffff000) << 32) | ((curr_maj_dec & 0xfff) << 8) | ((curr_min_dec & 0xffffff00) << 12) | (curr_min_dec & 0xff) ))
        if [ "$curr_stat_rdev" -ne "$curr_calc_rdev" ]; then
            printf "%s\n" "[FAIL] Current devnode stat is internally inconsistent ($curr_stat_rdev vs $curr_calc_rdev)!"
            printf "%s\n" "[SAFETY STOP] Inconsistent devnode identity. Refusing module unload."
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

    # Cleanup-time sysfs identity verification:
    # While the module is loaded, its expected sysfs device identity must remain readable and verified.
    # If the module is loaded and sysfs is unreadable, fail closed (status 2).
    local sysfs_dev_path="/sys/class/fte3600/fte3600/dev"
    if [ ! -r "$sysfs_dev_path" ] && [ -r "/sys/bus/spi/devices/spi-FTE3600:00/fte3600/dev" ]; then
        sysfs_dev_path="/sys/bus/spi/devices/spi-FTE3600:00/fte3600/dev"
    fi

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

    if [ ! -r "$sysfs_dev_path" ]; then
        if [ "$module_loaded" -eq 1 ]; then
            printf "%s\n" "[FAIL] Expected sysfs device attribute is unreadable while module is loaded (checked class and bus paths)."
            printf "%s\n" "[SAFETY STOP] Character device identity cannot be cross-checked against sysfs. Refusing module unload."
            return 2
        fi
    else
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
            printf "%s\n" "[FAIL] Sysfs device attribute is malformed ($sysfs_dev_str)!\n"
            printf "%s\n" "[SAFETY STOP] Malformed sysfs identity. Refusing module unload."
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

# ------------------------------------------------------------------------------
# Guaranteed Non-Recursive Cleanup & Diagnostic Reporting Handler
# ------------------------------------------------------------------------------
# In accordance with Stage 2E.8 Option 1 (Never Automatically Unload the Fingerprint Driver),
# this handler NEVER unloads fte3600.ko, NEVER unbinds spi-FTE3600:00, and NEVER terminates
# any processes. It performs strictly read-only diagnostics, reports module and device state,
# logs mandatory policy banners, and releases script-owned resources (FD 200 / cooperative lock).
# ------------------------------------------------------------------------------
cleanup_handler() {
    local incoming_exit="${1:-0}"
    local signal_name="${2:-EXIT}"

    # Prevent recursive invocation
    if [ "$CLEANUP_IN_PROGRESS" -eq 1 ]; then
        return 0
    fi
    CLEANUP_IN_PROGRESS=1
    CLEANUP_ATTEMPTED=1

    # Disable set -e so failures in individual reporting steps
    # do not abort the cleanup sequence midway
    set +e

    printf "\n%s\n" "--- [CLEANUP] Post-Test Diagnostic & Resource Release Handler ---"
    printf "Invoked via: %s (incoming exit status: %d)\n" "$signal_name" "$incoming_exit"

    # --------------------------------------------------------------------------
    # Read-Only Diagnostic Inspection: Kernel Module Status
    # --------------------------------------------------------------------------
    printf "\n%s\n" "--- Module & Device State Audit ---"
    local mod_state=0
    local was_errexit_cleanup_mod=0
    if shopt -o -q errexit; then was_errexit_cleanup_mod=1; fi
    set +e
    query_module_state "fte3600"
    mod_state=$?
    if [ "$was_errexit_cleanup_mod" -eq 1 ]; then set -e; fi

    if [ "$mod_state" -eq 0 ]; then
        printf "%s\n" "[INFO] fte3600 module is currently loaded in kernel."
    elif [ "$mod_state" -eq 1 ]; then
        printf "%s\n" "[INFO] fte3600 module is confirmed absent from kernel."
    else
        printf "%s\n" "[WARN] Unable to determine fte3600 module state in kernel (inspection inconclusive)."
    fi

    # --------------------------------------------------------------------------
    # Read-Only Diagnostic Inspection: Character Device Node Status
    # --------------------------------------------------------------------------
    local target_devnode="${MOCK_TARGET_NODE:-/dev/fte3600}"
    printf "Checking character device node %s ... " "$target_devnode"
    if [ -e "$target_devnode" ]; then
        printf "%s\n" "[INFO] $target_devnode is PRESENT"
    else
        printf "%s\n" "[INFO] $target_devnode is ABSENT"
    fi

    # --------------------------------------------------------------------------
    # Read-Only Diagnostic Inspection: SPI Driver Binding Status (Never unbind)
    # --------------------------------------------------------------------------
    printf "Checking SPI driver binding link ... "
    if [ -d "$SPI_PATH/driver" ]; then
        local bound_driver
        bound_driver=$(readlink "$SPI_PATH/driver" 2>/dev/null || true)
        printf "[INFO] SPI device is BOUND to: %s\n" "$bound_driver"
    else
        printf "%s\n" "[INFO] SPI device is UNBOUND"
    fi

    # --------------------------------------------------------------------------
    # Read-Only Informative Descriptor Audit
    # --------------------------------------------------------------------------
    if [ -e "$target_devnode" ]; then
        printf "\n%s\n" "--- Informative Open-Descriptor Audit ---"
        check_open_descriptors || true
    fi

    # --------------------------------------------------------------------------
    # Read-Only Diagnostic Inspection: Hardware & GPIO State
    # --------------------------------------------------------------------------
    printf "\n%s\n" "--- Post-Test Hardware State ---"
    if [ -r "$PINCTRL_PINS" ]; then
        printf "PCH Pad 14 post-test: %s\n" "$(grep "pin 14 " "$PINCTRL_PINS" || true)"
        printf "PCH Pad 229 post-test: %s\n" "$(grep "pin 229 " "$PINCTRL_PINS" || true)"
    fi

    if [ -r "$DEBUGFS_GPIO" ]; then
        printf "GPIO lines claimed by fte3600 post-test: %s\n" \
            "$(grep "fte3600" "$DEBUGFS_GPIO" || echo "None")"
    fi

    printf "\n%s\n" "--- Kernel Ring Buffer Messages ---"
    dmesg | tail -n 15 || true

    # --------------------------------------------------------------------------
    # Option 1 Mandatory Policy Banners
    # --------------------------------------------------------------------------
    printf "\n%s\n" "==============================================================================="
    printf "%s\n" " OPTION 1 POLICY: AUTOMATIC MODULE UNLOAD IS PERMANENTLY DISABLED"
    printf "%s\n" "==============================================================================="

    if [ "$INITIAL_FTE3600_LOADED" -eq 1 ]; then
        printf "%s\n" "[POLICY] Module was pre-existing before script started. Script does not own module."
        printf "%s\n" "[POLICY] PRE-EXISTING MODULE PRESERVED IN KERNEL — NO UNLOAD PERFORMED"
    elif [ "$THIS_INVOCATION_INSERTED" -eq 1 ] && [ "${INSERTION_STATE:-NOT_ATTEMPTED}" = "DEFINITELY_SUCCEEDED" ]; then
        printf "%s\n" "[POLICY] MODULE LEFT LOADED BY POLICY — MANUAL CLEANUP REQUIRED"
        printf "%s\n" "[POLICY] To manually unload after inspection: sudo rmmod fte3600"
    elif [ "${INSERTION_STATE:-NOT_ATTEMPTED}" = "UNCERTAIN" ]; then
        printf "%s\n" "[POLICY] MODULE STATE MAY BE PRESENT — INSERTION OUTCOME UNCERTAIN; DO NOT AUTOMATICALLY REMOVE"
        printf "%s\n" "[POLICY] Manual inspection required to verify kernel module status."
    elif [ "${INSERTION_STATE:-NOT_ATTEMPTED}" = "DEFINITELY_FAILED" ]; then
        printf "%s\n" "[POLICY] Module insertion definitely failed. Script does not own any loaded module."
    else
        printf "[POLICY] Insertion state: %s. No automatic unload performed.\n" "${INSERTION_STATE:-NOT_ATTEMPTED}"
    fi
    printf "%s\n\n" "==============================================================================="

    # --------------------------------------------------------------------------
    # Release Script-Owned Cooperative Lock (FD 200) Only After Diagnostics
    # --------------------------------------------------------------------------
    if { true >&200; } 2>/dev/null; then
        flock -u 200 2>/dev/null || true
        exec 200>&- 2>/dev/null || true
        printf "%s\n" "[INFO] Script-owned cooperative lock released (FD 200 closed)."
    fi

    # --------------------------------------------------------------------------
    # Determine final exit status (Preserves original test exit status)
    # --------------------------------------------------------------------------
    # 1. If invoked via signal (INT=130, TERM=143, HUP=129), retain signal status.
    # 2. If test operations failed (incoming_exit != 0), retain primary test failure code.
    # 3. If test operations passed (0), return 0.
    local final_exit="$incoming_exit"
    if [ "$final_exit" -eq 0 ]; then
        printf "\n%s\n" "[PASS] Stage B test execution completed successfully (status 0)."
    else
        printf "\n[INFO] Script terminated with exit status %d (trigger: %s).\n" \
            "$incoming_exit" "$signal_name"
    fi

    # Unhook all traps before final exit to prevent recursion
    trap - EXIT INT TERM HUP
    exit "$final_exit"
}

# Trap registrations: ensures signals cannot run cleanup and resume the test script
trap 'cleanup_handler 130 "SIGINT"' INT
trap 'cleanup_handler 143 "SIGTERM"' TERM
trap 'cleanup_handler 129 "SIGHUP"' HUP
trap 'cleanup_handler $? "EXIT"' EXIT

# ------------------------------------------------------------------------------
# Pre-Flight Checks
# ------------------------------------------------------------------------------
printf "%s\n" "--- [PHASE 1] Pre-Flight System & Safety Inspection ---"

if [ "$(id -u)" -ne 0 ]; then
    printf "%s\n" "[FAIL] Stage B execution requires root privileges (sudo)."
    exit 1
fi

# 0. Cooperative test runner lock (FAIL-CLOSED)
# Note: flock coordinates cooperating instances of this test runner.
# It does NOT prevent uncoordinated external administrators or unrelated module loaders from acting.
LOCK_FILE="/run/lock/fte3600-test.lock"
printf "Acquiring fail-closed cooperative test runner lock: %s ... " "$LOCK_FILE"

# A. Verify /run/lock directory presence
if [ ! -d "/run/lock" ]; then
    printf "%s\n" "[FAIL] Lock directory /run/lock does not exist."
    printf "%s\n" "[SAFETY STOP] Cannot establish cooperative locking. Refusing to proceed."
    exit 1
fi

# B. Verify flock command is available
if ! command -v flock >/dev/null 2>&1; then
    printf "%s\n" "[FAIL] Utility 'flock' is not available."
    printf "%s\n" "[SAFETY STOP] Cannot establish cooperative locking. Refusing to proceed."
    exit 1
fi

# C. Open lock file descriptor (FD 200)
if ! exec 200>"$LOCK_FILE" 2>/dev/null; then
    printf "%s\n" "[FAIL] Unable to create/open lock file $LOCK_FILE."
    printf "%s\n" "[SAFETY STOP] Cannot acquire cooperative lock file descriptor. Refusing to proceed."
    exit 1
fi

# D. Acquire non-blocking exclusive flock
if ! flock -n 200; then
    printf "%s\n" "[FAIL] Another instance of the Stage B test runner is currently holding $LOCK_FILE!"
    printf "%s\n" "[SAFETY STOP] Concurrent execution prohibited. Refusing to proceed."
    exit 1
fi

printf "%s\n" "[PASS] (Exclusive cooperative lock held on FD 200)"

# 1. Kernel version
RUNNING_KERNEL=$(uname -r)
printf "Checking kernel version: %s ... " "$RUNNING_KERNEL"
if [ "$RUNNING_KERNEL" != "7.2.8-9-omarchy" ]; then
    printf "%s\n" "[FAIL] Unexpected kernel version."
    exit 1
fi
printf "%s\n" "[PASS]"

# 2. Module verification
printf "Checking module file: %s ... " "$MODULE_PATH"
if [ ! -f "$MODULE_PATH" ]; then
    printf "%s\n" "[FAIL] Module file does not exist. Run make in kernel/ first."
    exit 1
fi
printf "%s\n" "[PASS]"

MODULE_VERMAGIC=$(modinfo -F vermagic "$MODULE_PATH")
printf "Module vermagic: %s ... " "$MODULE_VERMAGIC"
if [[ "$MODULE_VERMAGIC" != "$RUNNING_KERNEL"* ]]; then
    printf "%s\n" "[FAIL] Module vermagic does not match running kernel."
    exit 1
fi
printf "%s\n" "[PASS]"

# 3. Existing module presence
printf "Checking existing module loading ... "
initial_mod_state=0
was_errexit_initial_mod=0
if shopt -o -q errexit; then was_errexit_initial_mod=1; fi
set +e
query_module_state "fte3600"
initial_mod_state=$?
if [ "$was_errexit_initial_mod" -eq 1 ]; then set -e; fi

if [ "$initial_mod_state" -eq 0 ]; then
    INITIAL_FTE3600_LOADED=1
    printf "%s\n" "[FAIL] Conflicting module already loaded: fte3600"
    exit 1
elif [ "$initial_mod_state" -eq 2 ]; then
    printf "%s\n" "[FAIL] Unable to determine fte3600 module state in kernel (inspection inconclusive)."
    exit 1
fi

spidev_mod_state=0
was_errexit_spidev=0
if shopt -o -q errexit; then was_errexit_spidev=1; fi
set +e
query_module_state "spidev"
spidev_mod_state=$?
if [ "$was_errexit_spidev" -eq 1 ]; then set -e; fi

if [ "$spidev_mod_state" -eq 0 ]; then
    printf "%s\n" "[FAIL] Conflicting module already loaded: spidev"
    exit 1
elif [ "$spidev_mod_state" -eq 2 ]; then
    printf "%s\n" "[FAIL] Unable to determine spidev module state in kernel (inspection inconclusive)."
    exit 1
fi
printf "%s\n" "[PASS] (Neither fte3600 nor spidev loaded)"

# 4. Device presence and binding
printf "Checking SPI device presence: %s ... " "$SPI_PATH"
if [ ! -d "$SPI_PATH" ]; then
    printf "%s\n" "[FAIL] SPI device does not exist."
    exit 1
fi
printf "%s\n" "[PASS]"

printf "Checking SPI device binding status ... "
if [ -d "$SPI_PATH/driver" ]; then
    printf "[FAIL] Device already bound to driver: %s\n" "$(readlink "$SPI_PATH/driver" 2>/dev/null || true)"
    exit 1
fi
printf "%s\n" "[PASS] (Device unbound)"

# 5. Device node presence
printf "Checking character device node presence: /dev/fte3600 ... "
if [ -e "/dev/fte3600" ]; then
    printf "%s\n" "[FAIL] /dev/fte3600 already exists."
    exit 1
fi
printf "%s\n" "[PASS] (Node absent)"

# 6. Tool binary verification
printf "Checking validation utility: %s ... " "$TOOL_BIN"
if [ ! -x "$TOOL_BIN" ]; then
    printf "%s\n" "[FAIL] Utility binary not found or not executable. Compile tools/fte3600-tool.c first."
    exit 1
fi
printf "%s\n" "[PASS]"

# 7. Legacy Userspace Safety Gate Audit
# Note: bin/fw9369-hw-tool is the legacy userspace spidev mock/preflight utility from Stage 2C.
# It does NOT govern, enforce, or authorize in-kernel FTE3600_IOC_RESET ioctls on /dev/fte3600.
# Stage B authorization is granted strictly and exclusively by explicit user consent.
printf "Auditing legacy userspace safety gate ... "
GATE_OUTPUT=""
GATE_RC=0
was_errexit_gate=0
if shopt -o -q errexit; then was_errexit_gate=1; fi
set +e
GATE_OUTPUT=$("$SAFETY_GATE_BIN" 2>&1)
GATE_RC=$?
if [ "$was_errexit_gate" -eq 1 ]; then set -e; fi

if [ "$GATE_RC" -eq 1 ] && echo "$GATE_OUTPUT" | grep -q "\[SAFETY GATE PASS\] Physical device access is blocked by policy"; then
    printf "%s\n" "[PASS] (Legacy spidev gate SEALED, exit code 1 verified via policy banner)"
else
    printf "[FAIL] Legacy safety gate returned unexpected output or code %d\n" "$GATE_RC"
    printf "Output was:\n%s\n" "$GATE_OUTPUT"
    exit 1
fi

# 8. Record Baseline Hardware State
printf "\n%s\n" "--- Baseline Hardware & Register State ---"
if [ -r "$PINCTRL_PINS" ]; then
    printf "PCH Pad 14 (Reset):   %s\n" "$(grep "pin 14 " "$PINCTRL_PINS" || true)"
    printf "PCH Pad 229 (IRQ):    %s\n" "$(grep "pin 229 " "$PINCTRL_PINS" || true)"
else
    printf "%s\n" "[WARN] Unable to read pinctrl debugfs at $PINCTRL_PINS"
fi

if [ -r "$DEBUGFS_GPIO" ]; then
    printf "GPIO lines claimed by fte3600: %s\n" \
        "$(grep "fte3600" "$DEBUGFS_GPIO" || echo "None")"
fi

DMESG_START_LINES=$(dmesg | wc -l)
printf "Dmesg baseline line count: %d\n\n" "$DMESG_START_LINES"

# ------------------------------------------------------------------------------
# Stage B Execution
# ------------------------------------------------------------------------------
printf "%s\n" "--- [PHASE 2] Module Insertion & Driver Probe ---"
printf "Executing: insmod %s\n" "$MODULE_PATH"

# ------------------------------------------------------------------------------
# Critical Section: Signal-Safe Module Insertion (Stage 2E.7.10/2E.7.11)
# ------------------------------------------------------------------------------
# Defer INT, TERM, HUP so that cleanup_handler cannot interrupt between command return
# and recording the exact ownership state (THIS_INVOCATION_INSERTED).
MODULE_INSERTION_ATTEMPTED=1
THIS_INVOCATION_INSERTED=0
INSERTION_STATE="$INSERTION_STATE_NOT_ATTEMPTED"
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

# Conservative State Machine Evaluation (Stage 2E.7.11):
# 1. If insmod terminated due to a signal (rc >= 128), outcome is UNCERTAIN:
if [ "$insmod_rc" -ge 128 ]; then
    INSERTION_STATE="$INSERTION_STATE_UNCERTAIN"
    THIS_INVOCATION_INSERTED=0
    MODULE_INSERTED=0
# 2. If insmod returned 0, insertion DEFINITELY SUCCEEDED:
elif [ "$insmod_rc" -eq 0 ]; then
    INSERTION_STATE="$INSERTION_STATE_DEFINITELY_SUCCEEDED"
    THIS_INVOCATION_INSERTED=1
    MODULE_INSERTED=1
# 3. If insmod returned non-zero (< 128), insertion DEFINITELY FAILED:
else
    INSERTION_STATE="$INSERTION_STATE_DEFINITELY_FAILED"
    THIS_INVOCATION_INSERTED=0
    MODULE_INSERTED=0
fi

# Critical state committed. Restore standard trap handlers.
trap 'cleanup_handler 130 "SIGINT"' INT
trap 'cleanup_handler 143 "SIGTERM"' TERM
trap 'cleanup_handler 129 "SIGHUP"' HUP

if [ "$was_errexit_insmod" -eq 1 ]; then set -e; fi

# Process any signal that was deferred during the critical section
if [ -n "$DEFERRED_SIGNAL" ]; then
    printf "[INFO] Processing deferred signal %s (%d) after critical section (state=%s, THIS_INVOCATION_INSERTED=%d).\n" \
        "$DEFERRED_SIGNAL_NAME" "$DEFERRED_SIGNAL" "$INSERTION_STATE" "$THIS_INVOCATION_INSERTED"
    cleanup_handler "$DEFERRED_SIGNAL" "$DEFERRED_SIGNAL_NAME"
fi

if [ "$INSERTION_STATE" = "$INSERTION_STATE_DEFINITELY_SUCCEEDED" ]; then
    printf "%s\n" "[PASS] (insmod succeeded; ownership verified and committed)"
elif [ "$INSERTION_STATE" = "$INSERTION_STATE_UNCERTAIN" ]; then
    printf "[FAIL] insmod was interrupted by signal (rc=%d; outcome UNCERTAIN)\n" "$insmod_rc"
    exit "$insmod_rc"
else
    printf "[FAIL] insmod failed with exit code %d (insertion DEFINITELY FAILED)\n" "$insmod_rc"
    exit "$insmod_rc"
fi
sleep 1

printf "\n%s\n" "--- Post-Probe Verification ---"

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

# 2. Verify SPI driver binding
printf "Checking SPI driver binding link ... "
if [ -d "$SPI_PATH/driver" ]; then
    BOUND_DRIVER=$(readlink "$SPI_PATH/driver" 2>/dev/null || true)
    printf "[PASS] Bound to: %s\n" "$BOUND_DRIVER"
else
    printf "%s\n" "[FAIL] SPI device failed to bind to driver."
    exit 1
fi

# 3. Verify character device node and validate device identity
printf "Checking character device node /dev/fte3600 ... "
if ! record_devnode_identity; then
    exit 1
fi

# 4. Check active GPIO claims
printf "\n%s\n" "Checking GPIO allocation in debugfs ..."
if [ -r "$DEBUGFS_GPIO" ]; then
    grep -E "gpio-14|fte3600" "$DEBUGFS_GPIO" || printf "%s\n" "[INFO] No specific fte3600 line in gpio debugfs."
fi

# 5. Check PCH pad register state under probe
if [ -r "$PINCTRL_PINS" ]; then
    printf "PCH Pad 14 under probe: %s\n" "$(grep "pin 14 " "$PINCTRL_PINS" || true)"
    printf "PCH Pad 229 under probe: %s\n" "$(grep "pin 229 " "$PINCTRL_PINS" || true)"
fi

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

# 7. Execute exactly ONE RESET ioctl
printf "\n%s\n" "--- [PHASE 4] Executing Exactly ONE Controlled Reset IOCTL ---"
printf "Invoking: %s reset\n" "$TOOL_BIN"
RESET_RC=0
was_errexit_reset=0
if shopt -o -q errexit; then was_errexit_reset=1; fi
set +e
"$TOOL_BIN" reset
RESET_RC=$?
if [ "$was_errexit_reset" -eq 1 ]; then set -e; fi
printf "Reset ioctl process return code: %d\n" "$RESET_RC"

if [ "$RESET_RC" -ne 0 ]; then
    printf "%s\n" "[WARN] Reset ioctl failed with non-zero exit code ($RESET_RC)."
fi

# 8. Query post-reset driver state via GET_STATE ioctl (Snapshot 2)
printf "\n%s\n" "--- [PHASE 5] Telemetry Snapshot 2: Post-Reset State ---"
SNAPSHOT2_RC=0
was_errexit_snap2=0
if shopt -o -q errexit; then was_errexit_snap2=1; fi
set +e
"$TOOL_BIN" get-state
SNAPSHOT2_RC=$?
if [ "$was_errexit_snap2" -eq 1 ]; then set -e; fi
if [ "$SNAPSHOT2_RC" -ne 0 ]; then
    printf "%s\n" "[WARN] Snapshot 2 (GET_STATE) returned non-zero exit code ($SNAPSHOT2_RC)."
fi

# 9. Pad status immediately following reset
if [ -r "$PINCTRL_PINS" ]; then
    printf "\n%s\n" "--- Hardware Pad Registers Post-Reset ---"
    printf "PCH Pad 14 post-reset: %s\n" "$(grep "pin 14 " "$PINCTRL_PINS" || true)"
    printf "PCH Pad 229 post-reset: %s\n" "$(grep "pin 229 " "$PINCTRL_PINS" || true)"
fi

# 10. Check kernel ring buffer for probe & reset messages
printf "\n%s\n" "--- Kernel Ring Buffer Messages During Probe & Reset ---"
DMESG_RC=0
was_errexit_dmesg=0
if shopt -o -q errexit; then was_errexit_dmesg=1; fi
set +e
dmesg | tail -n +"$((DMESG_START_LINES + 1))"
DMESG_RC=$?
if [ "$was_errexit_dmesg" -eq 1 ]; then set -e; fi
if [ "$DMESG_RC" -ne 0 ]; then
    printf "%s\n" "[WARN] dmesg command returned non-zero exit code ($DMESG_RC)."
fi

# ------------------------------------------------------------------------------
# Test Exit Status Resolution (Preserving Primary Reset Failure)
# ------------------------------------------------------------------------------
# Rule 1: A non-zero RESET_RC is the PRIMARY test failure.
# Rule 2: Subsequent Snapshot 2 or dmesg success cannot turn a failed reset into success.
# Rule 3: Subsequent Snapshot 2 or dmesg failure cannot replace the original RESET_RC.
# Rule 4: If reset succeeded but Snapshot 2 or dmesg failed, propagate that failure code.
# Rule 5: If all succeeded, exit code is 0.
TEST_EXIT_STATUS=0

if [ "$RESET_RC" -ne 0 ]; then
    TEST_EXIT_STATUS="$RESET_RC"
    printf "\n%s\n" "[FAIL] Stage B Primary Failure: Reset operation failed with exit code $RESET_RC."
    if [ "$SNAPSHOT2_RC" -ne 0 ]; then
        printf "[NOTE] Secondary failure: Snapshot 2 also failed with exit code %d (retaining primary failure %d).\n" \
            "$SNAPSHOT2_RC" "$TEST_EXIT_STATUS"
    fi
    if [ "$DMESG_RC" -ne 0 ]; then
        printf "[NOTE] Secondary failure: dmesg also failed with exit code %d (retaining primary failure %d).\n" \
            "$DMESG_RC" "$TEST_EXIT_STATUS"
    fi
elif [ "$SNAPSHOT2_RC" -ne 0 ]; then
    TEST_EXIT_STATUS="$SNAPSHOT2_RC"
    printf "\n%s\n" "[FAIL] Stage B Failure: Reset succeeded (0), but Snapshot 2 failed with exit code $SNAPSHOT2_RC."
elif [ "$DMESG_RC" -ne 0 ]; then
    TEST_EXIT_STATUS="$DMESG_RC"
    printf "\n%s\n" "[FAIL] Stage B Failure: Reset and Snapshot 2 succeeded, but dmesg returned exit code $DMESG_RC."
else
    TEST_EXIT_STATUS=0
    printf "\n%s\n" "[PASS] Stage B operations (Probe, Snapshot 1, Reset, Snapshot 2) executed with status 0."
fi

# Normal termination triggers the EXIT trap to execute cleanup_handler with TEST_EXIT_STATUS
exit "$TEST_EXIT_STATUS"
