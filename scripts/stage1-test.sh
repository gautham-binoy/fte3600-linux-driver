#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Stage 1 Hardware Test & Rollback Script
# Non-invasive verification of spidev + fte3600.ko pairing.
#
# Strictly:
#   - NO SPI transfers
#   - NO reset pulsing
#   - NO register or OTP writes
#   - NO firmware downloading
#
# Concurrency & System Locking Limitation:
#   This script operates in userspace bash and utilizes sysfs and module interfaces.
#   It does NOT acquire an atomic system-wide kernel mutex across arbitrary processes.
#   While deferred signal handling and post-operation state reconciliation protect
#   internal consistency against asynchronous signals (SIGINT, SIGTERM), the host system
#   MUST NOT run concurrent udev rule triggers, automated driver binding daemons,
#   package managers, or external kernel module unload/load commands during test execution.
#   Exclusive administrative control of SPI and GPIO subsystems is required.
#
set -euo pipefail
export LC_ALL=C

# ==============================================================================
# Configuration and State Tracking
# ==============================================================================

PROJECT_DIR=$(cd "$(dirname "$0")/.." && pwd)
DIAG_DIR="$PROJECT_DIR/diagnostics"
STAGE1_LOG="$DIAG_DIR/stage1-results.txt"
MODULE_PATH="$PROJECT_DIR/kernel/fte3600.ko"
SPI_DEV="spi-FTE3600:00"
SPI_PATH="/sys/bus/spi/devices/$SPI_DEV"
SPIDEV_DRIVER_PATH="/sys/bus/spi/drivers/spidev"
CANONICAL_SPIDEV_DRIVER_PATH=""

# State tracking flags to ensure we ONLY revert what this script modified
UIO_PRE_EXISTING=0
UIO_LOADED_BY_US=0
SPIDEV_LOADED_BY_US=0
DRIVER_OVERRIDE_SET_BY_US=0
SPIDEV_BOUND_BY_US=0
FTE3600_LOADED_BY_US=0
FTE3600_PROBE_CONFIRMED=0

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
INTERRUPT_SIGNAL=0

# ==============================================================================
# State Inspection & Normalization Helpers
# ==============================================================================

# Three-state module inspection: PRESENT, ABSENT, or UNKNOWN
# Uses /sys/module and /proc/modules independently.
get_module_state() {
    local mod="$1"
    local sys_state="UNKNOWN"
    local proc_state="UNKNOWN"

    # Method 1: sysfs inspection
    if [ -d /sys/module ]; then
        if [ -d "/sys/module/$mod" ]; then
            sys_state="PRESENT"
        elif [ ! -e "/sys/module/$mod" ] && [ ! -L "/sys/module/$mod" ]; then
            sys_state="ABSENT"
        fi
    fi

    # Method 2: /proc/modules inspection with strict exit status evaluation
    if [ -r /proc/modules ]; then
        local grep_status=0
        grep -q "^${mod}[[:space:]]" /proc/modules 2>/dev/null || grep_status=$?
        if [ "$grep_status" -eq 0 ]; then
            proc_state="PRESENT"
        elif [ "$grep_status" -eq 1 ]; then
            proc_state="ABSENT"
        else
            proc_state="UNKNOWN"
        fi
    fi

    # Synthesize results conservatively:
    # - PRESENT if either reliable source confirms presence
    # - ABSENT only when at least one reliable source successfully confirms absence and no reliable source confirms presence
    # - UNKNOWN when neither source can establish the state
    if [ "$sys_state" = "PRESENT" ] || [ "$proc_state" = "PRESENT" ]; then
        echo "PRESENT"
    elif [ "$sys_state" = "ABSENT" ] || [ "$proc_state" = "ABSENT" ]; then
        echo "ABSENT"
    else
        echo "UNKNOWN"
    fi
}

# Normalize driver_override: trim whitespace and treat "" and "(null)" as unset
normalize_override() {
    local val="$1"
    val=$(printf '%s' "$val" | tr -d '\r\n')
    if [ -z "$val" ] || [ "$val" = "(null)" ]; then
        echo ""
    else
        echo "$val"
    fi
}

# Resolve driver binding state of an SPI device:
# Returns:
#   - Canonical path (e.g. /sys/bus/spi/drivers/spidev): valid driver symlink pointing to an existing directory
#   - "UNBOUND": SPI device directory exists, and no driver entry exists
#   - "UNRESOLVED": SPI device directory is unavailable, driver entry is not a symlink, or driver symlink cannot be resolved to an existing directory
get_bound_driver_canonical() {
    local dev_path="$1"

    # If the device directory does not exist or is not a directory, state is UNRESOLVED
    if [ ! -d "$dev_path" ]; then
        echo "UNRESOLVED"
        return 0
    fi

    # If no driver entry exists (neither regular file/dir nor dangling symlink), device is UNBOUND
    if [ ! -e "$dev_path/driver" ] && [ ! -L "$dev_path/driver" ]; then
        echo "UNBOUND"
        return 0
    fi

    # A driver entry exists: strictly require it to be a symlink (-L)
    if [ ! -L "$dev_path/driver" ]; then
        echo "UNRESOLVED"
        return 0
    fi

    # Driver symlink exists; resolve canonical target and verify it is a directory
    local target
    target=$(readlink -f "$dev_path/driver" 2>/dev/null || realpath "$dev_path/driver" 2>/dev/null || true)
    if [ -n "$target" ] && [ -d "$target" ]; then
        echo "$target"
    else
        echo "UNRESOLVED"
    fi
}

# Strictly validate and resolve canonical spidev driver path without fallback
resolve_spidev_canonical_path() {
    if [ ! -d "$SPIDEV_DRIVER_PATH" ]; then
        return 1
    fi
    local resolved
    resolved=$(readlink -f "$SPIDEV_DRIVER_PATH" 2>/dev/null || realpath "$SPIDEV_DRIVER_PATH" 2>/dev/null || true)
    if [ -n "$resolved" ] && [ -d "$resolved" ]; then
        CANONICAL_SPIDEV_DRIVER_PATH="$resolved"
        return 0
    fi
    return 1
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
    PENDING_SIGNAL=143
    echo "" >&2
    echo ">> [DEFERRED] SIGTERM received; deferred until state reconciliation completes." >&2
}

enter_critical() {
    if [ "$IN_CRITICAL_SECTION" -ne 0 ]; then
        echo "Internal Error: Nested critical section detected!" >&2
        cleanup
        exit 1
    fi
    # 1. Reset pending signal register
    PENDING_SIGNAL=0
    # 2. Install deferred handlers first so any incoming signal is recorded
    trap on_critical_int INT
    trap on_critical_term TERM
    # 3. Mark critical section as active
    IN_CRITICAL_SECTION=1
}

