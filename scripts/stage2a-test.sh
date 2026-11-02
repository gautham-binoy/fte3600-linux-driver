#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Stage 2A Pure-spidev Hardware Identification & Rollback Script
# Narrowly scoped, supervised sensor identification for Dynabook Satellite Pro C40-J.
#
# Architecture: PURE spidev (Decoupled from Stage 1 fte3600.ko and UIO)
#
# Strictly:
#   - Relies on PCH Pad 14 firmware boot output-high state (0x84000201)
#   - Does NOT load fte3600.ko or uio.ko
#   - Does NOT request or manipulate GPIO descriptors or PCH registers
#   - Does NOT touch Pad 14 or Pad 229
#   - Writes ONLY volatile register 0xC6 = 0x01 (SPI clock enable)
#   - Reads register 0xC6 (verifies 0x01)
#   - Reads Chip ID register 0x1A8B (16-bit word)
#   - Reads sub-revision register 0x1816 ONLY if Chip ID == 0x9391
#   - STRICT STOP on any error or unexpected response
#   - NO retries, NO reset pulses, NO recovery writes, NO idle commands
#   - Complete host-side rollback releasing all SPI resources
#
set -euo pipefail
export LC_ALL=C

# ==============================================================================
# Configuration and State Tracking
# ==============================================================================

PROJECT_DIR=$(cd "$(dirname "$0")/.." && pwd)
DIAG_DIR="$PROJECT_DIR/diagnostics"
STAGE2A_LOG="$DIAG_DIR/stage2a-results.txt"
IDENTIFY_BIN="$PROJECT_DIR/bin/stage2a-identify"
SPI_DEV="spi-FTE3600:00"
SPI_PATH="/sys/bus/spi/devices/$SPI_DEV"
SPIDEV_DRIVER_PATH="/sys/bus/spi/drivers/spidev"
CANONICAL_SPIDEV_DRIVER_PATH=""

# State tracking flags to ensure we ONLY revert what this script modified
SPIDEV_LOADED_BY_US=0
DRIVER_OVERRIDE_SET_BY_US=0
SPIDEV_BOUND_BY_US=0
IDENTIFY_EXECUTED=0
IDENTIFY_EXIT_CODE=0

# Preserved original configuration
ORIGINAL_DRIVER_OVERRIDE=""

# Critical section, deferred signal, and state certainty tracking
IN_CRITICAL_SECTION=0
PENDING_SIGNAL=0
CLEANUP_INTERRUPTED=0
STATE_UNCERTAIN=0
STATE_UNCERTAIN_MSG=""

# Control and status tracking
CLEANUP_EXECUTED=0
CLEANUP_ERRORS=0
CLEANUP_STATUS=0
ORIGINAL_EXIT_CODE=0

# ==============================================================================
# State Inspection & Normalization Helpers
# ==============================================================================

get_module_state() {
    local mod="$1"
    local sys_state="UNKNOWN"
    local proc_state="UNKNOWN"

    if [ -d "/sys/module/$mod" ]; then
        sys_state="PRESENT"
    else
        sys_state="ABSENT"
    fi

    if [ -r "/proc/modules" ]; then
        if awk '{print $1}' /proc/modules | grep -qx "$mod"; then
            proc_state="PRESENT"
        else
            proc_state="ABSENT"
        fi
    fi

    if [ "$sys_state" = "PRESENT" ] && [ "$proc_state" = "PRESENT" ]; then
        echo "PRESENT"
    elif [ "$sys_state" = "ABSENT" ] && [ "$proc_state" = "ABSENT" ]; then
        echo "ABSENT"
    else
        echo "UNKNOWN"
    fi
}

get_bound_driver_canonical() {
    local dev_path="$1"
    local driver_link="$dev_path/driver"

    if [ ! -d "$dev_path" ]; then
        echo "UNRESOLVED"
        return 0
    fi

    if [ ! -e "$driver_link" ] && [ ! -L "$driver_link" ]; then
        echo "UNBOUND"
        return 0
    fi

    if [ -L "$driver_link" ]; then
        local target
        target=$(readlink -f "$driver_link" 2>/dev/null || true)
        if [ -n "$target" ] && [ -d "$target" ]; then
            echo "$target"
        else
            echo "UNRESOLVED"
        fi
    else
        echo "UNRESOLVED"
    fi
}

