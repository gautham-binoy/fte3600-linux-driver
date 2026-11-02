# Stage 2C.3 — Reset Architecture & Pad 14 Limitations Review

**Document ID:** `STAGE2C3_RESET_AND_PAD14_REVIEW`  
**Date:** 2026-11-02  
**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target SoC:** Intel Tiger Lake-LP PCH (`INT34C5:00`)  
**Investigated Signal:** Pad 14 (`SPKR` / ACPI Pin 14 / GPP_B14)  
**Investigation Scope:** Investigation 3 — Independent static audit of software reset capabilities, Intel PCH lock registers, ACPI ownership, and reference driver recovery pathways.  
**Execution Boundary:** Static analysis only. Zero hardware access.

---

## 1. Executive Summary & Distinction of Five Reset Boundaries

Earlier project reports broadly asserted that "Pad 14 cannot be toggled by software because it is locked."

This independent investigation deconstructs that assertion into five distinct hardware, kernel, and architectural boundaries. It proves that while Pad 14 is indeed unavailable for software reset pulses in our pure-spidev architecture, the exact reasons vary significantly across the stack:

| Boundary Dimension | Real-World Status | Exact Technical Mechanism |
| :--- | :---: | :--- |
| **A. Pinctrl Configuration** | **CANNOT BE CHANGED** | Intel PCH `PADCFGLOCK` bit is asserted (`[LOCKED]`), write-protecting `PADCFG0[31:2]` and `PADCFG1`. |
| **B. GPIO Direction** | **CANNOT BE CHANGED** | ACPI `_CRS` declares `IoRestrictionOutputOnly`; `pinctrl-intel` rejects direction changes on locked pads. |
| **C. GPIO Output Toggling** | **UNAVAILABLE IN PURE-SPIDEV** | In hardware, `PADCFGLOCKTX` is NOT set. However, userspace cannot toggle it because ACPI marks it `Exclusive`, and pure-spidev loads no GPIO driver. |
| **D. Reset Controller** | **DOES NOT EXIST** | Intel PCH GPIO does not register a Linux `reset_controller_dev`. Subsystem calls are non-existent. |
| **E. Alternative Reset Routes** | **UNAVAILABLE** | ACPI lacks `_RST` and Power Resources (`_PR0`/`_PS3`); FT9362 silicon lacks an in-band SPI soft-reset command. |

---

## 2. Deep Technical Breakdown across All Layers

### 2.1 ACPI Device Resources & Ownership
In `diagnostics/privileged/acpi-fte3600-only.dsl` lines 70252–70280:
```asl
Method (_CRS, 0, Serialized)
{
    Name (FBUF, ResourceTemplate ()
    {
        SpiSerialBusV2 (0x0000, PolarityLow, FourWireMode, 0x08, ...)
        GpioIo (Exclusive, PullUp, 0x0000, 0x0000, IoRestrictionOutputOnly,
            "\\_SB.GPI0", 0x00, ResourceConsumer, ,) { 0x0008 }
        GpioInt (Edge, ActiveHigh, ExclusiveAndWake, PullNone, 0x0000,
            "\\_SB.GPI0", 0x00, ResourceConsumer, ,) { 0x0000 }
    })
    ...
    RSTP = GNUM (0x0900000E)  // Pad 14 (0x0E)
    INTP = GNUM (0x090E0003)  // Pad 229 / Pin 323
    Return (FBUF)
}
```
- **Exclusive Flag:** The reset line is marked `Exclusive` to `\_SB.PC00.SPI1.FP01`. Under the Linux GPIO subsystem (`gpiolib`), any attempt by userspace (via `/dev/gpiochip0`) to claim an exclusive ACPI GPIO descriptor is rejected with `-EBUSY`.
- **Direction Restriction:** `IoRestrictionOutputOnly` instructs the kernel that the hardware cannot be configured as an input.

---

### 2.2 Intel PCH Register Telemetry & Lock Bits
In `diagnostics/stage2a-results.txt:18`:
```text
pin 14 (SPKR) 14:INT34C5:00 GPIO 0x84000201 0x0003c026 0x00000000 [LOCKED]
```
In the Intel Tiger Lake-LP PCH specification and `drivers/pinctrl/intel/pinctrl-intel.c`:
1. **`PADCFG0 = 0x84000201`:**
   - Bit 31 (`RST_CONF`): `1` (Reset on Host Reset / Deep Sleep).
   - Bit 26 (`RXEVCFG`): `0b00` (Level triggered, disabled).
   - Bit 9 (`GPIORXDIS`): `1` (Input receiver buffer is disabled).
   - Bit 8 (`GPIOTXDIS`): `0` (Output transmitter buffer is enabled).
   - Bit 0 (`GPIOTXSTATE`): `1` (Output latch drives physical HIGH / 3.3V).