leave_critical() {
    if [ "$IN_CRITICAL_SECTION" -ne 1 ]; then
        echo "Internal Error: leave_critical called outside critical section!" >&2
        cleanup
        exit 1
    fi
    # 1. Capture pending signal recorded during the operation/reconciliation
    local sig="$PENDING_SIGNAL"
    PENDING_SIGNAL=0
    # 2. Mark critical section as inactive
    IN_CRITICAL_SECTION=0
    # 3. Restore normal immediate rollback handlers
    trap on_int INT
    trap on_term TERM

    # 4. If a signal arrived between step 1 and step 3, incorporate it
    if [ "$PENDING_SIGNAL" -ne 0 ]; then
        sig="$PENDING_SIGNAL"
        PENDING_SIGNAL=0
    fi

    # 5. Dispatch deferred signal immediately now that state is reconciled
    if [ "$sig" -ne 0 ]; then
        if [ "$sig" -eq 130 ]; then
            on_int
        else
            on_term
        fi
    fi
}

# Dedicated handlers for cleanup routine: avoids silent abandonment while
# preventing SIG_IGN from propagating to cleanup child processes.
on_cleanup_int() {
    CLEANUP_INTERRUPTED=130
    echo "" >&2
    echo ">> [NOTICE] SIGINT (Ctrl+C) received during rollback; continuing cleanup sequence to preserve device safety..." >&2
}

on_cleanup_term() {
    CLEANUP_INTERRUPTED=143
    echo "" >&2
    echo ">> [NOTICE] SIGTERM received during rollback; continuing cleanup sequence to preserve device safety..." >&2
}

# ==============================================================================
# Robust, Signal-Protected, Independent Cleanup Routine
# ==============================================================================

