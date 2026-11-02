#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Convenience launcher for Stage 1 test in terminal
#
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

printf 'FTE3600 Stage 1 Hardware Test & Rollback\n'
printf 'This script loads spidev, binds it to spi-FTE3600:00, loads fte3600.ko,\n'
printf 'verifies the sysfs/GPIO/UIO state, and cleanly rolls back.\n\n'

sudo "$SCRIPT_DIR/stage1-test.sh"
status=$?

printf '\n'
if [ "$status" -eq 0 ]; then
    printf 'Stage 1 test and rollback completed successfully.\n'
else
    printf 'Stage 1 test exited with status %d.\n' "$status"
fi
printf 'Output log: %s/../diagnostics/stage1-results.txt\n' "$SCRIPT_DIR"
printf '\nPress Enter to close this window...'
read -r _
