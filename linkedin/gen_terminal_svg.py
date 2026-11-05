#!/usr/bin/env python3
import subprocess

svg_content = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1200 800" width="1200" height="800">
  <defs>
    <linearGradient id="bgGrad" x1="0%" y1="0%" x2="100%" y2="100%">
      <stop offset="0%" stop-color="#0a0e17"/>
      <stop offset="100%" stop-color="#06090e"/>
    </linearGradient>
    <linearGradient id="termGrad" x1="0%" y1="0%" x2="0%" y2="100%">
      <stop offset="0%" stop-color="#141923"/>
      <stop offset="100%" stop-color="#0d1117"/>
    </linearGradient>
    <filter id="shadow" x="-5%" y="-5%" width="110%" height="110%">
      <feDropShadow dx="0" dy="16" stdDeviation="24" flood-color="#000000" flood-opacity="0.7"/>
    </filter>
  </defs>

  <!-- Canvas Background -->
  <rect width="1200" height="800" fill="url(#bgGrad)"/>
  
  <!-- Subtle decorative grid -->
  <g stroke="#1a2333" stroke-width="1" opacity="0.3">
    <line x1="0" y1="160" x2="1200" y2="160"/>
    <line x1="0" y1="320" x2="1200" y2="320"/>
    <line x1="0" y1="480" x2="1200" y2="480"/>
    <line x1="0" y1="640" x2="1200" y2="640"/>
    <line x1="240" y1="0" x2="240" y2="800"/>
    <line x1="480" y1="0" x2="480" y2="800"/>
    <line x1="720" y1="0" x2="720" y2="800"/>
    <line x1="960" y1="0" x2="960" y2="800"/>
  </g>

  <!-- Terminal Window Frame -->
  <g filter="url(#shadow)" transform="translate(60, 40)">
    <!-- Outer Border / Shadow Base -->
    <rect width="1080" height="720" rx="14" fill="none" stroke="#238636" stroke-width="2" opacity="0.4"/>
    <rect width="1080" height="720" rx="14" fill="url(#termGrad)"/>
    
    <!-- Titlebar -->
    <path d="M 0,14 Q 0,0 14,0 L 1066,0 Q 1080,0 1080,14 L 1080,44 L 0,44 Z" fill="#161b22"/>
    <line x1="0" y1="44" x2="1080" y2="44" stroke="#30363d" stroke-width="1"/>

    <!-- Window controls -->
    <circle cx="28" cy="22" r="7" fill="#ff5f56"/>
    <circle cx="50" cy="22" r="7" fill="#ffbd2e"/>
    <circle cx="72" cy="22" r="7" fill="#27c93f"/>

    <!-- Window title -->
    <text x="540" y="27" fill="#8b949e" font-family="'JetBrains Mono', 'Fira Code', 'DejaVu Sans Mono', monospace" font-size="13" font-weight="600" text-anchor="middle">titan@omarchy-laptop: ~/fte3600-linux-driver — zsh</text>

    <!-- Terminal Content -->
    <g font-family="'JetBrains Mono', 'Fira Code', 'DejaVu Sans Mono', monospace" font-size="13.5" xml:space="preserve">
      <!-- Step 1: Tool verification -->
      <text x="32" y="80" fill="#79c0ff" font-weight="700">~ ❯ </text>
      <text x="65" y="80" fill="#e6edf3">fte3600-tool calibrate</text>
      <text x="32" y="104" fill="#3fb950">>> FDT analog front-end stabilized: Channel 0..3 thresholds committed</text>
      <text x="32" y="126" fill="#3fb950">>> Calibration committed: sensor armed, state READY (state=4)</text>

      <!-- Step 2: Omarchy setup command -->
      <text x="32" y="160" fill="#79c0ff" font-weight="700">~ ❯ </text>
      <text x="65" y="160" fill="#e6edf3" font-weight="600">omarchy setup security fingerprint</text>
      
      <text x="32" y="186" fill="#8b949e">Setting up fingerprint scanner for authentication.</text>
      <text x="32" y="208" fill="#d2a8ff">Let's setup your right index finger as the first fingerprint.</text>
      <text x="32" y="228" fill="#8b949e">Keep moving the finger around on sensor until the process completes.</text>

      <text x="32" y="258" fill="#58a6ff">Using device /net/reactivated/Fprint/Device/0 (FocalTech FT9362 SPI)</text>
      <text x="32" y="280" fill="#e6edf3">Enrolling right-index-finger finger...</text>

      <!-- Enrollment passes -->
      <text x="48" y="306" fill="#3fb950">✓ Enroll stage 1..4: passed (capture 64x80 matrix)</text>
      <text x="48" y="326" fill="#e3b341">⚠ Enroll swipe: swipe-too-short (adaptive AFE threshold compensation)</text>
      <text x="48" y="346" fill="#3fb950">✓ Enroll stage 5..18: passed</text>
      <text x="48" y="366" fill="#3fb950">✓ Enroll stage 19..20: passed</text>
      <text x="48" y="392" fill="#2ea043" font-weight="700">>> Enroll result: enroll-completed</text>
      <text x="48" y="414" fill="#56d364" font-weight="700">Fingerprint enrolled successfully!</text>

      <!-- Verification step -->
      <text x="32" y="446" fill="#d2a8ff">Now let's verify that it's working correctly...</text>
      <text x="32" y="468" fill="#e6edf3">Listing enrolled fingers: [0] right-index-finger</text>
      <text x="32" y="490" fill="#79c0ff">Verify started! Verifying: right-index-finger...</text>
      
      <!-- MATCH RESULT BANNER -->
      <rect x="32" y="504" width="720" height="32" rx="6" fill="#1b4728" stroke="#3fb950" stroke-width="1.5"/>
      <text x="48" y="525" fill="#7ee787" font-weight="700" font-size="14.5">★ Verify result: VERIFY-MATCH (done) — Biometrics Authenticated!</text>

      <!-- PAM & System security configuration -->
      <text x="32" y="562" fill="#58a6ff">Configuring sudo for fingerprint authentication...</text>
      <text x="32" y="584" fill="#3fb950">✓ Adding clamshell gate to sudo (lid state detection active)</text>
      <text x="32" y="606" fill="#3fb950">✓ Creating polkit-1 configuration with fingerprint authentication</text>
      <text x="32" y="628" fill="#3fb950">✓ Configuring lock screen for fingerprint authentication</text>

      <!-- Final status box -->
      <rect x="32" y="646" width="1016" height="50" rx="8" fill="#161b22" stroke="#58a6ff" stroke-width="1"/>
      <text x="48" y="670" fill="#58a6ff" font-weight="700">System Ready:</text>
      <text x="175" y="670" fill="#e6edf3">Fingerprint authentication active for </text>
      <text x="475" y="670" fill="#79c0ff" font-weight="700">sudo</text>
      <text x="515" y="670" fill="#e6edf3">, </text>
      <text x="530" y="670" fill="#79c0ff" font-weight="700">polkit</text>
      <text x="585" y="670" fill="#e6edf3">, and </text>
      <text x="635" y="670" fill="#79c0ff" font-weight="700">Lock Screen (Super + Ctrl + L)</text>
      <text x="48" y="688" fill="#8b949e" font-size="12">Kernel module: fte3600.ko (Linux 6.x) | Driver: libfprint-2.so (Match-on-Host FT9362)</text>
    </g>
  </g>
</svg>
"""

svg_path = "/home/titan/fte3600-linux-driver/linkedin/terminal_proof.svg"
png_path = "/home/titan/fte3600-linux-driver/linkedin/terminal_proof.png"

with open(svg_path, "w") as f:
    f.write(svg_content)

print("Saved terminal_proof.svg")
subprocess.run(["rsvg-convert", "-w", "1200", "-h", "800", svg_path, "-o", png_path], check=True)
print("Converted to terminal_proof.png")
