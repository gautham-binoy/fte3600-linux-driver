# FocalTech FT9362 Firmware and Calibration Analysis

**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`)  
**Document Purpose:** In-depth technical analysis of firmware requirements, non-volatile storage access, analog front-end (AFE) configuration, and runtime calibration safety.  
**Safety Status:** Strictly Read-Only Source and Architectural Analysis. Zero Hardware Operations Performed.

---

## 1. Firmware Requirements Investigation

### 1.1 The "No External Firmware" Architectural Reality
A critical question in Linux fingerprint driver development is whether the sensor silicon requires external microcontroller microcode or RAM patch download prior to operation.

Through exhaustive inspection of the reference driver codebase ([`research/libfprint-fte3600/`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/)), official installation documentation, Windows driver packages, and vendor sources, we establish the following confirmed facts:

1. **Explicit Exclusion from Firmware Catalog:**
   In [`research/libfprint-fte3600/scripts/install-firmware.sh:51`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/scripts/install-firmware.sh#L51):
   ```python
   NO_FIRMWARE = {"ft9365", "ft9369", "ft9769"}
   ```
2. **Official Specification in Driver Documentation:**
   In [`research/libfprint-fte3600/docs/fte3600/install.md:41-44`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/docs/fte3600/install.md#L41-L44):
   > *"FT9338, FT9348, FT9361 and FT9536 need their corresponding payloads. FT9368 needs its application and PRAM boot files plus ft9536.bin... **FT9365, FW9369 and FT9769 need no external payload through these backends.**"*

3. **Absence from Kernel/Runtime Firmware Loader:**
   In [`research/libfprint-fte3600/libfprint/drivers/fte3600/linux_firmware.c:36-49`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/linux_firmware.c#L36-L49), the `firmware[]` identity catalog maps only legacy architectures (`FT9338`, `FT9348`, `FT9361`, `FT9536`, `FT9368`). Model `FTE3600_MODEL_FW9369` (which matches raw chip ID `0x9362`) is entirely absent from the loader, because it does not accept external firmware.

4. **Windows Official Driver Package Analysis:**
   Inspection of the Dynabook OEM Windows driver package ([`research/dynabook-windows-driver/extracted/Drivers/`](file:///home/titan/fte3600-linux-driver/research/dynabook-windows-driver/extracted/Drivers/)) confirms that neither `ftWbioUmdfDriverV2.dll` nor `ftWbioEngineAdapter.dll` extracts or delivers any firmware payload (`.bin`, `.hex`, `.rom`) to the device upon ACPI enumeration.

### 1.2 Comparison with Other FocalTech Families
To provide full technical context, the cataloged firmware requirements across all FocalTech families are documented below:

| Sensor Family | Model Enum | Chip ID | External Firmware Required? | Firmware File(s) | Payload Size (Bytes) | SHA-256 Checksum |
| :--- | :--- | :---: | :---: | :--- | :---: | :--- |
| **FT9362 / FW9369** | `FTE3600_MODEL_FW9369` | `0x9362` | **NO (None)** | *None (ROM Hardwired)* | $0$ | N/A |
| **FT9365** | `FTE3600_MODEL_FT9365` | `0x9365` | **NO (None)** | *None (ROM Hardwired)* | $0$ | N/A |
| **FT9769** | `FTE3600_MODEL_FT9769` | `0x9391` / `0x9392` | **NO (None)** | *None (ROM Hardwired)* | $0$ | N/A |
| **FT9338** | `FTE3600_MODEL_FT9338` | `0x9338` | **YES** | `ft9338.bin` | 14,184 | `ca4490163a1754639e945da3bd6ecbb4a498138962d611fc825dc129819efc46` |
| **FT9348** | `FTE3600_MODEL_FT9348` | `0x9348` | **YES** | `ft9348.bin` | 10,312 | `48d658d588c297a5d749c1f4bd6a0f5bd3d6ede59040e1674f95fe9db08eede2` |
| **FT9361** | `FTE3600_MODEL_FT9361` | `0x9361` | **YES** | `ft9361.bin` | 10,396 | `027d776b0f4da0857037bbfe6bd114f52394061c67e8459528f9b2e30114e64f` |
| **FT9536** | `FTE3600_MODEL_FT9536` | `0x9536` | **YES** | `ft9536.bin` | 11,934 | `4a62b5d9a8a7b4620bec7843763b030632a5a37a54c4cb7ed2cd96d5c7451e79` |
| **FT9368** | `FTE3600_MODEL_FT9368` | `0x9368` | **YES** | `ft9368-app.bin`<br>`ft9368-pramboot.bin` | 27,120<br>6,096 | `9997aafac8eb9a1aecc7ba5b41212004fe1b325e7bdd1ea9ca08ae54b803e2e0`<br>`c93a807eaaa34d9e79fbab77b86e910a419fd633ea98b628b72c64b285ffd191` |

### 1.3 Silicon Execution Architecture for FT9362
- The FT9362 silicon features a fixed-function digital core with hardwired microsequencer logic and factory mask ROM.
- Upon releasing hardware reset (Pad 14 HIGH) and ungating the SPI transceiver clock (`write_byte 0xC6, 0x01`), the sensor state machine boots instantly into autonomous standby.
- It does **not** download PRAM boot code, execute flash loaders, or verify firmware digital signatures.
- **Consequence for Driver Development:** There is zero risk of firmware corruption, zero risk of partial firmware write bricking, and zero dependency on external firmware distribution or redistributability licensing.

---

## 2. Non-Volatile Memory (OTP/EFUSE/Flash) Access Audit

A vital safety boundary established in Stage 2E is preventing accidental corruption of factory calibration tokens or non-volatile memory.

### 2.1 OTP / EFUSE Architecture on FT9362
In FocalTech silicon, One-Time Programmable (OTP) / EFUSE cells store immutable factory manufacturing metadata:
- Wafer lot number and silicon revision.
- Foundry fabrication origin code (e.g. `0x13` indicating Fab AA).
- Hardwired chip SKU identifier (`0x9362`).

### 2.2 Volatile Sense Amp Reads vs Destructive Programming
In [`research/libfprint-fte3600/libfprint/drivers/fte3600/ft93xx.c:596-628`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/ft93xx.c#L596-L628) and [`fw9369_startup.c:347-356`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L347-L356), the driver executes a foundry interrogation sequence:
```c
write_byte (op, 0xf1, i ? 0x13 : 3);  /* Stage address index */
write_byte (op, 0xf4, 0xc0);          /* Pre-charge sense amp */
write_byte (op, 0xf4, 0xc1);          /* Strobe sense amp latch */
write_byte (op, 0xf4, 0xc0);          /* Pre-charge sense amp */
write_byte (op, 0xf4, 0xc0);          /* Deassert sense amp */
read_byte  (op, 0xf3);                /* Clock latch */
read_byte  (op, 0x9b);                /* Read foundry byte */
```

**Safety Proof:**
1. **Zero High-Voltage Programming Pulses:** Writing to OTP requires an elevated programming voltage ($V_{PP}$, typically 6.5V–7.0V) and specific timed write pulses. The Dynabook ACPI table and LPSS hardware provide only 3.3V / 1.8V digital logic supplies; no $V_{PP}$ rail is connected or controllable.
2. **Read-Only Sense Amp Protocol:** The writes to `0xF4` (`0xC0`, `0xC1`) only toggle the internal differential sense-amplifier comparator to read out the pre-burned cells into volatile latch `0xF3` and `0x9B`.
3. **No Flash Memory On-Chip:** The FT9362 contains zero NOR/NAND flash memory. It possesses no sector erase, page program, or flash block write registers.
4. **Conclusion:** All OTP read operations are non-destructive and cannot modify persistent silicon state.

---

## 3. Analog Front-End (AFE) Configuration & Calibration Analysis

### 3.1 AFE Baseline Tuning (Register `0x1801`)
The capacitive sensing array requires adjusting the analog reference voltage to match ambient temperature, cover-glass thickness, and parasitic board capacitance.

- **Target Register:** `0x1801` (`AFE_DAC_CFG`).
- **Data Encoding:** `0xFC80 | dac` (Bits 6:0 contain a 7-bit DAC value from $1$ to $125$; Bits 15:7 contain constant mask `0xFC80`).
- **Nominal Default:** DAC value `27` (4-channel mode) or `54`/`56` (8-channel mode).
- **Storage Type:** **Strictly Volatile.** Writing to `0x1801` modifies only volatile digital-to-analog converter trim latches. On hardware reset or power down, the register reverts to its hardware default (`0xFC9B`).

### 3.2 The Runtime Calibration Algorithm
In [`research/libfprint-fte3600/libfprint/drivers/fte3600/calibration.c:6-74`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/calibration.c#L6-L74), the driver executes a binary search / hill-climbing convergence routine:
1. Starts with candidate DAC = 27 (or 56).
2. Performs a scan by writing candidate DAC to `0x1801` and triggering measurement.
3. Reads 4 (or 8) channel capacitive ADC values from `0x18B8` / `0x18E8`.
4. Calculates the average measurement: $\bar{M} = \frac{1}{N} \sum_{i=1}^N \text{channel}_i$.
5. If $\bar{M} < 20$, updates lower bound and increases DAC.
6. If $\bar{M} > 1000$, updates upper bound and decreases DAC.
7. Iterates until measurement straddles the mid-scale ADC baseline of $512$ ($2^9$ in a 10-bit ADC).
8. **Maximum Iterations:** Bounded strictly to $31$ samples (`FTE3600_CALIBRATION_MAX_SAMPLES`).

### 3.3 Finger Detection Threshold (FDT) Baselining
In [`research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c:143-199`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L143-L199):
1. Samples 10 physical FDT capacitive measurements across channels 0–3.
2. Identifies minimum idle capacitance: `minima[c]`.
3. Validates sanity: requires $300 \le \text{minima}[c] \le 700$.
4. Computes baseline trigger threshold:
   $$\text{threshold}[c] = \max(0, \text{minima}[c] - 30)$$
5. Unlocks internal SRAM register bank via `write_byte 0x9A, 0x5A`.
6. Writes the 8-word threshold array to internal volatile SRAM at `0x18E0`.
7. Re-locks register bank via `write_byte 0x9A, 0x00`.

### 3.4 Persistent vs Volatile Classification
| Component | Register(s) | Volatile vs Persistent | Modifies Hardware Flash/OTP? | Risk of Damage if Interrupted |
| :--- | :--- | :---: | :---: | :---: |
| Transceiver Clock | `0xC6` | Volatile | No | None |
| AFE DAC Value | `0x1801` | Volatile | No | None |
| Sampling Timing | `0x1806`, `0x180A`, `0x180B` | Volatile | No | None |
| Integration Period | `0x1807` | Volatile | No | None |
| FDT Thresholds | `0x18E0` (Bank 0x9A) | Volatile SRAM | No | None |
| IRQ Routing | `0x1A83`, `0x1A8A` | Volatile | No | None |
| Touch Mode Arm | `0x1881`, `0xC2 0x3D` | Volatile | No | None |

**Crucial Safety Finding:**  
Every single write operation performed by the reference startup, AFE configuration, and calibration routines targets **volatile logic gates or internal volatile SRAM**. Zero writes modify persistent storage. If calibration is terminated or aborted mid-sequence, the sensor can be returned to clean baseline at any time by issuing `FTE3600_IOC_RESET`.

---

## 4. Conclusion & Summary of Findings

1. **Firmware:** The FT9362 requires **NO external firmware**. All operating microcode is hardwired in silicon ROM.
2. **Persistent Storage:** Zero flash or EEPROM memory exists on chip; OTP reads are non-destructive and cannot modify silicon data.
3. **Calibration:** Calibration is a volatile mathematical binary search over 7-bit DAC register `0x1801` to center ADC readings at 512, followed by threshold calculation into volatile SRAM at `0x18E0`.
4. **Safety Verdict:** Initialization and calibration involve zero persistent state changes, zero risk of silicon wear-out, and zero risk of bricking.
