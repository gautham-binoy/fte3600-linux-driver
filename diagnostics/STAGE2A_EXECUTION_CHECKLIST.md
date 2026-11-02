# FTE3600 Linux Driver — Stage 2A Execution Checklist

**Date/Time:** 2026-11-02T15:45:00+05:30  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Device:** `spi-FTE3600:00` on Intel GSPI #1 (`pxa2xx-spi.2`)  
**Architecture Revision:** **v2.0 (Pure spidev Architecture)**  
*(Supersedes v1.0 dual-driver model based on STAGE2A_RESET_LINE_ROLLBACK_AUDIT.md)*  
**Document Status:** STATIC CHECKLIST ONLY — ZERO HARDWARE EXECUTION  
**Authority Statement:** **EXECUTION OF THIS SEQUENCE REQUIRES SEPARATE, EXPLICIT USER AUTHORIZATION. THE AGENT IS STRICTLY PROHIBITED FROM RUNNING THIS SEQUENCE AUTONOMOUSLY.**

> [!NOTE] Architectural Revision Notice (Pure spidev)
> Based on the deep register decode in `diagnostics/STAGE2A_RESET_LINE_ROLLBACK_AUDIT.md`, PCH Pad 14 was established to be already driven active-high (3.3V) by UEFI BIOS firmware (`0x84000201`). Consequently, `fte3600.ko` and `uio.ko` are completely omitted from Stage 2A. Stage 2A operates exclusively through mainline `spidev`, eliminating all GPIO manipulation, UIO dependencies, and Pad 229 register modification risks.

---

## 1. Required Manual Checks Before Execution (Pre-Execution Audit)

Prior to authorizing or executing Stage 2A, the operator must verify via read-only checks:
- [ ] Running kernel matches `7.2.8-9-omarchy`.
- [ ] `spidev` module is ABSENT: `lsmod | grep spidev` returns empty.
- [ ] `fte3600` module is ABSENT: `lsmod | grep fte3600` returns empty.
- [ ] SPI device `spi-FTE3600:00` exists at `/sys/bus/spi/devices/spi-FTE3600:00` and `driver` symlink is ABSENT (unbound).
- [ ] `driver_override` is empty (`(null)` or `""`).
- [ ] Device nodes `/dev/spidev*` are ABSENT.
- [ ] PCH Pad 14 register matches baseline: `0x84000201 0x0003c026` (Pad reset inactive output high).
- [ ] PCH Pad 229 register is in high-Z input mode: `0x80100100` or `0x82000102` with `0x00000029` (TX disabled).

---

## 2. SPI Interface Configuration & Parameters

The host Intel GSPI controller #1 (`pxa2xx-spi.2`) must be configured via standard ioctl commands on `/dev/spidev1.0`:

| Parameter | Required Value | Evidential Source |
| :--- | :--- | :--- |
| **SPI Mode** | `SPI_MODE_0` (CPOL=0, CPHA=0) | ACPI DSDT `_CRS` (`ClockPolarityLow`, `ClockPhaseFirst`); `kernel/fte3600.c:86-93`. |
| **Clock Frequency** | 1,000,000 Hz (1 MHz) | ACPI DSDT `_CRS` (`ConnectionSpeed = 1000000`). |
| **Bits Per Word** | 8 bits | ACPI DSDT `_CRS` (`DataBitLength = 8`). |
| **Bit Order** | MSB First | Standard Linux spidev default; Intel GSPI controller architecture. |
| **Chip Select (CS)** | Active Low | Controller managed (`cs_control = 1`). |
| **CS Continuity** | Asserted only during active transfer | CS deasserted high during delays and between transactions. |

---

## 3. Exact Ordered Protocol Sequence

The Stage 2A sequence consists of up to four discrete SPI transactions separated by host-side delays:

```
[Host Controller]                            [FocalTech Sensor]
       |                                             |
       |--- 1. write_byte 0xC6, 0x01 (CS Low) ------>| (Ungate SPI clock)
       |<-- CS Deasserts High -----------------------|
       |                                             |
       |    [ 4 ms Host Sleep (CS High) ]            | (Transceiver lock)
       |                                             |
       |--- 2. read_byte 0xC6 (CS Low) ------------->|
       |<-- Returns 0x01 in byte 4 (CS High) --------| (Verify clock active)
       |                                             |
       |--- 3. read_word 0x1A8B (CS Low) ----------->|
       |<-- Returns Chip ID & CRC (CS High) ---------| (Interrogate ID)
       |                                             |
       |    [ If Chip ID == 0x9391 ]:                |
       |--- 4. read_word 0x1816 (CS Low) ----------->|
       |<-- Returns Sub-Revision & CRC (CS High) ----| (Distinguish 9395)
       |                                             |
       v                                             v
  [STOP & RUN IMMEDIATE ROLLBACK]              [Residual State 0xC6=1]
```

