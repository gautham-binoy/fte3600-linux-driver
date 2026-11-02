# Stage 2C.2 — Hardware State Record & Uncertainty Reconciliation

**Document ID:** `STAGE2C2_HARDWARE_STATE_RECORD`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Host Controller:** Intel Tiger Lake-LP PCH (`pxa2xx-spi.2` / `INT34C5:00`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`, CRC `0x1C53`)  
**Scope:** Formal reconciliation of host vs. physical hardware states following Stage 2A, throughout Stage 2B, and into Stage 2C.2.  
**Execution Gate:** Strictly static analysis. No hardware access, no MMIO writes, no GPIO toggling, no module loading.

---

## 1. Executive Summary & Hardware Invariance

Since the conclusion of the Stage 2A hardware identification test at **2026-11-02T16:04:14+05:30**, **ZERO hardware operations** have been performed on the Dynabook Satellite Pro C40-J. 

All subsequent stages:
- Stage 2B.1 (Safety and Protocol Verification Audit)
- Stage 2B.2 (Independent Evidence Verification)
- Stage 2B.3 (Minimal Identification Scope Audit)
- Stage 2B.4 (Active Handshake Research)
- Stage 2B.5 (Independent 2C.1B Verification)
- Stage 2C.2 (Integrated Initialization & Calibration Research)

have been executed under strict read-only, static-analysis rules. The host operating system and physical hardware remain in the exact post-Stage-2A state.

---

## 2. Host-Side State vs. Sensor Silicon State Distinction

A fundamental architectural principle established across this project is the strict distinction between **Host-Side Rollback** and **Silicon-Internal State Restoration**.

| Boundary Dimension | Current Verified State | Certainty Level | Verification Method |
| :--- | :--- | :--- | :--- |
| **Kernel Module State** | `spidev` ABSENT, `fte3600` ABSENT | 100% Verified | `lsmod`, `/proc/modules` |
| **Driver Binding** | `spi-FTE3600:00` UNBOUND | 100% Verified | sysfs `/sys/bus/spi/devices/spi-FTE3600:00/driver` |
| **Driver Override** | Clean (`""`) | 100% Verified | sysfs `/sys/bus/spi/devices/spi-FTE3600:00/driver_override` |
| **Device Nodes** | `/dev/spidev*` ABSENT | 100% Verified | `/dev` directory listing |
| **SPI Bus Physical Lines** | SCLK quiescent (0V), CS# deasserted (3.3V high), MOSI floating/quiescent | 100% Inferred from bus controller state | Intel LPSS controller idling in low-power state |
| **PCH Pad 14 (RST_N)** | `0x84000201` [LOCKED] (Output High, deasserted reset) | 100% Verified | `/sys/kernel/debug/pinctrl/INT34C5:00/pins` |
| **PCH Pad 229 (IRQ)** | `0x82000100` / `0x82000102` [LOCKED tx] (High-Z Input) | 100% Verified | `/sys/kernel/debug/pinctrl/INT34C5:00/pins` |
| **Sensor Register `0xC6`** | Unknown (`0x01`, decayed, or reset) | **UNKNOWN** | Cannot be read without bus transaction |
| **Sensor MCU State** | Idle or quiescent sensing mode | **UNKNOWN** | Internal silicon logic state |
| **Sensor FIFO Pointer** | Reset / zero | Inferred | Last transaction deasserted CS |

---

## 3. PCH GPIO Pad Telemetry & Analysis

### 3.1 Pad 14 (Hardware Reset Line `RST_N`)
- **Telemetry:** `pin 14 (SPKR) 14:INT34C5:00 GPIO 0x84000201 0x0003c026 0x00000000 [LOCKED]`
- **Status:** **Completely Unchanged from Factory Baseline**.
- **Hardware Mode:** Output buffer enabled (`GPIORXDIS=1, GPIOTXDIS=0`), driven HIGH (`TXSTATE=1`, 3.3V).
- **Control Lock:** Hardware locked by platform firmware (`PADRSTCFG` lock).
- **Operational Reality:** The upstream reference driver attempts to pulse this pad low for 15 ms to recover from hardware errors. On this Dynabook laptop, the kernel pinctrl driver refuses all userspace and kernel requests to manipulate this locked pin. **Hardware reset pulses cannot be generated.** Only an ACPI S5 cold reboot can assert a physical reset pulse via platform power management.

### 3.2 Pad 229 (Sensor IRQ Line)
- **Telemetry:** `pin 229 (CPU_GP_0) 323:INT34C5:00 GPIO 0x82000100 0x00000029 0x00000000 [LOCKED tx]`
- **Comparison to Historical Baseline:**
  - *Pre-Stage 1 Baseline:* `PADCFG0 = 0x80100100`
  - *Current State:* `PADCFG0 = 0x82000100` (or `0x82000102` depending on instantaneous pin voltage)
- **Differences Analyzed:**
  1. `PADCFG0_RXEVCFG` (Bits `[26:25]`): Changed from `0b00` (`LEVEL`) to `0b01` (`EDGE`). Configured by Linux kernel IRQ subsystem during Stage 1 GPIO interrupt probe.
  2. `PADCFG0_GPIROUTIOXAPIC` (Bit `[20]`): Changed from `1` (IOxAPIC direct route) to `0` (GPIO Community router).
- **Electrical Safety:**
  - In both baseline and current states, `GPIOTXDIS=1` and `GPIORXDIS=0`. The pad remains in **High-Z Input Mode**.
  - There is **zero electrical conflict** and zero drive contention with the sensor's IRQ output pin.
- **Policy on MMIO Manipulation:**
  - Raw MMIO writes via `/dev/mem` or custom kernel modules to force `PADCFG0` back to `0x80100100` are **strictly prohibited**. Direct MMIO manipulation risks memory corruption, pinctrl subsystem desynchronization, and hardware lock violations.
  - The configuration will automatically revert to pristine firmware default (`0x80100100`) upon the next full system ACPI cold reboot.

---

## 4. Sensor-Internal State & Uncertainties

### 4.1 Register `0xC6` (SPI Transceiver Wakeup)
- In Stage 2A, the host transmitted `0x09 0xF6 0xC6 0x01` to enable the FT9362 SPI transceiver clock, verified by readback returning `0x01`.
- Following Stage 2A, host rollback completely halted SPI bus clocks and deasserted CS#.
- **Known Facts:**
  1. The host driver has completely disconnected from the bus.
  2. The FT9362 silicon has remained continuously powered by laptop standby rail (+3.3V AUX).
- **Unknown Realities:**
  1. Does `0xC6` remain `0x01` indefinitely while VDD is present?
  2. Does `0xC6` autonomously clear to `0x00` after a bus inactivity timeout?
  3. Does the FT9362 internal watchdog timer reset registers during prolonged idle?
- **Conclusion:** It is impossible to verify register `0xC6` without initiating a new SPI read transaction. Any future operation must assume `0xC6` is in an unknown state and must explicitly write `0xC6 = 0x01` as its very first transaction.

### 4.2 Protected Bank & AFE Registers
- During Stage 2A, registers `0x9A`, `0x80`, and the AFE memory maps (`0x1800`..`0x18FF`, `0x00C0`..`0x00C2`) were **NEVER accessed**.
- The FT9362 analog front-end and baseline SRAM are in their power-on reset default state (or factory OTP load state).
- No EEPROM or OTP write operations have ever been attempted or executed.

---

## 5. Invariance Ledger Across All Project Stages

| Stage | Date | Hardware Access? | SPI Transferred | GPIO Modified | Notes |
| :--- | :--- | :---: | :---: | :---: | :--- |
| **Stage 0 / 1** | Prior | Read-only / IRQ test | None | Pad 229 IRQ config | Stage 1 baseline discovery |
| **Stage 2A** | 2026-11-02 16:04 | **YES (Supervised)** | 4 transactions | None | Identified FT9362 (`0x9362`, CRC `0x1C53`). Host rolled back cleanly. |
| **Stage 2B.1** | 2026-11-02 16:22 | **NO** | None | None | Register safety static audit |
| **Stage 2B.2** | 2026-11-02 16:34 | **NO** | None | None | Static verification & 2C.1 gate |
| **Stage 2B.3** | 2026-11-02 16:47 | **NO** | None | None | Minimal scope review (skipped 2C.1A) |
| **Stage 2B.4** | 2026-11-02 17:08 | **NO** | None | None | 2C.1B static research |
| **Stage 2B.5** | 2026-11-02 17:23 | **NO** | None | None | Cancelled isolated 2C.1B |
| **Stage 2C.2** | 2026-11-02 17:33 | **NO** | None | None | Complete FW9369 startup research |

---

## 6. Verification & Safety Guidelines for Next Stages

1. **Host Isolation Maintained:** Keep `spidev` unloaded and `spi-FTE3600:00` unbound during all future static review and userspace tool authoring phases.
2. **Defensive Re-initialization:** When hardware access is eventually authorized for an integrated initialization test, the software sequence must:
   - Not assume `0xC6` is `0x00` or `0x01`; always explicitly assert `0xC6 = 0x01` and verify readback.
   - Assert command `0x5A` and acknowledge with `0xA5` to guarantee MCU synchronization.
   - Set SPI FIFO buffer size parameter `bufsiz=65536` when loading `spidev`.
3. **No Intrusive Pad Restoration:** Do not attempt software restore on Pad 229; it is safe in High-Z input mode. If pristine platform restoration is ever required, perform an ACPI S5 cold reboot.
