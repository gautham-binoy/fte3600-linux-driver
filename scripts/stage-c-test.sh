#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Stage C — Minimal FT9362 Hardware Identification Execution Script
# Target: Dynabook Satellite Pro C40-J (PYS46G-06J02D)
# Policy: Option 1 (Module retained loaded and bound in kernel)
#
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
LOG_FILE="$REPO_DIR/diagnostics/stage-c-results.txt"
LOCK_FILE="/run/lock/fte3600-test.lock"
SPI_DEV_DIR="/sys/bus/spi/devices/spi-FTE3600:00"
TOOL_BIN="$REPO_DIR/bin/fte3600-tool"

# Redirect all stdout & stderr to tee so it is displayed in terminal AND saved to log
exec > >(tee -a "$LOG_FILE") 2>&1

printf "===============================================================================\n"
printf " Stage C — Authorized FT9362 Silicon Identification Sequence\n"
printf " Timestamp: %s\n" "$(date -Iseconds)"
printf " Target Device: /dev/fte3600 (spi-FTE3600:00)\n"
printf " Policy: Option 1 Retention (Driver retained loaded and bound post-test)\n"
printf "===============================================================================\n\n"

# 1. Root privilege check
if [ "$EUID" -ne 0 ]; then
    printf "[ERROR] stage-c-test.sh must be run with root privileges (sudo).\n"
    exit 1
fi

# 2. Acquire cooperative lock
exec 200>"$LOCK_FILE"
if ! flock -n 200; then
    printf "[ERROR] Could not acquire exclusive lock on %s. Another test is running.\n" "$LOCK_FILE"
    exit 1
fi
printf "[PASS] Cooperative lock acquired on %s (FD 200)\n" "$LOCK_FILE"

# 3. Environment pre-flight
printf "--- Pre-Flight System Checks ---\n"
KERNEL_VER=$(uname -r)
printf "Kernel Release:           %s\n" "$KERNEL_VER"

if ! lsmod | grep -q "^fte3600\b"; then
    printf "[FAIL] Driver fte3600 is not loaded in kernel!\n"
    exit 1
fi
printf "Driver Module:            fte3600 (Loaded: %s)\n" "$(lsmod | grep "^fte3600\b")"

if [ ! -c "/dev/fte3600" ]; then
    printf "[FAIL] Device node /dev/fte3600 does not exist!\n"
    exit 1
fi
printf "Device Node:              /dev/fte3600 (%s)\n" "$(ls -l /dev/fte3600)"

if [ ! -x "$TOOL_BIN" ]; then
    printf "[FAIL] Validation utility %s not found or not executable!\n" "$TOOL_BIN"
    exit 1
fi
printf "Validation Utility:       %s (Executable)\n" "$TOOL_BIN"

if [ ! -d "$SPI_DEV_DIR" ]; then
    printf "[FAIL] SPI sysfs directory %s does not exist!\n" "$SPI_DEV_DIR"
    exit 1
fi
printf "SPI Link:                 %s (Bound to %s)\n" "$SPI_DEV_DIR" "$(readlink "$SPI_DEV_DIR/driver" || echo "unbound")"

# 4. Pre-identification baseline telemetry
printf "\n--- Baseline Telemetry & SPI Statistics ---\n"
DMESG_BASELINE=$(dmesg | wc -l)
printf "Dmesg Baseline Lines:     %d\n" "$DMESG_BASELINE"

printf "Baseline SPI Statistics:\n"
for stat in bytes bytes_rx bytes_tx messages transfers errors; do
    if [ -f "$SPI_DEV_DIR/statistics/$stat" ]; then
        printf "  %-12s: %s\n" "$stat" "$(cat "$SPI_DEV_DIR/statistics/$stat")"
    fi
done

printf "\nInitial Driver State (Snapshot 1):\n"
"$TOOL_BIN" get-state
SNAP1_RC=$?
if [ "$SNAP1_RC" -ne 0 ]; then
    printf "[FAIL] Failed to read initial driver state via get-state (rc=%d)\n" "$SNAP1_RC"
    exit 1
fi

# 5. Execute Authorized Single Identification IOCTL
printf "\n===============================================================================\n"
printf " EXECUTING AUTHORIZED IDENTIFICATION IOCTL: bin/fte3600-tool init\n"
printf " Frame 1: Write 0xC6 = 0x01 (transceiver clock un-gate)\n"
printf " Delay:   4-5 ms stabilization\n"
printf " Frame 2: Readback 0xC6 (expect 0x01)\n"
printf " Frame 3: Read 0x1A8B (expect ID 0x9362, CRC 0x1C53)\n"
printf "===============================================================================\n\n"

INIT_START_TIME=$(date +%s%N)
"$TOOL_BIN" init
INIT_RC=$?
INIT_END_TIME=$(date +%s%N)
INIT_DURATION_MS=$(( (INIT_END_TIME - INIT_START_TIME) / 1000000 ))

printf "\nIdentification ioctl completed with return code: %d (duration: %d ms)\n" "$INIT_RC" "$INIT_DURATION_MS"

# 6. Post-identification telemetry
printf "\n--- Post-Identification Telemetry & Diagnostics ---\n"

printf "Post-Identification Driver State (Snapshot 2):\n"
"$TOOL_BIN" get-state
SNAP2_RC=$?

printf "\nPost-Identification SPI Statistics:\n"
for stat in bytes bytes_rx bytes_tx messages transfers errors; do
    if [ -f "$SPI_DEV_DIR/statistics/$stat" ]; then
        printf "  %-12s: %s\n" "$stat" "$(cat "$SPI_DEV_DIR/statistics/$stat")"
    fi
done

printf "\nKernel Ring Buffer Messages (dmesg delta since baseline):\n"
dmesg | tail -n +"$((DMESG_BASELINE + 1))" | grep -E "fte3600|spi|pxa2xx" || printf "  (No new fte3600 kernel messages logged)\n"

# 7. Option 1 Retention Enforcement
printf "\n--- Option 1 Retention Policy Enforcement ---\n"
printf "Module fte3600 retained loaded: %s\n" "$(lsmod | grep "^fte3600\b" || echo "MISSING")"
printf "Device node retained:          %s\n" "$(ls -l /dev/fte3600 2>/dev/null || echo "MISSING")"
printf "SPI link retained:             %s\n" "$(readlink "$SPI_DEV_DIR/driver" 2>/dev/null || echo "UNBOUND")"
printf "[PASS] Zero automatic unloads or unbinds performed.\n"

# 8. Release lock
flock -u 200
exec 200>&-
printf "\n[PASS] Cooperative lock released.\n"

if [ "$INIT_RC" -eq 0 ]; then
    printf "\n>>> STAGE C IDENTIFICATION SUCCESSFUL (STATUS 0) <<<\n"
else
    printf "\n>>> STAGE C IDENTIFICATION FAILED (STATUS %d) <<<\n" "$INIT_RC"
fi

exit "$INIT_RC"
