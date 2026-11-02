#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Convenience launcher for Stage 2A Supervised Sensor Identification in terminal
#
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

printf 'FTE3600 Stage 2A Supervised Sensor Identification (Pure spidev)\n'
printf 'This script executes the minimal, strictly bounded identification sequence:\n'
printf '  1. Binds spidev to spi-FTE3600:00 (Pure spidev; no fte3600.ko, no UIO, no GPIO manipulation)\n'
printf '  2. Writes volatile reg 0xC6 = 0x01 (SPI clock enable)\n'
printf '  3. Reads reg 0xC6 (verifies 0x01)\n'
printf '  4. Reads Chip ID reg 0x1A8B (validates framing & CRC-CCITT)\n'
printf '  5. Conditionally reads 0x1816 if Chip ID is 0x9391\n'
printf '  6. Executes complete host rollback\n\n'

sudo "$SCRIPT_DIR/stage2a-test.sh"
status=$?

printf '\n'
if [ "$status" -eq 0 ]; then
    printf 'Stage 2A identification and rollback completed successfully.\n'
else
    printf 'Stage 2A identification exited with status %d.\n' "$status"
fi
printf 'Output log: %s/../diagnostics/stage2a-results.txt\n' "$SCRIPT_DIR"
printf '\nPress Enter to close this window...'
read -r _
