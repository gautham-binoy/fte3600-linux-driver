#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Convenience launcher: run the read-only privileged collector in a terminal
# and keep the window open so the user can read the result.
#
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

printf 'FTE3600 read-only privileged diagnostics\n'
printf 'The script reads ACPI tables, debugfs, /proc and sysfs only.\n'
printf 'It does not load modules, bind drivers, or write to the device.\n\n'

sudo "$SCRIPT_DIR/collect-privileged.sh"
status=$?

printf '\n'
if [ "$status" -eq 0 ]; then
    printf 'Collector finished successfully.\n'
else
    printf 'Collector exited with status %d.\n' "$status"
fi
printf 'Output: %s/diagnostics/acpi and %s/diagnostics/privileged\n' \
    "$SCRIPT_DIR/.." "$SCRIPT_DIR/.."
printf '\nPress Enter to close this window...'
read -r _
