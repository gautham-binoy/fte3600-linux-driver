# LinkedIn Announcement Pack: FocalTech FT9362 / FTE3600 Linux Driver

This directory contains publication-ready copy, technical breakdowns, and high-resolution visual assets to announce the **FocalTech FT9362 / FTE3600 Linux Fingerprint Driver & Match-on-Host Integration**.

---

## 📸 Media Assets Included

| File | Resolution | Description |
| :--- | :---: | :--- |
| [`linkedin_hero_banner.png`](./linkedin_hero_banner.png) | 1200 x 675 (16:9) | High-impact AI cyber/hardware banner depicting biometric scanning on Linux. |
| [`architecture_diagram.png`](./architecture_diagram.png) | 1200 x 800 | 5-tier architecture breakdown: Silicon ➔ Kernel ➔ Libfprint ➔ D-Bus ➔ PAM. |
| [`terminal_proof.png`](./terminal_proof.png) | 1200 x 800 | High-contrast terminal proof displaying live enrollment, `verify-match`, and PAM activation. |

> **Pro-Tip for LinkedIn Algorithm:** Attach all three images as a **multi-image post** in the order:
> 1. `linkedin_hero_banner.png` (Visual hook)
> 2. `terminal_proof.png` (Live hardware proof)
> 3. `architecture_diagram.png` (Technical architecture)
>
> *(Alternatively, post the GitHub repo URL in the first comment rather than the main text body to maximize feed impressions!)*

---

## 🚀 Post Option 1: The Engineering Story (Recommended)
*Best for broad reach, engagement, storytelling, and developer networking.*

```markdown
Every Linux user knows this heartbreak:

You buy a sleek, modern laptop with incredible hardware. You wipe Windows, install Linux, and fall in love with your setup... until you check your fingerprint scanner.

"No fingerprint sensor detected."
"Unsupported proprietary hardware."
Forums full of unanswered threads from 2022.

The culprit? The FocalTech FT9362 / FTE3600 biometric sensor wired to the Intel LPSS SPI2 bus. Zero Linux drivers. Zero open documentation. Total vendor lock-in.

Instead of accepting a dead sensor, we reverse-engineered the hardware protocol from scratch and built a production-grade Linux driver stack! 🐧⚡

Here is what we engineered:

1️⃣ Linux Kernel Driver (fte3600.ko):
• Native Linux 6.x ACPI platform driver binding directly to Intel LPSS SPI2.
• ACPI GPIO interrupt handling on INTEL0001 Pad 14.
• DMA-quarantined asynchronous SPI transfers to prevent kernel buffer clobbering.
• Thread-safe leased state machine with a 5000ms watchdog lease to prevent device starvation.
• Dynamic Analog Front-End (AFE) calibration and 22-byte capacitive threshold banking.

2️⃣ Match-on-Host Userspace Integration:
• Custom driver integrated directly into libfprint (v2.0).
• Ingests 64x80 16-bit raw capacitive matrix frames over /dev/fte3600.
• Minutiae extraction and matching powered by NIST Biometric Image Software (NBIS).
• Seamless D-Bus registration under /net/reactivated/Fprint/Device/0.

3️⃣ Full Linux PAM Security Integration:
• Native PAM wiring: sudo, Polkit authentication dialogs, and desktop lock screens authenticate in under 300ms.
• Automatic clamshell lid-state gating to prevent accidental triggers while docked.

The feeling of running:
$ omarchy setup security fingerprint
...
>> Enroll result: enroll-completed
>> Verify result: verify-match (done)
...and unlocking a Linux terminal with a single touch of the physical sensor is unmatched.

Hardware reverse engineering and kernel hacking remind me why open source is so powerful. If the vendor won't support your hardware, the community can build it.

Check out the architecture diagram and live terminal logs in the images below! 👇

Code, kernel module, and installation guides are open source on GitHub (link in the first comment)!

#Linux #KernelDevelopment #ReverseEngineering #EmbeddedSystems #OpenSource #OperatingSystems #Cybersecurity #Biometrics #ArchLinux #CProgramming #HardwareHacking
```

---

## 🛠️ Post Option 2: The Systems & Kernel Deep-Dive
*Best for systems programmers, kernel engineers, and technical recruiters.*

```markdown
From Silicon to PAM: Implementing a Linux Kernel & Match-on-Host Driver for the FocalTech FT9362 / FTE3600 SPI Fingerprint Sensor.

Most modern laptop fingerprint scanners run in proprietary firmware or "Match-on-Chip" setups. The FocalTech FT9362 / FTE3600, however, is a pure Match-on-Host capacitive matrix device operating over Intel LPSS SPI2 with an ACPI INTEL0001 Pad 14 edge interrupt.

Without manufacturer Linux support, bringing this sensor to life required architecting a full 5-tier software stack:

🔹 Hardware & Signal Level:
The sensor outputs a 64x80 16-bit capacitive pixel matrix (10,240 bytes raw payload) accompanied by high-frequency Analog Front-End (AFE) baseline shifts. We reverse-engineered the 22-byte threshold calibration bank to lock analog baselines before triggering acquisition frames.

🔹 Kernel Module (fte3600.ko):
• Implements dynamic character device nodes (/dev/fte3600 and /dev/focal_moh_spi).
• Enforces non-contiguous, cache-line aligned DMA quarantine buffers for SPI transceivers to prevent memory corruption under high I/O pressure.
• Strict ioctl() state machine (RESET -> INIT -> CALIBRATE -> CAPTURE) with a leasing token mechanism protecting against multi-process collision.

🔹 Libfprint Match-on-Host Architecture:
Because the sensor does not perform on-chip template hashing, raw frames are ingested by a custom libfprint driver module. The pipeline performs contrast normalization, minutiae extraction (MINDTCT), and biometric verification (BOZORTH3).

🔹 PAM & Desktop Integration:
Registered with fprintd over system D-Bus, allowing transparent authentication across pam_fprintd, sudo, polkit-1, and display managers with built-in clamshell lid detection.

Result: 100% biometric enrollment completion, sub-300ms authentication latency, and zero kernel warnings on Linux 6.x.

Open-source systems programming at its finest. Repository & documentation link below.

#LinuxKernel #SystemsProgramming #C #DriverDevelopment #EmbeddedLinux #ReverseEngineering #Biometrics #OperatingSystems
```

---

## ⚡ Post Option 3: Short & Punchy Announcement
*Best for fast impressions, mobile viewers, and quick updates.*

```markdown
They said this fingerprint sensor was "Windows Only."
We made it run natively on Linux. 🐧🚀

Presenting the open-source Linux kernel & libfprint driver for the FocalTech FT9362 / FTE3600 fingerprint sensor:

✅ Native Linux 6.x kernel module (fte3600.ko)
✅ Intel LPSS SPI2 & ACPI GPIO IRQ integration
✅ Libfprint v2.0 Match-on-Host support
✅ 20-stage physical biometric enrollment tested & verified
✅ Sub-300ms verification match for sudo, Polkit, and lock screens

Swipe through to see the system architecture and terminal verification logs! 📸

GitHub repository link in the comments. Let's make all hardware open! 💻

#Linux #OpenSource #Kernel #Embedded #ReverseEngineering #TechCommunity
```

---

## 💬 Suggested First Comment (To Include Link)

```markdown
🔗 Explore the full driver source code, architecture specifications, and installation scripts here:
👉 https://github.com/<your-username>/fte3600-linux-driver

Feedback, issues, and PRs are warmly welcomed! If you're on a laptop with an FT9362 / FTE3600 sensor, test it out and share your results.
```