cleanup() {
    # Disarm EXIT trap to prevent recursion
    trap - EXIT

    # Guard against multiple executions
    if [ "$CLEANUP_EXECUTED" -eq 1 ]; then
        return "$CLEANUP_STATUS"
    fi
    CLEANUP_EXECUTED=1

    # Arm dedicated cleanup handlers so cleanup cannot be abandoned midway,
    # without using 'trap ""' which would pass SIG_IGN to child processes.
    trap on_cleanup_int INT
    trap on_cleanup_term TERM

    echo ""
    echo "========================================================"
    echo " CLEANUP / ROLLBACK INITIATED"
    echo " Timestamp: $(date -Iseconds)"
    echo "========================================================"

    # Temporarily allow errors so each rollback step runs independently
    set +e

    # Report uncertain state if flagged
    if [ "$STATE_UNCERTAIN" -eq 1 ]; then
        echo ">> [UNCERTAIN STATE DETECTED]: $STATE_UNCERTAIN_MSG" >&2
        echo "   Performing best-effort cleanup of all potentially armed resources." >&2
        echo "   Manual hardware and sysfs inspection is strongly recommended." >&2
    fi

    # 1. Unload fte3600 if it was loaded by this script
    local fte_post="UNKNOWN"
    local fte_confirmed_absent=0
    local spi_confirmed_unbound=0

    if [ "$FTE3600_LOADED_BY_US" -eq 1 ]; then
        echo ">> Rollback step 1: Unloading fte3600..."
        local rmmod_fte_ok=0
        if rmmod fte3600 2>&1; then
            rmmod_fte_ok=1
            echo "   [OK] rmmod fte3600 command succeeded."
        else
            echo "   [WARN] rmmod fte3600 command returned nonzero; checking actual postcondition..." >&2
        fi
        # Independent three-state postcondition verification
        fte_post=$(get_module_state "fte3600")
        case "$fte_post" in
            ABSENT)
                if [ "$rmmod_fte_ok" -eq 1 ]; then
                    echo "   [OK] Postcondition verified: fte3600 is confirmed ABSENT."
                else
                    echo "   [WARN] Postcondition verified: fte3600 is confirmed ABSENT despite rmmod command exit status."
                fi
                FTE3600_LOADED_BY_US=0
                fte_confirmed_absent=1
                ;;
            PRESENT)
                echo "   [FAIL] Postcondition failed: fte3600 is confirmed still PRESENT in kernel!" >&2
                CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
                fte_confirmed_absent=0
                ;;
            UNKNOWN|*)
                echo "   [FAIL] Postcondition failed: fte3600 state is UNKNOWN (inspection methods inconclusive/unavailable)!" >&2
                CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
                STATE_UNCERTAIN=1
                STATE_UNCERTAIN_MSG="Rollback failed to confirm fte3600 absence (state is UNKNOWN)"
                fte_confirmed_absent=0
                ;;
        esac
    else
        # fte3600 was not loaded by us; verify its baseline presence/absence
        fte_post=$(get_module_state "fte3600")
        if [ "$fte_post" = "ABSENT" ]; then
            fte_confirmed_absent=1
        else
            fte_confirmed_absent=0
        fi
    fi

    # 2. Unbind spidev from spi-FTE3600:00 if it was bound by this script
    if [ "$SPIDEV_BOUND_BY_US" -eq 1 ]; then
        echo ">> Rollback step 2: Unbinding spidev from $SPI_DEV..."
        if [ "$fte_confirmed_absent" -ne 1 ]; then
            echo "   [WARN] Skipping spidev unbind: dependent fte3600 module removal was not confirmed (state: $fte_post)!" >&2
            echo "   Retaining spidev binding ownership flag ($SPI_DEV remains bound)." >&2
            CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
            STATE_UNCERTAIN=1
            STATE_UNCERTAIN_MSG="Dependent rollback skipped: spidev unbind withheld because fte3600 removal was not confirmed"
        else
            local can_unbind=0
            local unbind_cmd_ok=0
            local initial_target=""

            # Step 2a: Inspect current SPI device binding before requiring canonical spidev path resolution
            if [ ! -d "$SPI_PATH" ]; then
                echo "   [WARN] Pre-unbind check: $SPI_PATH does not exist or is not a directory." >&2
            else
                initial_target=$(get_bound_driver_canonical "$SPI_PATH")

                case "$initial_target" in
                    UNBOUND)
                        echo "   [INFO] $SPI_DEV is already unbound; skipping unbind write."
                        # Device is already unbound; do not resolve spidev driver path or attempt unbind
                        ;;
                    UNRESOLVED)
                        echo "   [WARN] Refusing to unbind: $SPI_DEV driver link is UNRESOLVED or dangling!" >&2
                        ;;
                    *)
                        # Device is actually bound to a driver: only now require canonical spidev path validation
                        local expected_spidev="$CANONICAL_SPIDEV_DRIVER_PATH"
                        if [ -z "$expected_spidev" ]; then
                            if resolve_spidev_canonical_path; then
                                expected_spidev="$CANONICAL_SPIDEV_DRIVER_PATH"
                            fi
                        fi

                        if [ -z "$expected_spidev" ]; then
                            echo "   [WARN] Refusing to unbind: canonical spidev driver path could not be resolved!" >&2
                        elif [ "$initial_target" = "$expected_spidev" ]; then
                            can_unbind=1
                        else
                            echo "   [WARN] Refusing to unbind: $SPI_DEV is bound to alien driver '$initial_target', not spidev!" >&2
                        fi
                        ;;
                esac
            fi

            # Perform unbind write only when positively confirmed bound to spidev
            if [ "$can_unbind" -eq 1 ]; then
                if [ -w "$SPIDEV_DRIVER_PATH/unbind" ]; then
                    if echo "$SPI_DEV" > "$SPIDEV_DRIVER_PATH/unbind" 2>&1; then
                        unbind_cmd_ok=1
                        echo "   [OK] $SPI_DEV unbind write command succeeded."
                    else
                        echo "   [WARN] Writing $SPI_DEV to $SPIDEV_DRIVER_PATH/unbind returned nonzero; checking actual postcondition..." >&2
                    fi
                else
                    echo "   [WARN] $SPIDEV_DRIVER_PATH/unbind is not writable; checking actual postcondition..." >&2
                fi
            fi

            # Step 2b: Postcondition check as the final authority for unbind success
            # Avoid duplicate error accounting: evaluate postcondition state once here.
            if [ ! -d "$SPI_PATH" ]; then
                echo "   [FAIL] Postcondition failed: $SPI_PATH does not exist or is not a directory!" >&2
                echo "   [WARN] Unbind postcondition state is UNKNOWN; retaining rollback ownership flag." >&2
                CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
                STATE_UNCERTAIN=1
                STATE_UNCERTAIN_MSG="Unbind postcondition uncertain: $SPI_PATH missing or inaccessible"
                spi_confirmed_unbound=0
            elif [ -e "$SPI_PATH/driver" ] || [ -L "$SPI_PATH/driver" ]; then
                local remaining_drv
                remaining_drv=$(get_bound_driver_canonical "$SPI_PATH")
                if [ "$remaining_drv" = "UNRESOLVED" ]; then
                    echo "   [FAIL] Postcondition failed: $SPI_DEV driver link is UNRESOLVED or dangling!" >&2
                    STATE_UNCERTAIN=1
                    STATE_UNCERTAIN_MSG="$SPI_DEV driver link is dangling or unresolved during rollback"
                else
                    echo "   [FAIL] Postcondition failed: $SPI_DEV remains bound to driver '$remaining_drv'!" >&2
                    STATE_UNCERTAIN=1
                    STATE_UNCERTAIN_MSG="$SPI_DEV remains bound to '$remaining_drv'"
                fi
                CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
                spi_confirmed_unbound=0
            else
                # Positively confirmed unbound: device directory exists and neither regular nor symlink driver entry exists
                if [ "$unbind_cmd_ok" -eq 1 ] || [ "$initial_target" = "UNBOUND" ]; then
                    echo "   [OK] Postcondition verified: $SPI_DEV is confirmed unbound."
                else
                    echo "   [WARN] Postcondition verified: $SPI_DEV is confirmed unbound despite unbind command status."
                fi
                SPIDEV_BOUND_BY_US=0
                spi_confirmed_unbound=1
            fi
        fi
    else
        # spidev was not marked bound by us; verify baseline binding state
        local baseline_drv
        baseline_drv=$(get_bound_driver_canonical "$SPI_PATH")
        if [ "$baseline_drv" = "UNBOUND" ]; then
            spi_confirmed_unbound=1
        else
            spi_confirmed_unbound=0
        fi
    fi

    # 3. Restore driver_override if it was modified by this script
    if [ "$DRIVER_OVERRIDE_SET_BY_US" -eq 1 ]; then
        echo ">> Rollback step 3: Restoring driver_override on $SPI_DEV..."
        if [ "$spi_confirmed_unbound" -ne 1 ]; then
            echo "   [WARN] Skipping driver_override restoration: $SPI_DEV is not positively confirmed unbound!" >&2
            echo "   Retaining driver_override ownership flag ($SPI_DEV driver_override remains modified)." >&2
            CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
            STATE_UNCERTAIN=1
            STATE_UNCERTAIN_MSG="driver_override restoration skipped because $SPI_DEV binding state is not confirmed unbound; manual inspection required"
        else
            local norm_orig
            norm_orig=$(normalize_override "$ORIGINAL_DRIVER_OVERRIDE")
            local restore_write_ok=0
            if [ -w "$SPI_PATH/driver_override" ]; then
                if echo "$norm_orig" > "$SPI_PATH/driver_override" 2>&1; then
                    restore_write_ok=1
                    echo "   [OK] driver_override restore write succeeded."
                else
                    echo "   [WARN] Writing to $SPI_PATH/driver_override returned nonzero; checking actual postcondition..." >&2
                fi
            else
                echo "   [WARN] $SPI_PATH/driver_override is not writable; checking actual postcondition..." >&2
            fi

            # Postcondition check: read back must strictly succeed
            local current_ov=""
            local read_ok=0
            if [ -r "$SPI_PATH/driver_override" ]; then
                if current_ov=$(cat "$SPI_PATH/driver_override" 2>&1); then
                    read_ok=1
                fi
            fi

            if [ "$read_ok" -eq 1 ]; then
                local norm_curr
                norm_curr=$(normalize_override "$current_ov")
                if [ "$norm_curr" = "$norm_orig" ]; then
                    if [ "$restore_write_ok" -eq 1 ]; then
                        echo "   [OK] Postcondition verified: driver_override restored to original value ('$norm_orig')."
                    else
                        echo "   [WARN] Postcondition verified: driver_override matches original value ('$norm_orig') despite write command exit status."
                    fi
                    DRIVER_OVERRIDE_SET_BY_US=0
                else
                    echo "   [FAIL] Postcondition failed: driver_override is '$norm_curr', expected '$norm_orig'!" >&2
                    CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
                fi
            else
                echo "   [FAIL] driver_override restoration UNVERIFIED: sysfs read failed!" >&2
                CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
                STATE_UNCERTAIN=1
                STATE_UNCERTAIN_MSG="driver_override restoration unverified due to sysfs read failure"
            fi
        fi
    fi

    # 4. Unload spidev if it was loaded by this script
    if [ "$SPIDEV_LOADED_BY_US" -eq 1 ]; then
        echo ">> Rollback step 4: Unloading spidev module..."
        if [ "$fte_confirmed_absent" -ne 1 ] || [ "$spi_confirmed_unbound" -ne 1 ]; then
            local missing_deps=""
            if [ "$fte_confirmed_absent" -ne 1 ] && [ "$spi_confirmed_unbound" -ne 1 ]; then
                missing_deps="fte3600 removal not confirmed (state: $fte_post) and $SPI_DEV unbind not confirmed"
            elif [ "$fte_confirmed_absent" -ne 1 ]; then
                missing_deps="fte3600 removal not confirmed (state: $fte_post)"
            else
                missing_deps="$SPI_DEV unbinding not confirmed (device may still be bound or link state uncertain)"
            fi
            echo "   [WARN] Skipping spidev unload: $missing_deps!" >&2
            echo "   Retaining spidev module ownership flag (spidev remains loaded)." >&2
            echo "   Manual kernel module and device inspection is recommended." >&2
            CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
            STATE_UNCERTAIN=1
            STATE_UNCERTAIN_MSG="Dependent rollback skipped: spidev unload withheld because $missing_deps"
        else
            local rmmod_spidev_ok=0
            if modprobe -r spidev 2>&1; then
                rmmod_spidev_ok=1
                echo "   [OK] modprobe -r spidev command succeeded."
            else
                echo "   [WARN] modprobe -r spidev command returned nonzero; checking actual postcondition..." >&2
            fi
            # Independent three-state postcondition verification
            local spidev_post
            spidev_post=$(get_module_state "spidev")
            case "$spidev_post" in
                ABSENT)
                    if [ "$rmmod_spidev_ok" -eq 1 ]; then
                        echo "   [OK] Postcondition verified: spidev is confirmed ABSENT."
                    else
                        echo "   [WARN] Postcondition verified: spidev is confirmed ABSENT despite modprobe -r command exit status."
                    fi
                    SPIDEV_LOADED_BY_US=0
                    ;;
                PRESENT)
                    echo "   [FAIL] Postcondition failed: spidev is confirmed still PRESENT in kernel!" >&2
                    CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
                    ;;
                UNKNOWN|*)
                    echo "   [FAIL] Postcondition failed: spidev state is UNKNOWN (inspection methods inconclusive/unavailable)!" >&2
                    CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
                    STATE_UNCERTAIN=1
                    STATE_UNCERTAIN_MSG="Rollback failed to confirm spidev absence (state is UNKNOWN)"
                    ;;
            esac
        fi
    fi

    # 5. Unload uio if it was loaded by this script
    if [ "$UIO_LOADED_BY_US" -eq 1 ]; then
        echo ">> Rollback step 5: Unloading uio module..."
        if [ "$fte_confirmed_absent" -ne 1 ]; then
            echo "   [WARN] Skipping uio unload: dependent fte3600 module removal was not confirmed (state: $fte_post)!" >&2
            echo "   Retaining uio module ownership flag (uio remains loaded)." >&2
            CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
            STATE_UNCERTAIN=1
            STATE_UNCERTAIN_MSG="Dependent rollback skipped: uio unload withheld because fte3600 removal was not confirmed"
        else
            local rmmod_uio_ok=0
            if modprobe -r uio 2>&1; then
                rmmod_uio_ok=1
                echo "   [OK] modprobe -r uio command succeeded."
            else
                echo "   [WARN] modprobe -r uio command returned nonzero; checking actual postcondition..." >&2
            fi
            # Independent three-state postcondition verification
            local uio_post
            uio_post=$(get_module_state "uio")
            case "$uio_post" in
                ABSENT)
                    if [ "$rmmod_uio_ok" -eq 1 ]; then
                        echo "   [OK] Postcondition verified: uio is confirmed ABSENT."
                    else
                        echo "   [WARN] Postcondition verified: uio is confirmed ABSENT despite modprobe -r command exit status."
                    fi
                    UIO_LOADED_BY_US=0
                    ;;
                PRESENT)
                    echo "   [FAIL] Postcondition failed: uio is confirmed still PRESENT in kernel!" >&2
                    echo "   (Module may still be referenced or held busy by kernel subsystem)." >&2
                    CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
                    ;;
                UNKNOWN|*)
                    echo "   [FAIL] Postcondition failed: uio state is UNKNOWN (inspection methods inconclusive/unavailable)!" >&2
                    CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
                    STATE_UNCERTAIN=1
                    STATE_UNCERTAIN_MSG="Rollback failed to confirm uio absence (state is UNKNOWN)"
                    ;;
            esac
        fi
    fi

    echo ""
    echo "--- Post-Rollback State Verification ---"
    echo ">> Checking loaded module states post-rollback:"
    local final_fte_state
    local final_spidev_state
    local final_uio_state
    final_fte_state=$(get_module_state "fte3600")
    final_spidev_state=$(get_module_state "spidev")
    final_uio_state=$(get_module_state "uio")

    case "$final_fte_state" in
        ABSENT)
            echo "   fte3600: ABSENT (unloaded)"
            ;;
        PRESENT)
            echo "   [FAIL] fte3600: PRESENT (still loaded in kernel!)" >&2
            ;;
        UNKNOWN|*)
            echo "   [FAIL] fte3600: UNKNOWN (state could not be established)" >&2
            ;;
    esac

    case "$final_spidev_state" in
        ABSENT)
            echo "   spidev : ABSENT (unloaded)"
            ;;
        PRESENT)
            echo "   [FAIL] spidev : PRESENT (still loaded in kernel!)" >&2
            ;;
        UNKNOWN|*)
            echo "   [FAIL] spidev : UNKNOWN (state could not be established)" >&2
            ;;
    esac

    if [ "$UIO_PRE_EXISTING" -eq 1 ]; then
        case "$final_uio_state" in
            PRESENT)
                echo "   uio    : PRESENT (pre-existing module preserved)"
                ;;
            ABSENT)
                echo "   [FAIL] uio    : ABSENT (pre-existing module was unloaded externally!)" >&2
                CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
                STATE_UNCERTAIN=1
                STATE_UNCERTAIN_MSG="Pre-existing uio module is missing post-rollback"
                ;;
            UNKNOWN|*)
                echo "   [FAIL] uio    : UNKNOWN (state could not be established)" >&2
                CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
                STATE_UNCERTAIN=1
                STATE_UNCERTAIN_MSG="Post-rollback uio state is UNKNOWN"
                ;;
        esac
    else
        case "$final_uio_state" in
            ABSENT)
                echo "   uio    : ABSENT (unloaded)"
                ;;
            PRESENT)
                echo "   [FAIL] uio    : PRESENT (still loaded in kernel!)" >&2
                ;;
            UNKNOWN|*)
                echo "   [FAIL] uio    : UNKNOWN (state could not be established)" >&2
                ;;
        esac
    fi

    local modules_all_absent=0
    local uio_clean=0
    if [ "$UIO_PRE_EXISTING" -eq 1 ]; then
        if [ "$final_uio_state" = "PRESENT" ]; then
            uio_clean=1
        fi
    else
        if [ "$final_uio_state" = "ABSENT" ]; then
            uio_clean=1
        fi
    fi

    if [ "$final_fte_state" = "ABSENT" ] && [ "$final_spidev_state" = "ABSENT" ] && [ "$uio_clean" -eq 1 ]; then
        if [ "$UIO_PRE_EXISTING" -eq 1 ]; then
            echo "   [OK] fte3600 and spidev are confirmed ABSENT; pre-existing uio preserved."
        else
            echo "   [OK] fte3600, spidev, and uio are confirmed ABSENT."
        fi
        modules_all_absent=1
    fi

    echo ">> Checking SPI device driver link:"
    local spi_audit_unbound=0
    if [ ! -d "$SPI_PATH" ]; then
        echo "   [FAIL] $SPI_PATH does not exist or is not a directory (state: UNRESOLVED)!" >&2
    else
        local diag_drv
        diag_drv=$(get_bound_driver_canonical "$SPI_PATH")
        case "$diag_drv" in
            UNBOUND)
                echo "   [OK] $SPI_DEV is confirmed UNBOUND (no driver link present)."
                spi_audit_unbound=1
                ;;
            UNRESOLVED)
                echo "   [FAIL] $SPI_DEV driver state is UNRESOLVED (dangling or invalid link)!" >&2
                ;;
            *)
                echo "   [FAIL] $SPI_DEV driver link is still present: $diag_drv" >&2
                ;;
        esac
    fi

    # Final audit consistency: Ensure audit failures prevent claiming complete rollback,
    # while avoiding double-counting if a failure was already counted in CLEANUP_ERRORS.
    if [ "$modules_all_absent" -ne 1 ] || [ "$spi_audit_unbound" -ne 1 ]; then
        if [ "$CLEANUP_ERRORS" -eq 0 ]; then
            CLEANUP_ERRORS=1
            STATE_UNCERTAIN=1
            STATE_UNCERTAIN_MSG="Post-rollback audit failed: one or more resources are not confirmed absent/unbound"
        fi
    fi

    echo ">> Checking /dev device nodes:"
    ls -l /dev/spidev* /dev/uio* 2>&1 || true

    echo ">> Checking PCH pinctrl pins 14 and 229 post-rollback:"
    local observed_p14=""
    local observed_p229=""
    if [ -r /sys/kernel/debug/pinctrl/INT34C5:00/pins ]; then
        observed_p14=$(grep -E "pin 14 " /sys/kernel/debug/pinctrl/INT34C5:00/pins 2>/dev/null || true)
        observed_p229=$(grep -E "pin 229 " /sys/kernel/debug/pinctrl/INT34C5:00/pins 2>/dev/null || true)
        echo "   Pinctrl pin 14 : ${observed_p14:-not found}"
        echo "   Pinctrl pin 229: ${observed_p229:-not found}"
    else
        echo "   [INFO] debugfs pinctrl not readable."
    fi

    echo ">> Checking debugfs GPIO post-rollback:"
    if [ -r /sys/kernel/debug/gpio ]; then
        grep -E "fte3600|gpio-14|gpio-323|gpio-229" /sys/kernel/debug/gpio 2>&1 || echo "   [OK] No lines claimed by fte3600 in debugfs gpio."
    else
        echo "   [INFO] debugfs gpio not readable."
    fi

    echo ""
    echo ">> Hardware Pad State & GPIO Configuration Advisory:"
    echo "   1. Software GPIO Ownership: Released on module exit (marked unclaimed in gpiolib)."
    echo "   2. Driver Configuration: $([ "$FTE3600_PROBE_CONFIRMED" -eq 1 ] && echo "fte3600 reported ready and configured reset GPIO" || echo "fte3600 readiness was not confirmed")"
    if [ -n "$observed_p14" ]; then
        echo "   3. Observed Post-Rollback Pinctrl Configuration: $observed_p14"
        echo "      Notice: Releasing a GPIO descriptor in Linux gpiolib releases the kernel's software"
        echo "      claim, but does not necessarily reprogram Intel PCH pad configuration registers (PADCFG0)"
        echo "      back to firmware defaults. The register values above reflect PCH controller configuration"
        echo "      only; physical electrical voltage and line levels are not established by this diagnostic."
    else
        echo "   3. Observed Post-Rollback Pinctrl Configuration: debugfs pinctrl was not readable."
        echo "      PCH pad configuration could not be inspected. Electrical voltage and physical line levels"
        echo "      are not established by this diagnostic."
    fi

    echo ""
    if [ "$CLEANUP_ERRORS" -gt 0 ] || [ "$STATE_UNCERTAIN" -eq 1 ]; then
        echo "========================================================"
        echo " ROLLBACK COMPLETED WITH $CLEANUP_ERRORS FAILURE/UNCERTAIN STATE(S)"
        echo " Manual inspection is strongly recommended."
        echo "========================================================"
    else
        echo "========================================================"
        echo " ROLLBACK COMPLETED CLEANLY (0 ERRORS)"
        if [ "$UIO_PRE_EXISTING" -eq 1 ]; then
            echo " Verified: fte3600 ABSENT, spidev ABSENT, $SPI_DEV UNBOUND (uio pre-existing)"
        else
            echo " Verified: fte3600 ABSENT, spidev ABSENT, uio ABSENT, $SPI_DEV UNBOUND"
        fi
        echo "========================================================"
    fi

    # If interrupted during cleanup, record that as the interrupt signal
    if [ "$CLEANUP_INTERRUPTED" -ne 0 ] && [ "$INTERRUPT_SIGNAL" -eq 0 ]; then
        INTERRUPT_SIGNAL="$CLEANUP_INTERRUPTED"
    fi

    # Determine status: preserve signal, preserve original error, or indicate cleanup failure
    if [ "$INTERRUPT_SIGNAL" -ne 0 ]; then
        CLEANUP_STATUS="$INTERRUPT_SIGNAL"
    elif [ "$ORIGINAL_EXIT_CODE" -ne 0 ]; then
        CLEANUP_STATUS="$ORIGINAL_EXIT_CODE"
    elif [ "$CLEANUP_ERRORS" -ne 0 ] || [ "$STATE_UNCERTAIN" -ne 0 ]; then
        CLEANUP_STATUS=1
    else
        CLEANUP_STATUS=0
    fi

    return "$CLEANUP_STATUS"
}