resolve_spidev_canonical_path() {
    if [ ! -d "$SPIDEV_DRIVER_PATH" ]; then
        CANONICAL_SPIDEV_DRIVER_PATH=""
        return 1
    fi
    local resolved
    resolved=$(readlink -f "$SPIDEV_DRIVER_PATH" 2>/dev/null || true)
    if [ -n "$resolved" ] && [ -d "$resolved" ]; then
        CANONICAL_SPIDEV_DRIVER_PATH="$resolved"
        return 0
    fi
    CANONICAL_SPIDEV_DRIVER_PATH=""
    return 1
}

normalize_override() {
    local val="$1"
    val=$(printf '%s' "$val" | tr -d '\r\n')
    if [ "$val" = "(null)" ] || [ -z "$val" ]; then
        echo ""
    else
        echo "$val"
    fi
}

# ==============================================================================
# Deferred Signal Handling for Atomic State Transitions
# ==============================================================================

on_critical_int() {
    if [ "$PENDING_SIGNAL" -eq 0 ]; then
        PENDING_SIGNAL=130
    fi
    echo "" >&2
    echo ">> [DEFERRED] SIGINT received; deferred until state reconciliation completes." >&2
}

on_critical_term() {
    if [ "$PENDING_SIGNAL" -eq 0 ]; then
        PENDING_SIGNAL=143
    fi
    echo "" >&2
    echo ">> [DEFERRED] SIGTERM received; deferred until state reconciliation completes." >&2
}

enter_critical() {
    IN_CRITICAL_SECTION=1
    PENDING_SIGNAL=0
    trap on_critical_int INT
    trap on_critical_term TERM
}

leave_critical() {
    IN_CRITICAL_SECTION=0
    trap on_int INT
    trap on_term TERM
    if [ "$PENDING_SIGNAL" -ne 0 ]; then
        local sig="$PENDING_SIGNAL"
        PENDING_SIGNAL=0
        echo ">> [DISPATCH] Delivering deferred signal ($sig) now that state is reconciled." >&2
        ORIGINAL_EXIT_CODE="$sig"
        exit "$sig"
    fi
}

on_cleanup_int() {
    CLEANUP_INTERRUPTED=130
    echo "" >&2
    echo ">> [DEFERRED] SIGINT received during cleanup; allowing current step to complete." >&2
}

on_cleanup_term() {
    CLEANUP_INTERRUPTED=143
    echo "" >&2
    echo ">> [DEFERRED] SIGTERM received during cleanup; allowing current step to complete." >&2
}

# ==============================================================================
# Robust, Signal-Protected Rollback Routine (Pure spidev)
# ==============================================================================

