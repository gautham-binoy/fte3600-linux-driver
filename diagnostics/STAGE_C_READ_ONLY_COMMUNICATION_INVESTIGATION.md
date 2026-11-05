# Stage 2E.16 — Stage C Preparation: Read-Only Sensor Communication Investigation

**Target System:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Target SPI Device:** `spi-FTE3600:00` on Intel LPSS SPI2 (`pxa2xx-spi.2`, CS0)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (x86_64, SMP PREEMPT_DYNAMIC)  
**Investigation Timestamp:** 2026-11-04T09:39:30+05:30  
**Status:** **INVESTIGATION COMPLETE — ZERO HARDWARE TRANSFERS ISSUED — READY FOR STAGE C TEST DESIGN**

---

## 1. Investigation of Available Protocol Documentation

A comprehensive audit of repository research, vendor string dumps, reference implementations, and ACPI DSDT firmware definitions was conducted to establish verified protocol facts and isolate unverified assumptions.

### 1.1 Documentation Sources Audited
1. **FocalTech Reference Implementation:** `research/libfprint-fte3600/libfprint/drivers/fte3600/`
   - `ft93xx.c`: Low-level SPI frame construction, byte/word read/write opcodes, wire CRC-16 algorithm, and probe routines.
   - `linux_board.c`: Linux SPI userspace transport ioctls, timing delays, and SPI configuration.
   - `fw9369_startup.c`: Chip startup sequence, transceiver clock gating, and OTP/EFUSE interactions.
   - `fte3600-ids.c`: ACPI device discovery (`FTE3600`) and device identification.
2. **ACPI DSDT Firmware Definition:** `diagnostics/acpi/dsdt.dsl` (`\_SB.PC00.SPI1.FP01`)
   - Hardware bus assignment, clock polarity, clock phase, speeds, and GPIO pin mapping.
3. **Windows Driver Package Analysis:** `research/dynabook-windows-driver/strings/`
   - String dumps from `ftWbioUmdfDriverV2.dll` and `ftWbioEngineAdapter.dll`.
4. **Prior Project Audits:**
   - `diagnostics/STAGE2_PROTOCOL_EVIDENCE_AUDIT.md`
   - `STAGE2D2_MINIMAL_INITIALIZATION_SAFETY_AUDIT.md`
   - `STAGE2E4_IMPLEMENTATION_REPORT.md`

---

### 1.2 Protocol Structure & Framing Rules

The FocalTech FT93xx SPI protocol is a proprietary, full-duplex synchronous master-slave protocol governed by strict framing rules:

```text
Full-Duplex Packet Layout:
MOSI: [ Opcode, ~Opcode, Addr_H / Addr, Addr_L, Dummy_0, Dummy_1, ... Data/Padding ... ]
MISO: [   ??  ,    ??  ,      ??       ,   ??  ,   ??   ,   ??   , D_MSB, D_LSB, CRC_H, CRC_L ]
```

1. **Noise-Immune Inverted Complement:**
   - Every command opcode byte must be immediately followed by its bitwise 1's complement (`~opcode`).
   - If `MOSI[1] != ~MOSI[0]`, the internal sensor hardware controller aborts the command immediately and leaves MISO undriven.
2. **Command Opcodes:**
   - `0x04` / `~0xFB`: Read Word (16-bit)
   - `0x05` / `~0xFA`: Write Word (16-bit)
   - `0x08` / `~0xF7`: Read Byte (8-bit)
   - `0x09` / `~0xF6`: Write Byte (8-bit)
3. **Address Encoding:**
   - **8-Bit Registers:** 1-byte address directly follows complement (e.g. `0xC6`).
   - **16-Bit Registers:** Encoded across 2 bytes with Bit 7 of the high address byte forced to 1:
     $$\text{Byte 2} = 0x80 \mid (\text{Address} \gg 8), \quad \text{Byte 3} = \text{Address} \ \& \ 0xFF$$
     For register `0x1A8B`: $\text{Byte 2} = 0x80 \mid 0x1A = 0x9A$, $\text{Byte 3} = 0x8B$.
