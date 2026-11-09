#!/usr/bin/env bash
# ==============================================================================
# FTE3600 / FT9362 Linux Full System Integration Installer & Guard Provisioner
# Installs kernel driver, userspace library, and self-healing daemon
# ==============================================================================
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "[-] Error: This script must be run as root (sudo bash tools/install-system.sh)" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KVER="$(uname -r)"

echo "[*] 1. Preparing and building FTE3600 Unified Kernel Driver..."
make -C "${SCRIPT_DIR}/kernel" clean >/dev/null 2>&1 || true
make -C "${SCRIPT_DIR}/kernel" modules
mkdir -p "/lib/modules/${KVER}/extra"
cp "${SCRIPT_DIR}/kernel/fte3600.ko" "/lib/modules/${KVER}/extra/fte3600.ko"
depmod -a

echo "[*] 2. Loading updated kernel driver module with power management..."
if lsmod | grep -q fte3600; then
    rmmod fte3600 2>/dev/null || true
fi
modprobe fte3600

echo "[*] 3. Setting up permanent system source repository in /usr/src/fte3600-1.0.0..."
mkdir -p /usr/src/fte3600-1.0.0
cp -r "${SCRIPT_DIR}/kernel/"* /usr/src/fte3600-1.0.0/ 2>/dev/null || true
mkdir -p /usr/src/fte3600-1.0.0/libfprint-package
cp -r "${SCRIPT_DIR}/libfprint-package/"* /usr/src/fte3600-1.0.0/libfprint-package/ 2>/dev/null || true
mkdir -p /usr/src/fte3600-1.0.0/tools
cp -r "${SCRIPT_DIR}/tools/"* /usr/src/fte3600-1.0.0/tools/ 2>/dev/null || true

echo "[*] 4. Ensuring required dependencies are installed..."
if ! command -v fprintd >/dev/null 2>&1; then
    echo "[*] Installing fprintd daemon via package manager..."
    if command -v pacman >/dev/null 2>&1; then
        if [[ ! -f /var/lib/pacman/db.lck ]]; then
            pacman -S --needed --noconfirm fprintd
        fi
    elif command -v apt-get >/dev/null 2>&1; then
        apt-get update && apt-get install -y fprintd libpam-fprintd
    elif command -v dnf >/dev/null 2>&1; then
        dnf install -y fprintd fprintd-pam
    fi
fi

if [[ ! -x "${SCRIPT_DIR}/tools/fte3600-tool" ]]; then
    echo "[*] Compiling tools/fte3600-tool..."
    gcc -std=c11 -O2 "${SCRIPT_DIR}/tools/fte3600-tool.c" -o "${SCRIPT_DIR}/tools/fte3600-tool"
fi
cp "${SCRIPT_DIR}/tools/fte3600-tool" /usr/local/bin/fte3600-tool
chmod 755 /usr/local/bin/fte3600-tool

echo "[*] 5. Installing FTE3600 self-healing guard executable..."
cp "${SCRIPT_DIR}/tools/fte3600-guard" /usr/local/bin/fte3600-guard
chmod 755 /usr/local/bin/fte3600-guard

echo "[*] 6. Installing FTE3600 early-boot systemd service..."
cp "${SCRIPT_DIR}/tools/fte3600-guard.service" /etc/systemd/system/fte3600-guard.service
systemctl daemon-reload
systemctl enable fte3600-guard.service

echo "[*] 7. Installing udev rules..."
cp "${SCRIPT_DIR}/tools/70-fte3600.rules" /etc/udev/rules.d/70-fte3600.rules
udevadm control --reload-rules 2>/dev/null || true

if [[ -f /etc/pacman.conf ]] && grep -q "usr/lib/libfprint-2.so" /etc/pacman.conf; then
    echo "[*] 8. Removing legacy NoExtract rule from pacman.conf (delegated to ALPM hook)..."
    sed -i '/NoExtract = usr\/lib\/libfprint-2\.so\*/d' /etc/pacman.conf
fi

if [[ -d /etc/pacman.d/hooks ]]; then
    echo "[*] 9. Installing ALPM pacman transaction hook..."
    cat <<'HOOKEOF' > /etc/pacman.d/hooks/95-fte3600.hook
[Trigger]
Type = Package
Operation = Install
Operation = Upgrade
Target = libfprint
Target = libfprint-git
Target = fprintd
Target = linux*

[Trigger]
Type = Path
Operation = Install
Operation = Upgrade
Target = usr/lib/modules/*/vmlinuz
Target = usr/lib/modules/*/modules.builtin
Target = usr/lib/fprintd
Target = usr/lib/libfprint*

[Action]
Description = FTE3600 Self-Healing Guard (Kernel Module & Biometric Library)...
When = PostTransaction
Exec = /usr/local/bin/fte3600-guard
HOOKEOF
fi

echo "[*] 10. Installing system-sleep hooks to eliminate suspend/resume desync..."
mkdir -p /usr/lib/systemd/system-sleep /etc/systemd/system-sleep
cp "${SCRIPT_DIR}/tools/fprintd-resume" /usr/lib/systemd/system-sleep/fprintd-resume
cp "${SCRIPT_DIR}/tools/fprintd-resume" /etc/systemd/system-sleep/fprintd-resume
chmod 755 /usr/lib/systemd/system-sleep/fprintd-resume /etc/systemd/system-sleep/fprintd-resume

if [[ -d /usr/share/omarchy/default/systemd/system-sleep ]]; then
    echo "[*] Updating Omarchy system-sleep template to prevent regression on lock updates..."
    cp "${SCRIPT_DIR}/tools/fprintd-resume" /usr/share/omarchy/default/systemd/system-sleep/fprintd-resume
    chmod 755 /usr/share/omarchy/default/systemd/system-sleep/fprintd-resume
fi

echo "[*] 11. Executing self-healing guard to configure hardware and daemons..."
/usr/local/bin/fte3600-guard

echo ""
echo "[+] =========================================================================="
echo "[+] FTE3600 Full-Stack Driver & Self-Healing Guard successfully provisioned!"
echo "[+] The stack is now immune to long sleep, kernel updates, and pacman upgrades."
echo "[+] =========================================================================="
echo ""
echo "Next step: Run 'omarchy setup security fingerprint' or 'fprintd-enroll' to register your fingerprint."
