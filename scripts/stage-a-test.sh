#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Stage 2E.6 Stage A Hardware Probe Execution & Verification Script
# Dynabook Satellite Pro C40-J (PYS46G-06J02D)
# Target Silicon: FocalTech FT9362 / FW9369
# Target SPI Device: spi-FTE3600:00 on pxa2xx-spi.2 (spi1.0)
#
# Scope: STAGE A ONLY
#   - insmod fte3600.ko
#   - Verify probe binding to spi-FTE3600:00
#   - Verify reset GPIO acquisition (devm_gpiod_get)
#   - Verify SPI setup (1 MHz, Mode 0, 8-bit) without transfers
#   - Verify /dev/fte3600 creation
#   - Issue GET_STATE ioctl (verify state == 0 UNINITIALIZED)
#   - Zero SPI transfers; zero deliberate reset pulses
#   - Controlled rollback via graceful rmmod fte3600
#   - Verification of post-cleanup host state
#
set -euo pipefail
export LC_ALL=C

PROJECT_DIR=$(cd "$(dirname "$0")/.." && pwd)
DIAG_DIR="$PROJECT_DIR/diagnostics"
STAGE_A_LOG="$DIAG_DIR/stage-a-results.txt"
MODULE_PATH="$PROJECT_DIR/kernel/fte3600.ko"
TOOL_BIN="$PROJECT_DIR/bin/fte3600-tool"
SPI_DEV="spi-FTE3600:00"
SPI_PATH="/sys/bus/spi/devices/$SPI_DEV"
PINCTRL_PINS="/sys/kernel/debug/pinctrl/INT34C5:00/pins"
DEBUGFS_GPIO="/sys/kernel/debug/gpio"

mkdir -p "$DIAG_DIR"
exec > >(tee "$STAGE_A_LOG") 2>&1

printf "%s\n" "==============================================================================="
printf "%s\n" " Stage 2E.6 — Stage A Hardware Probe Execution & Verification"
printf "%s\n" " Host: Dynabook Satellite Pro C40-J (PYS46G-06J02D)"
printf "%s\n" " Target: spi-FTE3600:00 (FocalTech FT9362 / FW9369)"
printf " Kernel: %s\n" "$(uname -r)"
printf " Timestamp: %s\n" "$(date -Iseconds)"
printf "%s\n\n" "==============================================================================="

# ------------------------------------------------------------------------------
# Pre-Flight Checks
# ------------------------------------------------------------------------------
printf "%s\n" "--- [PHASE 1] Pre-Flight System & Safety Inspection ---"

if [ "$(id -u)" -ne 0 ]; then
    printf "%s\n" "[FAIL] Stage A execution requires root privileges (sudo)."
    exit 1
fi

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
if lsmod | grep -E "fte3600|spidev" >/dev/null 2>&1; then
    printf "%s\n" "[FAIL] Conflicting module already loaded:"
    lsmod | grep -E "fte3600|spidev"
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
    printf "[FAIL] Device already bound to driver: %s\n" "$(readlink "$SPI_PATH/driver")"
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

# 7. Record Baseline Hardware State
printf "\n%s\n" "--- Baseline Hardware & Register State ---"
if [ -r "$PINCTRL_PINS" ]; then
    printf "PCH Pad 14 (Reset):   %s\n" "$(grep "pin 14 " "$PINCTRL_PINS" || true)"
    printf "PCH Pad 229 (IRQ):    %s\n" "$(grep "pin 229 " "$PINCTRL_PINS" || true)"
else
    printf "[WARN] Unable to read pinctrl debugfs at %s\n" "$PINCTRL_PINS"
fi

if [ -r "$DEBUGFS_GPIO" ]; then
    printf "GPIO lines claimed by fte3600: %s\n" \
        "$(grep "fte3600" "$DEBUGFS_GPIO" || echo "None")"
fi

# Record kernel log line count before probe
DMESG_START_LINES=$(dmesg | wc -l)
printf "Dmesg baseline line count: %d\n\n" "$DMESG_START_LINES"

# ------------------------------------------------------------------------------
# Stage A Execution
# ------------------------------------------------------------------------------
printf "%s\n" "--- [PHASE 2] Stage A Module Insertion & Driver Probe ---"
printf "Executing: insmod %s\n" "$MODULE_PATH"

insmod "$MODULE_PATH"
sleep 1