### Detailed Byte Sequences

| Step | Operation | Opcode / Target | Transmitted Bytes (MOSI Hex) | Len | Expected MISO | Success Criteria |
| :---: | :--- | :---: | :--- | :---: | :--- | :--- |
| **1** | Write Byte | `0x09` -> `0xC6` | `09 F6 C6 01` | 4 B | None (`NULL` rx) | `ioctl(SPI_IOC_MESSAGE)` returns 4. |
| **—** | Delay | — | — | — | — | `usleep(4000)` (4 ms). **CS DEASSERTED HIGH.** |
| **2** | Read Byte | `0x08` -> `0xC6` | `08 F7 C6 00 00` | 5 B | `?? ?? ?? ?? D0` | `ioctl` returns 5 AND `D0 == 0x01`. |
| **3** | Read Word | `0x04` -> `0x1A8B` | `04 FB 9A 8B 00 00 00 00 00 00` | 10 B | `?? ?? ?? ?? ?? ?? D1 D0 C1 C0` | Valid ID (`0x9362`, `0x9365`, `0x9391`) AND CRC valid. |
| **4** | Read Word *(cond)* | `0x04` -> `0x1816` | `04 FB 98 16 00 00 00 00 00 00` | 10 B | `?? ?? ?? ?? ?? ?? E1 E0 C1 C0` | *Only if ID==0x9391*: Extract revision word. |

---

## 4. Expected Response Offsets and CRC Rules

### 4.1 Response Offsets
- **Read Byte (`0xC6`):**
  - Offset 0–3: Master transmission turnaround echo.
  - Offset 4 (`buffer[4]`): Transceiver acknowledgment byte. Must equal `0x01`.
- **Read Word (`0x1A8B` and `0x1816`):**
  - Offsets 0–5: Command header and 16 turnaround clock cycles.
  - Offset 6 (`buffer[6]`): Data Word High Byte (MSB).
  - Offset 7 (`buffer[7]`): Data Word Low Byte (LSB).
  - Calculated Value: `word = ((uint16_t)buffer[6] << 8) | buffer[7]`.
  - Offset 8 (`buffer[8]`): CRC-CCITT High Byte.
  - Offset 9 (`buffer[9]`): CRC-CCITT Low Byte.
  - Received CRC: `crc_rx = ((uint16_t)buffer[8] << 8) | buffer[9]`.

