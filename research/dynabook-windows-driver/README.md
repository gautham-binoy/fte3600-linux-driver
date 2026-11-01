# Dynabook Windows FocalTech driver package (analysis only)

Source: Dynabook official support page for model `PYS46G-06J02D`
(Satellite Pro C40-J), Windows 11 64-bit driver list.

- Package: `FocalTech Fingerprint Reader Driver`
- Version: `2.0.3.81`
- File: `tch1093100a.exe`
- Size: 2,175,016 bytes
- SHA-256: `185b1de49762f421720dbf7abfcdd501925c559c18c1947e86ad4144245db5c5`
- Direct URL (Dynabook content CDN):
  `https://content.us.dynabook.com/content/support/downloads/tch1093100a.exe`

This directory contains the vendor package and extracted files **for
interoperability analysis only**. The files remain the property of their
respective copyright holders. Nothing here is redistributed, installed, or
flashed. Do not copy binaries or firmware into a distribution package without
checking the vendor licence.

## What the package is

A WinZip self-extracting archive containing a Windows Biometric Framework
UMDF driver:

```text
Drivers/ftWbioUmdfDriverV2.inf
Drivers/ftWbioUmdfDriverV2.dll
Drivers/ftWbioEngineAdapter.dll
Drivers/ftwbioumdfdriverv2.cat
Install.bat
PnPDriver.txt
tinstall.exe
tinstallwb.exe
tinstallwb.exe.ini
WBDPB44I.DLL
WBTOS45I.DLL
```

It is a driver package, not a standalone firmware update. No `.bin`, `.fw`,
`.img` or `.hex` payload is present in the archive.

## Relevant INF facts

```ini
[Standard.NTamd64]
%SPIDeviceName%=SPIdevice_Install, ACPI\FTE3600
%USBDeviceName%=USBdevice_Install, USB\VID_2808&PID_9338
```

The SPI installation uses `UmdfDirectHardwareAccess=AllowDirectHardwareAccess`,
so the UMDF driver talks to the SPI/GPIO/interrupt resources that ACPI assigns
to the device. The INF does not set SPI mode, speed, GPIO numbers, or firmware
names; those must come from ACPI `_CRS`/`_DSD`.

## Sensor types named in the driver

The UMDF binary logs this sensor-type table:

```text
1=FT9338W, 2=FT9348W, 3=FT9361W, 4=FT9368W,
5=FT9371W, 6=FT9536W, 7=FT9362W
```

The driver also contains a hardware-ID substring mapping for `9338`, `9536`,
`9348`, `93A8`, `7001`, `6100`, `3600`, `4800`. The ACPI ID `FTE3600`
matches `3600`, but that mapping selects an internal scene/algorithm type, not
a proven silicon chip ID. The actual sensor type is detected at runtime from
the chip. **Do not treat this package as proof that the laptop contains an
FT9338W.**

## Why this matters for the Linux port

- The vendor driver confirms that `ACPI\FTE3600` is expected to work over SPI
  with ACPI-provided resources.
- The supported list includes `FT9362W`, which is not in the clean-room
  reference driver's profile list. The installed part may therefore be
  unsupported by the reference libfprint driver.
- The Windows engine adapter maps only `FT9338`, `FT9348`, `FT9361`, `FW9368`
  and `FW9371`; other strings are present in the UMDF transport layer. The
  exact mapping for this machine still requires a runtime chip-ID read.

## Extracted text for review

`strings/` contains ASCII and UTF-16 string dumps of the binaries. These are
analysis artefacts, not source code.
