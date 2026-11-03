#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Stage 2E.6 Stage A Hardware Probe Execution Terminal Launcher
# Dynabook Satellite Pro C40-J (PYS46G-06J02D)
#
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

printf "===============================================================================\n"
printf " FTE3600 Stage 2E.6 — Stage A Hardware Probe Execution\n"
printf " This script runs the strictly bounded, read-only passive probe sequence:\n"
printf "   1. Loads fte3600.ko\n"
printf "   2. Allows ACPI match & probe on spi-FTE3600:00\n"
printf "   3. Acquires reset GPIO (devm_gpiod_get)\n"
printf "   4. Verifies /dev/fte3600 creation\n"
printf "   5. Queries initial state via GET_STATE (must be UNINITIALIZED / 0)\n"
printf "   6. Executes controlled host teardown via graceful rmmod fte3600\n"
printf "   * ZERO SPI transactions; ZERO deliberate reset pulses\n"
printf "===============================================================================\n\n"

sudo "$SCRIPT_DIR/stage-a-test.sh"
status=$?

printf "\n"
if [ "$status" -eq 0 ]; then
    printf "Stage A probe and cleanup completed successfully (Status 0).\n"
else
    printf "Stage A script exited with status %d.\n" "$status"
fi

printf "Detailed log: %s/../diagnostics/stage-a-results.txt\n" "$SCRIPT_DIR"
printf "\nPress Enter to close this window..."
read -r _