4. **Turnaround Dummy Latency:**
   - Reading 16-bit word registers requires 2 turnaround dummy bytes (`0x00 0x00`) while Chip Select remains asserted low. This allows the internal microcontroller 16 clock cycles to fetch data from internal register logic into the output shift register.
5. **Wire CRC-16 Algorithm:**
   - Standard CCITT polynomial: $x^{16} + x^{12} + x^5 + 1$ (`0x1021`), initial seed `0xFFFF`, MSB-first.
   - Evaluated exclusively over data bytes 6 and 7.
   - If MISO returns `0x00 0x00` in bytes 8..9, CRC verification is skipped by reference drivers (accommodating boot phases or hardware variants without CRC calculation enabled).

---

### 1.3 Verified Facts vs Unverified Assumptions

| Protocol Aspect | Verification Status | Source Reference | Evidence / Notes |
| :--- | :---: | :--- | :--- |
| **SPI Mode 0** | **VERIFIED FACT** | `dsdt.dsl:70278-70279` | `ClockPolarityLow` (CPOL=0), `ClockPhaseFirst` (CPHA=0). |
| **Bus Speed 1 MHz** | **VERIFIED FACT** | `dsdt.dsl:70278` | ACPI `_CRS` specifies `connection_speed = 0x000F4240` (1,000,000 Hz). |
| **Chip Select Active-Low** | **VERIFIED FACT** | `dsdt.dsl:70277` | ACPI `_CRS` specifies `PolarityLow`. |
| **Data Word Length 8-bit** | **VERIFIED FACT** | `dsdt.dsl:70277` | ACPI `_CRS` specifies `DataBitLength = 0x08`. |
| **Reset Line Timing** | **VERIFIED FACT** | `recovery.c:19`, `factory_startup.c:341` | 40 ms total (10 ms settle, 20 ms assert, 10 ms settle). |
| **Register 0xC6 Volatility** | **VERIFIED FACT** | `ft93xx.c:500-518`, `fw9369_startup.c:330` | Clock gating register; resets to 0x00 on hardware reset or power down. |
| **Primary Chip ID Register** | **VERIFIED FACT** | `ft93xx.c:543`, `fw9369_startup.c:338` | Register `0x1A8B` returns 16-bit hardware identifier. |
| **Physical Silicon Identity** | **UNVERIFIED ASSUMPTION** | Windows driver strings | `ftWbioUmdfDriverV2.dll` lists FT9362, FT9368, FT9369, FT9391. Physical silicon can only be proven via bus interrogation. |
| **Bare Read Capability** | **DISPROVEN ASSUMPTION** | `STAGE2_PROTOCOL_EVIDENCE_AUDIT.md` | Post-reset sensor has gated SPI clock. Bare read without `0xC6=0x01` returns bus float (`0x00` or `0xFF`). |
| **Magic Idle Sequences** | **UNVERIFIED / FORBIDDEN** | `ft93xx.c:259`, `fw9369_release.c:451` | Sequences `0x5A/0xA5` carry contradictory names (`wake` vs `idle`) and undocumented internal state machine side effects. |

---

## 2. Kernel Driver Implementation Inspection

The active driver source [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c) and header [`kernel/fte3600.h`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.h) were inspected:

### 2.1 SPI Transfer Engine: `fte3600_spi_sync_transfer()`
- **Location:** [`kernel/fte3600.c:173-224`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L173-L224)
- **Concurrency & Locking:** Enforces `lockdep_assert_held(&data->lock)`.
- **Safety Checks:**
  - Validates `!data->spi` and `!atomic_read(&data->offline)`.
  - Rejects `NULL` buffers and zero-length transfers.
  - Enforces upper bound `len <= FTE3600_MAX_SPI_TRANSFER_LEN` (10,246 bytes).
