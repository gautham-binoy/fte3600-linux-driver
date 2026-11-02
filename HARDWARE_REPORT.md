# HARDWARE REPORT

Date of inspection: 2026-11-01
Host: `titan`
Raw inventory: `diagnostics/host-inventory.txt`

## 1. Platform

| Item | Observed value |
| --- | --- |
| Vendor | Dynabook Inc. |
| Product | SATELLITE PRO C40-J |
| Product version | PYS46G-06J02D |
| Board | DBIIP306 V3.0 |
| BIOS vendor | American Megatrends International, LLC. |
| BIOS version | TN16SV1.16 |
| BIOS date | 2023-02-08 |
| CPU | 11th Gen Intel Core i5-1135G7 |
| Architecture | x86_64 |
| OS | Omarchy 4.0.6 (Arch Linux based) |
| Running kernel | 7.2.8-9-omarchy |
| Kernel headers | `/usr/lib/modules/7.2.8-9-omarchy/build` present, version matches |
| Secure Boot | disabled (`SecureBoot=0`) |
| Boot | LUKS + btrfs, no Windows partition present |

## 2. Fingerprint device topology

```text
ACPI namespace
  \_SB.PC00.SPI1                    (Intel GSPI #1 controller)
    \_SB.PC00.SPI1.FP01             (ACPI device, HID FTE3600)

Linux device tree
  pci 0000:00:1e.3                  Intel Tiger Lake-LP GSPI #1, 8086:a0ab
    intel-lpss                      PCI glue
      pxa2xx-spi.2                  SPI controller
        spi_master/spi1
          spi-FTE3600:00            SPI slave device (no driver bound)
```

Confirmed sysfs values:

```text
/sys/bus/acpi/devices/FTE3600:00/hid       = FTE3600
/sys/bus/acpi/devices/FTE3600:00/path      = \_SB_.PC00.SPI1.FP01
/sys/bus/acpi/devices/FTE3600:00/status    = 15
/sys/bus/acpi/devices/FTE3600:00/modalias  = acpi:FTE3600:FTE3600:
/sys/bus/spi/devices/spi-FTE3600:00/modalias = acpi:FTE3600:FTE3600:
/sys/bus/spi/devices/spi-FTE3600:00/uevent  = MODALIAS=acpi:FTE3600:FTE3600:
```

ACPI status `15` means present, enabled, shown in UI and functioning. There is
no `driver` symlink under the SPI device; no driver has claimed it. All SPI
statistics for `spi-FTE3600:00` are zero, so no transfer has been attempted.

The SPI controller is `spi1`, not the SPI NOR flash controller `spi0`:

```text
spi0 -> PCI 00:1f.5, driver spi-nor, bound to spi0.0
spi1 -> PCI 00:1e.3, driver pxa2xx-spi, slave spi-FTE3600:00 unbound
```

## 3. SPI controller