2. **Lock Flags (`[LOCKED]` vs `[LOCKED tx]` vs `[LOCKED full]`):**
   - In `pinctrl-intel.c`, the display logic inspects two separate register bits:
     - `PADCFGLOCK` (Pad Configuration Lock): Protects `PADCFG0[31:2]` and `PADCFG1`.
     - `PADCFGLOCKTX` (Pad TX / Output Latch Lock): Protects `PADCFG0[1:0]` (`GPIOTXDIS` and `GPIOTXSTATE`).
   - For Pad 14, debugfs outputs `[LOCKED]`. This indicates that **`PADCFGLOCK` is set, but `PADCFGLOCKTX` is NOT set**.
   - **Conclusion on Hardware:** Physical silicon does NOT lock the Tx data latch in hardware. However, Linux kernel pinctrl policies and ACPI resource ownership prevent unauthorized userspace access.

---

### 2.3 How the Reference Driver Bypassed This (and Why Pure-spidev Cannot)
In `research/libfprint-fte3600/libfprint/drivers/fte3600/linux_board.c:296-305`:
The vendor implementation did **not** use standard userspace `/dev/gpiochip0`. Instead:
1. It loaded a proprietary out-of-tree kernel module (`kernel/fte3600.c`).
2. That kernel module bound to the ACPI device, claimed the `Exclusive` Pad 14 GPIO descriptor from inside the kernel (`fwnode_gpiod_get_index`), and registered an artificial, secondary 1-line GPIO chip (`/dev/gpiochip1`) named `"fte3600-glue"`.
3. The userspace library then opened `/dev/gpiochip1` and sent `GPIO_V2_LINE_SET_VALUES_IOCTL` to toggle the line.

**In our pure-spidev architecture:**
- No custom out-of-tree kernel driver is loaded.
- `spi-FTE3600:00` is bound directly to `spidev`.
- `spidev` only binds to the SPI slave interface; it completely ignores ACPI GPIO resources.
- Pad 14 remains permanently unmanaged, driven HIGH by the UEFI BIOS boot configuration (`0x84000201`).
- Therefore, userspace software **cannot generate physical reset pulses**.

---

### 2.4 Absence of Linux Reset Controller & Alternative Mechanisms
1. **Reset Controller Subsystem:** Neither `INT34C5:00` (Intel PCH GPIO) nor `pxa2xx-spi` implements the Linux kernel `reset_controller_dev` structure. Standard kernel reset APIs (`devm_reset_control_get()`, `reset_control_assert()`) are completely absent.
2. **ACPI Power & Reset Methods:** The `FP01` device in ACPI DSDT has zero power management methods (`_PS0`, `_PS3`, `_PR0`) and zero reset methods (`_RST`). Software cannot cycle device power rails through ACPI.
3. **In-Band SPI Reset:** Neither the FT9362 datasheet nor the reverse-engineered FW9369 reference driver includes an in-band software command to trigger a hardware reset. The command set (`0x5A`, `0xA5`, `0xC2 0x3D`, `0xC0 0x3F`) triggers MCU sleep/idle transitions and AFE scans, but cannot reset frozen silicon registers.

---

## 3. Operational Consequence & Recovery Strategy

Because Pad 14 cannot be pulsed from software:
1. **Upstream Recovery Code is Inoperable:** Routines such as `reset_pulse()` in `recovery.c:20-30` or `fte3600_reset_and_sync()` cannot execute on this platform without loading unsafe kernel modules.
2. **Defensive Loop Limits:** Every polling loop in the driver must enforce strict, bounded timeouts (maximum 20 iterations or 100 ms) to avoid deadlocking if the sensor MCU fails to acknowledge.
3. **The Conservative Recovery Path:** If the sensor's internal silicon state machine ever enters an unrecoverable stall or deadlock, an **ACPI S5 cold reboot (full laptop power down)** is the only guaranteed recovery method to re-assert the hardware reset line and re-initialize silicon registers to factory defaults.