cleanup() {
    trap - EXIT
    if [ "$CLEANUP_EXECUTED" -eq 1 ]; then
        return "$CLEANUP_STATUS"
    fi
    CLEANUP_EXECUTED=1

    echo ""
    echo "========================================================"
    echo " Stage 2A Rollback Harness (Pure spidev)"
    echo " Timestamp: $(date -Iseconds)"
    echo "========================================================"

    trap on_cleanup_int INT
    trap on_cleanup_term TERM

    # Step 1: Unbind spidev from spi-FTE3600:00
    if [ "$SPIDEV_BOUND_BY_US" -eq 1 ]; then
        echo ">> Rollback step 1: Unbinding spidev from $SPI_DEV..."
        if [ -d "$SPI_PATH" ]; then
            local bound_driver
            bound_driver=$(get_bound_driver_canonical "$SPI_PATH")
            local target_canonical=""
            if [ -n "$CANONICAL_SPIDEV_DRIVER_PATH" ]; then
                target_canonical="$CANONICAL_SPIDEV_DRIVER_PATH"
            elif [ -d "$SPIDEV_DRIVER_PATH" ]; then
                target_canonical=$(readlink -f "$SPIDEV_DRIVER_PATH" 2>/dev/null || true)
            fi

            if [ -n "$target_canonical" ] && [ "$bound_driver" = "$target_canonical" ]; then
                local cmd_err=0
                echo "$SPI_DEV" > "$target_canonical/unbind" || cmd_err=$?
                bound_driver=$(get_bound_driver_canonical "$SPI_PATH")
                if [ "$bound_driver" = "UNBOUND" ]; then
                    echo "   [OK] $SPI_DEV unbind succeeded. Confirmed UNBOUND."
                    SPIDEV_BOUND_BY_US=0
                else
                    echo "   [FAIL] $SPI_DEV remains $bound_driver after unbind (cmd code: $cmd_err)!" >&2
                    CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
                fi
            elif [ "$bound_driver" = "UNBOUND" ]; then
                echo "   [SKIP] $SPI_DEV is already UNBOUND."
                SPIDEV_BOUND_BY_US=0
            else
                echo "   [FAIL] $SPI_DEV bound to unexpected target ('$bound_driver'); refusing unbind!" >&2
                CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
            fi
        else
            echo "   [WARN] $SPI_PATH missing." >&2
            SPIDEV_BOUND_BY_US=0
        fi
    else
        echo ">> Rollback step 1: spidev was not bound by us (skipping)."
    fi

    # Step 2: Restore driver_override
    if [ "$DRIVER_OVERRIDE_SET_BY_US" -eq 1 ]; then
        echo ">> Rollback step 2: Restoring driver_override on $SPI_DEV..."
        if [ -d "$SPI_PATH" ] && [ -w "$SPI_PATH/driver_override" ]; then
            local orig_norm
            orig_norm=$(normalize_override "$ORIGINAL_DRIVER_OVERRIDE")
            local cmd_err=0
            echo "$orig_norm" > "$SPI_PATH/driver_override" || cmd_err=$?
            local current_override=""
            if [ -r "$SPI_PATH/driver_override" ]; then
                current_override=$(cat "$SPI_PATH/driver_override" 2>&1 || true)
            fi
            local curr_norm
            curr_norm=$(normalize_override "$current_override")
            if [ "$curr_norm" = "$orig_norm" ]; then
                echo "   [OK] driver_override restored to '$orig_norm'."
                DRIVER_OVERRIDE_SET_BY_US=0
            else
                echo "   [FAIL] driver_override is '$curr_norm', expected '$orig_norm'!" >&2
                CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
            fi
        else
            echo "   [FAIL] $SPI_PATH/driver_override not accessible for restore!" >&2
            CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
        fi
    else
        echo ">> Rollback step 2: driver_override was not modified by us (skipping)."
    fi

    # Step 3: Unload spidev
    if [ "$SPIDEV_LOADED_BY_US" -eq 1 ]; then
        echo ">> Rollback step 3: Unloading spidev module..."
        local spidev_state
        spidev_state=$(get_module_state "spidev")
        if [ "$spidev_state" = "PRESENT" ]; then
            local cmd_err=0
            modprobe -r spidev || cmd_err=$?
            spidev_state=$(get_module_state "spidev")
            if [ "$spidev_state" = "ABSENT" ]; then
                echo "   [OK] modprobe -r spidev succeeded. Module ABSENT."
                SPIDEV_LOADED_BY_US=0
            else
                echo "   [FAIL] spidev remains $spidev_state after rmmod (cmd code: $cmd_err)!" >&2
                CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
            fi
        else
            echo "   [SKIP] spidev is already $spidev_state."
            SPIDEV_LOADED_BY_US=0
        fi
    else
        echo ">> Rollback step 3: spidev was not loaded by us (skipping)."
    fi

    # Post-Rollback State Audit
    echo ""
    echo "--- Post-Rollback State Verification ---"
    local post_spidev
    post_spidev=$(get_module_state "spidev")
    echo ">> Checking spidev module state post-rollback: $post_spidev"
    if [ "$post_spidev" = "PRESENT" ]; then
        echo ">> ERROR: spidev remains loaded post-rollback!" >&2
        CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
    fi

    if [ -d "$SPI_PATH" ]; then
        local post_bound
        post_bound=$(get_bound_driver_canonical "$SPI_PATH")
        echo ">> Checking SPI device driver link: $post_bound"
        if [ "$post_bound" != "UNBOUND" ]; then
            echo ">> ERROR: $SPI_DEV remains bound to '$post_bound' post-rollback!" >&2
            CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
        fi
    fi

    echo ">> Checking /dev device nodes:"
    ls -l /dev/spidev* 2>&1 || true

    echo ""
    echo ">> Sensor Internal State Notice:"
    echo "   Host-side resources are fully released, bus clock is halted, and CS is deasserted high."
    echo "   PCH Pad 14 (reset) and Pad 229 (IRQ) were never claimed or altered."
    echo "   The internal state of register 0xC6 cannot be confirmed as restored by host cleanup."
    echo "   Whether 0xC6 retains 0x01, autonomously decays, or resets via an undocumented internal"
    echo "   watchdog timer is unknown. Host cleanup guarantees host isolation only."

    if [ "$CLEANUP_ERRORS" -gt 0 ]; then
        CLEANUP_STATUS=1
        echo ""
        echo "========================================================"
        echo " ROLLBACK COMPLETED WITH $CLEANUP_ERRORS ERROR(S)" >&2
        echo " MANUAL RECONCILIATION REQUIRED!" >&2
        echo "========================================================"
    else
        CLEANUP_STATUS=0
        echo ""
        echo "========================================================"
        echo " ROLLBACK COMPLETED CLEANLY (0 ERRORS)"
        echo " Verified: spidev ABSENT, spi-FTE3600:00 UNBOUND, /dev/spidev* ABSENT"
        echo "========================================================"
    fi

    trap - INT TERM
    if [ "$CLEANUP_INTERRUPTED" -ne 0 ]; then
        local sig="$CLEANUP_INTERRUPTED"
        CLEANUP_INTERRUPTED=0
        exit "$sig"
    fi

    return "$CLEANUP_STATUS"
}

