#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Stage 2E.8 Stage B Hardware Reset Test Terminal Launcher
# Dynabook Satellite Pro C40-J (PYS46G-06J02D)
# Option 1 Mandatory Policy: Never Automatically Unload the Fingerprint Driver
#
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

printf "===============================================================================\n"
printf " FTE3600 Stage 2E.8 — Stage B Controlled Hardware Reset Test\n"
printf " This script runs the strictly bounded, single-reset execution sequence:\n"
printf "   1. Loads fte3600.ko\n"
printf "   2. Verifies driver probe and /dev/fte3600\n"
printf "   3. Takes Snapshot 1 via GET_STATE (initial state)\n"
printf "   4. Issues exactly ONE FTE3600_IOC_RESET (40 ms reset pulse sequence)\n"
printf "   5. Takes Snapshot 2 via GET_STATE (post-reset state)\n"
printf "   6. Releases cooperative lock; leaves module loaded and bound by policy\n"
printf "   * ZERO automatic module unloads (rmmod permanently disabled in harness)\n"
printf "   * ZERO SPI transactions; ZERO INIT ioctls; ZERO CAPTURE ioctls\n"
printf "===============================================================================\n\n"

sudo "$SCRIPT_DIR/stage-b-test.sh"
status=$?

printf "\n"
if [ "$status" -eq 0 ]; then
    printf "Stage B reset test completed successfully (Status 0).\n"
    printf "NOTE: Module fte3600 remains loaded by Option 1 policy. Manual rmmod required.\n"
else
    printf "Stage B script exited with status %d.\n" "$status"
    printf "NOTE: Driver/module state preserved in kernel. Manual inspection required.\n"
fi

printf "Detailed log: %s/../diagnostics/stage-b-results.txt\n" "$SCRIPT_DIR"
printf "\nPress Enter to close this window..."
read -r _
