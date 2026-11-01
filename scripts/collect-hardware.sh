#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Read-only, unprivileged hardware inventory for the FTE3600 project.
# It never loads modules, binds drivers, changes GPIO state or writes to SPI.
#
# Usage:
#   ./scripts/collect-hardware.sh > diagnostics/host-inventory.txt 2>&1
#
set -u
export LC_ALL=C

section() {
    printf '\n\n===== %s =====\n' "$1"
}

kv() {
    local key=$1 file=$2
    printf '%-28s: ' "$key"
    if [ -r "$file" ]; then
        cat "$file"
    else
        printf '<unreadable>\n'
    fi
}

section "Timestamp and host"
date -u '+%Y-%m-%dT%H:%M:%SZ'
printf 'hostname: %s\n' "$(cat /proc/sys/kernel/hostname)"

section "Operating system"
if [ -r /etc/os-release ]; then cat /etc/os-release; fi
printf 'kernel: '; uname -r
printf 'arch:   '; uname -m
printf 'cmdline: '; cat /proc/cmdline

section "DMI / firmware"
for f in sys_vendor product_name product_version product_family board_vendor board_name board_version bios_vendor bios_version bios_date chassis_type; do
    kv "$f" "/sys/class/dmi/id/$f"
done

section "CPU"
grep -m1 '^model name' /proc/cpuinfo || true

section "Secure Boot / EFI"
if [ -d /sys/firmware/efi ]; then
    printf 'EFI firmware present: yes\n'
    if [ -r /sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c ]; then
        printf 'SecureBoot efivar (4 bytes after attributes): '
        od -An -t u1 -j 4 -N 1 /sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c
    else
        printf 'SecureBoot efivar: <unreadable without root>\n'
    fi
else
    printf 'EFI firmware present: no\n'
fi