# ==============================================================================
# Signal Handlers (INT=130, TERM=143) and EXIT Trap
# ==============================================================================

on_int() {
    trap on_cleanup_int INT
    trap on_cleanup_term TERM
    trap - EXIT
    INTERRUPT_SIGNAL=130
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
    INTERRUPT_SIGNAL=143
    ORIGINAL_EXIT_CODE=143
    echo "" >&2
    echo ">> Caught SIGTERM. Initiating emergency rollback..." >&2
    cleanup
    exit 143
}

on_exit() {
    local exit_code=$?
    trap on_cleanup_int INT
    trap on_cleanup_term TERM
    trap - EXIT

    if [ "$IN_CRITICAL_SECTION" -eq 1 ]; then
        echo ">> [WARNING] Process exiting while inside critical section!" >&2
    fi

    if [ "$ORIGINAL_EXIT_CODE" -eq 0 ] && [ "$exit_code" -ne 0 ]; then
        ORIGINAL_EXIT_CODE="$exit_code"
    fi

    cleanup
    local cleanup_res=$?

    if [ "$INTERRUPT_SIGNAL" -ne 0 ]; then
        exit "$INTERRUPT_SIGNAL"
    elif [ "$cleanup_res" -ne 0 ]; then
        exit "$cleanup_res"
    elif [ "$ORIGINAL_EXIT_CODE" -ne 0 ]; then
        exit "$ORIGINAL_EXIT_CODE"
    fi
    exit 0
}

