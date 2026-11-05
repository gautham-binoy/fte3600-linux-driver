#!/usr/bin/env bash
# Configure PAM for Omarchy Fingerprint Authentication (sudo, polkit, lockscreen)
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "[-] Error: Must be run as root (sudo bash tools/enable-pam-auth.sh)" >&2
    exit 1
fi

fprintd_gate="auth      [success=1 default=ignore] pam_exec.so quiet /usr/bin/omarchy-hw-laptop-closed"

echo "[*] Configuring sudo for fingerprint authentication..."
if ! grep -q pam_fprintd.so /etc/pam.d/sudo; then
    sed -i '1i auth      sufficient pam_fprintd.so' /etc/pam.d/sudo
fi
if ! grep -q 'omarchy-hw-laptop-closed' /etc/pam.d/sudo; then
    sed -i "/pam_fprintd\.so/i $fprintd_gate" /etc/pam.d/sudo
fi

echo "[*] Configuring polkit for fingerprint authentication..."
if [[ -f /etc/pam.d/polkit-1 ]]; then
    if ! grep -q 'pam_fprintd.so' /etc/pam.d/polkit-1; then
        sed -i '1i auth      sufficient pam_fprintd.so' /etc/pam.d/polkit-1
    fi
    if ! grep -q 'omarchy-hw-laptop-closed' /etc/pam.d/polkit-1; then
        sed -i "/pam_fprintd\.so/i $fprintd_gate" /etc/pam.d/polkit-1
    fi
else
    tee /etc/pam.d/polkit-1 >/dev/null <<EOF
$fprintd_gate
auth      sufficient pam_fprintd.so
auth      required pam_unix.so

account   required pam_unix.so
password  required pam_unix.so
session   required pam_unix.so
EOF
fi

echo "[*] Configuring lock screen for fingerprint authentication..."
tee /etc/pam.d/omarchy-lock-fingerprint >/dev/null <<'EOF'
#%PAM-1.0
auth       required                    pam_fprintd.so
account    include                     system-local-login
EOF

echo "[+] Fingerprint authentication successfully enabled for sudo, polkit, and lock screen!"
