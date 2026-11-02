# FTE3600 Linux Driver — Stage 2 Read-Only Identification Specification

**Date/Time:** 2026-11-02T14:20:00+05:30  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Device:** `spi-FTE3600:00` on Intel GSPI #1 (`pxa2xx-spi.2`)  
**Status:** SPECIFICATION ONLY (NOT IMPLEMENTED / NOT EXECUTED)  

---

## 1. Objective and Scope

The goal of Stage 2 is to perform **non-destructive, read-only hardware identification** of the fingerprint sensor via the SPI bus.

Stage 2 must determine:
1. Whether the physical sensor communicates over the SPI bus.
2. The exact hardware **Chip ID** (e.g. `0x9391`, `0x9362`, `0x9395`, `0x9349`).
3. The hardware silicon revision / sub-model.
4. The communication integrity (framing acknowledgment, CRC validation).

### Strict Prohibitions
To eliminate any risk of bricking, data loss, or hardware alteration, Stage 2 strictly enforces:
- **NO Sensor Writes to NVRAM/Flash:** No flash erase, write, or firmware download.
- **NO EFUSE / OTP Access:** Registers `0xf1`, `0xf4`, `0xf6`, and `0xf7` are strictly barred.
- **NO Sensor Calibration:** No offset, gain, or DAC calibration routines.
- **NO Reset Pulsing:** The reset GPIO line (Pad 14) remains continuously held inactive-high (asserted high, logical inactive).
- **NO Interrupt Manipulation:** No interrupts will be unmasked, triggered, or waited upon.
- **NO Undocumented Commands:** Only command byte sequences verified in published open-source reference implementations will be utilized.

---

## 2. Hardware Access Architecture & Permissions

### Architecture
Stage 2 builds directly upon the proven Stage 1 infrastructure:
1. **Kernel Driver Layer:**
   - `fte3600.ko` is inserted to claim the ACPI companion node (`FTE3600:00`), configure the reset line inactive-high, and export status attributes.
   - `spidev` is loaded and bound to `spi-FTE3600:00` via `driver_override`.
2. **Userspace Interface:**
   - Communication is conducted exclusively via standard Linux ioctl calls on `/dev/spidev1.0`.
   - No custom kernel SPI modifications are required.
3. **Permissions:**
   - Requires administrative privileges (`root` or membership in group with read/write access to `/dev/spidev1.0`).

---

## 3. SPI Configuration Requirements

Derived from the verified platform ACPI DSDT table (`_SB.PC00.SPI1.FP01`):

| Parameter | Required Value | Justification |
| :--- | :--- | :--- |
| **SPI Mode** | `SPI_MODE_0` (CPOL=0, CPHA=0) | ACPI DSDT: `ClockPolarityLow`, `ClockPhaseFirst`. |
| **Clock Frequency** | 1,000,000 Hz (1 MHz) | ACPI DSDT: `connection_speed = 1000000`. |
| **Bits Per Word** | 8 | Standard byte-oriented framing. |
| **Bit Order** | MSB First | Standard SPI protocol. |
| **Chip Select** | Active Low | ACPI DSDT: `PolarityLow` (handled by controller). |
| **Max Transfer Size** | 16 bytes | Probe transfers require at most 10 bytes per packet. |

---

## 4. Verified Identification Protocol & Command Sequence

Analysis of the reference implementations (`SamSeven777/libfprint-fte3600`, `vobademi/FTEXX00-Ubuntu`, and Dynabook Windows driver string dumps) reveals the exact FocalTech SPI command protocol:

### 4.1 Packet Framing Rules
Every command frame consists of paired command bytes where each control byte is followed by its bitwise inverse (`~byte`):
- **Command / Idle:** `{cmd, (uint8_t)~cmd, 0x00}` (3 bytes)
- **Read Byte:** `{0x08, 0xf7, address, 0x00, 0x00}` (5 bytes)  
  *Response:* The read data is returned in byte 4 (`buffer[4]`).
- **Write Volatile Byte:** `{0x09, 0xf6, address, value}` (4 bytes)
- **Read 16-bit Word:** `{0x04, 0xfb, (uint8_t)(0x80 | (address >> 8)), (uint8_t)address, 0, 0, 0, 0, 0, 0}` (10 bytes)  
  *Response:* The 16-bit word is returned in `(buffer[6] << 8) | buffer[7]`.  
  *CRC:* `buffer[8]` and `buffer[9]` contain a 16-bit CRC-CCITT (polynomial `0x1021`, init `0xffff`).
- **Write 16-bit Word:** `{0x05, 0xfa, (uint8_t)(0x80 | (address >> 8)), (uint8_t)address, 0, 0, (uint8_t)(value >> 8), (uint8_t)value}` (8 bytes)

### 4.2 Two-Stage Safe Execution Sequence

To respect the user's strict restriction on sensor writes, Stage 2 will execute in two distinct phases:

#### Phase 1: Pure Passive SPI Read (Zero Writes)
1. Send a 10-byte **Read Word** packet targeting Chip ID register `0x1a8b`:
   ```text
   MOSI: [ 0x04, 0xfb, 0x9a, 0x8b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 ]
   MISO: [   ??,   ??,   ??,   ??,   ??,   ??,   D1,   D0,  CRCH, CRCL ]
   ```
