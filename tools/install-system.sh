#!/usr/bin/env bash
# FTE3600 / FT9362 Linux Full System Integration Installer
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "[-] Error: This script must be run as root (sudo bash tools/install-system.sh)" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KVER="$(uname -r)"

echo "[*] 1. Preparing and Installing FTE3600 Unified Kernel Driver..."
if [[ ! -f "${SCRIPT_DIR}/kernel/fte3600.ko" ]] || ! modinfo "${SCRIPT_DIR}/kernel/fte3600.ko" 2>/dev/null | grep -q "${KVER}"; then
    echo "[*] Compiling fte3600.ko module for kernel ${KVER}..."
    make -C "${SCRIPT_DIR}/kernel" clean
    make -C "${SCRIPT_DIR}/kernel" modules
fi

if [[ ! -x "${SCRIPT_DIR}/tools/fte3600-tool" ]]; then
    echo "[*] Compiling tools/fte3600-tool..."
    gcc -std=c11 -O2 "${SCRIPT_DIR}/tools/fte3600-tool.c" -o "${SCRIPT_DIR}/tools/fte3600-tool"
fi

mkdir -p "/lib/modules/${KVER}/extra"
cp "${SCRIPT_DIR}/kernel/fte3600.ko" "/lib/modules/${KVER}/extra/"
depmod -a

echo "[*] 2. Reloading kernel driver module..."
rmmod fte3600 2>/dev/null || true
modprobe fte3600

echo "[*] Configuring module auto-load on boot..."
mkdir -p /etc/modules-load.d
echo "fte3600" > /etc/modules-load.d/fte3600.conf

if [[ -d /etc/pacman.d/hooks ]]; then
    echo "[*] Installing pacman hook for automatic driver rebuilds on kernel updates..."
    cat <<'HOOKEOF' > /etc/pacman.d/hooks/95-fte3600.hook
[Trigger]
Type = Path
Operation = Install
Operation = Upgrade
Target = usr/lib/modules/*/vmlinuz
Target = usr/lib/modules/*/modules.builtin
Target = usr/lib/libfprint-2.so*

[Action]
Description = Rebuilding & Restoring FTE3600 Fingerprint Stack...
When = PostTransaction
Exec = /bin/bash /home/titan/fte3600-linux-driver/tools/install-system.sh
HOOKEOF
fi

echo "[*] Checking device nodes..."
if [[ -c /dev/fte3600 ]]; then
    echo "[+] /dev/fte3600 created successfully."
else
    echo "[-] Warning: /dev/fte3600 not found."
fi

if [[ -c /dev/focal_moh_spi ]]; then
    echo "[+] /dev/focal_moh_spi created successfully."
else
    echo "[-] Warning: /dev/focal_moh_spi not found."
fi

echo "[*] 3. Ensuring fprintd service and Omarchy platform registration..."
if ! command -v fprintd >/dev/null 2>&1; then
    echo "[*] Installing fprintd daemon via package manager..."
    if command -v pacman >/dev/null 2>&1; then
        if [[ ! -f /var/lib/pacman/db.lck ]]; then
            pacman -S --needed --noconfirm --overwrite '/usr/lib/libfprint*' fprintd
        fi
    elif command -v apt-get >/dev/null 2>&1; then
        apt-get update && apt-get install -y fprintd libpam-fprintd
    elif command -v dnf >/dev/null 2>&1; then
        dnf install -y fprintd fprintd-pam
    fi
fi

if [[ -d /usr/share/omarchy || -d /usr/share/omarchy-platform ]]; then
    echo "[*] Registering FTE3600 with Omarchy hardware platform detector..."
    mkdir -p /usr/share/omarchy-platform
    if ! grep -q "spi-FTE3600:00" /usr/share/omarchy-platform/fingerprint-readers 2>/dev/null; then
        echo "/sys/bus/spi/devices/spi-FTE3600:00/modalias acpi:FTE3600:FTE3600:" >> /usr/share/omarchy-platform/fingerprint-readers
    fi
fi

echo "[*] 4. Installing FT9362-compatible libfprint userspace driver..."
BACKUP_DIR="/var/backups/fte3600"
mkdir -p "${BACKUP_DIR}"
if [[ -f /usr/lib/libfprint-2.so.2.0.0 && ! -f "${BACKUP_DIR}/libfprint-2.so.2.0.0.stock_bak" ]]; then
    if [[ $(stat -c%s /usr/lib/libfprint-2.so.2.0.0 2>/dev/null) -ne 2154176 ]]; then
        cp /usr/lib/libfprint-2.so.2.0.0 "${BACKUP_DIR}/libfprint-2.so.2.0.0.stock_bak"
        echo "[+] Backed up original libfprint to ${BACKUP_DIR}/libfprint-2.so.2.0.0.stock_bak"
    fi
fi

# Critical: Remove any backup files from /usr/lib/ so ldconfig does not accidentally bind to them
rm -f /usr/lib/libfprint-2.so.2.0.0.stock_bak /usr/lib/*stock_bak*

cp "${SCRIPT_DIR}/libfprint-package/libfprint-2.so.2.0.0" /usr/lib/libfprint-2.so.2.0.0
chmod 755 /usr/lib/libfprint-2.so.2.0.0
ln -sf libfprint-2.so.2.0.0 /usr/lib/libfprint-2.so.2
ln -sf libfprint-2.so.2.0.0 /usr/lib/libfprint-2.so
ldconfig

echo "[*] 5. Configuring fprintd systemd service override..."
mkdir -p /etc/systemd/system/fprintd.service.d
cp "${SCRIPT_DIR}/libfprint-package/override.conf" /etc/systemd/system/fprintd.service.d/override.conf

systemctl daemon-reload
systemctl restart fprintd || true

echo "[+] FTE3600 hardware and userspace integration successfully installed!"
echo ""
echo "Next step: Run 'omarchy setup security fingerprint' or 'fprintd-enroll' to register your fingerprint."
