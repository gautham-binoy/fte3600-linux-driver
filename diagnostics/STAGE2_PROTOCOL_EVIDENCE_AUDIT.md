# FTE3600 Linux Driver — Stage 2 Protocol Evidence Audit

**Date/Time:** 2026-11-02T14:30:00+05:30  
**Target Device:** `spi-FTE3600:00` on Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Audit Type:** Static Source Code & Protocol Evidence Audit (Zero Hardware Access)  
**Evaluated Documents:**
- `diagnostics/STAGE2_READ_ONLY_IDENTIFICATION_SPEC.md`
- `diagnostics/STAGE1_GPIO_RESTORATION_INVESTIGATION.md`
- `PROGRESS.md`
- Reference Driver Source: `research/libfprint-fte3600/libfprint/drivers/fte3600/` (`ft93xx.c`, `linux_board.c`, `fw9369_startup.c`, `fw9369_release.c`, `capture.c`)
- Windows Driver Strings: `research/dynabook-windows-driver/strings/` (`ftWbioUmdfDriverV2.dll`, `ftWbioEngineAdapter.dll`)
- ACPI DSDT Specification: `diagnostics/acpi/` (`_SB.PC00.SPI1.FP01`)
- Kernel Pin Controller Specification: `drivers/pinctrl/intel/pinctrl-intel.h`

---

## Executive Summary & Final Verdict

### Final Verdict: `BLOCKED — SENSOR INITIALIZATION REQUIRED`

### Verdict Justification
1. **The "Read-Only" Identification Paradox:**  
   The proposed Stage 2 specification (`diagnostics/STAGE2_READ_ONLY_IDENTIFICATION_SPEC.md`) establishes a two-phase test where Phase 1 attempts a pure, zero-register-write read of Chip ID register `0x1A8B`. However, exhaustive audit of the primary FocalTech reference implementation (`ft93xx.c:520-540` and `fw9369_startup.c:328-338`) demonstrates that **no bare read on register `0x1A8B` is ever executed on a cold or uninitialized sensor**. On cold boot or post-reset, the sensor's internal high-speed SPI transceiver logic and digital register domain are clock-gated. A bare read without prior wake-up returns floating bus lines (all `0x00` or all `0xFF`), failing identification.
2. **Mandatory Register Writes:**  
   To successfully read `0x1A8B`, the reference implementation mandates active preparatory register writes:
   - Writing volatile register `0xC6 = 0x01` (`spi_mode()`, `ft93xx.c:500-518`) to activate the SPI transceiver clock.
   - Writing volatile registers `0xFD = 0x0A` and `0xFE = 0x7F` (`probe_inner()`, `ft93xx.c:529-536`) to configure internal reference voltages.
   Because these operations are explicit register write transactions (`write_byte`, opcode `0x09`), they directly violate a strict zero-write / "read-only" mandate.
3. **Idle Command Ambiguity:**  
   The proposed "idle" command byte sequences (`0x5A` and `0xA5`) carry conflicting semantics across the vendor code base (alternately termed `idle`, `wake`, or `configuration_prefix`) with completely undocumented hardware state transitions. They cannot be authorized without hardware documentation.
4. **Conclusion:**  
   Stage 2 hardware execution cannot be authorized under a "pure read-only" charter. Identification requires a revised specification that explicitly permits and restricts **volatile initialization writes** while continuing to strictly bar non-volatile (Flash/OTP/EFUSE) writes and destructive operations.

---

## 1. Byte-by-Byte Verification of Read Packet

The proposed 10-byte identification packet for reading register `0x1A8B`:
```text
MOSI: [ 0x04, 0xFB, 0x9A, 0x8B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 ]
MISO: [   ??,   ??,   ??,   ??,   ??,   ??,   D1,   D0,  CRCH, CRCL ]
```

### 1.1 Detailed Byte Analysis
- **Byte 0 (`0x04` — Opcode):**  
  *Evidence:* `ft93xx.c:204` (`read_word`).  
  `0x04` is the proprietary FocalTech SPI opcode for a 16-bit word read (`READ_WORD_16`). In comparison, opcode `0x05` is Write Word, `0x08` is Read Byte, and `0x09` is Write Byte.