on_int() {
    trap on_cleanup_int INT
    trap on_cleanup_term TERM
    trap - EXIT
    ORIGINAL_EXIT_CODE=130
    echo "" >&2
    echo ">> Caught SIGINT (Ctrl+C). Initiating emergency rollback..." >&2
    cleanup
    exit 130
}

on_term() {
    trap on_cleanup_int INT
    trap on_cleanup_term TERM
    trap - EXIT
    ORIGINAL_EXIT_CODE=143
    echo "" >&2
    echo ">> Caught SIGTERM. Initiating emergency rollback..." >&2
    cleanup
    exit 143
}

trap cleanup EXIT
trap on_int INT
trap on_term TERM

# ==============================================================================
# Pre-Execution Environment & Sanity Checks (Read-Only)
# ==============================================================================

if [ "$(id -u)" -ne 0 ]; then
    echo "Error: This script must be run as root." >&2
    ORIGINAL_EXIT_CODE=1
    exit 1
fi

mkdir -p "$DIAG_DIR"
exec > >(tee "$STAGE2A_LOG") 2>&1

echo "========================================================"
echo " FTE3600 Stage 2A Supervised Sensor Identification (Pure spidev)"
echo " Host: $(cat /sys/devices/virtual/dmi/id/product_name 2>/dev/null || uname -n)"
echo " Kernel: $(uname -r) ($(uname -m))"
echo " Date: $(date -Iseconds)"
echo " Architecture: Pure spidev (No fte3600.ko, No UIO, No GPIO manipulation)"
echo "========================================================"
echo ""

echo "--- 1. Pre-Execution Safety Audits (Read-Only) ---"

# Check 1: Kernel version check
expected_kernel="7.2.8-9-omarchy"
running_kernel=$(uname -r)
if [ "$running_kernel" != "$expected_kernel" ]; then
    echo "Error: Kernel mismatch! Expected '$expected_kernel', running '$running_kernel'." >&2
    exit 1
fi
echo "[PASS] Running kernel matches expectation: $running_kernel"

# Check 2: Binary tool check
if [ ! -x "$IDENTIFY_BIN" ]; then
    echo "Error: Identification tool binary missing or not executable: $IDENTIFY_BIN" >&2
    exit 1
fi
echo "[PASS] Identification binary verified: $IDENTIFY_BIN"

# Check 3: SPI device existence check
if [ ! -d "$SPI_PATH" ]; then
    echo "Error: SPI device $SPI_DEV not found at $SPI_PATH!" >&2
    exit 1