- **Execution:** Dispatches synchronous `spi_sync(data->spi, &m)`.
- **Fail-Closed Semantics:** On any error or short transfer (`actual_length != len`), immediately sets `data->state = FTE3600_STATE_UNKNOWN`, logs error, and aborts.

### 2.2 Bus Configuration in Probe: `fte3600_spi_probe()`
- **Location:** [`kernel/fte3600.c:610-619`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L610-L619)
  ```c
  spi->mode = SPI_MODE_0;
  spi->bits_per_word = 8;
  spi->max_speed_hz = 1000000;
  ret = spi_setup(spi);
  ```
- **Current Status:** Executed during Stage B probe; returned status 0.

### 2.3 Hardware Identification Function: `fte3600_hw_identify()`
- **Location:** [`kernel/fte3600.c:260-365`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L260-L365)
- **Precondition:** Explicitly requires `data->state == FTE3600_STATE_RESET_SEQUENCE_ISSUED`.
- **Sequential Steps:**
  1. **Step A (Clock Un-gate):** `write_byte(0xC6, 0x01)` using 4-byte frame `[ 0x09, 0xF6, 0xC6, 0x01 ]`.
  2. **Step B (Stabilization Delay):** `usleep_range(4000, 5000)` (4 ms delay with CS high).
  3. **Step C (Readback Verification):** `read_byte(0xC6)` using 5-byte frame `[ 0x08, 0xF7, 0xC6, 0x00, 0x00 ]`. Verifies `rx[4] == 0x01`.
  4. **Step D (Chip ID Interrogation):** `read_word(0x1A8B)` using 10-byte frame `[ 0x04, 0xFB, 0x9A, 0x8B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 ]`.
  5. **Step E (Wire CRC Verification):** Evaluates `fte3600_crc16_wire(&rx[6], 2)` against `rx[8..9]`.
  6. **Step F (Gating Check):** Validates that `chip_id != 0x0000`, `chip_id != 0xFFFF`, and `chip_id == 0x9362`.
  7. **State Transition:** Upon success, transitions to `FTE3600_STATE_IDENTIFIED` (state 8).

### 2.4 Call Paths & Prohibited Operations in Driver
- **`FTE3600_IOC_INIT`:** Wired to `fte3600_hw_identify(data)`. Never invoked in Stage B.
- **`FTE3600_IOC_CAPTURE`:** Unconditionally returns `-EOPNOTSUPP`. Physical FIFO transfer is disabled.
- **Forbidden Operations:** Zero calls exist in the driver for OTP/EFUSE programming (`0xF1`, `0xF4`, `0xF6`, `0xF7`), flash write, or magic idle/wake sequences (`0x5A`, `0xA5`).

---

## 3. Current Live System Evidence Review

Read-only inspection of the active system was conducted while `fte3600.ko` remains loaded under Option 1 retention:

| Inspection Target | Command / Path | Current Observed State | Evidence Meaning |
| :--- | :--- | :--- | :--- |
| **Driver Module** | `lsmod \| grep fte3600` | `fte3600 20480 0` | Loaded in kernel (Option 1 retention active). |
| **Driver State** | Stage B Snapshot 2 Telemetry | `State: 2 (RESET_SEQUENCE_ISSUED)` | Ready for identification precondition. |
| **SPI Binding Link** | `readlink -f .../spi-FTE3600:00/driver` | `/sys/bus/spi/drivers/fte3600` | Hardware ACPI SPI device bound to driver. |
| **Device Node** | `ls -la /dev/fte3600` | `crw------- 1 root root 511, 0` | Character device present and responsive. |
| **Open Descriptors** | Script cleanup audit | `0 open descriptors` across 333 processes | No leaked or active userspace handles. |
| **SPI Master Device** | Parent device node | `pxa2xx-spi.2` on Intel LPSS SPI2 | Controller platform driver active. |
| **SPI Traffic Statistics** | `/sys/bus/spi/devices/spi-FTE3600:00/statistics/` | `bytes=0, messages=0, transfers=0, errors=0` | **Physical proof that ZERO SPI transactions have been issued.** |
| **Kernel Log Status** | `dmesg` / `stage-b-results.txt` | `driver bound (passive probe, state UNINITIALIZED)` | Zero kernel errors, warnings, or faults. |

