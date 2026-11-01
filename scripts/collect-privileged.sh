#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Privileged, READ-ONLY hardware diagnostics for the FTE3600 project.
#
# This script does NOT:
#   - load or unload kernel modules
#   - bind or unbind drivers
#   - change GPIO state, SPI configuration or ACPI settings
#   - write to any device
#
# It only reads ACPI tables, debugfs, /proc and sysfs and stores the result
# inside the project directory.
#
# Usage:
#   sudo ./scripts/collect-privileged.sh
#
set -u
export LC_ALL=C

if [ "$(id -u)" -ne 0 ]; then
    echo "This script must run as root (read-only diagnostics)." >&2
    exit 1
fi

PROJECT_DIR=$(cd "$(dirname "$0")/.." && pwd)
ACPI_DIR="$PROJECT_DIR/diagnostics/acpi"
PRIV_DIR="$PROJECT_DIR/diagnostics/privileged"
mkdir -p "$ACPI_DIR" "$PRIV_DIR"

section() {
    printf '\n\n===== %s =====\n' "$1"
}

run_to() {
    local outfile=$1
    shift
    {
        printf '$ %s\n' "$*"
        "$@" 2>&1
    } >> "$outfile"
}

section "ACPI binary tables"
(
    cd "$ACPI_DIR" || exit 1
    rm -f ./*.dat ./*.dsl 2>/dev/null || true
    acpidump > acpidump.txt 2>&1
    # acpixtract preserves duplicate SSDTs with unique names; -b is a fallback.
    acpixtract -a acpidump.txt > acpixtract.log 2>&1 || true
    if ! ls ./*.dat >/dev/null 2>&1; then
        acpidump -b >/dev/null 2>&1 || true
    fi
)
ls -l "$ACPI_DIR" > "$PRIV_DIR/acpi-files.txt" 2>&1

section "Decompile ACPI tables"
(
    cd "$ACPI_DIR" || exit 1
    # -f keeps going after warnings; external references to other tables are
    # resolved because all dumped tables are in the same directory.
    for table in ./*.dat; do
        [ -e "$table" ] || continue
        iasl -d -f "$table" >/dev/null 2>&1 || true
    done
)
ls -l "$ACPI_DIR"/*.dsl > "$PRIV_DIR/acpi-dsl-files.txt" 2>&1 || true

section "Extract FTE3600 and controller blocks"
python3 "$PROJECT_DIR/scripts/extract_acpi_blocks.py" "$ACPI_DIR" \
    FTE3600 FP01 SPI1 > "$PRIV_DIR/acpi-fte3600-blocks.dsl" 2>&1 || true
python3 "$PROJECT_DIR/scripts/extract_acpi_blocks.py" "$ACPI_DIR" \
    FTE3600 > "$PRIV_DIR/acpi-fte3600-only.dsl" 2>&1 || true

section "Kernel log"
dmesg > "$PRIV_DIR/dmesg.txt" 2>&1 || true

section "Interrupts"
cp /proc/interrupts "$PRIV_DIR/interrupts.txt" 2>/dev/null || true
cp /proc/iomem "$PRIV_DIR/iomem.txt" 2>/dev/null || true

section "GPIO debugfs"
if [ -r /sys/kernel/debug/gpio ]; then
    cp /sys/kernel/debug/gpio "$PRIV_DIR/gpio-debugfs.txt" 2>/dev/null || true
else
    echo "debugfs gpio not readable" > "$PRIV_DIR/gpio-debugfs.txt"
fi

section "Pinctrl debugfs"
: > "$PRIV_DIR/pinctrl.txt"
for dir in /sys/kernel/debug/pinctrl/*; do
    [ -d "$dir" ] || continue
    {
        printf '\n### %s\n' "$dir"
        for f in pins pinmux-pins pinconf-pins pinconf-groups gpio-ranges gpio-line-names; do
            [ -r "$dir/$f" ] || continue
            printf '\n--- %s ---\n' "$f"
            cat "$dir/$f" 2>&1 || true
        done
    } >> "$PRIV_DIR/pinctrl.txt"
done

section "Character devices"
{
    ls -l /dev/gpiochip* /dev/spidev* /dev/uio* /dev/focal* 2>&1
    printf '\n/dev/misc:\n'
    ls -l /dev/misc 2>&1 || true
} > "$PRIV_DIR/char-devices.txt" 2>&1

section "GPIO line information"
if command -v gpioinfo >/dev/null 2>&1; then
    gpioinfo > "$PRIV_DIR/gpioinfo.txt" 2>&1 || true
else
    echo "gpioinfo not installed (package: libgpiod)" > "$PRIV_DIR/gpioinfo.txt"
fi

section "Secure Boot variables"
{
    for var in SecureBoot SetupMode VendorKeys; do
        for f in /sys/firmware/efi/efivars/${var}-8be4df61-93ca-11d2-aa0d-00e098032b8c; do
            [ -r "$f" ] || continue
            printf '%s: ' "$var"
            od -An -t u1 -j 4 "$f" 2>&1 || true
        done
    done
} > "$PRIV_DIR/secureboot.txt" 2>&1

section "ACPI device sysfs resources"
{
    for dev in /sys/bus/acpi/devices/FTE3600:* /sys/bus/acpi/devices/INT34C5:*; do
        [ -d "$dev" ] || continue
        printf '\n### %s\n' "$dev"
        find "$dev" -maxdepth 2 -type f -print 2>/dev/null | sort | while read -r f; do
            printf '\n--- %s ---\n' "$f"
            cat "$f" 2>&1 || true
        done
    done
} > "$PRIV_DIR/acpi-sysfs.txt" 2>&1

section "Done"
echo "Read-only diagnostics written to:"
echo "  $ACPI_DIR"
echo "  $PRIV_DIR"