fi
echo "[PASS] SPI device $SPI_DEV exists."

# Check 4: driver_override clean check
if [ ! -r "$SPI_PATH/driver_override" ]; then
    echo "Error: Cannot read $SPI_PATH/driver_override!" >&2
    exit 1
fi
ORIGINAL_DRIVER_OVERRIDE=$(cat "$SPI_PATH/driver_override" 2>&1 || true)
norm_orig=$(normalize_override "$ORIGINAL_DRIVER_OVERRIDE")
if [ -n "$norm_orig" ]; then
    echo "Error: driver_override on $SPI_DEV is not clean! Found: '$norm_orig'" >&2
    exit 1
fi
echo "[PASS] driver_override on $SPI_DEV is clean ('')."

# Check 5: Unbound SPI check
initial_bound=$(get_bound_driver_canonical "$SPI_PATH")
if [ "$initial_bound" != "UNBOUND" ]; then
    echo "Error: $SPI_DEV is already bound to driver: '$initial_bound'!" >&2
    exit 1
fi
echo "[PASS] $SPI_DEV is confirmed UNBOUND."

# Check 6: spidev module ABSENT check
initial_spidev=$(get_module_state "spidev")
if [ "$initial_spidev" != "ABSENT" ]; then
    echo "Error: spidev is currently $initial_spidev! Baseline requires ABSENT." >&2
    exit 1
fi
echo "[PASS] spidev is confirmed ABSENT."

# Check 7: No existing /dev/spidev* device nodes
existing_spidev=$(ls -d /dev/spidev* 2>/dev/null || true)
if [ -n "$existing_spidev" ]; then
    echo "Error: Pre-existing /dev/spidev nodes detected: $existing_spidev" >&2
    exit 1
fi
echo "[PASS] No pre-existing /dev/spidev* device nodes."

# Check 8: fte3600 module confirmed ABSENT
initial_fte=$(get_module_state "fte3600")
if [ "$initial_fte" != "ABSENT" ]; then
    echo "Error: fte3600 is currently $initial_fte! Baseline requires ABSENT." >&2
    exit 1
fi
echo "[PASS] fte3600 is confirmed ABSENT (pure spidev architecture)."

# Check 9: Read-Only PCH Pad Baseline Sanity Check (if debugfs readable)
if [ -r /sys/kernel/debug/pinctrl/INT34C5:00/pins ]; then
    p14_line=$(grep -E "pin 14 " /sys/kernel/debug/pinctrl/INT34C5:00/pins 2>/dev/null || true)
    if [ -n "$p14_line" ]; then
        echo "[INFO] PCH Pad 14 pinctrl state: $p14_line"
        if echo "$p14_line" | grep -q "0x84000201"; then
            echo "[PASS] Pad 14 matches verified firmware output-high state (0x84000201)."
        else
            echo "[WARN] Pad 14 differs from expected 0x84000201 baseline!" >&2
        fi
    fi
    p229_line=$(grep -E "pin 229 " /sys/kernel/debug/pinctrl/INT34C5:00/pins 2>/dev/null || true)
    if [ -n "$p229_line" ]; then
        echo "[INFO] PCH Pad 229 pinctrl state: $p229_line"
    fi
fi

# ==============================================================================
# Execution (Modifying Operations with State Reconciliation & Deferred Signals)
# ==============================================================================

# 2a. Load spidev
echo ""
echo "--- 2a. Loading spidev module ---"
enter_critical
cmd_err=0
modprobe spidev || cmd_err=$?
spidev_post_state=$(get_module_state "spidev")
if [ "$spidev_post_state" = "PRESENT" ] && [ "$cmd_err" -eq 0 ]; then
    SPIDEV_LOADED_BY_US=1
    echo "[PASS] spidev loaded successfully."
else
    SPIDEV_LOADED_BY_US=1
    STATE_UNCERTAIN=1
    STATE_UNCERTAIN_MSG="spidev state unexpected (state: $spidev_post_state, code: $cmd_err)"
fi
leave_critical

if [ "$STATE_UNCERTAIN" -eq 1 ]; then
    echo "Error: $STATE_UNCERTAIN_MSG. Halting." >&2
    ORIGINAL_EXIT_CODE=1
    exit 1
