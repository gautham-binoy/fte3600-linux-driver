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

The device node is readable only by root and `libgpiod` (`gpioinfo`) is not
installed. Line-level data must therefore be read from
`/sys/kernel/debug/gpio` and the pinctrl debugfs files with root privileges.
No GPIO number, reset polarity or interrupt polarity has been assumed.

The only GPIO interrupt currently visible is the touchpad:

```text
159: ... intel-gpio 264 SYNA3602:00
```

There is no fingerprint IRQ because no driver has requested one. The ACPI
`GpioInt`/`Interrupt` resource must supply it.

## 5. Interrupts

Relevant `/proc/interrupts` entries at inspection time:

```text
14:  IR-IO-APIC 14-fasteoi   INT34C5:00
37:  IR-IO-APIC 37-fasteoi   idma64.2, pxa2xx-spi.2
159: intel-gpio 264          SYNA3602:00
```

The fingerprint interrupt is not allocated yet. Its trigger type and polarity
are unknown until the ACPI `_CRS` is read.

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
- No driver is bound; no SPI traffic has occurred.
- Secure Boot is disabled.
- The running kernel has spidev, UIO, pxa2xx-spi and Tiger Lake pinctrl
  support.
- Matching kernel headers are installed.
- fprintd/libfprint are absent.
- The Synaptics device is the touchpad, not the sensor.

### Unknown (must be collected, not guessed)

- Sensor silicon family and chip ID.
- ACPI `_CRS`: SPI clock phase/polarity, CS polarity, speed, data size.
- Reset GPIO controller/pin, active level and initial state.
- Interrupt GPIO/IRQ number, trigger edge and polarity.
- Presence and content of `_DSD` properties (reset-gpios, irq-gpios, firmware
  names, etc.).
- Firmware requirement and payload identity.
- Whether the reference driver's protocol and image pipeline match this part.

## 10. Next evidence required

The decisive missing artifact is the ACPI firmware description. Run the
prepared read-only collector as root:

```sh
sudo /home/titan/fte3600-linux-driver/scripts/collect-privileged.sh
```

It writes:

```text
diagnostics/acpi/                 binary tables, acpidump.txt, decompiled .dsl
diagnostics/privileged/           dmesg, gpio/pinctrl debugfs, blocks
```

Specifically inspect:

- the `Device (FP01)` block (SPI serial bus, GpioIo, GpioInt, `_DSD`);
- the parent `Device (SPI1)` and GPIO controller blocks;
- `/sys/kernel/debug/gpio` for pin ownership;
- pinctrl `pins`, `pinmux-pins`, `gpio-ranges` for pin mapping;
- `dmesg` for ACPI/resource errors.

No GPIO or protocol conclusion will be drawn before those files exist.