# Arm signals and exit trap
trap on_int INT
trap on_term TERM
trap on_exit EXIT

# ==============================================================================
# Pre-Execution Environment & Sanity Checks
# ==============================================================================

if [ "$(id -u)" -ne 0 ]; then
    echo "Error: This script must be run as root." >&2
    ORIGINAL_EXIT_CODE=1
    exit 1
fi

mkdir -p "$DIAG_DIR"
exec > >(tee "$STAGE1_LOG") 2>&1

echo "========================================================"
echo " FTE3600 STAGE 1 VERIFICATION & ROLLBACK"
echo " Timestamp: $(date -Iseconds)"
echo " Kernel: $(uname -r)"
echo " Device: $SPI_DEV"
echo "========================================================"

echo ""
echo "--- 1. Baseline Pre-Execution Safety Checks ---"
echo ">> Concurrency Advisory: This script does not take an atomic system-wide kernel lock."
echo "   Ensure exclusive execution: no concurrent udev rules, modprobe actions, or"
echo "   external processes should alter SPI or GPIO resources while this script runs."

# Check 1: Target module existence
if [ ! -f "$MODULE_PATH" ]; then
    echo "Error: $MODULE_PATH not found. Build kernel/fte3600.ko first." >&2
    ORIGINAL_EXIT_CODE=1
    exit 1
