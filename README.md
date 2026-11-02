# FTE3600 Linux driver for Dynabook Satellite Pro C40-J

End-to-end research and engineering project for the built-in FocalTech FTE3600
SPI fingerprint reader in a **Dynabook Satellite Pro C40-J** running
**Omarchy Linux (Arch Linux)**.

> Status: **Phase 1/2 — hardware inspection and compatibility investigation.**
> No driver has been installed, no kernel module has been loaded, no SPI
> transfer has been performed, and no system configuration has been changed.

## The machine

| Item | Value |
| --- | --- |
| Laptop | Dynabook Satellite Pro C40-J (`PYS46G-06J02D`) |
| Board | DBIIP306 V3.0 |
| BIOS | AMI `TN16SV1.16`, 2023-02-08 |
| CPU | Intel Core i5-1135G7 (Tiger Lake) |
| OS | Omarchy 4.0.6 (Arch Linux based) |
| Running kernel | `7.2.8-9-omarchy` (x86_64) |
| Fingerprint ACPI device | `FTE3600:00`, path `\_SB.PC00.SPI1.FP01` |
| Fingerprint SPI bus | Intel GSPI #1 at PCI `00:1e.3`, Linux `spi1` |
| Touchpad (not fingerprint) | `SYNA3602:00` on I2C0, `\_SB.PC00.I2C0.TPD1` |
| Secure Boot | disabled (`SecureBoot=0`) |
| fprintd / libfprint | not installed |

## What is confirmed

- The fingerprint reader is enumerated as an ACPI SPI device with HID
  `FTE3600`, modalias `acpi:FTE3600:FTE3600:`.
- It sits on the Intel Tiger Lake GSPI #1 controller (`8086:a0ab`) driven by
  `pxa2xx-spi`, not on the SPI NOR flash controller (`spi0`).
- ACPI `_CRS` specifies SPI Mode 0, 1 MHz (`0x000F4240`), Active-Low CS.
- ACPI `_CRS` specifies Reset GPIO: `GpioIo` on `\_SB.GPI0`, pin 14 (pad 14, SPKR), OutputOnly, PullUp.
- ACPI `_CRS` specifies Interrupt GPIO: `GpioInt` on `\_SB.GPI0`, pin 323 (pad 229, CPU_GP_0), Edge, ActiveHigh.
- ACPI `_INI` executes `SHPO` setting host pad ownership for both GPIOs.
- ACPI `FP01` contains no `_DSD` node.
- No kernel driver is bound to it; all SPI transfer counters are zero.
- The Synaptics SYNA3602 is a separate I2C touchpad and is not the fingerprint
  sensor.
- Secure Boot is off, so an out-of-tree module does not need MOK signing on
  this machine.

## What is not yet known

- The exact sensor family/silicon variant (FT9338, FT9348, FT9361, FT9362,
  FT9368, FT9536, FW9369, FT9769, or another part).
- Whether PCH pad locks (`[LOCKED]`) restrict runtime reset GPIO assertions.
- Whether firmware download is required, and which firmware payload.
- Whether the existing open-source implementation's SPI protocol applies to this
  specific silicon.

These are being resolved from the ACPI tables and the reference projects, not
from guesses. See `COMPATIBILITY_REPORT.md`.

## Project layout

```
research/        cloned reference projects and protocol notes
diagnostics/     collected host inventory and ACPI dumps (no biometric data)
kernel/          out-of-tree kernel glue (not yet installed)
libfprint/       libfprint integration work
fprintd/         fprintd / PAM integration work
packaging/       Arch PKGBUILD / DKMS packaging
scripts/         read-only collectors and setup helpers
tests/           independent test procedures
docs/            design, installation and troubleshooting documents
```

Top-level documents:

- `PROGRESS.md` — chronological engineering log and current state
- `HARDWARE_REPORT.md` — what the machine actually exposes
- `COMPATIBILITY_REPORT.md` — comparison with existing implementations
- `DRIVER_DESIGN.md` — design (written after hardware identification)
- `INSTALLATION.md` — install/rollback procedure (not yet applicable)
- `TROUBLESHOOTING.md` — diagnostics and known failure modes
- `TEST_RESULTS.md` — only tests that were actually executed

## Safety rules followed by this project

- No BIOS or device firmware flashing.
- No blind SPI writes; every transfer must be justified by a documented
  protocol and performed on a device that has been correctly identified.
- No guessed GPIO numbers or interrupt polarity.
- No overwriting of PAM or system configuration without a backup and a
  password-authentication fallback.
- No automatic installation of an unverified kernel module.
- No raw fingerprint images or biometric templates are collected or stored.

## Reproducing the inspection

```sh
./scripts/collect-hardware.sh > diagnostics/host-inventory.txt 2>&1
sudo ./scripts/collect-privileged.sh     # read-only; ACPI tables + debugfs
```

The privileged collector does not load modules, bind drivers, change GPIO
state or write to SPI. It only reads ACPI tables and debugfs.