- **Byte 1 (`0xFB` — One's Complement Verification):**  
  *Evidence:* `ft93xx.c:204` (`0xfb = ~0x04`).  
  The FocalTech SPI bus interface enforces hardware noise immunity by requiring every command opcode byte to be followed immediately by its bitwise bit-inverted complement (`~cmd`). If Byte 1 does not equal `~Byte 0`, the hardware aborts the command without asserting MISO.
- **Byte 2 (`0x9A` — Encoded High Address Byte):**  
  *Evidence:* `ft93xx.c:204` (`(uint8_t)(0x80 | (address >> 8))`).  
  For address `0x1A8B`: `address >> 8 = 0x1A` (`0001 1010b`).  
  Bit 7 is forced high (`0x80`), yielding `0x80 | 0x1A = 0x9A` (`1001 1010b`). In FocalTech framing, Bit 7 set to 1 designates word-addressed internal register space.
- **Byte 3 (`0x8B` — Low Address Byte):**  
  *Evidence:* `ft93xx.c:205` (`(uint8_t)address`).  
  For address `0x1A8B`: `address & 0xFF = 0x8B`.
- **Bytes 4–5 (`0x00, 0x00` — Turnaround Latency Cycles):**  
  *Evidence:* `ft93xx.c:205` (`0, 0`).  
  The sensor internal microcontroller requires 16 clock cycles (2 dummy bytes) while Chip Select remains asserted low to decode the address, initiate internal SRAM/register read, and latch the data into the SPI shift register.
- **Bytes 6–7 (Data Response Word):**  
  *Evidence:* `ft93xx.c:225` (`*value = (uint16_t)((uint16_t)buffer[6] << 8 | buffer[7])`).  
  The 16-bit register value is clocked out on MISO MSB first: Byte 6 is MSB, Byte 7 is LSB.
- **Bytes 8–9 (CRC-CCITT Validation):**  
  *Evidence:* `ft93xx.c:209-224`.  
  - **Algorithm:** CRC-CCITT (polynomial `0x1021`, initial seed `0xFFFF`, MSB-first bit shifts).
  - **Data Covered:** Evaluated strictly over payload bytes 6 and 7:
    ```c
    uint16_t crc = 0xffff;
    for (size_t i = 6; i < 8; ++i) {
        crc = (uint16_t)(crc ^ ((unsigned int)buffer[i] << 8));
        for (unsigned int bit = 0; bit < 8; ++bit)
            crc = (uint16_t)(((unsigned int)crc << 1) ^ ((crc & 0x8000) != 0 ? 0x1021u : 0u));
    }
    ```
  - **Bypass Rule:** `if (buffer[8] != 0 || buffer[9] != 0)`. If both CRC bytes returned on MISO are `0x00`, CRC verification is skipped by the driver. This accommodates hardware variants or boot phases where CRC calculation is disabled.
- **Chip Select (CS) Framing Behavior:**  
  *Evidence:* `linux_board.c:427-440` (`tr.len = (uint32_t)size; checked_ioctl(b, b->spi, SPI_IOC_MESSAGE(1), &tr);`).  
  The transaction is submitted as a single 10-byte full-duplex transfer. CS is asserted active-low by the GSPI controller before byte 0 and **remains asserted continuously** throughout all 10 bytes, deasserting high only after byte 9 is clocked.

---

## 2. SPI Parameter Evidence Matrix

| Parameter | Value | Status | Primary Evidence / Source Citation |
| :--- | :--- | :---: | :--- |
| **SPI Mode** | `SPI_MODE_0` (CPOL=0, CPHA=0) | **VERIFIED** | ACPI DSDT `\_SB.PC00.SPI1.FP01` (`ClockPolarityLow`, `ClockPhaseFirst`); `kernel/fte3600.c:86-93`; `linux_board.c:690`. |
| **Clock Frequency** | 1,000,000 Hz (1 MHz) | **VERIFIED** | ACPI DSDT `_CRS` (`connection_speed = 1000000`); `kernel/fte3600.c:92`; `linux_board.c:700`. |
| **Bits Per Word** | 8 | **VERIFIED** | ACPI DSDT `_CRS` (`DataBitLength = 8`); `linux_board.c:431, 704`. |
| **Bit Order** | MSB First | **VERIFIED** | Linux `spidev` default; Intel GSPI controller architecture; bit-shift logic in `ft93xx.c:204, 214, 225`. |
| **Chip Select** | Active Low | **VERIFIED** | ACPI DSDT `_CRS` (`device_polarity = PolarityLow`); controller managed (`cs_control = 1`). |
| **Inter-Byte Delay** | 0 µs | **VERIFIED** | Single contiguous transfer `tr.len = 10` in `linux_board.c:427-432`. Dummy bytes 4-5 provide internal latency. |
| **Inter-Retry Delay** | 10 ms | **INFERRED** | `ft93xx.c:167` (`wait_ms(op, 10)`); empirical polling intervals in `spi_mode` (4 ms) and `probe_inner` (1 ms). |

---

## 3. "Read-Only" vs "Electrically Passive" Analysis

### 3.1 Physical Nature of SPI Communication
It is critical to distinguish between **register-space read-only operations** and **electrically passive observation**:
- An **electrically passive** inspection involves zero active signal driving (e.g. logic analyzer or oscilloscope listening to already active traces).
- SPI is a strictly synchronous master-slave protocol. The sensor is a slave and has no pull-up, pull-down, or autonomous clock. To read a single register:
  1. The host GSPI controller **actively drives** the Chip Select line low.
  2. The host GSPI controller **actively generates** 80 clock cycles at 1 MHz on the SCK line.
  3. The host GSPI controller **actively drives** MOSI with the 10-byte request frame.
Calling an SPI read transaction "passive" is physically inaccurate. It is an active master-driven bus transaction.

### 3.2 Reference Implementation Audit on Cold Sensor Behavior
The specification proposes sending a bare read packet (`04 FB 9A 8B ...`) to a cold sensor without preparatory writes.
An exhaustive audit of the reference drivers reveals that **this sequence is never executed in production**:

```c
/* research/libfprint-fte3600/libfprint/drivers/fte3600/ft93xx.c:520-544 */
static int
probe_inner (const struct operation *op, struct fte3600_ft93xx_identity *identity)
{
  uint8_t byte = 0;
  uint16_t word;

  /* Step 1: Active preparatory voltage configuration writes */
  for (unsigned int attempt = 0; attempt < 5; ++attempt)
    {
      if (!write_byte (op, 0xfd, 0x0a) || !write_byte (op, 0xfe, 0x7f))
        return 0;
      wait_ms (op, 1);
      if (!read_byte (op, 0xfe, &byte))
        return 0;
      if (byte == 7f)
        break;
    }
  ...
  /* Step 2: Active SPI mode activation write (register 0xc6 = 1) */
  if (!spi_mode (op))
    return 0;

  /* Step 3: Only NOW is Chip ID read attempted */
  for (unsigned int attempt = 0; attempt < 3; ++attempt)
    {
      if (!read_word (op, 0x1a8b, &word))
        return 0;
...
```

**Finding:**  
When the sensor boots up (or after hardware reset), its internal high-speed SPI logic is unclocked and in low-power idle. If the host issues a bare read without executing the `0xFD/0xFE` voltage handshake and `0xC6` clock enable, the sensor will not drive MISO. The host will read bus float (`0x00` or `0xFF`). A strictly zero-write read test is virtually guaranteed to fail.

---

## 4. Phase 2 Assessment: Volatile Wake-Up (`09 F6 C6 01`)

### 4.1 Packet Breakdown
- **Byte 0 (`0x09`):** Opcode for Write Byte (`write_byte`, `ft93xx.c:196`).
- **Byte 1 (`0xF6`):** Inverted complement (`~0x09 = 0xF6`).
- **Byte 2 (`0xC6`):** Register address `0xC6`.
- **Byte 3 (`0x01`):** Value written (`0x01`).

### 4.2 Function and Volatility of Register `0xC6`
- **Function:** Controlled by `spi_mode()` in `ft93xx.c:500-518`. Register `0xC6` gates the internal clock to the sensor's high-speed SPI transceiver and digital controller core. Setting Bit 0 enables the SPI engine; clearing it gates the clock to save power.
- **Volatility:** Register `0xC6` resides entirely in volatile flip-flops within the peripheral digital logic. It does **not** map to Flash memory, OTP, or EEPROM. Upon power removal or assertion of the hardware reset line (Pad 14), register `0xC6` instantly resets to `0x00`.
- **Constraint Assessment:**  
  Writing `0xC6 = 1` carries **zero risk of permanent hardware alteration**. However, because opcode `0x09` is an explicit register write, it violates a strict "read-only" mandate. If Stage 2 is to succeed, the user must explicitly authorize volatile initialization writes.

---

## 5. Idle Command Audit (`5A A5 00` & `A5 5A 00`)

The proposed specification suggests issuing "idle" commands (`5A A5 00` and `A5 5A 00`) upon test completion. Audit of the codebase reveals severe contradictions and lack of documentation:
1. **Contradictory Semantic Names:**
   - In `ft93xx.c:259-281`, `0x5A` and `0xA5` are called inside `idle()`.
   - In `fw9369_release.c:451`, `{0x5a, 0xa5, 0}` is explicitly named `wake`, while `{0xa5, 0x5a, 0}` is named `end`.
   - In `factory_startup.c:150-158`, both commands are issued during sensor wake-up initialization.
   - In `capture.c:221-228`, they are issued as framing prefixes around image capture.
2. **Undocumented Hardware State Transitions:**
   There is zero public documentation or vendor specification explaining what internal state machine transitions are triggered by `0x5A` vs `0xA5`. In `ft93xx.c:268-277`, sending `0x5A` requires polling register `0x80` until it returns `0x50`, and if it does not, repeatedly sending `0xC0` (`command(op, 0xc0)`).
3. **Audit Verdict on Idle Commands:**  
   **UNKNOWN / PROHIBITED.** These commands should not be blindly issued in Stage 2. Returning the sensor to a clean state must rely on driver rollback (unbinding `spidev` and releasing GPIOs) rather than unverified magic byte sequences.

---

## 6. Chip Identification Claims & Model Matrix

### 6.1 Register Architecture
- Primary Chip ID: Register `0x1A8B` (16-bit word).
- Sub-Revision / Window Extension: Register `0x1816` (16-bit word).

### 6.2 Reference Driver Evidence (`ft93xx.c:12-23, 543-558`)
In `probe_inner()`, after reading `0x1A8B`:
```c
if (lookup (word) != NULL) {
    if (word == 0x9391) {
        if (!read_word (op, 0x1816, &word))
            return 0;
        if (word == 0x0fff)
            identity->chip_id = 0x9395;
    }
    return 1;
}
```
If register `0x1A8B` returns `0x9391`, the driver performs a secondary read of register `0x1816`. If `0x1816` returns `0x0FFF`, the silicon is actually an **FT9395** (36x176 pixels). If not `0x0FFF`, it is an **FT9391** (40x196 pixels).

### 6.3 Separation of Windows Driver Evidence from Physical Hardware
- **Windows Driver Strings:** Analysis of `research/dynabook-windows-driver/strings/` revealed:
  - `ftWbioEngineAdapter.dll`: Contains string `FtRansacEdage_32f_FT9391`.
  - `ftWbioUmdfDriverV2.dll`: Contains classes and strings for `FT9338`, `FT9348`, `FT9361`, `FT9368`, `FT9369`, `FT9371`, `FT9536`, and `FT9362`.
- **Critical Distinction:**  
  The string `FtRansacEdage_32f_FT9391` proves that the Dynabook Windows driver package *includes support* for FT9391 algorithms. It **does not prove** that the physical sensor soldered to this Dynabook Satellite Pro C40-J motherboard is an FT9391. Dynabook utilizes common driver installers across multiple notebook models containing different FocalTech sensors (such as FT9362, FT9369, or FT9395). Only physical interrogation of register `0x1A8B` can conclusively identify the hardware.

---

## 7. GPIO Pad 229 Restoration Review

In `STAGE1_GPIO_RESTORATION_INVESTIGATION.md`, Pad 229 showed a register difference between baseline and post-rollback:
- **Baseline:** `0x80100100 0x00000029`
- **Post-Rollback:** `0x82000102 0x00000029`

### 7.1 Register Bitfield Verification (`drivers/pinctrl/intel/pinctrl-intel.h`)
- **Bit 1 (`0x02` vs `0x00`):** `PADCFG0_GPIORXSTATE`.  
  This is a **read-only live input state bit**. It reflects the instantaneous physical voltage sampled on the pad input buffer. Because Pad 229 has `PullNone` termination (`PADCFG1 = 0x00000029`), the line is high-impedance and floats.
- **Bits 25 & 20 (`0x02000000` vs `0x00100000`):**  
  - Baseline `0x80100000`: `PADCFG0_GPIROUTIOXAPIC = BIT(20)` with level trigger (`PADCFG0_RXEVCFG_LEVEL = 0`).
  - Post-test `0x82000000`: `PADCFG0_RXEVCFG_EDGE = 1 << 25`.
- **Kernel Lifecycle:**  
  When `fte3600.ko` probed, `pinctrl-intel` configured the interrupt to edge-triggered rising to match the ACPI DSDT declaration (`GpioInt, Edge, ActiveHigh`). When `fte3600.ko` was removed, Linux `gpiolib` released the interrupt resources via `.irq_release_resources = gpiochip_irq_relres`. Linux pinctrl drivers do not re-mux or restore pad configuration registers back to firmware boot defaults on unbind.
- **Safety Assessment:**  
  Throughout and after the test, `PADCFG0_GPIOTXDIS` (`BIT(8) = 0x100`) remained permanently set (output buffer disabled / input-only mode). The pad cannot drive voltage or draw current. Zero electrical risk exists.

---

## 8. Unresolved Questions & Missing Documentation

1. **Vendor Datasheet Absence:**  
   No official FocalTech FT9391 / FT9362 hardware register specification is publicly available. All knowledge is derived from the reverse-engineered `libfprint-fte3600` driver.
2. **Registers `0xFD` and `0xFE` Purpose:**  
   The reference driver performs a handshake writing `0xFD = 0x0A` and `0xFE = 0x7F` before reading `0xFE`. The physical circuitry controlled by these registers (analog bias, internal LDO voltage, or oscillator trimming) is undocumented.
3. **Power-On Reset Settling Time:**  
   The settling latency required between ACPI power assertion (`_PS0`) and the first valid SPI transaction is not documented in hardware errata.

---

## 9. Recommendations for Stage 2 Authorization

To safely and successfully proceed to Stage 2:
1. **Abandon "Pure Read-Only" Assumption:**  
   Do not attempt a Phase 1 zero-write read; reference evidence proves it will return bus float.
2. **Formulate a "Volatile Initialization" Specification:**  
   Define Stage 2 as permitting strictly **volatile initialization writes** (`write_byte 0xc6, 1`, and if required, `write_byte 0xfd, 0x0a` and `write_byte 0xfe, 0x7f`).
3. **Maintain Absolute Prohibition on Non-Volatile Writes:**  
   Strictly forbid registers `0xF1`, `0xF4`, `0xF6`, and `0xF7` (OTP / Flash programming).
4. **Omit Undocumented Idle Commands:**  
   Do not transmit `5A A5 00` or `A5 5A 00`. Let rollback unload modules and return pins to baseline.
