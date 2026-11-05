#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Stage C FT9362 Hardware Identification Terminal Launcher
# Dynabook Satellite Pro C40-J (PYS46G-06J02D)
# Option 1 Mandatory Policy: Never Automatically Unload the Fingerprint Driver
#
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

printf "===============================================================================\n"
printf " FTE3600 Stage C — Controlled Hardware Identification Test\n"
printf " This script executes the authorized single identification ioctl:\n"
printf "   1. Verifies fte3600 is loaded and bound in RESET_SEQUENCE_ISSUED state\n"
printf "   2. Takes Snapshot 1 via GET_STATE\n"
printf "   3. Records baseline SPI bus statistics\n"
printf "   4. Issues exactly ONE FTE3600_IOC_INIT via bin/fte3600-tool init\n"
printf "   5. Takes Snapshot 2 via GET_STATE\n"
printf "   6. Records post-identification SPI bus statistics and dmesg log\n"
printf "   7. Releases lock; leaves driver loaded and bound (Option 1 retention)\n"
printf "   * ZERO retries; ZERO module unloads; ZERO SPI unbinds\n"
printf "   * ZERO CAPTURE ioctls; ZERO calibration; ZERO OTP/EFUSE access\n"
printf "===============================================================================\n\n"

sudo "$SCRIPT_DIR/stage-c-test.sh"
status=$?

printf "\n"
if [ "$status" -eq 0 ]; then
    printf "Stage C identification test completed successfully (Status 0).\n"
else
    printf "Stage C script exited with status %d.\n" "$status"
fi

printf "Detailed log: %s/../diagnostics/stage-c-results.txt\n" "$SCRIPT_DIR"
printf "\nPress Enter to close this window..."
read -r _
