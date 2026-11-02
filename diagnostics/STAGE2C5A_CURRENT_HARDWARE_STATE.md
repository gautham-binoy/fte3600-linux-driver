# Stage 2C.5A — Current Hardware and Host State Reconstruction

**Document ID:** `STAGE2C5A_CURRENT_HARDWARE_STATE`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Host Environment:** Linux `7.2.8-9-omarchy` (x86_64)  
**Execution Boundary:** Static analysis and historical log reconstruction only. Zero hardware access.

---

## 1. Executive Summary

This document establishes the verified baseline of the host system and the fingerprint sensor hardware as of Stage 2C.5A. The reconstruction is based strictly on historical empirical evidence recorded during Stage 1 (`diagnostics/stage1-results.txt`), Stage 2A (`diagnostics/stage2a-results.txt`), privileged ACPI/debugfs dumps (`diagnostics/privileged/`), and subsequent static analysis stages.

**Crucial Safety Principle:** We distinguish rigorously between *host-side software state* (which was verified 100% cleanly rolled back) and *sensor-side silicon state* (which cannot be verified without physical bus communication). We do **not** assume or claim that the physical sensor has returned to its pristine pre-Stage 1 power-on state.

---

## 2. Chronological State Reconstruction

### 2.1 Pre-Stage 1 Initial Baseline State
- **Kernel & OS:** Linux `7.2.8-9-omarchy`, Arch-based Omarchy distribution, unprivileged user `titan`.
- **SPI Device Node:** `spi-FTE3600:00` enumerated under ACPI path `\_SB.PC00.SPI1.FP01`, bus `pxa2xx-spi.2`.
- **Driver Binding:** Completely unbound (`/sys/bus/spi/devices/spi-FTE3600:00/driver` does not exist).
- **`driver_override`:** Empty string `''`.
- **Kernel Modules:** `spidev`, `fte3600`, and `uio` were confirmed **ABSENT** from kernel memory (`lsmod`).
- **Character Devices:** Zero `/dev/spidev*` and zero `/dev/uio*` device nodes existed.
- **PCH Pad 14 (`SPKR` / Reset):** `0x84000201 0x0003c026 0x00000000 [LOCKED]`
  - Output buffer enabled (`GPIOTXDIS = 0`), output driven HIGH (`GPIOTXSTATE = 1`), `PADCFGLOCK` asserted.
- **PCH Pad 229 (`CPU_GP_0` / IRQ):** `0x80100100 0x00000029 0x00000000 [LOCKED tx]`
  - High-impedance input mode (`GPIOTXDIS = 1`, `GPIORXDIS = 0`), live receiver state `GPIORXSTATE = 0`, `PADCFGLOCKTX` asserted.

### 2.2 Stage 1 Execution & Rollback Results
- **Execution (2026-11-02T14:03:39+05:30):**
  - Loaded `uio` and `spidev`.
  - Set `driver_override = spidev` on `spi-FTE3600:00` and bound device.
  - Inserted vendor out-of-tree module `fte3600.ko`.
  - Verified enumeration of `/dev/gpiochip1`, `/dev/spidev1.0`, and `/dev/uio0`.
  - `fte3600.ko` claimed Pad 14 (reset) and Pad 229 (IRQ) via `gpiolib`.
  - `PADCFG0` of Pad 229 transitioned from `0x80100100` to `0x82000100` (`RXEVCFG_EDGE` set, `GPIROUTIOXAPIC` cleared by `pinctrl-intel`).
- **Controlled Rollback (2026-11-02T14:03:39+05:30):**
  - `rmmod fte3600`: Succeeded. Postcondition verified ABSENT.
  - Unbound `spi-FTE3600:00`: Succeeded. Confirmed UNBOUND.
  - Restored `driver_override` to `''`: Succeeded.
  - `modprobe -r spidev`: Succeeded. Confirmed ABSENT.
  - `modprobe -r uio`: Succeeded. Confirmed ABSENT.
  - Device nodes `/dev/spidev*` and `/dev/uio*` ceased to exist.
  - Debugfs GPIO confirmed zero lines claimed by `fte3600`.
  - Pad 14 retained exact baseline configuration: `0x84000201 [LOCKED]`.
  - Pad 229 was observed at `0x82000102 [LOCKED tx]` (see Section 3 for full analysis).
  - Rollback completed cleanly with 0 errors.