section "ACPI FTE3600 device"
for dev in /sys/bus/acpi/devices/*; do
    [ -r "$dev/hid" ] || continue
    hid=$(cat "$dev/hid" 2>/dev/null || true)
    case "$hid" in
        FTE3600|FTE4800|FTE6600|FTE6900)
            echo "device: $(basename "$dev")"
            kv "  hid" "$dev/hid"
            kv "  status" "$dev/status"
            kv "  path" "$dev/path"
            kv "  modalias" "$dev/modalias"
            printf '  physical_node            : %s\n' "$(readlink -f "$dev/physical_node" 2>/dev/null || echo none)"
            ;;
    esac
done

section "ACPI touchpad (for exclusion)"
for dev in /sys/bus/acpi/devices/SYNA*; do
    [ -d "$dev" ] || continue
    echo "device: $(basename "$dev")"
    kv "  hid" "$dev/hid"
    kv "  path" "$dev/path"
    printf '  physical_node            : %s\n' "$(readlink -f "$dev/physical_node" 2>/dev/null || echo none)"
done

section "SPI devices"
for dev in /sys/bus/spi/devices/*; do
    echo "--- $(basename "$dev")"
    kv "modalias" "$dev/modalias"
    kv "uevent" "$dev/uevent"
    printf 'driver                     : %s\n' "$(readlink -f "$dev/driver" 2>/dev/null || echo none)"
    if [ -d "$dev/statistics" ]; then
        for f in "$dev"/statistics/*; do
            printf 'stat %-22s: %s\n' "$(basename "$f")" "$(cat "$f" 2>/dev/null || echo '?')"
        done
    fi
done

section "SPI masters"
for master in /sys/class/spi_master/*; do
    [ -d "$master" ] || continue
    echo "--- $(basename "$master")"
    printf 'device                     : %s\n' "$(readlink -f "$master/device" 2>/dev/null || echo none)"
    printf 'firmware_node              : %s\n' "$(readlink -f "$master/firmware_node" 2>/dev/null || echo none)"
    printf 'software_node              : %s\n' "$(readlink -f "$master/software_node" 2>/dev/null || echo none)"
done

section "SPI controller PCI device 00:1e.3"
if [ -r /sys/bus/pci/devices/0000:00:1e.3/uevent ]; then
    cat /sys/bus/pci/devices/0000:00:1e.3/uevent
    kv "driver" /sys/bus/pci/devices/0000:00:1e.3/driver
fi

section "GPIO character devices"
ls -l /dev/gpiochip* 2>/dev/null || echo "none"
for dev in /sys/bus/gpio/devices/*; do
    [ -d "$dev" ] || continue
    echo "--- $(basename "$dev") -> $(readlink -f "$dev")"
    printf 'dev                        : %s\n' "$(cat "$dev/dev" 2>/dev/null || echo '?')"
    printf 'firmware_node              : %s\n' "$(readlink -f "$dev/firmware_node" 2>/dev/null || echo none)"
done

section "Pinctrl platform devices"
for dev in /sys/bus/platform/devices/INT34C5:*; do
    [ -d "$dev" ] || continue
    echo "--- $(basename "$dev")"
    printf 'driver                     : %s\n' "$(readlink -f "$dev/driver" 2>/dev/null || echo none)"
    printf 'modalias                   : %s\n' "$(cat "$dev/modalias" 2>/dev/null || echo '?')"
done

section "Interrupts (relevant)"
grep -Ei 'GSPI|spi|gpio|INT34|i2c|pinctrl' /proc/interrupts 2>/dev/null || echo "no matching lines"

section "Loaded modules"
lsmod | grep -Ei 'spi|pinctrl|gpio|uio|focal|fprint' || echo "no matching modules"

section "Kernel configuration"
for opt in CONFIG_SPI CONFIG_SPI_MASTER CONFIG_SPI_PXA2XX CONFIG_SPI_PXA2XX_PCI CONFIG_SPI_SPIDEV CONFIG_PINCTRL_TIGERLAKE CONFIG_GPIO_TIGERLAKE CONFIG_UIO CONFIG_ACPI CONFIG_GPIO_SYSFS CONFIG_DEBUG_FS; do
    value=""
    if [ -r "/boot/config-$(uname -r)" ]; then
        value=$(grep -E "^${opt}=" "/boot/config-$(uname -r)" 2>/dev/null || true)
    fi
    if [ -z "$value" ] && [ -r /proc/config.gz ]; then
        value=$(zgrep -E "^${opt}=" /proc/config.gz 2>/dev/null || true)
    fi
    printf '%-34s %s\n' "$opt" "${value:-<not found>}"
done

section "spidev module"
if command -v modinfo >/dev/null 2>&1; then
    modinfo spidev 2>/dev/null | head -20 || echo "spidev module not available"
else
    echo "modinfo unavailable"
fi
printf 'loaded bufsiz: '
if [ -r /sys/module/spidev/parameters/bufsiz ]; then
    cat /sys/module/spidev/parameters/bufsiz
else
    echo "<spidev not loaded>"
fi
printf 'modprobe.d fte3600 files:\n'
find /etc/modprobe.d /usr/lib/modprobe.d -maxdepth 1 -iname '*fte*' -o -maxdepth 1 -iname '*spidev*' 2>/dev/null || true

section "Kernel headers / toolchain"
printf 'build dir                  : %s\n' "$(readlink -f "/lib/modules/$(uname -r)/build" 2>/dev/null || echo none)"
printf 'kernel version file        : %s\n' "$(cat "/lib/modules/$(uname -r)/build/version" 2>/dev/null || echo '?')"
printf 'gcc                        : %s\n' "$(gcc --version 2>/dev/null | head -1 || echo missing)"
printf 'make                       : %s\n' "$(make --version 2>/dev/null | head -1 || echo missing)"
printf 'dkms                       : %s\n' "$(dkms --version 2>/dev/null || echo missing)"
printf 'meson                      : %s\n' "$(meson --version 2>/dev/null || echo missing)"
printf 'ninja                      : %s\n' "$(ninja --version 2>/dev/null || echo missing)"
printf 'pkg-config                 : %s\n' "$(pkg-config --version 2>/dev/null || echo missing)"

section "Relevant installed packages"
pacman -Q 2>/dev/null | grep -Ei 'linux|dkms|fprint|libfprint|meson|ninja|glib2|gusb|gudev|spi|gpio|acpica|dmidecode' || echo "none matched"

section "libfprint / fprintd presence"
for b in fprintd fprintd-enroll fprintd-verify fprintd-list fprintd-delete; do
    printf '%-20s: %s\n' "$b" "$(command -v "$b" 2>/dev/null || echo missing)"
done
printf 'libfprint shared objects:\n'
ldconfig -p 2>/dev/null | grep -i libfprint || echo "  none in ldconfig cache"
find /usr/lib /usr/local/lib -maxdepth 3 -name 'libfprint-2.so*' 2>/dev/null || true
printf 'fprintd units:\n'
find /usr/lib/systemd/system /etc/systemd/system -maxdepth 2 -iname 'fprintd*' 2>/dev/null || true

section "Firmware files matching FocalTech/FTE"
for dir in /usr/lib/firmware /lib/firmware /usr/local/lib/firmware; do
    [ -d "$dir" ] || continue
    find "$dir" -maxdepth 3 \( -iname '*fte*' -o -iname '*focal*' -o -iname '*ft93*' -o -iname '*ft97*' \) 2>/dev/null
done
echo "(end of firmware search)"

section "Input devices (touchpad exclusion)"
for dev in /sys/class/input/input*; do
    [ -d "$dev" ] || continue
    printf '%s: %s\n' "$(basename "$dev")" "$(cat "$dev/name" 2>/dev/null || echo '?')"
done

section "USB devices"
lsusb 2>/dev/null || echo "lsusb unavailable"

section "Block devices"
lsblk -o NAME,SIZE,FSTYPE,LABEL,MOUNTPOINTS 2>/dev/null || true

section "Relevant kernel log lines (current boot)"
journalctl -k -b --no-pager 2>/dev/null | grep -Ei 'FTE3600|focal|spi1|pxa2xx|GSPI|00:1e\.3|INT34C5|gpiochip' | tail -120 || true

section "ACPI table permissions"
ls -l /sys/firmware/acpi/tables/ 2>/dev/null | head -50 || true

section "Debugfs / GPIO availability"
printf 'debugfs mounted            : '
grep -q ' /sys/kernel/debug debugfs ' /proc/mounts && echo yes || echo no
printf 'debugfs gpio readable      : '
if [ -r /sys/kernel/debug/gpio ]; then echo yes; else echo "no (root required)"; fi
printf 'debugfs pinctrl readable   : '
if [ -r /sys/kernel/debug/pinctrl ]; then echo yes; else echo "no (root required)"; fi

section "End of inventory"