fi

# Check 2: SPI device presence
if [ ! -d "$SPI_PATH" ]; then
    echo "Error: SPI slave device $SPI_PATH does not exist in sysfs!" >&2
    ORIGINAL_EXIT_CODE=1
    exit 1
fi

# Check 2b: Pre-load Stage 1 validation of SPI drivers subsystem
# Timing & Lifecycle Advisory:
#   In the Linux kernel sysfs architecture, /sys/bus/spi/drivers/spidev does NOT exist
#   prior to loading the spidev module. Therefore, Stage 1 pre-load validation verifies
#   that the parent SPI drivers subsystem (/sys/bus/spi/drivers) exists and is accessible.
#   Full canonical path resolution for the spidev driver directory is strictly performed
#   in Stage 2 validation immediately after loading spidev and strictly before performing
#   any modifying sysfs operations (driver_override write, bind write) or loading fte3600.
if [ ! -d "/sys/bus/spi/drivers" ]; then
    echo "Error: SPI drivers subsystem /sys/bus/spi/drivers does not exist!" >&2
    ORIGINAL_EXIT_CODE=1
    exit 1
fi
echo "[PASS] Stage 1 pre-load validation: SPI drivers subsystem (/sys/bus/spi/drivers) is accessible."

# Check 3: Strictly verify and read driver_override
if [ ! -r "$SPI_PATH/driver_override" ]; then
    echo "Error: $SPI_PATH/driver_override does not exist or is not readable!" >&2
    ORIGINAL_EXIT_CODE=1
    exit 1
fi

if ! ORIGINAL_DRIVER_OVERRIDE=$(cat "$SPI_PATH/driver_override"); then
    echo "Error: Failed to read $SPI_PATH/driver_override!" >&2
    ORIGINAL_EXIT_CODE=1
    exit 1
fi

# Trim any trailing whitespace/newlines
ORIGINAL_DRIVER_OVERRIDE=$(printf '%s' "$ORIGINAL_DRIVER_OVERRIDE")

# Abort if driver_override is non-empty (sysfs reports empty or "(null)" when unconfigured)
if [ -n "$ORIGINAL_DRIVER_OVERRIDE" ] && [ "$ORIGINAL_DRIVER_OVERRIDE" != "(null)" ]; then
    echo "Error: $SPI_DEV has an active pre-existing driver_override: '$ORIGINAL_DRIVER_OVERRIDE'" >&2
    echo "Aborting to prevent clobbering external driver configuration." >&2
    ORIGINAL_EXIT_CODE=1
    exit 1
fi
echo "[PASS] driver_override on $SPI_DEV is readable and clean ('$ORIGINAL_DRIVER_OVERRIDE')."

# Check 4: Ensure SPI device is not already claimed and driver link is not dangling
initial_drv=$(get_bound_driver_canonical "$SPI_PATH")
case "$initial_drv" in
    UNBOUND)
        echo "[PASS] $SPI_DEV is unbound (no active or dangling driver link)."
        ;;
    UNRESOLVED)
        echo "Error: $SPI_DEV driver state is UNRESOLVED (device directory missing, non-symlink entry, or dangling driver symlink)!" >&2
        echo "Aborting to prevent interference with existing driver bindings." >&2
        ORIGINAL_EXIT_CODE=1
        exit 1
        ;;
    *)
        echo "Error: $SPI_DEV is already bound to driver: $initial_drv" >&2
        echo "Aborting to prevent interference with existing driver bindings." >&2
        ORIGINAL_EXIT_CODE=1
        exit 1
        ;;
esac

# Check 5: Strictly verify spidev is not already loaded using three-state inspection
spidev_pre_state=$(get_module_state "spidev")
case "$spidev_pre_state" in
    ABSENT)
        echo "[PASS] spidev is confirmed not loaded."
        ;;
    PRESENT)
        echo "Error: spidev is already loaded in the kernel." >&2
        echo "Aborting to avoid altering external module configuration." >&2
        ORIGINAL_EXIT_CODE=1
        exit 1
        ;;
    UNKNOWN|*)
        echo "Error: Unable to establish whether spidev is loaded (inspection inconclusive)." >&2
        echo "Aborting before making any modifications." >&2
        ORIGINAL_EXIT_CODE=1
        exit 1
        ;;
esac

# Check 6: Strictly verify fte3600 is not already loaded using three-state inspection
fte_pre_state=$(get_module_state "fte3600")
case "$fte_pre_state" in
    ABSENT)
        echo "[PASS] fte3600 is confirmed not loaded."
        ;;
    PRESENT)
        echo "Error: fte3600 is already loaded in the kernel." >&2
        echo "Aborting to avoid collision with existing instance." >&2
        ORIGINAL_EXIT_CODE=1
        exit 1
        ;;
    UNKNOWN|*)
        echo "Error: Unable to establish whether fte3600 is loaded (inspection inconclusive)." >&2
        echo "Aborting before making any modifications." >&2
        ORIGINAL_EXIT_CODE=1
        exit 1
        ;;
esac

# Check 7: Inspect baseline state of prerequisite uio module using three-state inspection
uio_pre_state=$(get_module_state "uio")
case "$uio_pre_state" in
    ABSENT)
        echo "[PASS] uio is confirmed not loaded; will be loaded on demand and tracked for rollback."
        UIO_PRE_EXISTING=0
        ;;
    PRESENT)
        echo "[INFO] uio is already loaded in the kernel; will be preserved and not unloaded on rollback."
        UIO_PRE_EXISTING=1
        ;;
    UNKNOWN|*)
        echo "Error: Unable to establish whether uio is loaded (inspection inconclusive)." >&2
        echo "Aborting before making any modifications." >&2
        ORIGINAL_EXIT_CODE=1
        exit 1
        ;;
esac

# Record dmesg position to isolate messages generated during this run
DMESG_START_LINE=$(dmesg | wc -l)

# ==============================================================================
# Execution (Modifying Operations with State Reconciliation & Deferred Signals)
# ==============================================================================