| Item | Value |
| --- | --- |
| PCI address | `0000:00:1e.3` |
| PCI ID | `8086:a0ab` (Tiger Lake-LP GSPI #1) |
| PCI class | `0x0c8000` (serial bus controller) |
| Linux driver stack | `intel-lpss` -> `pxa2xx-spi` |
| Controller IRQ | 37 (`idma64.2, pxa2xx-spi.2`) |
| SPI master | `spi1` |
| Slave device | `spi-FTE3600:00` |
| ACPI SPI speed | 1,000,000 Hz (1 MHz, `0x000F4240`) |
| ACPI SPI mode | Mode 0 (CPOL=0, CPHA=0, 8 bits, 4-wire) |
| ACPI CS polarity | Active Low (`PolarityLow`) |

Kernel configuration relevant to this path:

```text
CONFIG_SPI=y
CONFIG_SPI_MASTER=y
CONFIG_SPI_PXA2XX=m
CONFIG_SPI_PXA2XX_PCI=m
CONFIG_SPI_SPIDEV=m
CONFIG_UIO=m
CONFIG_PINCTRL_TIGERLAKE=m
CONFIG_DEBUG_FS=y
```

`spidev` is available as a module but is not loaded. Its `bufsiz` parameter is
therefore not currently set; the reference implementation requires 65536.

## 4. GPIO

The Tiger Lake pinctrl controller is enumerated as `INT34C5:00` and exposes one
GPIO character device:

```text
/dev/gpiochip0        crw------- root root 254,0
/sys/devices/platform/INT34C5:00/gpiochip0
firmware_node: /sys/devices/LNXSYSTM:00/LNXSYBUS:00/INT34C5:00
driver: /sys/bus/platform/drivers/tigerlake-pinctrl
```

From `dsdt.dsl` decompiled inspection of `\_SB.PC00.SPI1.FP01._CRS`:

1. **Reset Pin:** `GpioIo` on `\_SB.GPI0`, OutputOnly, PullUp.
   - ACPI assignment: `RSTP = GNUM (0x0900000E)`.
   - In PCH group 0, offset 14 -> ACPI pin 14.
   - On `INT34C5:00`, this corresponds to pin 14 (`SPKR` pad).
   - In debugfs pinctrl: `pin 14 (SPKR) 14:INT34C5:00 GPIO ... [LOCKED] (MUX UNCLAIMED) (GPIO UNCLAIMED)`.

2. **Interrupt Pin:** `GpioInt` on `\_SB.GPI0`, Edge, ActiveHigh, ExclusiveAndWake, PullNone.
   - ACPI assignment: `INTP = GNUM (0x090E0003)`.
   - In PCH group 14, base 320, offset 3 -> ACPI pin 323.
   - On `INT34C5:00`, this corresponds to pin 229 (`CPU_GP_0` pad).
   - In debugfs pinctrl: `pin 229 (CPU_GP_0) 323:INT34C5:00 GPIO ... [LOCKED tx] (MUX UNCLAIMED) (GPIO UNCLAIMED)`.

3. **Pad Ownership (`_INI`):**
   `SHPO (0x090E0003, One)` and `SHPO (0x0900000E, One)` set host pad ownership for both lines to driver/OS control.

4. **Device Properties (`_DSD`):**
   `FP01` has no `_DSD` node. Named GPIO properties like `reset-gpios` do not exist in firmware.

The only GPIO interrupt currently visible in `/proc/interrupts` is the touchpad:

```text
159: ... intel-gpio 264 SYNA3602:00
```

There is no fingerprint IRQ because no driver has requested one yet.

## 5. Interrupts

Relevant `/proc/interrupts` entries at inspection time:

```text
14:  IR-IO-APIC 14-fasteoi   INT34C5:00
37:  IR-IO-APIC 37-fasteoi   idma64.2, pxa2xx-spi.2
159: intel-gpio 264          SYNA3602:00
```

From ACPI `_CRS`, the fingerprint interrupt is an edge-triggered active-high `GpioInt` on `\_SB.GPI0` pin 323 (pad 229, `CPU_GP_0`). The interrupt is not requested or claimed in the interrupt controller until userspace explicitly opens the `/dev/uioX` device created by the ACPI glue driver. Probing the driver does not attach an IRQ handler.

## 6. Firmware

No file matching `fte`, `focal`, `ft93*` or `ft97*` exists under
`/usr/lib/firmware`, `/lib/firmware` or `/usr/local/lib/firmware`.

Whether the sensor needs a firmware download is **not yet known**. The
reference implementation states that FT9338, FT9348, FT9361 and FT9536 need
external payloads, while FT9365, FW9369 and FT9769 do not (FT9368 needs
multiple payloads). The chip family must be established before any firmware
question can be answered.

## 7. Userspace fingerprint stack

```text
fprintd, fprintd-enroll, fprintd-verify, fprintd-list  : not installed
libfprint shared objects in ldconfig                    : none
fprintd systemd units                                   : none
```

`libfprint` and `fprintd` will need to be installed or built only after the
hardware path is proven.

## 8. Touchpad exclusion

The touchpad is a separate I2C device:

```text
ACPI:   \_SB.PC00.I2C0.TPD1
HID:    SYNA3602
Linux:  i2c-SYNA3602:00 on i2c_designware.0 / i2c-0
Input:  "SYNA3602:00 093A:0255 Touchpad" and "... Mouse"
IRQ:    intel-gpio 264
```

It is not the fingerprint reader and will not be touched.

## 9. Confirmed vs unknown

### Confirmed

- FTE3600 is an ACPI-enumerated SPI slave on Intel GSPI #1 (`spi1`).
- The ACPI path is `\_SB.PC00.SPI1.FP01`.
- ACPI `_CRS` specifies SPI Mode 0, 1 MHz (`0x000F4240`), Active-Low CS.
- ACPI `_CRS` specifies Reset GPIO: `GpioIo` on `\_SB.GPI0`, pin 14 (pad 14, SPKR), OutputOnly, PullUp.
- ACPI `_CRS` specifies Interrupt GPIO: `GpioInt` on `\_SB.GPI0`, pin 323 (pad 229, CPU_GP_0), Edge, ActiveHigh.
- ACPI `_INI` executes `SHPO` setting host ownership for both pins.
- ACPI `FP01` has no `_DSD` node.
- No driver is bound; no SPI traffic has occurred.
- Secure Boot is disabled.
- The running kernel has spidev, UIO, pxa2xx-spi and Tiger Lake pinctrl
  support.
- Matching kernel headers are installed.
- fprintd/libfprint are absent.
- The Synaptics device is the touchpad, not the sensor.

### Unknown (must be verified experimentally, not guessed)

- Exact physical sensor silicon family and chip ID (e.g., FT9361, FT9362, FT9348, FT9338).
- Whether Intel pad lock (`[LOCKED]` / `[LOCKED tx]`) prevents runtime GPIO direction changes.
- Firmware requirement and payload identity.
- Whether the reference driver's protocol and image pipeline match this part.

## 10. ACPI Diagnostics Summary

ACPI DSDT and SSDT tables were successfully dumped and decompiled under
`diagnostics/acpi/` and verified:
- Device `FP01` (`\_SB.PC00.SPI1.FP01`) provides 1 SPI resource, 1 `GpioIo` reset line, and 1 `GpioInt` interrupt line.
- SPI: 1.0 MHz, 8-bit, 4-wire, Mode 0 (`CPOL=0`, `CPHA=0`), Active-Low CS (`PolarityLow`).
- Reset: `\_SB.GPI0` pin 14 (pad 14, `SPKR`), output-capable, pull-up bias.
- Interrupt: `\_SB.GPI0` pin 323 (pad 229, `CPU_GP_0`), edge-sensitive, active-high.
- Both pins are granted host OS ownership via `SHPO` in `_INI`.
- No ACPI `_DSD` node exists.

## 11. Kernel Glue Source Audit & Hardware Interaction

The source code of `kernel/fte3600.c` was thoroughly audited:

1. **Module Load (`fte3600_init`):**
   - Registers platform driver `fte3600-glue`.
   - Registers SPI bus notifier `fte3600_spi_notifier`.
   - Iterates SPI devices with `fte3600_is_spidev()`.
   - **Crucial Safety Guarantee:** If `spidev` is not bound to `spi-FTE3600:00`, loading `fte3600.ko` does **not** attach to any device, does not invoke `probe`, does not request any GPIO or IRQ, and changes **zero** hardware state.

2. **Device Attachment & Probe (`fte3600_probe`):**
   - Invoked only after `spidev` binds to `spi-FTE3600:00`.
   - Validates `_CRS` resources against policy rules.
   - Claims reset line via `fwnode_gpiod_get_index` and calls `fte3600_deassert()`: sets GPIO direction to OUTPUT, logical `0` (physical HIGH / inactive). The sensor is held out of reset.
   - Claims IRQ line via `fwnode_gpiod_get_index` and sets direction to INPUT.
   - **Does NOT request IRQ at probe time:** IRQ handler is hooked only when userspace opens `/dev/uioX`.
   - Registers a single-line `gpiochip` exposing only the reset line to root processes.
   - Performs **zero** SPI transactions; SPI remains entirely under `spidev` control.

3. **Module Unload (`fte3600_exit`):**
   - Unregisters SPI notifier and detaches glue devices.
   - Unregisters UIO device (automatically releasing any open IRQ).
   - Ensures reset line is left deasserted (physical HIGH).
   - Removes driver GPIO chip and releases all GPIO descriptors (`gpiod_put`).
   - Unregisters platform driver and destroys cleanup workqueue.
   - Complete software cleanup with zero resource leaks. Note: releasing the descriptor in Linux gpiolib clears the software claim, but gpiolib intentionally does not reprogram the underlying Intel PCH pad configuration registers (`PADCFG0`), leaving pad 14 configured as output driving physical HIGH. Pinctrl debugfs reflects register bits rather than a physical voltage measurement.

## 12. Silicon Identification Sequence Audit

The silicon identification routine from `discovery.c` was analyzed against strings in the official Dynabook Windows driver (`ftWbioUmdfDriverV2.dll`):

1. **Vendor Equivalence:** The routine directly mirrors the official vendor function `ft_feature_devinit_DetectSensorTypeWithoutFW` and `ft_feature_devinit_DistinguishSensorFT95A8byOtp` / `ft_feature_devinit_DistinguishSensorFT9338byOtp`.
2. **Commands Issued:**
   - Command 1: Pure SPI Read `0x90 0x00 0x00` (3 bytes, standard Read ID).
   - If marker is not present: prepares read at register `0x85C0` using `[0x06, 0xF9, 0x00]`, `[0x05, 0xFA, 0x85, 0xC0, ...]`, write `0xA4=1`, and reads 8 bytes via `[0x04, 0xFB, 0x85, 0xC0, ...]`.
   - Reads OTP status via register `0xF3` with OTP read mode enabled via `0xF4=1` and immediately cleared via `0xF4=0`.
3. **Non-Destructive Nature:**
   - The sequence sends **zero** flash erase (`0x20`, `0xD8`, `0xC7`) or flash program (`0x02`) commands.
   - Register writes are volatile control/status registers inside the MCU.
   - OTP read mode does not program OTP bits (programming requires high voltage $V_{PP}$).
   - Designed to run cleanly "WithoutFW" before firmware upload.
