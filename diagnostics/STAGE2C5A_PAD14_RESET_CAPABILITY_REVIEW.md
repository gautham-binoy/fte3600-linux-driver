# Stage 2C.5A — PCH Pad 14 and Hardware Reset Capability Review

**Document ID:** `STAGE2C5A_PAD14_RESET_CAPABILITY_REVIEW`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target SoC:** Intel Tiger Lake-LP PCH (`INT34C5:00`)  
**Signal Identified:** ACPI Pin 14 / PCH Pad 14 (GPP_B14 / `SPKR` / Sensor Reset Line)  
**Execution Boundary:** Static analysis and architectural audit only. Zero hardware access.

---

## 1. Executive Summary

Earlier project discussions sometimes treated "Pad 14 is locked" as an undifferentiated barrier implying that physical reset is completely impossible. This independent audit performs a deep architectural separation of the hardware, kernel, and driver boundaries governing Pad 14.

### Fundamental Categorization & Separation
To ensure absolute clarity, four distinct layers of reset availability are established:

| Boundary Dimension | Status | Key Technical Finding |
| :--- | :---: | :--- |
| **A. Userspace GPIO Access** | **UNAVAILABLE** | ACPI marks the line `Exclusive`. Linux `gpiolib` blocks userspace (`/dev/gpiochip0`) access with `-EBUSY`. |
| **B. Current Driver Architecture (Pure-spidev)** | **CANNOT ACCESS RESET** | `spidev` binds strictly to the SPI bus interface; it has no GPIO awareness and cannot toggle pins. |
| **C. Kernel-Space Driver Framework** | **ARCHITECTURALLY SUPPORTED** | A custom kernel driver binding to the ACPI device node could legitimately obtain the GPIO descriptor via `gpiod_get()` and toggle the pin. Hardware locks (`PADCFGLOCKTX`) do **not** block output toggling. |
| **D. Physical Reset Capability** | **PHYSICALLY AVAILABLE ON SILICON** | Pad 14 is physically wired to the FT9362 reset pin; Intel PCH hardware output buffer is active and driven HIGH (`0x84000201`). |

---

## 2. ACPI Device Resources & Exclusive Ownership

In `diagnostics/privileged/acpi-fte3600-only.dsl` lines 70252–70280:

```asl
Device (FP01)
{
    Name (_HID, "FTE3600")
    ...
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
}
```

### Analysis of ACPI Resource Flags
1. **`Exclusive` Flag:**
   - Declares that Pad 14 is allocated exclusively to device `\_SB.PC00.SPI1.FP01`.
   - In Linux `drivers/gpio/gpiolib.c`:
     When a userspace process attempts to request a GPIO line via `GPIO_GET_LINEINFO_IOCTL` or `GPIO_V2_GET_LINE_IOCTL` on `/dev/gpiochip0`, the kernel checks whether the line is reserved by an ACPI resource template.
     Because Pad 14 is marked `Exclusive`, any unprivileged or generic userspace request returns `-EBUSY`.
2. **`IoRestrictionOutputOnly`:**
   - Instructs the kernel that Pad 14 cannot be converted into an input buffer.
3. **`PullUp` Bias:**
   - Matches the electrical active-low reset requirement of the FT9362 sensor (reset is asserted when pulled low; deasserted when pulled high).

---

## 3. Intel PCH Register Telemetry & Hardware Lock Analysis

In `diagnostics/stage2a-results.txt:18`:
```text
pin 14 (SPKR) 14:INT34C5:00 GPIO 0x84000201 0x0003c026 0x00000000 [LOCKED]
```

### 3.1 PADCFG0 Bitfield Analysis (`0x84000201`)
- **Bit 31 (`RST_CONF = 1`):** Pad resets only on Host Reset or Deep Sleep.
- **Bit 26:25 (`RXEVCFG = 00`):** Interrupt receiver disabled.
- **Bit 9 (`GPIORXDIS = 1`):** Input receiver buffer is disabled.
- **Bit 8 (`GPIOTXDIS = 0`):** **Output transmitter buffer is ENABLED.**
- **Bit 0 (`GPIOTXSTATE = 1`):** **Output data latch drives HIGH (3.3V).**

