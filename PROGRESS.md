# PROGRESS

## 2026-11-01

### Stage

Phase 1 — hardware inspection (mostly complete, waiting on privileged ACPI
collection) and Phase 2 — compatibility investigation (in progress).

### Completed

- Inspected OS, kernel, architecture, DMI/BIOS and Secure Boot state.
- Located the fingerprint device: ACPI `FTE3600:00` at `\_SB.PC00.SPI1.FP01`.
- Confirmed it is an SPI slave on Intel GSPI #1 (`00:1e.3`, `spi1`), not the
  SPI NOR controller (`spi0`).
- Confirmed no driver is bound and no SPI transfers have occurred.
- Confirmed the Synaptics SYNA3602 is an I2C touchpad at
  `\_SB.PC00.I2C0.TPD1`, unrelated to the fingerprint reader.
- Confirmed Secure Boot is disabled.
- Confirmed fprintd, libfprint and any fingerprint daemon are not installed.
- Confirmed the running kernel is `7.2.8-9-omarchy` with matching headers at
  `/usr/lib/modules/7.2.8-9-omarchy/build`.
- Created the project directory layout and read-only diagnostics scripts.
- Cloned and began reviewing the reference implementation
  `SamSeven777/libfprint-fte3600`.
- Reviewed the vendor-derived `FTEXX00-Ubuntu` kernel module and its `alt`
  variant for comparison (SPI mode, reset GPIO, IRQ handling).
- Identified the reference kernel glue architecture: ACPI resource validation,
  spidev, GPIO character device and UIO IRQ, with no sensor protocol in kernel.
- Identified the reference libfprint discovery protocol and supported sensor
  families.

### Commands executed (selection)

```text
uname -a
cat /etc/os-release
cat /sys/bus/spi/devices/spi-FTE3600:00/{modalias,uevent}
cat /sys/bus/acpi/devices/FTE3600:00/{hid,path,status,modalias}
ls -l /sys/bus/spi/devices/
ls -l /sys/bus/gpio/devices/
cat /proc/interrupts
lsmod
journalctl -k -b
pacman -Q
git clone https://github.com/SamSeven777/libfprint-fte3600 research/libfprint-fte3600
./scripts/collect-hardware.sh > diagnostics/host-inventory.txt
```

### Files created

```text
README.md
PROGRESS.md
HARDWARE_REPORT.md
COMPATIBILITY_REPORT.md
scripts/collect-hardware.sh
scripts/collect-privileged.sh
scripts/extract_acpi_blocks.py
diagnostics/host-inventory.txt
research/libfprint-fte3600/        (upstream reference clone)
research/ftexx00-ubuntu/           (vendor-derived reference clone)
research/fte-spi-arch/             (Arch packaging reference clone)
research/aur-focaltech-spi-dkms/   (AUR packaging reference clone)
research/aur-libfprint-ftexx00/    (AUR packaging reference clone)
research/fte3600-kvadra-notes/     (tested-setup notes clone)
```

### Errors / limitations encountered

- `sudo` requires a password, so ACPI tables and debugfs GPIO/pinctrl state
  cannot be read by the agent non-interactively. A read-only root script is
  prepared; the user must run it or approve a privileged run.
- `dmidecode` and `libgpiod` are not installed. DMI data was obtained from
  `/sys/class/dmi/id`; GPIO state will be obtained from debugfs instead.
- `meson` and `ninja` are not installed, so libfprint cannot yet be built.
  This will be addressed only after hardware compatibility is established.
- The `ftfpteams/ubuntu_spi` repository is unavailable (DMCA takedown); the
  vendor-derived mirror in `FTEXX00-Ubuntu` is being used only as a reference.

### Tests passed

- None of the driver/enrollment tests are applicable yet. No driver is loaded
  and no hardware access has been performed.

### Tests failed

- None executed.

### Remaining work

- Collect ACPI DSDT/SSDT and confirm the FTE3600 `_CRS`/`_DSD` resources.
- Map the reset and interrupt GPIOs to the Tiger Lake pinctrl controller.
- Determine the sensor silicon family.
- Decide whether the reference kernel glue and libfprint driver are compatible.
- Build (but do not install) the kernel glue and libfprint driver.
- Obtain explicit approval for any privileged/hardware-testing step.

### Next action

Run the prepared read-only privileged collector with root privileges:

```sh
sudo /home/titan/fte3600-linux-driver/scripts/collect-privileged.sh
```

Then parse `diagnostics/acpi/` and `diagnostics/privileged/` to identify the
SPI mode, reset GPIO, IRQ GPIO/polarity and any firmware properties.

### Additional investigation — official Dynabook Windows driver package

- Queried the Dynabook support content API and obtained the direct URL for the
  FocalTech Windows driver `2.0.3.81` (`tch1093100a.exe`).
- Downloaded it to `research/dynabook-windows-driver/` and verified:
  - size 2,175,016 bytes
  - SHA-256 `185b1de49762f421720dbf7abfcdd501925c559c18c1947e86ad4144245db5c5`
  - WinZip self-extracting PE32 archive containing a UMDF driver.
- Extracted and inspected the INF. It matches `ACPI\FTE3600` and
  `USB\VID_2808&PID_9338`; the SPI install enables direct hardware access.
- Confirmed the package contains no standalone `.bin`/`.fw`/`.img` payload.
- Found the driver's sensor-type table:
  `1=FT9338W, 2=FT9348W, 3=FT9361W, 4=FT9368W, 5=FT9371W, 6=FT9536W,
  7=FT9362W`.
- Found a hardware-ID substring map (`9338`, `9536`, `9348`, `93A8`, `7001`,
  `6100`, `3600`, `4800`). `FTE3600` matches `3600`, but this only selects an
  internal scene/algorithm type, **not** a proven chip ID.
- The presence of `FT9362W` supports the earlier warning that this machine may
  use a family the clean-room driver does not yet support.
- Wrote `research/dynabook-windows-driver/README.md` and string dumps under
  `research/dynabook-windows-driver/strings/`.
- Updated `COMPATIBILITY_REPORT.md` sections 5.1 and 6.

No installation, firmware write or privileged operation was performed.