> [!IMPORTANT]
> **Reset vs SPI Independence:** The successful execution of `FTE3600_IOC_RESET` in Stage B proves strictly that the 40 ms electrical pulse was dispatched to the GPIO controller. It does **not** prove that SPI bus communication is operational, that MISO is active, or that the sensor silicon is responsive.

---

## 4. Definition of a Safe Stage C Identification Test

Based on the verified reference implementation and kernel driver architecture, a safe, non-destructive identification transaction is defined:

### 4.1 Transaction Specification
1. **Opcode & Target Registers:**
   - **Op 1 (Clock Un-gate):** Write byte `0xC6 = 0x01` (`[ 0x09, 0xF6, 0xC6, 0x01 ]`).
   - **Wait Interval:** 4 ms to 5 ms stabilization wait (`usleep_range(4000, 5000)`), CS deasserted HIGH.
   - **Op 2 (Clock Verification):** Read byte `0xC6` (`[ 0x08, 0xF7, 0xC6, 0x00, 0x00 ]`).
   - **Op 3 (Silicon Identification):** Read word `0x1A8B` (`[ 0x04, 0xFB, 0x9A, 0x8B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 ]`).
2. **SPI Parameters:**
   - **Mode:** `SPI_MODE_0` (CPOL=0, CPHA=0).
   - **Clock Rate:** 1,000,000 Hz (1 MHz).
   - **Word Length:** 8 bits/word, MSB-first.
   - **Chip Select:** Active-low, asserted continuously for the exact byte length of each transaction.
3. **Expected Responses:**
   - **Op 1:** 4 bytes received on MISO (don't care / dummy `0x00`).
   - **Op 2:** 5 bytes received on MISO; Byte 4 must equal `0x01`.
   - **Op 3:** 10 bytes received on MISO; Bytes 6..7 must equal `0x9362` (Chip ID), Bytes 8..9 must equal `0x1C53` (Wire CRC).
4. **Failure Diagnostics & Fault Isolation:**
   - **Bus Floating High (`0xFFFF`):** MISO line pulled up or disconnected.
   - **Bus Grounded (`0x0000`):** MISO line grounded or unpowered.
   - **Clock Gating Failure (`rx[4] != 0x01`):** Sensor failed to start internal SPI clock tree.
   - **Wire CRC Mismatch (`crc_rx != crc_calc`):** Signal integrity fault or line capacitance noise.
   - **Variant Mismatch (`chip_id != 0x9362`):** Silicon is an alternate FocalTech variant (e.g. FT9391).
5. **Execution Limits:**
   - **Attempts:** Exactly **ONE** identification attempt.
   - **Retries:** **ZERO RETRIES**. If any step fails, the driver transitions to `FTE3600_STATE_UNKNOWN` and halts fail-closed.
   - **Forbidden Operations:** Zero OTP/EFUSE access, zero FIFO reads, zero analog trim writes.

---

## 5. Explicit Safety Confirmation

1. **Hardware Communication Status:** Exactly **ZERO** SPI transfers, zero sensor register reads/writes, and zero bus transactions were performed during this stage.
2. **Driver Lifecycle Status:** The kernel module `fte3600.ko` remains loaded and bound to `spi-FTE3600:00` under Option 1 retention.
3. **No Safety Gate Bypass:** Legacy safety gates and cooperative locks remain intact.

---

## 6. Investigation Conclusion

STAGE C PREPARATION COMPLETE — NO HARDWARE COMMUNICATION PERFORMED
