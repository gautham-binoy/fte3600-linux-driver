#!/usr/bin/env python3
import subprocess

# 1. Generate Architecture Diagram SVG
arch_svg = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1200 800" width="1200" height="800">
  <defs>
    <linearGradient id="bg" x1="0%" y1="0%" x2="100%" y2="100%">
      <stop offset="0%" stop-color="#0b0f19" />
      <stop offset="50%" stop-color="#111827" />
      <stop offset="100%" stop-color="#070a12" />
    </linearGradient>
    <linearGradient id="cardGrad" x1="0%" y1="0%" x2="0%" y2="100%">
      <stop offset="0%" stop-color="#1f2937" stop-opacity="0.8" />
      <stop offset="100%" stop-color="#111827" stop-opacity="0.9" />
    </linearGradient>
    <linearGradient id="cyanAccent" x1="0%" y1="0%" x2="100%" y2="0%">
      <stop offset="0%" stop-color="#06b6d4" />
      <stop offset="100%" stop-color="#3b82f6" />
    </linearGradient>
    <linearGradient id="emeraldAccent" x1="0%" y1="0%" x2="100%" y2="0%">
      <stop offset="0%" stop-color="#10b981" />
      <stop offset="100%" stop-color="#059669" />
    </linearGradient>
    <linearGradient id="purpleAccent" x1="0%" y1="0%" x2="100%" y2="0%">
      <stop offset="0%" stop-color="#8b5cf6" />
      <stop offset="100%" stop-color="#6366f1" />
    </linearGradient>
    <filter id="glow" x="-20%" y="-20%" width="140%" height="140%">
      <feGaussianBlur stdDeviation="4" result="blur" />
      <feComposite in="SourceGraphic" in2="blur" operator="over" />
    </filter>
  </defs>

  <!-- Background -->
  <rect width="1200" height="800" fill="url(#bg)" />

  <!-- Subtle Grid Lines -->
  <g stroke="#1f293d" stroke-width="0.5" opacity="0.4">
    <path d="M0,100 H1200 M0,200 H1200 M0,300 H1200 M0,400 H1200 M0,500 H1200 M0,600 H1200 M0,700 H1200" />
    <path d="M150,0 V800 M300,0 V800 M450,0 V800 M600,0 V800 M750,0 V800 M900,0 V800 M1050,0 V800" />
  </g>

  <!-- Title Badge & Headers -->
  <rect x="50" y="35" width="220" height="30" rx="15" fill="#1e293b" stroke="#38bdf8" stroke-width="1.5" />
  <text x="160" y="55" font-family="system-ui, -apple-system, sans-serif" font-size="12" font-weight="700" fill="#38bdf8" text-anchor="middle" letter-spacing="1.5">LINUX KERNEL DRIVER</text>
  
  <text x="50" y="105" font-family="system-ui, -apple-system, sans-serif" font-size="32" font-weight="800" fill="#f8fafc">FTE3600 / FT9362 Full-Stack Architecture</text>
  <text x="50" y="135" font-family="system-ui, -apple-system, sans-serif" font-size="16" fill="#94a3b8">Physical SPI Hardware to PAM Biometric Security Stack</text>

  <!-- Layer 1: Hardware & Motherboard -->
  <g transform="translate(50, 160)">
    <rect width="1100" height="95" rx="12" fill="url(#cardGrad)" stroke="#334155" stroke-width="1.5" />
    <rect x="0" y="0" width="6" height="95" rx="3" fill="#f59e0b" />
    <text x="30" y="32" font-family="system-ui, sans-serif" font-size="18" font-weight="700" fill="#fbbf24">1. HARDWARE LAYER (Dynabook Satellite Pro C40-J)</text>
    
    <!-- Badges -->
    <rect x="30" y="48" width="210" height="32" rx="6" fill="#1e293b" stroke="#475569" stroke-width="1" />
    <text x="135" y="69" font-family="monospace" font-size="12" fill="#e2e8f0" text-anchor="middle">Silicon: FT9362 / FW9369</text>

    <rect x="255" y="48" width="210" height="32" rx="6" fill="#1e293b" stroke="#475569" stroke-width="1" />
    <text x="360" y="69" font-family="monospace" font-size="12" fill="#e2e8f0" text-anchor="middle">Controller: Intel LPSS SPI2</text>

    <rect x="480" y="48" width="180" height="32" rx="6" fill="#1e293b" stroke="#475569" stroke-width="1" />
    <text x="570" y="69" font-family="monospace" font-size="12" fill="#e2e8f0" text-anchor="middle">ACPI Reset: Pad 14</text>

    <rect x="675" y="48" width="190" height="32" rx="6" fill="#1e293b" stroke="#475569" stroke-width="1" />
    <text x="770" y="69" font-family="monospace" font-size="12" fill="#e2e8f0" text-anchor="middle">Bus: Mode 0 @ 1.0 MHz</text>

    <rect x="880" y="48" width="190" height="32" rx="6" fill="#1e293b" stroke="#475569" stroke-width="1" />
    <text x="975" y="69" font-family="monospace" font-size="12" fill="#e2e8f0" text-anchor="middle">Matrix: 64x80 (10.2 KB)</text>
  </g>

  <!-- Connectors -->
  <path d="M600,255 V280" stroke="#38bdf8" stroke-width="2" stroke-dasharray="4,4" />

  <!-- Layer 2: Kernel Driver (fte3600.ko) -->
  <g transform="translate(50, 280)">
    <rect width="1100" height="150" rx="12" fill="url(#cardGrad)" stroke="#0284c7" stroke-width="2" />
    <rect x="0" y="0" width="6" height="150" rx="3" fill="url(#cyanAccent)" />
    <text x="30" y="32" font-family="system-ui, sans-serif" font-size="18" font-weight="700" fill="#38bdf8">2. KERNEL SUBSYSTEM (kernel/fte3600.ko)</text>

    <!-- Subsystem Boxes -->
    <rect x="30" y="50" width="245" height="80" rx="8" fill="#0f172a" stroke="#1e293b" stroke-width="1.5" />
    <text x="45" y="75" font-family="system-ui, sans-serif" font-size="14" font-weight="600" fill="#e2e8f0">Async SPI Engine</text>
    <text x="45" y="95" font-family="system-ui, sans-serif" font-size="11" fill="#94a3b8">• 2-Ref kref Accounting</text>
    <text x="45" y="112" font-family="system-ui, sans-serif" font-size="11" fill="#94a3b8">• 250ms Timeout Watchdog</text>

    <rect x="290" y="50" width="245" height="80" rx="8" fill="#0f172a" stroke="#1e293b" stroke-width="1.5" />
    <text x="305" y="75" font-family="system-ui, sans-serif" font-size="14" font-weight="600" fill="#e2e8f0">DMA Memory Quarantine</text>
    <text x="305" y="95" font-family="system-ui, sans-serif" font-size="11" fill="#94a3b8">• Safe FREED(t) ≡ 0 Invariant</text>
    <text x="305" y="112" font-family="system-ui, sans-serif" font-size="11" fill="#94a3b8">• Max 67.8 KB Hard Cap</text>

    <rect x="550" y="50" width="245" height="80" rx="8" fill="#0f172a" stroke="#1e293b" stroke-width="1.5" />
    <text x="565" y="75" font-family="system-ui, sans-serif" font-size="14" font-weight="600" fill="#e2e8f0">AFE Calibration Loop</text>
    <text x="565" y="95" font-family="system-ui, sans-serif" font-size="11" fill="#94a3b8">• 4-Ch Capacitive Touch</text>
    <text x="565" y="112" font-family="system-ui, sans-serif" font-size="11" fill="#94a3b8">• 22-Byte Threshold Banking</text>

    <rect x="810" y="50" width="260" height="80" rx="8" fill="#0f172a" stroke="#1e293b" stroke-width="1.5" />
    <text x="825" y="75" font-family="system-ui, sans-serif" font-size="14" font-weight="600" fill="#e2e8f0">Protocol &amp; CRC Engine</text>
    <text x="825" y="95" font-family="system-ui, sans-serif" font-size="11" fill="#94a3b8">• CCITT CRC-16 (0x1C53)</text>
    <text x="825" y="112" font-family="system-ui, sans-serif" font-size="11" fill="#94a3b8">• Single Burst 10.2 KB FIFO</text>
  </g>

  <!-- Connectors -->
  <path d="M300,430 V455" stroke="#38bdf8" stroke-width="2" />
  <path d="M900,430 V455" stroke="#a855f7" stroke-width="2" />

  <!-- Layer 3: Dual Character Device Nodes -->
  <g transform="translate(50, 455)">
    <rect width="1100" height="85" rx="12" fill="url(#cardGrad)" stroke="#334155" stroke-width="1.5" />
    <rect x="0" y="0" width="6" height="85" rx="3" fill="url(#purpleAccent)" />
    <text x="30" y="32" font-family="system-ui, sans-serif" font-size="18" font-weight="700" fill="#c084fc">3. EXPOSED CHARACTER DEVICE NODES</text>

    <rect x="30" y="45" width="510" height="30" rx="6" fill="#1e1b4b" stroke="#6366f1" stroke-width="1" />
    <text x="285" y="65" font-family="monospace" font-size="12" font-weight="bold" fill="#a5b4fc" text-anchor="middle">/dev/fte3600 (Structured UAPI &amp; Diagnostic Leasing)</text>

    <rect x="560" y="45" width="510" height="30" rx="6" fill="#1e1b4b" stroke="#a855f7" stroke-width="1" />
    <text x="815" y="65" font-family="monospace" font-size="12" font-weight="bold" fill="#e9d5ff" text-anchor="middle">/dev/focal_moh_spi (Vendor Match-on-Host Bridge)</text>
  </g>

  <!-- Connectors -->
  <path d="M600,540 V565" stroke="#10b981" stroke-width="2" stroke-dasharray="4,4" />

  <!-- Layer 4: Userspace Biometric Stack -->
  <g transform="translate(50, 565)">
    <rect width="1100" height="85" rx="12" fill="url(#cardGrad)" stroke="#334155" stroke-width="1.5" />
    <rect x="0" y="0" width="6" height="85" rx="3" fill="#10b981" />
    <text x="30" y="32" font-family="system-ui, sans-serif" font-size="18" font-weight="700" fill="#34d399">4. USERSPACE BIOMETRIC ENGINE</text>

    <rect x="30" y="45" width="510" height="30" rx="6" fill="#064e3b" stroke="#059669" stroke-width="1" />
    <text x="285" y="65" font-family="system-ui, sans-serif" font-size="12" font-weight="600" fill="#a7f3d0" text-anchor="middle">libfprint-2.so (FT9362 Match-on-Host Algorithmic Driver)</text>

    <rect x="560" y="45" width="510" height="30" rx="6" fill="#064e3b" stroke="#059669" stroke-width="1" />
    <text x="815" y="65" font-family="system-ui, sans-serif" font-size="12" font-weight="600" fill="#a7f3d0" text-anchor="middle">fprintd.service (D-Bus System Daemon)</text>
  </g>

  <!-- Connectors -->
  <path d="M600,650 V675" stroke="#10b981" stroke-width="2" />

  <!-- Layer 5: Linux OS & Security Integration -->
  <g transform="translate(50, 675)">
    <rect width="1100" height="90" rx="12" fill="url(#cardGrad)" stroke="#10b981" stroke-width="2" />
    <rect x="0" y="0" width="6" height="90" rx="3" fill="url(#emeraldAccent)" />
    <text x="30" y="30" font-family="system-ui, sans-serif" font-size="18" font-weight="700" fill="#4ade80">5. PAM AUTHENTICATION &amp; OPERATING SYSTEM</text>

    <g transform="translate(30, 42)">
      <rect width="245" height="36" rx="6" fill="#064e3b" stroke="#10b981" stroke-width="1" />
      <text x="122" y="23" font-family="system-ui, sans-serif" font-size="13" font-weight="bold" fill="#f0fdf4" text-anchor="middle">✓ sudo Authentication</text>
    </g>

    <g transform="translate(290, 42)">
      <rect width="245" height="36" rx="6" fill="#064e3b" stroke="#10b981" stroke-width="1" />
      <text x="122" y="23" font-family="system-ui, sans-serif" font-size="13" font-weight="bold" fill="#f0fdf4" text-anchor="middle">✓ Polkit-1 Privilege Gate</text>
    </g>

    <g transform="translate(550, 42)">
      <rect width="245" height="36" rx="6" fill="#064e3b" stroke="#10b981" stroke-width="1" />
      <text x="122" y="23" font-family="system-ui, sans-serif" font-size="13" font-weight="bold" fill="#f0fdf4" text-anchor="middle">✓ Lock Screen (Super+Ctrl+L)</text>
    </g>

    <g transform="translate(810, 42)">
      <rect width="260" height="36" rx="6" fill="#064e3b" stroke="#10b981" stroke-width="1" />
      <text x="130" y="23" font-family="system-ui, sans-serif" font-size="13" font-weight="bold" fill="#f0fdf4" text-anchor="middle">✓ Clamshell Gate Protection</text>
    </g>
  </g>
</svg>
"""

with open("/home/titan/fte3600-linux-driver/linkedin/architecture_diagram.svg", "w") as f:
    f.write(arch_svg)

print("Saved architecture_diagram.svg")
subprocess.run(["rsvg-convert", "-w", "1200", "-h", "800", "/home/titan/fte3600-linux-driver/linkedin/architecture_diagram.svg", "-o", "/home/titan/fte3600-linux-driver/linkedin/architecture_diagram.png"])
print("Converted to architecture_diagram.png")