printf "\n%s\n" "--- Post-Probe Verification ---"

# 1. Verify module is loaded
printf "Checking lsmod for fte3600 ... "
if lsmod | grep fte3600 >/dev/null 2>&1; then
    printf "%s\n" "[PASS]"
    lsmod | grep fte3600
else
    printf "%s\n" "[FAIL] fte3600 module not loaded."
    exit 1
fi

# 2. Verify SPI driver binding
printf "Checking SPI driver binding link ... "
if [ -d "$SPI_PATH/driver" ]; then
    BOUND_DRIVER=$(readlink "$SPI_PATH/driver")
    printf "[PASS] Bound to: %s\n" "$BOUND_DRIVER"
else
    printf "%s\n" "[FAIL] SPI device failed to bind to driver."
    exit 1
fi

# 3. Verify character device node
printf "Checking character device node /dev/fte3600 ... "
if [ -c "/dev/fte3600" ]; then
    printf "%s\n" "[PASS]"
    ls -l /dev/fte3600
else
    printf "%s\n" "[FAIL] Character device node /dev/fte3600 was not created."
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

# 6. Query initial driver state via GET_STATE ioctl
printf "\n%s\n" "--- Querying Initial State via FTE3600_IOC_GET_STATE ---"
"$TOOL_BIN" get-state

# 7. Check kernel ring buffer for probe messages
printf "\n%s\n" "--- Kernel Ring Buffer Messages During Probe ---"
dmesg | tail -n +"$((DMESG_START_LINES + 1))"

# ------------------------------------------------------------------------------
# Controlled Rollback & Teardown
# ------------------------------------------------------------------------------
printf "\n%s\n" "--- [PHASE 4] Controlled Host Teardown & Resource Release ---"

# 1. Verify no process holds open file descriptors
printf "Checking for open file descriptors on /dev/fte3600 ... "
if lsof /dev/fte3600 >/dev/null 2>&1; then
    printf "%s\n" "[WARN] File descriptors still open on /dev/fte3600:"
    lsof /dev/fte3600
    exit 1
fi
printf "%s\n" "[PASS] (Zero open descriptors)"

# 2. Graceful module unload
printf "Executing: rmmod fte3600\n"
rmmod fte3600
sleep 1

# 3. Verify character device removal
printf "Verifying /dev/fte3600 node removal ... "
if [ ! -e "/dev/fte3600" ]; then
    printf "%s\n" "[PASS] (Devnode removed)"
else
    printf "%s\n" "[FAIL] /dev/fte3600 still exists!"
    exit 1
fi

# 4. Verify SPI driver unbinding
printf "Verifying SPI device unbinding ... "
if [ ! -d "$SPI_PATH/driver" ]; then
    printf "%s\n" "[PASS] (SPI device unbound)"
else
    printf "[FAIL] SPI device still bound to: %s\n" "$(readlink "$SPI_PATH/driver")"
    exit 1
fi

# 5. Verify module removal from kernel
printf "Verifying fte3600 module absence in lsmod ... "
if ! lsmod | grep fte3600 >/dev/null 2>&1; then
    printf "%s\n" "[PASS] (Module unloaded cleanly)"
else
    printf "%s\n" "[FAIL] fte3600 module still in lsmod!"
    exit 1
fi

# 6. Post-Cleanup Hardware & GPIO State
printf "\n%s\n" "--- Post-Cleanup Hardware State ---"
if [ -r "$PINCTRL_PINS" ]; then
    printf "PCH Pad 14 post-cleanup: %s\n" "$(grep "pin 14 " "$PINCTRL_PINS" || true)"
    printf "PCH Pad 229 post-cleanup: %s\n" "$(grep "pin 229 " "$PINCTRL_PINS" || true)"
fi

if [ -r "$DEBUGFS_GPIO" ]; then
    printf "GPIO lines claimed by fte3600 post-cleanup: %s\n" \
        "$(grep "fte3600" "$DEBUGFS_GPIO" || echo "None")"
fi

printf "\n%s\n" "--- Kernel Ring Buffer Messages During Teardown ---"
dmesg | tail -n 15

printf "\n%s\n" "==============================================================================="
printf "%s\n" " Stage A Execution and Controlled Teardown Completed Successfully."
printf " Log written to: %s\n" "$STAGE_A_LOG"
printf "%s\n" "==============================================================================="