### 2.3 Stage 2A Pure-spidev Identification & Rollback Results
- **Execution (2026-11-02T16:04:13+05:30):**
  - Architecture: **Pure spidev** (zero out-of-tree kernel modules, zero UIO, zero GPIO claims).
  - Pre-execution checks verified: Pad 14 at `0x84000201 [LOCKED]`, Pad 229 at `0x82000100 [LOCKED tx]`.
  - Loaded `spidev`, set `driver_override = spidev`, bound `spi-FTE3600:00` -> `/dev/spidev1.0`.
  - Executed minimal identification binary `stage2a-identify`:
    - **Operation 1:** Wrote volatile register `0xC6 = 0x01` (`[ 0x09, 0xF6, 0xC6, 0x01 ]`).
    - **Operation 2:** Delayed 4 ms.
    - **Operation 3:** Read back register `0xC6` (`[ 0x08, 0xF7, 0xC6, 0x00, 0x00 ]`) -> returned `0x01`.
    - **Operation 4:** Interrogated Chip ID register `0x1A8B` (`[ 0x04, 0xFB, 0x9A, 0x8B, ... ]`) -> returned `0x9362`, CRC `0x1C53`. Calculated CRC `0x1C53` -> **EXACT MATCH**.
- **Controlled Rollback (2026-11-02T16:04:14+05:30):**
  - Unbound `spidev` from `spi-FTE3600:00`. Confirmed UNBOUND.
  - Restored `driver_override` to `''`. Confirmed RESTORED.
  - Unloaded `spidev` (`modprobe -r spidev`). Confirmed ABSENT.
  - Verified `/dev/spidev*` does not exist.
  - Pad 14 and Pad 229 were **never claimed or accessed** during Stage 2A.
  - Host-side rollback completed with zero errors.

---

## 3. Sensor Internal State: Known vs. Unknown

### 3.1 The Last Known Register `0xC6` Transaction
- **Timestamp:** 2026-11-02T16:04:13+05:30.
- **Transaction:** Write `0xC6 = 0x01`, verified by readback `0xC6 == 0x01`.
- **Purpose:** Enables the sensor's internal SPI slave transceiver clock domain.

