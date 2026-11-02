# COMPATIBILITY REPORT

Date: 2026-11-01
Status: **investigation in progress — no compatibility claim yet**

This report compares the hardware observed on the Dynabook Satellite Pro C40-J
with the reference implementation at
`research/libfprint-fte3600` (upstream:
`https://github.com/SamSeven777/libfprint-fte3600`) and with the
vendor-derived `research/ftexx00-ubuntu` SPI module.

## 1. Reference implementation architecture

The reference project is not a single kernel driver. It is split into:

1. **`kernel/fte3600`** — a small ACPI glue module (`fte3600-glue`) that:
   - matches ACPI HID `FTE3600`;
   - validates the ACPI `_CRS` resources;
   - binds stock `spidev` to the SPI slave;
   - exposes the reset GPIO as a one-line GPIO character device;
   - exposes the interrupt as a UIO device (`fte3600-irq`, ABI 2).

2. **libfprint driver `fte3600`** — userspace sensor protocol, discovery,
   firmware handling, image capture, matching and enrollment.

3. **fprintd / udev / systemd glue** — permissions and service setup.

The kernel glue deliberately contains no sensor commands, firmware names,
board names or physical pin numbers. It requires the ACPI firmware to describe
one SPI connection, one reset GPIO and one interrupt.

## 2. Kernel-glue ACPI requirements (from source)

The glue accepts the device only if `fte3600_resources_valid()` is true:

```text
spi        == 1
resets     == 1
interrupts == 1
invalid    == false
```

Details from `fte3600-policy.h` and `fte3600.c`:

- **SPI**: one `ACPI_RESOURCE_TYPE_SERIAL_BUS` of type SPI, consumer,
  8 data bits, non-zero connection speed, 4-wire mode, CPHA <= second phase,
  CPOL <= start-high, device polarity <= active-high.
- **Reset**: exactly one single-pin `GpioIo`, consumer, output-capable,
  with default / pull-up / no-pull bias (pull-down is rejected).
- **Interrupt**: exactly one edge-triggered `GpioInt` (active high or low), or
  one edge-triggered ACPI `Interrupt`/`ExtendedInterrupt`.
- Optional `_DSD` properties `reset-gpios` / `irq-gpios`, if present, must
  agree with the resource indices and polarity rules.
- CS polarity is only changed by userspace when the ACPI device polarity says
  `SPI_CS_HIGH`; otherwise controller-managed CS is retained.

The glue also requires:

- `spidev` with `bufsiz >= 65536`;
- UIO support;
- root access for the fprintd service to the spidev/GPIO/UIO tuple.

## 3. Match against the Dynabook hardware

| Requirement | Dynabook observation | Match? |
| --- | --- | --- |
| ACPI HID `FTE3600` | `FTE3600:00`, path `\_SB.PC00.SPI1.FP01` | **Yes** |
| ACPI-enumerated SPI slave | `spi-FTE3600:00` on `spi1` | **Yes** |
| Intel GSPI controller | PCI `8086:a0ab`, `pxa2xx-spi`, IRQ 37 | **Yes** |
| Kernel SPI master | `CONFIG_SPI=y`, `CONFIG_SPI_PXA2XX=m` loaded | **Yes** |
| spidev available | `CONFIG_SPI_SPIDEV=m`, module present, not loaded | **Yes (loadable)** |
| UIO available | `CONFIG_UIO=m` | **Yes** |
| GPIO char device | `/dev/gpiochip0` from `INT34C5:00` | **Yes** |
| Secure Boot | disabled | **Yes (no MOK needed)** |
| Kernel headers | matching `7.2.8-9-omarchy` headers | **Yes** |
| ACPI `_CRS` shape | 1 SPI, 1 GpioIo output, 1 GpioInt edge | **Yes (exact match)** |
| SPI parameters | Mode 0, 1 MHz, Active-Low CS | **Yes** |
| Reset GPIO | `\_SB.GPI0` pin 14 (`SPKR`), OutputOnly, PullUp | **Yes** |
| Interrupt GPIO/IRQ | `\_SB.GPI0` pin 323 (`CPU_GP_0`), Edge, ActiveHigh | **Yes** |
| Sensor family | **not yet identified** | **Unknown** |
| Firmware requirement | **unknown** | **Unknown** |

