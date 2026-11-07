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

echo "[*] 1. Setting up permanent system source repository in /usr/src/fte3600-1.0.0..."
mkdir -p /usr/src/fte3600-1.0.0
cp -r "${SCRIPT_DIR}/kernel/"* /usr/src/fte3600-1.0.0/ 2>/dev/null || true
mkdir -p /usr/src/fte3600-1.0.0/libfprint-package
cp -r "${SCRIPT_DIR}/libfprint-package/"* /usr/src/fte3600-1.0.0/libfprint-package/ 2>/dev/null || true

echo "[*] 2. Ensuring required dependencies are installed..."
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

if [[ ! -x "${SCRIPT_DIR}/tools/fte3600-tool" ]]; then
    echo "[*] Compiling tools/fte3600-tool..."
    gcc -std=c11 -O2 "${SCRIPT_DIR}/tools/fte3600-tool.c" -o "${SCRIPT_DIR}/tools/fte3600-tool"
fi

echo "[*] 3. Installing FTE3600 self-healing guard executable..."
cp "${SCRIPT_DIR}/tools/fte3600-guard" /usr/local/bin/fte3600-guard
chmod 755 /usr/local/bin/fte3600-guard

echo "[*] 4. Installing FTE3600 early-boot systemd service..."
cp "${SCRIPT_DIR}/tools/fte3600-guard.service" /etc/systemd/system/fte3600-guard.service
systemctl daemon-reload
systemctl enable fte3600-guard.service

echo "[*] 5. Installing udev rules..."
cp "${SCRIPT_DIR}/tools/70-fte3600.rules" /etc/udev/rules.d/70-fte3600.rules
udevadm control --reload-rules 2>/dev/null || true

if [[ -f /etc/pacman.conf ]] && ! grep -q "usr/lib/libfprint-2.so" /etc/pacman.conf; then
    echo "[*] 6. Protecting libfprint in pacman.conf (NoExtract)..."
    sed -i '/^\[options\]/a NoExtract = usr/lib/libfprint-2.so*' /etc/pacman.conf
fi

if [[ -d /etc/pacman.d/hooks ]]; then
    echo "[*] 7. Installing ALPM pacman transaction hook..."
    cat <<'HOOKEOF' > /etc/pacman.d/hooks/95-fte3600.hook
[Trigger]
Type = Path
Operation = Install
Operation = Upgrade
Target = usr/lib/modules/*/vmlinuz
Target = usr/lib/modules/*/modules.builtin
Target = usr/lib/libfprint-2.so*

[Action]
Description = FTE3600 Self-Healing Guard (Kernel Module & Biometric Library)...
When = PostTransaction
Exec = /usr/local/bin/fte3600-guard
HOOKEOF
fi

echo "[*] 8. Installing system-sleep hook to eliminate suspend/resume desync..."
mkdir -p /etc/systemd/system-sleep
cp "${SCRIPT_DIR}/tools/fprintd-resume" /etc/systemd/system-sleep/fprintd-resume
chmod 755 /etc/systemd/system-sleep/fprintd-resume

echo "[*] 9. Executing self-healing guard to configure hardware and daemons..."
/usr/local/bin/fte3600-guard

echo ""
echo "[+] =========================================================================="
echo "[+] FTE3600 Full-Stack Driver & Self-Healing Guard successfully provisioned!"
echo "[+] The stack is now immune to kernel updates, pacman upgrades, and ldconfig."
echo "[+] =========================================================================="
echo ""
echo "Next step: Run 'omarchy setup security fingerprint' or 'fprintd-enroll' to register your fingerprint."
