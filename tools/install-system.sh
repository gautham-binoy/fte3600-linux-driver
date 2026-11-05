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

echo "[*] 3. Installing FT9362-compatible libfprint userspace driver..."
if [[ -f /usr/lib/libfprint-2.so.2.0.0 && ! -f /usr/lib/libfprint-2.so.2.0.0.stock_bak ]]; then
    cp /usr/lib/libfprint-2.so.2.0.0 /usr/lib/libfprint-2.so.2.0.0.stock_bak
    echo "[+] Backed up original libfprint to /usr/lib/libfprint-2.so.2.0.0.stock_bak"
fi

cp "${SCRIPT_DIR}/libfprint-package/libfprint-2.so.2.0.0" /usr/lib/libfprint-2.so.2.0.0
chmod 755 /usr/lib/libfprint-2.so.2.0.0
ln -sf libfprint-2.so.2.0.0 /usr/lib/libfprint-2.so.2
ln -sf libfprint-2.so.2.0.0 /usr/lib/libfprint-2.so

echo "[*] 4. Configuring fprintd systemd service override..."
mkdir -p /etc/systemd/system/fprintd.service.d
cp "${SCRIPT_DIR}/libfprint-package/override.conf" /etc/systemd/system/fprintd.service.d/override.conf

systemctl daemon-reload
systemctl restart fprintd || true

echo "[+] FTE3600 hardware and userspace integration successfully installed!"
echo ""
echo "Next step: Run 'omarchy setup security fingerprint' or 'fprintd-enroll' to register your fingerprint."