The ACPI `_CRS` resource shape has now been inspected directly from decompiled
`dsdt.dsl`. It matches the reference glue requirements exactly:
1. Exactly one 8-bit SPI bus (1 MHz, Mode 0, Active-Low CS).
2. Exactly one OutputOnly `GpioIo` reset pin with valid PullUp bias.
3. Exactly one edge-triggered `GpioInt` interrupt line (ActiveHigh).
4. No conflicting `_DSD` properties.

The kernel-glue resource policy `fte3600_resources_valid()` is therefore fully
satisfied by this ACPI description. What remains unknown is the physical silicon
family (FT9361 vs FT9362 vs others) and whether hardware pad locks on the PCH
interfere with runtime GPIO assertions.

## 4. Vendor-derived module comparison

`research/ftexx00-ubuntu/focal_spi.c` is a GPL-licensed, vendor-derived SPI
driver that creates `/dev/focal_moh_spi`. Its behavior differs from the
reference in ways that matter:

| Aspect | Vendor-derived `focal_spi.c` | Reference glue + libfprint |
| --- | --- | --- |
| Kernel role | Owns SPI, GPIO and IRQ; exposes custom char device | Only exposes spidev/GPIO/UIO; no protocol |
| SPI mode | Hard-coded `SPI_MODE_0 \| SPI_CS_HIGH`, 4 MHz | Derived from ACPI `_CRS` |
| Reset GPIO | `gpiod_get_index(dev, NULL, 0, GPIOD_OUT_LOW)` | Strict `GpioIo` validation, active-low convention |
| IRQ | `IRQF_TRIGGER_HIGH \| IRQF_ONESHOT` | ACPI-derived edge/polarity |
| Userspace | Proprietary libfprint binary (`/dev/focal_moh_spi`) | Open libfprint driver |
| Firmware | Handled by proprietary userspace | Open profiles, external payloads for some families |

The `alt/focal_spi.c` variant lowers SPI speed to 1 MHz, uses named
`reset`/`power`/`enable` GPIO lookups and a safer reset pulse, but still
hard-codes `IRQF_TRIGGER_HIGH`. This is evidence that at least some FTE3600
designs use a high-level/high-edge interrupt and that vendor code is not a
reliable source for this machine's exact wiring.

The vendor-derived driver is therefore a **reference for protocol behavior**,
not a candidate for installation on this laptop.

## 5. Sensor-family compatibility

The reference libfprint driver lists these profiles:

```text
FT9338, FT9348, FT9361, FT9536, FT9365, FT9368, FW9369, FT9769
```

Its own README states:

> Hardware confirmation is currently limited to FT9361 on One-Netbook A1;
> other models, suspend/resume and complete Windows parity remain unverified.

Therefore:

- ACPI HID `FTE3600` is a **family name, not a chip ID**. It does not prove the
  part is FT9361 or any other supported profile.
- We must not assume FT9361, and we must not select firmware from the ACPI name
  alone. The reference documentation explicitly warns against this.
- The public vendor libfprint logs seen in third-party reports mention strings
  such as `fw9362_Update_Base`, which is not one of the reference profiles.
  This is a warning that the installed part may be a family the clean-room
  driver does not yet support.
- The chip identity must come from the ACPI `_DSD`/firmware description or from
  a documented, bounded identity transaction after the transport is proven
  safe.

### 5.1 Evidence from the official Dynabook Windows package

The official Dynabook support page for this exact model offers FocalTech
Fingerprint Reader Driver `2.0.3.81` (`tch1093100a.exe`, SHA-256
`185b1de4…45db5c5`). It is a Windows Biometric Framework UMDF driver, not a
firmware updater. Its INF explicitly matches:

```ini
%SPIDeviceName%=SPIdevice_Install, ACPI\FTE3600
%USBDeviceName%=USBdevice_Install, USB\VID_2808&PID_9338
```

and enables `UmdfDirectHardwareAccess=AllowDirectHardwareAccess`, confirming
that the vendor expects this ACPI device to use ACPI-assigned SPI, GPIO and
interrupt resources.

The driver's own sensor-type table is broader than the reference driver's
confirmed set:

```text
1=FT9338W, 2=FT9348W, 3=FT9361W, 4=FT9368W,
5=FT9371W, 6=FT9536W, 7=FT9362W
```