2. **Evaluate Response:**
   - If MISO returns a recognized 16-bit Chip ID (e.g. `0x9391`, `0x9362`, `0x9395`) and valid CRC, the sensor is already in active communication mode. Phase 1 succeeds with **ZERO sensor writes**.
   - If MISO returns all `0x00` or all `0xFF`, the sensor's internal SPI interface is unclocked / in low-power idle.

#### Phase 2: Volatile Interface Wake-Up (Conditioned on User Approval)
If Phase 1 confirms that the sensor requires volatile SPI wake-up, execute the minimal, non-volatile volatile register wake sequence:
1. **SPI Mode Wake:**
   - Write volatile register `0xc6 = 0x01`:
     `MOSI: [ 0x09, 0xf6, 0xc6, 0x01 ]`
   - Wait 4 ms.
   - Read volatile register `0xc6`:
     `MOSI: [ 0x08, 0xf7, 0xc6, 0x00, 0x00 ]`
   - Verify MISO returns `0x01` in byte 4 (`spi_mode_acknowledged`).
2. **Read Chip ID Register (`0x1a8b`):**
   - Transmit Read Word packet for `0x1a8b`:
     `MOSI: [ 0x04, 0xfb, 0x9a, 0x8b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 ]`
   - Extract `chip_id = (buffer[6] << 8) | buffer[7]`.
   - Verify CRC-CCITT on `buffer[6..7]` matches `buffer[8..9]`.
3. **Read Sub-Revision / Window Extension Register (`0x1816`):**
   - Transmit Read Word packet for `0x1816`:
     `MOSI: [ 0x04, 0xfb, 0x98, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 ]`
   - Extract 16-bit revision word (differentiates FT9391 from FT9395).
4. **Return Interface to Idle:**
   - Transmit idle command:
     `MOSI: [ 0x5a, 0xa5, 0x00 ]`
   - Wait 1 ms.
   - Transmit post-idle command:
     `MOSI: [ 0xa5, 0x5a, 0x00 ]`

---

## 5. Expected Responses & Device Identification Table

| Chip ID (`0x1a8b`) | Extension (`0x1816`) | Model | Sensor Dimensions | Known Laptops / Drivers |
| :---: | :---: | :--- | :---: | :--- |
| **`0x9391`** | `!= 0x0fff` | **FT9391** | 40 x 196 (extra rows = 4) | Dynabook Satellite Pro C40-J (Windows driver strings) |
| **`0x9391`** | `== 0x0fff` | **FT9395** | 36 x 176 | Variant silicon using 9391 base ID |
| **`0x9362`** | — | **FT9362 / FW9369** | 64 x 80 | KVADRA NAU LE14U / FocalTech reference |
| **`0x9365`** | — | **FT9365** | 64 x 80 | FocalTech reference |
| **`0x9349`** | — | **FT9349** | 80 x 128 | FocalTech large area sensor |
| **`0x9397`** | — | **FT9397** | 32 x 176 | Thin-bezel power-button sensor |
| **`0x0000` / `0xffff`** | — | **COMM FAILURE** | — | Bus floating, device unpowered, or in reset |

---

## 6. Timeouts and Error Handling

- **SPI ioctl Timeout:** 200 ms per transfer.
- **Maximum Retries:** 3 attempts for any unacknowledged transaction.
- **Inter-Transaction Delay:** 10 ms between retries.
- **CRC Error Handling:** If CRC check fails, the packet is flagged as corrupted and discarded; retry count increments.
- **Fail-Safe Abort:** If after 3 attempts the chip ID remains `0x0000` or `0xffff`, the test immediately aborts and proceeds to rollback.

---

## 7. Safety Boundaries & Rollback Strategy

1. **Immediate Teardown:**
   Regardless of whether identification succeeds or fails, Stage 2 must execute the automated rollback harness:
   - Close `/dev/spidev1.0`.
   - Remove `fte3600.ko` via `rmmod fte3600`.
   - Unbind `spi-FTE3600:00` from `spidev`.
   - Clear `driver_override`.
   - Unload `spidev` and `uio`.
   - Verify 0 remaining character devices and 0 loaded test modules.
2. **Signal Protection:**
   The userspace test tool must trap `SIGINT` (Ctrl+C) and `SIGTERM` to issue the idle command and invoke cleanup prior to exit.

---

## 8. Acceptance Criteria

Stage 2 will be judged a **PASS** if and only if:
1. MISO returns non-floating data that conforms to FocalTech SPI framing.
2. Register `0x1a8b` yields a valid, recognized 16-bit Chip ID in the `0x93xx` family.
3. If CRC is present, the computed CRC matches the received CRC bytes.
4. Rollback succeeds with zero errors, returning the host system to the clean baseline.

---

## 9. List of Open Unknowns

1. **Passive Response vs Wake Requirement:** It is unknown whether the Dynabook sensor will reply to Phase 1 (zero writes) without first asserting `0xc6=1` to enable the SPI transceiver.
2. **Hardware Revision:** Windows driver strings reference `FT9391`, but KVADRA reference notes reference `FW9369/FT9362`. Stage 2 will decisively resolve which silicon is soldered on this Dynabook motherboard.
3. **Power-On Reset Timing:** The exact power-up latency of the sensor after cold boot will be observed.