if [ "$UIO_PRE_EXISTING" -eq 0 ]; then
    echo ""
    echo "--- 2a. Loading prerequisite uio module ---"
    enter_critical
    cmd_err=0
    modprobe --first-time uio || cmd_err=$?

    # Reconcile actual kernel module state using three-state inspection
    uio_post_state=$(get_module_state "uio")
    case "$uio_post_state" in
        PRESENT)
            if [ "$cmd_err" -eq 0 ]; then
                UIO_LOADED_BY_US=1
                echo "[PASS] uio loaded successfully."
            else
                # Case C: Baseline ABSENT, modprobe returned error, but module is PRESENT.
                # Ownership is ambiguous; refusing to assume ownership or unload.
                UIO_LOADED_BY_US=0
                STATE_UNCERTAIN=1
                STATE_UNCERTAIN_MSG="modprobe --first-time uio returned error (code $cmd_err) but uio is PRESENT; ownership is ambiguous, refusing to assume ownership or unload"
                CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
                echo ">> [AMBIGUOUS OWNERSHIP]: $STATE_UNCERTAIN_MSG" >&2
            fi
            ;;
        ABSENT)
            UIO_LOADED_BY_US=0
            if [ "$cmd_err" -eq 0 ]; then
                STATE_UNCERTAIN=1
                STATE_UNCERTAIN_MSG="modprobe --first-time uio returned 0 but uio is confirmed ABSENT"
                CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
                echo ">> [UNEXPECTED STATE]: $STATE_UNCERTAIN_MSG" >&2
            fi
            ;;
        UNKNOWN|*)
            # Case D: Baseline ABSENT, module state after load is UNKNOWN (inconclusive).
            # UNKNOWN state must never authorize destructive operations; refusing ownership.
            UIO_LOADED_BY_US=0
            STATE_UNCERTAIN=1
            STATE_UNCERTAIN_MSG="Unable to establish whether uio is loaded (inspection inconclusive); refusing to assume ownership or unload"
            CLEANUP_ERRORS=$((CLEANUP_ERRORS + 1))
            echo ">> [INCONCLUSIVE STATE]: $STATE_UNCERTAIN_MSG" >&2
            ;;
    esac
    leave_critical

    if [ "$STATE_UNCERTAIN" -eq 1 ]; then
        echo "Error: $STATE_UNCERTAIN_MSG. Halting execution; manual inspection may be required." >&2
        ORIGINAL_EXIT_CODE=1
        exit 1
    elif [ "$UIO_LOADED_BY_US" -ne 1 ]; then
        echo "Error: Failed to load uio module (exit code: $cmd_err)." >&2
        if [ "$cmd_err" -ne 0 ]; then
            ORIGINAL_EXIT_CODE="$cmd_err"
            exit "$cmd_err"
        else
            ORIGINAL_EXIT_CODE=1
            exit 1
        fi
    fi
else
    echo ""
    echo "--- 2a. Prerequisite uio module is already loaded (preserved) ---"
fi

echo ""
echo "--- 2b. Loading spidev (Default 4096-byte bufsiz) ---"
enter_critical
cmd_err=0
modprobe spidev || cmd_err=$?

# Reconcile actual kernel module state using three-state inspection
spidev_post_state=$(get_module_state "spidev")
case "$spidev_post_state" in
    PRESENT)
        SPIDEV_LOADED_BY_US=1
        if [ "$cmd_err" -ne 0 ]; then
            echo ">> Warning: modprobe exited with code $cmd_err, but spidev is confirmed loaded in kernel. Rollback flag armed." >&2
        fi
        ;;
    ABSENT)
        SPIDEV_LOADED_BY_US=0
        ;;
    UNKNOWN|*)
        STATE_UNCERTAIN=1
        STATE_UNCERTAIN_MSG="Unable to establish whether spidev is loaded (inspection inconclusive)"
        SPIDEV_LOADED_BY_US=1
        ;;
esac
leave_critical

if [ "$STATE_UNCERTAIN" -eq 1 ]; then
    echo "Error: $STATE_UNCERTAIN_MSG. Halting execution; manual inspection may be required." >&2
    ORIGINAL_EXIT_CODE=1
    exit 1
fi

if [ "$cmd_err" -ne 0 ]; then
    echo "Error: Failed to load spidev module (code $cmd_err)!" >&2
    ORIGINAL_EXIT_CODE="$cmd_err"
    exit "$cmd_err"
fi

if [ "$SPIDEV_LOADED_BY_US" -ne 1 ]; then
    echo "Error: modprobe reported success, but spidev was not found in kernel!" >&2
    ORIGINAL_EXIT_CODE=1
    exit 1
fi
echo "[PASS] spidev loaded successfully."

# Stage 2 validation: Strictly verify canonical spidev driver path now that module is loaded
# Timing limitation: This check could not run prior to modprobe because sysfs creates
# /sys/bus/spi/drivers/spidev dynamically upon module registration. We strictly require this
# validation to succeed before performing any sysfs modifications (driver_override, bind) or
# loading dependent kernel modules (fte3600.ko).
if ! resolve_spidev_canonical_path; then
    echo "Error: spidev is loaded, but $SPIDEV_DRIVER_PATH does not exist or fails canonical path resolution!" >&2
    ORIGINAL_EXIT_CODE=1
    exit 1
fi
echo "[PASS] Stage 2 post-load validation: spidev driver directory verified: $CANONICAL_SPIDEV_DRIVER_PATH"

echo ""
echo "--- 3. Setting driver_override and Binding spidev ---"
enter_critical
cmd_err=0
echo "spidev" > "$SPI_PATH/driver_override" || cmd_err=$?

# Reconcile actual driver_override state regardless of command exit status
current_override=""
read_ok=0
if [ -r "$SPI_PATH/driver_override" ]; then
    if current_override=$(cat "$SPI_PATH/driver_override" 2>&1); then
        read_ok=1
    fi
fi

norm_curr=""
norm_orig=$(normalize_override "$ORIGINAL_DRIVER_OVERRIDE")

if [ "$read_ok" -eq 1 ]; then
    norm_curr=$(normalize_override "$current_override")
    if [ "$norm_curr" = "spidev" ]; then
        DRIVER_OVERRIDE_SET_BY_US=1
        if [ "$cmd_err" -ne 0 ]; then
            echo ">> Warning: echo write exited with code $cmd_err, but driver_override contains 'spidev'. Rollback flag armed." >&2
        fi
    elif [ "$norm_curr" != "$norm_orig" ]; then
        DRIVER_OVERRIDE_SET_BY_US=1
        echo ">> Warning: driver_override changed to unexpected value: '$norm_curr'. Rollback flag armed." >&2
    fi
else
    STATE_UNCERTAIN=1
    STATE_UNCERTAIN_MSG="Unable to read back $SPI_PATH/driver_override after write attempt; override state uncertain"
    DRIVER_OVERRIDE_SET_BY_US=1
fi
leave_critical

if [ "$STATE_UNCERTAIN" -eq 1 ]; then
    echo "Error: $STATE_UNCERTAIN_MSG. Halting execution; manual inspection may be required." >&2
    ORIGINAL_EXIT_CODE=1
    exit 1
fi

if [ "$cmd_err" -ne 0 ]; then
    echo "Error: Failed to write 'spidev' to $SPI_PATH/driver_override (code $cmd_err)!" >&2
    ORIGINAL_EXIT_CODE="$cmd_err"
    exit "$cmd_err"
fi

if [ "$norm_curr" != "spidev" ]; then
    echo "Error: driver_override write completed, but readback was '$norm_curr' instead of 'spidev'!" >&2
    ORIGINAL_EXIT_CODE=1
    exit 1
fi
echo "[PASS] driver_override set to spidev."

enter_critical
cmd_err=0
echo "$SPI_DEV" > "$SPIDEV_DRIVER_PATH/bind" || cmd_err=$?

# Reconcile actual driver binding state: strictly resolve symlink target
bound_driver_canonical=$(get_bound_driver_canonical "$SPI_PATH")