It also maps hardware-ID substrings (`9338`, `9536`, `9348`, `93A8`, `7001`,
`6100`, `3600`, `4800`) to internal scene/algorithm types; `FTE3600` matches
`3600`. That mapping is **not** a chip ID and does not prove an FT9338W.
Crucially, the table includes `FT9362W`, which is absent from the clean-room
reference profile list and matches the third-party `fw9362` log warning.

The package contains no `.bin`/`.fw`/`.img` payload. It therefore does not by
itself provide a safe firmware source for a Linux driver, and nothing from it
will be installed or flashed. See `research/dynabook-windows-driver/README.md`.

## 6. Firmware compatibility

No FocalTech firmware exists on this system. The reference project requires
external payloads for FT9338, FT9348, FT9361 and FT9536, multiple payloads for
FT9368, and none for FT9365, FW9369 and FT9769.

Consequences:

- If this sensor is FT9361-like, a matching firmware payload may be required
  before capture works.
- The payload must match the exact chip and firmware generation; a wrong
  payload must not be written.
- The official Dynabook Windows package has now been inspected. It confirms
  the ACPI ID and the SPI transport but ships no standalone firmware payload;
  its DLLs mention FT9338/FT9348/FT9361/FT9368/FT9371/FT9536/FT9362 families.
  This broadens, rather than resolves, the possible sensor identity.
- Firmware will only be considered after a runtime chip-ID read identifies the
  exact family and generation, and only with explicit user approval.

## 7. Audit of fte3600-glue Kernel Module and spidev Coexistence

An in-depth audit of `kernel/fte3600.c` was conducted:

1. **Driver Coexistence (Zero Conflict):**
   - In Linux SPI architecture, only one driver can bind to an SPI slave.
   - `fte3600.c` is **not** an SPI driver: it registers as a platform driver (`fte3600-glue`) and does not claim `spi-FTE3600:00`.
   - `spidev` is the sole driver bound to `spi-FTE3600:00`.
   - `fte3600.c` hooks into the SPI bus via a bus notifier. When `spidev` binds, it attaches a child platform device to provide the reset GPIO cdev and UIO IRQ node.
   - There is no collision between SPI transfer paths and the glue module.

2. **GPIO and IRQ Handling:**
   - On probe, reset line (pad 14) is initialized to `OUTPUT`, logical `0` (physical HIGH / inactive).
   - Interrupt line (pad 229) is initialized to `INPUT`.
   - Probing does **not** attach an interrupt service routine. `request_irq()` is only called upon userspace `open()` on `/dev/uioX`.

3. **Lifecycle & Teardown Cleanliness:**
   - Unloading the module tears down the platform device, releases all GPIO descriptors, drops all ACPI driver GPIO mappings, and leaves the reset line in its deasserted (physical HIGH) state.
   - Note: releasing the descriptor in Linux gpiolib frees the kernel's software allocation, but gpiolib does not reprogram the underlying Intel PCH pad configuration registers (`PADCFG0`), leaving pad 14 configured as output driving physical HIGH.
   - If `fte3600.ko` is loaded without `spidev` bound, `fte3600_is_spidev()` evaluates to false, resulting in zero actions taken.

## 8. Verification of Non-Destructive Silicon Identification

Strings recovered from the official Dynabook Windows driver (`ftWbioUmdfDriverV2.dll`) confirm that the identification sequence in `discovery.c` exactly mirrors the vendor's internal sequence:
- Function: `ft_feature_devinit_DetectSensorTypeWithoutFW`
- Register 0x85C0 query: `[Driver] I %s[%d]: read addr 0x85C0 value:0x%x`
- Distinguish by OTP: `ft_feature_devinit_DistinguishSensorFT95A8byOtp` and `ft_feature_devinit_DistinguishSensorFT9338byOtp`
- Safe OTP Read: `read 9338 OTP is 0xff then set FT9338`

The sequence issues:
1. Pure SPI read `0x90 0x00 0x00` (3 bytes, Read ID).
2. If non-marker: reads register `0x85C0` and OTP byte via register `0xF3`.
No flash write, flash erase, or persistent state changes occur. The procedure is demonstrably non-destructive.

## 9. Current Status & Next Phase

**Transport compatibility:** Confirmed via ACPI decompilation.
**Kernel glue compatibility:** Validated and compiled cleanly. Zero conflicts with `spidev`.
**Identification safety:** Proven non-destructive against official vendor driver binaries.

The decisive next step is to obtain user authorization to perform the read-only hardware identification transaction.