### 4.2 CRC-CCITT Specification
- **Algorithm:** CRC-CCITT (polynomial `0x1021`, initial seed `0xFFFF`, MSB-first bit shifts).
- **Data Covered:** Evaluated strictly over `buffer[6]` and `buffer[7]` (2 data payload bytes).
- **Zero-CRC Distinction:**  
  If `buffer[8..9] == 0x0000`, the reference driver bypasses CRC verification ([`ft93xx.c:209`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/ft93xx.c#L209)).  
  **Important:** Bypassing CRC on zero is a **software heuristic of the reference driver**, NOT physical proof of data validity. If a zero CRC is received, the test harness must log: `CRC: UNVERIFIED (ZERO-VALUE EXCEPTION)`.

---

## 5. Explicitly Allowed vs Prohibited Access

### 5.1 Explicitly Allowed Operations
- `write_byte` to register `0xC6` with value `0x01` (4 bytes).
- `read_byte` from register `0xC6` (5 bytes).
- `read_word` from register `0x1A8B` (10 bytes).
- `read_word` from register `0x1816` (10 bytes) — **only if register `0x1A8B` returns `0x9391`**.

### 5.2 Explicitly Prohibited Operations
- ❌ **ANY OTP / EFUSE access:** Registers `0xF0` through `0xFF` (including `0xF1`, `0xF3`, `0xF4`, `0xF6`, `0xF7`).
- ❌ **ANY Analog voltage configuration:** Registers `0xFD` and `0xFE`.
- ❌ **ANY Interrupt status clear:** Register `0x1A84`.
- ❌ **ANY Oscillator / clock trim modification:** Register `0x1A8E`.
- ❌ **ANY Command byte sequences:** Commands `0x5A`, `0xA5`, `0xC0`, `0xC4`.
- ❌ **ANY Burst reads / FIFO reads:** Opcode `0x06`.
- ❌ **ANY GPIO Reset toggling:** Pad 14 must remain static inactive-high.
- ❌ **ANY GPIO Interrupt manipulation:** Pad 229 must not be unmasked or waited upon.

---

## 6. Deterministic Failure Handling (Strict STOP Policy)

If ANY anomaly occurs during execution, the test harness must immediately **STOP**, log the raw telemetry, and invoke host rollback. **No retries, no recovery writes, and no resets are permitted.**

| Failure Condition | Diagnostic Observation | Mandatory Action |
| :--- | :--- | :--- |
| **Write Transport Error** | `ioctl` fails or returns `< 4` on `0xC6` write. | **STOP.** Log errno. Abort to rollback. |
| **Verify Read Transport Error** | `ioctl` fails or returns `< 5` on `0xC6` read. | **STOP.** Log errno. Abort to rollback. |
| **Verify Value Mismatch** | `buffer[4] != 0x01` on `0xC6` read. | **STOP.** Log received byte. Abort to rollback. |
| **All-Zeroes Bus Response** | `buffer[6..7] == 0x0000` on `0x1A8B`. | **STOP.** Log grounded/inactive bus. Abort to rollback. |
| **All-Ones Bus Response** | `buffer[6..7] == 0xFFFF` on `0x1A8B`. | **STOP.** Log floating/unpowered bus. Abort to rollback. |
| **CRC Mismatch** | `crc_computed != crc_rx` (when `crc_rx != 0x0000`). | **STOP.** Flag corrupted transmission. Abort to rollback. |
| **Unknown Chip ID** | Value is non-zero/non-FFFF, but not `0x9362`, `0x9365`, or `0x9391`. | **STOP.** Log unproven ID. Abort to rollback. |
| **Sub-Revision Read Error** | Transport error or CRC mismatch on `0x1816`. | **STOP.** Log base ID `0x9391`, sub-rev UNKNOWN. Rollback. |

---

## 7. Sensor Variant Compatibility Gate

Only variants with dedicated initialization and identification source evidence in the reference driver repository are authorized for recognition:

| Reported ID (`0x1A8B`) | Sub-Rev (`0x1816`) | Identified Silicon | Primary Source Evidence | Action |
| :---: | :---: | :--- | :--- | :---: |
| **`0x9362`** | — | **FT9362 / FW9369** | `fw9369_startup.c:338`, `fw9369_release.c:442` | Accept & Log |
| **`0x9365`** | — | **FT9365** | `factory_startup.c:220`, `test-ft93xx-backend.c:212` | Accept & Log |
| **`0x9391`** | `!= 0x0FFF` | **FT9391** | `ft93xx.c:551` | Accept & Log |
| **`0x9391`** | `== 0x0FFF` | **FT9395** | `ft93xx.c:555` | Accept & Log |
| *Any Other Value* | — | **UNKNOWN / UNPROVEN** | None | **MANDATORY STOP** |

---

## 8. Host-Side Rollback Steps (Pure spidev)

Regardless of whether identification succeeds or triggers a STOP, the test harness must execute the verified pure-spidev rollback sequence:
1. Close file descriptor `/dev/spidev1.0` and terminate userspace tool.
2. Unbind `spi-FTE3600:00` from `spidev`:  
   `echo "spi-FTE3600:00" > /sys/bus/spi/drivers/spidev/unbind`
3. Clear driver override:  
   `echo "" > /sys/bus/spi/devices/spi-FTE3600:00/driver_override`
4. Unload spidev module:  
   `modprobe -r spidev`
5. Perform read-only verification: confirm `spidev` is ABSENT, 0 `/dev/spidev*` character devices present, and `spi-FTE3600:00` is UNBOUND.
6. *(Archived v1.0 Step Omitted)*: `fte3600.ko` and `uio.ko` are **never loaded** and GPIO lines are **never claimed**, leaving Pad 14 and Pad 229 untouched in their exact boot baseline states.

---

## 9. Sensor-Internal State Uncertainty Disclosure

The operator must understand the physical distinction between host-side restoration and sensor internal state:
- **Host State (Guaranteed Restored):** 100% restored. Mainline `spidev` module is unloaded, `/dev/spidev*` nodes are destroyed, and sysfs `driver_override` is cleared. SPI bus clock is halted and Chip Select is held deasserted high. PCH Pad 14 and Pad 229 were never touched.
- **Sensor Internal State (Uncertain / Unverified):** Operation 1 wrote `0xC6 = 0x01` to ungate the sensor's internal SPI transceiver clock. The available evidence does not prove the persistence, decay, or autonomous reset behavior of register `0xC6`:
  - **Unknown Retention Behavior:** Whether register `0xC6` retains `0x01`, decays over time, or autonomously resets via an undocumented internal watchdog timer is unknown.
  - **Host Cleanup Cannot Confirm Restoration:** Host-side rollback releases host resources, but cannot inspect or confirm the physical internal state of the sensor without issuing further SPI transactions (which would contradict cleanup isolation).
  - **Full Physical Reset:** If a return to a guaranteed unclocked power-on baseline is required, the operator should perform a full ACPI S5 cold reboot or power cycle.

---

## 10. Final Authorization Boundary

> ### ⚠️ STRICT EXECUTION GATE
> 
> **No automated execution is authorized.**  
> The agent has completed the safety audit, claims correction, and protocol checklist.  
> Execution of Stage 2A requires a separate, explicit user instruction and must be performed under direct user oversight.