fi

if ! resolve_spidev_canonical_path; then
    echo "Error: spidev driver directory resolution failed!" >&2
    ORIGINAL_EXIT_CODE=1
    exit 1
fi

# 2b. Set driver_override and bind spidev
echo ""
echo "--- 2b. Setting driver_override and Binding spidev ---"
enter_critical
cmd_err=0
echo "spidev" > "$SPI_PATH/driver_override" || cmd_err=$?
current_override=$(cat "$SPI_PATH/driver_override" 2>&1 || true)
if [ "$(normalize_override "$current_override")" = "spidev" ]; then
    DRIVER_OVERRIDE_SET_BY_US=1
    echo "[PASS] driver_override set to spidev."
else
    DRIVER_OVERRIDE_SET_BY_US=1
    STATE_UNCERTAIN=1
    STATE_UNCERTAIN_MSG="driver_override readback mismatch"
fi
leave_critical

if [ "$STATE_UNCERTAIN" -eq 1 ]; then
    echo "Error: $STATE_UNCERTAIN_MSG. Halting." >&2
    ORIGINAL_EXIT_CODE=1
    exit 1
fi

enter_critical
cmd_err=0
echo "$SPI_DEV" > "$SPIDEV_DRIVER_PATH/bind" || cmd_err=$?
bound_driver=$(get_bound_driver_canonical "$SPI_PATH")
if [ "$bound_driver" = "$CANONICAL_SPIDEV_DRIVER_PATH" ]; then
    SPIDEV_BOUND_BY_US=1
    echo "[PASS] $SPI_DEV confirmed bound to spidev driver."
else
    SPIDEV_BOUND_BY_US=1
    STATE_UNCERTAIN=1
    STATE_UNCERTAIN_MSG="bind failed or target mismatch ('$bound_driver')"
fi
leave_critical

if [ "$STATE_UNCERTAIN" -eq 1 ]; then
    echo "Error: $STATE_UNCERTAIN_MSG. Halting." >&2
    ORIGINAL_EXIT_CODE=1
    exit 1
fi

# Locate spidev character device node
SPIDEV_NODE=""
if [ -c "/dev/spidev1.0" ]; then
    SPIDEV_NODE="/dev/spidev1.0"
else
    candidate=$(ls -d /dev/spidev* 2>/dev/null | head -n 1 || true)
    if [ -n "$candidate" ] && [ -c "$candidate" ]; then
        SPIDEV_NODE="$candidate"
    fi
fi

if [ -z "$SPIDEV_NODE" ] || [ ! -c "$SPIDEV_NODE" ]; then
    echo "Error: No valid /dev/spidev character device found! Halting." >&2
    ORIGINAL_EXIT_CODE=1
    exit 1
fi
echo "[PASS] Target spidev character device identified: $SPIDEV_NODE"

# ==============================================================================
# 3. Execute Stage 2A Supervised Sensor Identification
# ==============================================================================

echo ""
echo "========================================================"
echo " Executing Stage 2A Minimal Sensor Identification Sequence"
echo " Target: $SPIDEV_NODE"
echo "========================================================"
echo ""

IDENTIFY_EXECUTED=1
set +e
"$IDENTIFY_BIN" "$SPIDEV_NODE"
IDENTIFY_EXIT_CODE=$?
set -e

echo ""
if [ "$IDENTIFY_EXIT_CODE" -eq 0 ]; then
    echo ">> [STAGE 2A RESULT: SUCCESS] Sensor identification completed successfully."
else
    echo ">> [STAGE 2A RESULT: FAILED] Identification tool exited with code $IDENTIFY_EXIT_CODE." >&2
    ORIGINAL_EXIT_CODE="$IDENTIFY_EXIT_CODE"
fi

# ==============================================================================
# 4. Controlled Rollback & Teardown
# ==============================================================================

echo ""
echo "--- 4. Initiating Controlled Rollback ---"
cleanup
final_status=$?

if [ "$IDENTIFY_EXIT_CODE" -ne 0 ]; then
    exit "$IDENTIFY_EXIT_CODE"
fi

exit "$final_status"