case "$bound_driver_canonical" in
    "$CANONICAL_SPIDEV_DRIVER_PATH")
        # Confirmed bound to spidev
        SPIDEV_BOUND_BY_US=1
        if [ "$cmd_err" -ne 0 ]; then
            echo ">> Warning: bind write exited with code $cmd_err, but $SPI_DEV is confirmed bound to spidev. Rollback flag armed." >&2
        fi
        ;;
    UNBOUND)
        # Device directory exists and is definitely unbound
        SPIDEV_BOUND_BY_US=0
        ;;
    UNRESOLVED)
        # Device directory missing, non-symlink entry, or dangling symlink.
        # Bind operation may have succeeded even though inspection failed.
        # Arm flag so cleanup performs re-inspection, set uncertain state, and halt.
        SPIDEV_BOUND_BY_US=1
        STATE_UNCERTAIN=1
        STATE_UNCERTAIN_MSG="Unable to determine driver binding state: $SPI_DEV is UNRESOLVED or inaccessible"
        ;;
    *)
        # Bound to another driver
        SPIDEV_BOUND_BY_US=0
        STATE_UNCERTAIN=1
        STATE_UNCERTAIN_MSG="$SPI_DEV driver link exists but target is unexpected ('$bound_driver_canonical') instead of spidev"
        ;;
esac
leave_critical

if [ "$STATE_UNCERTAIN" -eq 1 ]; then
    echo "Error: $STATE_UNCERTAIN_MSG. Halting execution; manual inspection may be required." >&2
    ORIGINAL_EXIT_CODE=1
    exit 1
fi

if [ "$cmd_err" -ne 0 ]; then
    echo "Error: Failed to bind $SPI_DEV to spidev (code $cmd_err)!" >&2
    ORIGINAL_EXIT_CODE="$cmd_err"
    exit "$cmd_err"
fi

if [ "$SPIDEV_BOUND_BY_US" -ne 1 ]; then
    echo "Error: bind write completed, but $SPI_DEV was not confirmed bound to spidev!" >&2
    ORIGINAL_EXIT_CODE=1
    exit 1
fi
echo "[PASS] $SPI_DEV confirmed bound to spidev driver ($bound_driver_canonical)."

echo ""
echo "--- 4. Loading fte3600.ko ---"
enter_critical
cmd_err=0
insmod "$MODULE_PATH" || cmd_err=$?

# Reconcile actual fte3600 module state using three-state inspection
fte_post_state=$(get_module_state "fte3600")
case "$fte_post_state" in
    PRESENT)
        FTE3600_LOADED_BY_US=1
        if [ "$cmd_err" -ne 0 ]; then
            echo ">> Warning: insmod exited with code $cmd_err, but fte3600 is confirmed loaded in kernel. Rollback flag armed." >&2
        fi
        ;;
    ABSENT)
        FTE3600_LOADED_BY_US=0
        ;;
    UNKNOWN|*)
        STATE_UNCERTAIN=1
        STATE_UNCERTAIN_MSG="Unable to establish whether fte3600 is loaded (inspection inconclusive)"
        FTE3600_LOADED_BY_US=1
        ;;
esac
leave_critical

if [ "$STATE_UNCERTAIN" -eq 1 ]; then
    echo "Error: $STATE_UNCERTAIN_MSG. Halting execution; manual inspection may be required." >&2
    ORIGINAL_EXIT_CODE=1
    exit 1
fi

if [ "$cmd_err" -ne 0 ]; then
    echo "Error: Failed to insert $MODULE_PATH (code $cmd_err)!" >&2
    ORIGINAL_EXIT_CODE="$cmd_err"
    exit "$cmd_err"
fi

if [ "$FTE3600_LOADED_BY_US" -ne 1 ]; then
    echo "Error: insmod reported success, but fte3600 was not found in kernel!" >&2
    ORIGINAL_EXIT_CODE=1
    exit 1
fi
echo "[PASS] fte3600.ko inserted successfully."

# ==============================================================================
# Diagnostics and State Inspection (Read-Only)
# ==============================================================================

echo ""
echo "--- 5. Diagnostics and State Inspection ---"

echo ">> Checking /dev character devices:"
ls -l /dev/spidev* /dev/uio* /dev/gpiochip* 2>&1 || true

echo ""
echo ">> Checking sysfs platform device attributes:"
GLUE_DEV=$(find "$SPI_PATH/" -maxdepth 2 -name "fte3600-glue.*" -print | head -n 1 || true)
if [ -n "$GLUE_DEV" ]; then
    echo "Glue platform device detected: $GLUE_DEV"
    for attr in fte3600_glue_abi fte3600_status fte3600_ngpio fte3600_generation \
                fte3600_acpi_mode fte3600_acpi_speed_hz fte3600_cs_control \
                fte3600_irq_active_low fte3600_irq_source; do
        if [ -f "$GLUE_DEV/$attr" ]; then
            printf '  %-25s: %s\n' "$attr" "$(cat "$GLUE_DEV/$attr")"
        fi
    done
    if [ -f "$GLUE_DEV/fte3600_status" ] && [ "$(cat "$GLUE_DEV/fte3600_status")" = "ready" ]; then
        FTE3600_PROBE_CONFIRMED=1
    fi
else
    echo "Warning: fte3600-glue platform device not found under $SPI_DEV!"
fi

echo ""
echo ">> Checking UIO device enumeration:"
for u in /sys/class/uio/*; do
    [ -d "$u" ] || continue
    name=$(cat "$u/name" 2>/dev/null || echo "unknown")
    ver=$(cat "$u/version" 2>/dev/null || echo "unknown")
    echo "  $u -> name: $name, version: $ver"
done

echo ""
echo ">> Checking debugfs GPIO state:"
if [ -r /sys/kernel/debug/gpio ]; then
    grep -A 5 -B 1 "fte3600" /sys/kernel/debug/gpio 2>&1 || true
    grep -E "gpio-14|gpio-323|gpio-229|INT34C5" /sys/kernel/debug/gpio 2>&1 || true
else
    echo "debugfs gpio not readable."
fi

echo ""
echo ">> Checking PCH pinctrl pins 14 and 229:"
if [ -r /sys/kernel/debug/pinctrl/INT34C5:00/pins ]; then
    grep -E "pin 14 |pin 229 " /sys/kernel/debug/pinctrl/INT34C5:00/pins || true
else
    echo "debugfs pinctrl not readable."
fi

echo ""
echo ">> Kernel log entries generated during Stage 1:"
dmesg | tail -n +"$((DMESG_START_LINE + 1))"

# ==============================================================================
# Controlled Rollback & Post-Rollback State Audit
# ==============================================================================

echo ""
echo "--- 6. Initiating Controlled Rollback ---"

cleanup
final_status=$?
trap - INT TERM

if [ "$INTERRUPT_SIGNAL" -ne 0 ]; then
    exit "$INTERRUPT_SIGNAL"
elif [ "$final_status" -ne 0 ]; then
    echo ""
    echo "========================================================"
    echo " STAGE 1 FAILED DURING ROLLBACK (exit code $final_status)"
    echo "========================================================" >&2
    exit "$final_status"
fi

echo ""
echo "========================================================"
echo " STAGE 1 VERIFICATION & ROLLBACK COMPLETED SUCCESSFULLY"
echo " Output recorded in: $STAGE1_LOG"
echo "========================================================"
exit 0