### 3.2 The Pinctrl Lock Flags (`[LOCKED]` vs. `[LOCKED tx]`)
In `drivers/pinctrl/intel/pinctrl-intel.c`, the display logic inspects two distinct lock registers:
1. **`PADCFGLOCK` (Configuration Lock Register):**
   - Write-protects `PADCFG0[31:2]` (mode, direction, pull resistors).
   - For Pad 14, `PADCFGLOCK` **IS SET**.
   - This means software **cannot change the pad mode or disable the output driver**.
2. **`PADCFGLOCKTX` (Output Latch Lock Register):**
   - Write-protects `PADCFG0[1:0]` (`GPIOTXDIS` and `GPIOTXSTATE`).
   - For Pad 14, `PADCFGLOCKTX` **IS NOT SET** (indicated by `[LOCKED]`, rather than `[LOCKED full]` or `[LOCKED tx]`).
   - **Critical Silicon Finding:** The hardware allows writing to `GPIOTXSTATE`! If software writes bit 0, the physical Intel PCH pin **will toggle between 0V and 3.3V**.

> [!IMPORTANT]
> The hardware itself does **not** block reset toggling. The limitation is entirely imposed by the software and driver architecture: userspace cannot reach the pin because it is claimed exclusively by ACPI, and `spidev` does not bind GPIO descriptors.

---

## 4. Reset Access Across Driver Architectures

### 4.1 How the Reference Driver Accessed Reset (`kernel/fte3600.c`)
In `research/libfprint-fte3600/kernel/fte3600/fte3600.c`:
1. The out-of-tree kernel driver registered an ACPI SPI driver (`fte3600_spi_driver`) that matched `FTE3600`.
2. Inside the kernel, it called:
   `f->lines[FTE3600_RESET] = fwnode_gpiod_get_index(dev_fwnode(dev), NULL, 0, GPIOD_OUT_HIGH, "fte3600-reset");`
   Because this request came from the legitimate ACPI driver for `FP01`, Linux `gpiolib` granted the exclusive descriptor.
3. The kernel module then exported an artificial, private 1-pin GPIO controller (`/dev/gpiochip1`) named `"fte3600-glue"`.
4. Userspace opened `/dev/gpiochip1` and toggled the line.

### 4.2 Why Pure-spidev Cannot Access Reset
1. In Stage 2A and the planned pure-spidev architecture, no custom kernel driver is loaded.
2. `spi-FTE3600:00` is bound directly to `spidev` via `driver_override`.
3. `spidev` only binds to the SPI slave interface. It has zero code to parse ACPI `GpioIo` resources or acquire GPIO descriptors.
4. Pad 14 remains permanently unmanaged by the operating system, sitting in its firmware-initialized boot state (`0x84000201`, HIGH).
5. **Operational Result:** In pure-spidev mode, userspace has **zero ability to toggle Pad 14**.

---

## 5. Alternative Reset Mechanisms: Fact vs. Myth

| Mechanism | Available? | Technical Evidence |
| :--- | :---: | :--- |
| **Linux Reset Controller API** | **NO** | Neither `INT34C5:00` (PCH GPIO) nor `pxa2xx-spi` implements `struct reset_controller_dev`. Subsystem calls (`reset_control_assert()`) fail to resolve. |
| **ACPI Reset Method (`_RST`)** | **NO** | Disassembly of `FP01` in `diagnostics/privileged/acpi-fte3600-blocks.dsl` confirms zero `_RST` control methods exist. |
| **ACPI Power Rail Cycling (`_PR0` / `_PS3`)** | **NO** | `FP01` declares zero power resources and zero power states. Motherboard supplies fixed 3.3V standby rail. |
| **In-Band SPI Soft Reset Command** | **NO** | Neither the FT9362 datasheet nor reference code defines an SPI software reset opcode. |
| **ACPI S5 Cold Power Cycle (Reboot)** | **YES** | Shutting down the machine to S5 drops power rails and triggers platform PCH hardware reset upon reboot. |

---

## 6. Audit Conclusion & Strategic Synthesis

1. **Hardware Reality:** Pad 14 is physically capable of driving reset, and its Tx data latch is not hardware-locked.
2. **Current Architecture Reality:** Under the pure-spidev architecture, software reset pulses are **completely unavailable** because userspace cannot bypass ACPI exclusive ownership.
3. **Driver Design Mandate:** Any pure-spidev implementation cannot rely on software reset for error recovery. If the sensor silicon deadlocks or corrupts its internal register state, the only established, conservative recovery procedure is a **full ACPI S5 system cold reboot**.