### 3.2 Known Facts
1. The sensor responded with valid complementary SPI framing during Stage 2A.
2. The sensor identified itself as FT9362 (`0x9362`) with mathematically valid wire CRC `0x1C53`.
3. No further SPI transactions have been executed on the physical bus since 16:04:14.
4. Chip Select (CS#) was deasserted HIGH upon completion of Stage 2A Operation 4.
5. PCH Pad 14 (reset) remains continuously driven HIGH (`0x84000201`), meaning the sensor has **not received a hardware reset pulse**.

### 3.3 Unknown Physical Realities
1. **Register `0xC6` Retention:** Whether volatile register `0xC6` currently retains `0x01`, autonomous internal decay occurred, or an internal silicon watchdog reset it is **UNKNOWN**.
2. **Internal Silicon Clocks:** Whether the internal FT9362 oscillator or transceiver PLL is currently running or entered an idle low-power state is **UNKNOWN**.
3. **Power Consumption:** The standby current draw of the sensor silicon in its post-Stage 2A state is **UNKNOWN**.
4. **Internal State Machine Pointer:** The internal state of the sensor's command parser is **UNKNOWN** (though CS# deassertion typically resets SPI byte framing).

> [!WARNING]
> Because physical register `0xC6` cannot be read without binding `spidev` and executing SPI transactions, the true silicon internal state is classified as **PHYSICALLY UNKNOWN**. Any future hardware plan must assume `0xC6` could be in an undefined state and must unconditionally begin by re-asserting `0xC6 = 0x01`.

---

## 4. Hardware Resource Ownership & Current Host Architecture

The following matrix documents the exact ownership of every hardware resource associated with the fingerprint subsystem:

| Hardware Resource | Underlying Hardware / Controller | ACPI Declaration | Current Ownership | Access Method in Planned Driver |
| :--- | :--- | :--- | :--- | :--- |
| **SPI Slave Device** | `spi-FTE3600:00` | `\_SB.PC00.SPI1.FP01` | **UNCLAIMED / UNBOUND** | Linux `spidev` via `driver_override` |
| **SPI Master Controller** | Intel LPSS GSPI #1 (`00:1e.3`) | `\_SB.PC00.SPI1` | Kernel `pxa2xx-spi.2` | Controlled via `/dev/spidev1.0` ioctls |
| **SPI DMA Engine** | Intel LPSS DMA 64-bit | Platform device `idma64.2` | Kernel DMA Engine | Bound automatically by `pxa2xx-spi` |
| **Reset Pin (Pad 14)** | Intel PCH GPP_B14 (`SPKR`) | `GpioIo(Exclusive, OutputOnly)` | **UNCLAIMED / FIRMWARE** | Driven HIGH by PCH (`0x84000201`); no userspace access |
| **IRQ Pin (Pad 229)** | Intel PCH Pad 229 (`CPU_GP_0`) | `GpioInt(Exclusive, Edge)` | **UNCLAIMED / UNBOUND** | High-Z input mode; polling planned for userspace |
| **VCC Power Rail** | Motherboard 3.3V standby rail | None (`_PR0`/`_PS3` absent) | Platform Hardware | Always powered; no software power switch |

---

## 5. Comprehensive State Classification Matrix

To eliminate ambiguity, all aspects of the host and sensor state are separated into four rigorous categories:

| Subsystem / Element | Confirmed Historical Observation | Static Source-Code Conclusion | Current Unverified State | Unknown Physical State |
| :--- | :---: | :---: | :---: | :---: |
| **Host Kernel Modules** | Confirmed ABSENT in Stage 1 & 2A logs | Makefile/source confirms mock builds in userspace | `spidev`/`fte3600` assumed absent | — |
| **SPI Driver Binding** | Confirmed UNBOUND in Stage 2A rollback | `driver_override` restored to `''` | Sysfs node assumed unlinked | — |
| **Device Nodes (`/dev`)** | Confirmed ABSENT in Stage 2A log | Stub code never calls `open()` | `/dev/spidev*` assumed absent | — |
| **PCH Pad 14 Configuration** | Confirmed `0x84000201 [LOCKED]` | Declared `OutputOnly` in ACPI `_CRS` | PCH register assumed unchanged | Physical line level assumed HIGH (3.3V) |
| **PCH Pad 229 Configuration** | Confirmed `0x82000100` / `0x82000102` | `pinctrl-intel` sets Edge IRQ bits | PCH register assumed unchanged | Ambient floating noise level on line |
| **Sensor Silicon ID** | Confirmed `0x9362`, CRC `0x1C53` | Canonical spec matches FT9362 | — | Silicon verified as FT9362 |
| **Sensor Register `0xC6`** | Confirmed `0x01` at 16:04:13 | Required before word registers | Current value unverified | **UNKNOWN (Persistence/decay unverified)** |
| **Sensor Analog Trim Bank** | Unaccessed in Stage 2A | Specification requires `0x9A=0x5A` | — | Factory default or uncalibrated state |
| **Sensor FIFO Buffer** | Unaccessed in Stage 2A | Spec defines 10,246 B burst read | — | Empty / un-triggered state |

---

## 6. Audit Conclusion

The host operating system is in a pristine, completely isolated baseline state. No driver modules are loaded, no device links are active, and no hardware resources are leased. However, because physical reset is unavailable and SPI communication has ceased, the internal silicon state of the FT9362 sensor is **formally UNKNOWN**. Any future hardware test must be engineered defensively to account for this uncertainty.
