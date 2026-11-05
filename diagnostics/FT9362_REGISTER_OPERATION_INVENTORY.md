# FocalTech FT9362 Register Operation Inventory

**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Host Controller:** Intel LPSS SPI2 (`pxa2xx-spi.2`, CS0) on Dynabook Satellite Pro C40-J  
**Document Purpose:** Exhaustive register map, SPI framing specifications, and chronological operation inventory for the post-identification initialization and capture paths.  
**Safety Classification:** Strictly Read-Only Source and Protocol Analysis. Zero Hardware Transfers Executed.

---

## 1. Master Register Map

The FT9362 utilizes a hybrid register architecture:
1. **8-bit Peripheral Control Registers:** Single-byte address space, accessed via `0x09` (write) and `0x08` (read).
2. **16-bit Configuration & Status Registers:** Two-byte address space (`0x1800`–`0x1A8B`), accessed via `0x05` (write word), `0x04` (read word), and `0x06` (burst FIFO read).
3. **Special Hardware Commands:** 2-byte command strobes (`{ cmd, ~cmd }`).

| Register Address | Name / Functional Block | Width | Access | Framing Opcode | Reset Value | Safety Class | Functional Purpose & Description |
| :---: | :--- | :---: | :---: | :---: | :---: | :---: | :--- |
| `0x80` | `HW_STATUS` | 8-bit | RO | `0x08 / 0xF7` | `0x00` | SAFE_READ | Hardware controller status: `0x50` = Idle/Ready, `0x54` = Frame Capture Ready. |
| `0x8E` | `CORE_CTRL` | 8-bit | WO | `0x09 / 0xF6` | `0x00` | VOLATILE_WRITE | Digital core clock / ADC sequencing enable (configured to `0xF4` during capture). |
| `0x90` | `PRESCALER_0` | 8-bit | WO | `0x09 / 0xF6` | `0x00` | VOLATILE_WRITE | Internal clock prescaler divider low byte. |
| `0x91` | `PRESCALER_1` | 8-bit | WO | `0x09 / 0xF6` | `0x00` | VOLATILE_WRITE | Internal clock prescaler divider high byte (`0x07` during capture init). |
| `0x92` | `PRESCALER_2` | 8-bit | WO | `0x09 / 0xF6` | `0x00` | VOLATILE_WRITE | Internal clock fine timing trim (`0xD0` during capture init). |
| `0x9A` | `BANK_UNLOCK` | 8-bit | WO | `0x09 / 0xF6` | `0x00` | VOLATILE_WRITE | Bank protection lock: `0x5A` unlocks internal threshold SRAM; `0x00` relocks. |
| `0x9B` | `FOUNDRY_CODE`| 8-bit | RO | `0x08 / 0xF7` | Varies | SAFE_READ | Silicon fab identity: bits 7:2 contain foundry code (`0x13` $\rightarrow$ `0xAA` fab). |
| `0xC0` | `CMD_ABORT` | Strobe | WO | `0xC0 / 0x3F` | N/A | VOLATILE_WRITE | Abort active acquisition / force state machine to idle. |
| `0xC2` | `CMD_ARM_FDT` | Strobe | WO | `0xC2 / 0x3D` | N/A | VOLATILE_WRITE | Arm hardware finger-presence detection (FDT) background scanning. |
| `0xC4` | `CMD_TRIGGER` | Strobe | WO | `0xC4 / 0x3B` | N/A | VOLATILE_WRITE | Trigger physical capacitive frame acquisition into FIFO. |
| `0xC6` | `CLK_UNGATE` | 8-bit | RW | `0x09 / 0x08` | `0x00` | VOLATILE_WRITE | SPI transceiver PLL clock un-gate: `0x01` enables clock, `0x00` disables. |
| `0xF1` | `OTP_ADDR` | 8-bit | WO | `0x09 / 0xF6` | `0x00` | VOLATILE_WRITE | Non-volatile sense amp address index (used strictly for read strobe). |
| `0xF3` | `OTP_DATA` | 8-bit | RO | `0x08 / 0xF7` | Varies | SAFE_READ | Sense amp data readback (reads factory silicon configuration). |
| `0xF4` | `OTP_CTRL` | 8-bit | WO | `0x09 / 0xF6` | `0x00` | VOLATILE_WRITE | Sense amp strobe sequence (`0xC0` $\rightarrow$ `0xC1` $\rightarrow$ `0xC0`). |
| `0x1800` | `AFE_ACQ_CTRL`| 16-bit | RW | `0x05 / 0x04` | `0x0000` | VOLATILE_WRITE | Acquisition mode: `0x07FE` (FDT), `0x4FFE` (Capture). Bit 0 enables FIFO. |
| `0x1801` | `AFE_DAC_CFG` | 16-bit | RW | `0x05 / 0x04` | `0xFC9B` | VOLATILE_WRITE | Analog front-end DAC baseline: `0xFC80 \| dac` (7-bit DAC, nominal 27 or 54). |
| `0x1804` | `AFE_INT_TIME`| 16-bit | RW | `0x05 / 0x04` | `0x27C8` | VOLATILE_WRITE | Integration time & gain shift: `0x27C0 \| shift \| (shift >= 2 ? 8 : 0)`. |
| `0x1805` | `AFE_ENABLE` | 16-bit | RW | `0x05 / 0x04` | `0x0010` | VOLATILE_WRITE | Analog channel enable and filtering mask (`value & 0xFFEF`). |
| `0x1806` | `AFE_SH_TIME1`| 16-bit | RW | `0x05 / 0x04` | `0x0000` | VOLATILE_WRITE | Sample-and-hold phase timing 1: `0x0980` (FDT) or `0x0480` (Capture). |
| `0x1807` | `AFE_PERIOD` | 16-bit | RW | `0x05 / 0x04` | `0x1671` | VOLATILE_WRITE | Frame line timing: `((period - 1) << 5) \| 1` (period = 150 or 200 µs). |
| `0x1808` | `AFE_FDT_ROWS`| 16-bit | RW | `0x05 / 0x04` | `0x0801` | VOLATILE_WRITE | Matrix active rows for finger detection: `0x0801` for 4-channel mode. |
| `0x180A` | `AFE_SH_TIME2`| 16-bit | RW | `0x05 / 0x04` | `0x0000` | VOLATILE_WRITE | Sample-and-hold phase timing 2: `0x0987` (FDT) or `0x0483` (Capture). |
| `0x180B` | `AFE_SH_TIME3`| 16-bit | RW | `0x05 / 0x04` | `0x0000` | VOLATILE_WRITE | Sample-and-hold phase timing 3: `0x0491` (FDT) or `0x0208` (Capture). |
| `0x180C` | `AFE_CAL_BYP` | 16-bit | RW | `0x05 / 0x04` | `0x0600` | VOLATILE_WRITE | Calibration bypass: `0x0600` (normal runtime), `0x0000` (during calibration).|
| `0x180D` | `AFE_REF_DIV` | 16-bit | RW | `0x05 / 0x04` | `0x0384` | VOLATILE_WRITE | Internal voltage reference divider trim: `(value & 0xFC00) \| 0x0384`. |
| `0x1811` | `AFE_CDS_CTRL`| 16-bit | RW | `0x05 / 0x04` | `0x01FE` | VOLATILE_WRITE | Correlated Double Sampling (CDS) filter trim: `(value & 0xFC00) \| 0x01FE`. |
| `0x1880` | `FDT_THRESH` | 16-bit | RW | `0x05 / 0x04` | `0x0000` | VOLATILE_WRITE | FDT baseline thresholds: high byte `threshold_high`, low byte `threshold_low`.|
| `0x1881` | `FDT_RATE_CFG`| 16-bit | RW | `0x05 / 0x04` | `0x0081` | VOLATILE_WRITE | FDT poll rate and mode: bit 0 = arm, bit 1 = scan, bit 7 = touch active. |
| `0x1884` | `FDT_OVERSAMP`| 16-bit | RW | `0x05 / 0x04` | `0x0003` | VOLATILE_WRITE | Oversampling factor for capacitive noise rejection (`average - 1`). |
| `0x1885` | `FDT_TRIGGER` | 16-bit | WO | `0x05 / 0xfa` | `0x0000` | VOLATILE_WRITE | Written with `1` to initiate immediate FDT channel measurement. |
| `0x1887` | `AFE_CH_MODE` | 16-bit | RW | `0x05 / 0x04` | `0x0002` | VOLATILE_WRITE | Decimation mode: `5` or `2` (FDT), `2` or `0` (Capture). |
| `0x1888` | `AFE_PWR_DOWN`| 16-bit | RW | `0x05 / 0x04` | `0x0000` | VOLATILE_WRITE | Analog rail low-power sleep mask: `value & 0xFC03`. |
| `0x18B0` / `0x18E0` | `FDT_BANK_WR`| 16B | WO | `0x05 / 0xfa` | N/A | VOLATILE_WRITE | 8-word threshold array written to internal SRAM bank under `0x9A = 0x5A`.|
| `0x18B8` / `0x18E8` | `FDT_ADC_RD` | 16B | RO | `0x04 / 0xfb` | N/A | SAFE_READ | Burst read of 4 to 8 physical capacitive channel measurements. |
| `0x1A05` | `FIFO_DATA` | Burst | RO | `0x06 / 0xF9` | N/A | SAFE_READ | 5,120-word (10,240-byte) image buffer containing 64x80 16-bit raw pixel frame.|
| `0x1A82` | `IRQ_STATUS` | 16-bit | RO | `0x04 / 0xFB` | `0x0000` | SAFE_READ | Event status flags: bit 3 = FDT scan done, bit 5 = Capture FIFO ready. |
| `0x1A83` | `IRQ_ENABLE` | 16-bit | RW | `0x05 / 0x04` | `0x0000` | VOLATILE_WRITE | Interrupt routing enable: `(value & 0xFF80) \| 0x16` enables external IRQ pin.|
| `0x1A84` | `IRQ_ACK` | 16-bit | WO | `0x05 / 0xFA` | `0x0000` | VOLATILE_WRITE | Write-1-to-clear interrupt acknowledge (`0x2F` for FDT, `0x0020` for capture).|
| `0x1A8A` | `IRQ_PAD_CFG`| 16-bit | RW | `0x05 / 0x04` | `0x00FF` | VOLATILE_WRITE | IRQ pin physical output driver: `0x00FF` enables active-high push-pull. |
| `0x1A8B` | `CHIP_ID` | 16-bit | RO | `0x04 / 0xFB` | `0x9362` | SAFE_READ | Hardwired chip silicon identifier and wire CRC (`0x9362`, CRC `0x1C53`). |

---

## 2. Chronological Post-Identification Initialization Sequence

The reference implementation in [`fw9369_startup.c`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c) executes the following exact sequence after Stage C identification to initialize and arm the sensor:

### Phase 1: Wake & Hardware State Clearing
| Seq | Function | Source Line | Opcode / Complement | Reg / Addr | W/R | Data Payload | Delay | Expected Return | Purpose | State Class |
| :---: | :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :--- | :---: |
| 1 | `prefix()` | `fw9369_startup.c:343` | `0x05 / 0xFA` | `0x1A84` | W | `0xFFFF` | None | ACK | Clear all pending interrupt/event latches. | VOLATILE_WRITE |
| 2 | `idle()` | `fw9369_startup.c:106` | Strobe | `0x5A 0xA5`| W | `None` | 1 ms | None | Issue hardware wake strobe. | VOLATILE_WRITE |
| 3 | `idle()` | `fw9369_startup.c:108` | `0x08 / 0xF7` | `0x80` | R | `None` | None | `0x50` | Verify hardware controller entered idle/ready state. | SAFE_READ |
| 4 | `idle()` | `fw9369_startup.c:113` | Strobe | `0xA5 0x5A`| W | `None` | None | None | Complete wake handshake sequence. | VOLATILE_WRITE |

### Phase 2: Foundry Interrogation (Silicon Fab Tuning)
| Seq | Function | Source Line | Opcode / Complement | Reg / Addr | W/R | Data Payload | Delay | Expected Return | Purpose | State Class |
| :---: | :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :--- | :---: |
| 5 | `prefix()` | `fw9369_startup.c:349` | `0x09 / 0xF6` | `0xF1` | W | `0x03` | None | ACK | Index factory configuration register 0x03. | VOLATILE_WRITE |
| 6 | `prefix()` | `fw9369_startup.c:350` | `0x09 / 0xF6` | `0xF4` | W | `0xC0` | None | ACK | Sense amp strobe sequence part 1. | VOLATILE_WRITE |
| 7 | `prefix()` | `fw9369_startup.c:351` | `0x09 / 0xF6` | `0xF4` | W | `0xC1` | None | ACK | Sense amp strobe sequence part 2 (active latch). | VOLATILE_WRITE |
| 8 | `prefix()` | `fw9369_startup.c:352` | `0x09 / 0xF6` | `0xF4` | W | `0xC0` | None | ACK | Sense amp strobe sequence part 3. | VOLATILE_WRITE |
| 9 | `prefix()` | `fw9369_startup.c:353` | `0x09 / 0xF6` | `0xF4` | W | `0xC0` | None | ACK | Sense amp strobe sequence part 4. | VOLATILE_WRITE |
| 10| `prefix()` | `fw9369_startup.c:354` | `0x08 / 0xF7` | `0xF3` | R | `None` | None | Dummy | Dummy read to clock sense amp latch. | SAFE_READ |
| 11| `prefix()` | `fw9369_startup.c:359` | `0x08 / 0xF7` | `0x9B` | R | `None` | None | `(val >> 2) == 0x13` | Read foundry ID (0x13 indicates 0xAA fab, period=150µs). | SAFE_READ |
| 12| `prefix()` | `fw9369_startup.c:371` | `0x05 / 0xFA` | `0x1A84` | W | `0xFFFF` | None | ACK | Re-clear event latches post-interrogation. | VOLATILE_WRITE |

### Phase 3: AFE Configuration & Calibration Baseline
| Seq | Function | Source Line | Opcode / Complement | Reg / Addr | W/R | Data Payload | Delay | Expected Return | Purpose | State Class |
| :---: | :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :--- | :---: |
| 13| `configure()`| `fw9369_fdt.c:177` | `0x05 / 0xFA` | `0x1801` | W | `0xFC80 \| dac` | None | ACK | Write initial DAC baseline (nominal 27 or 56). | VOLATILE_WRITE |
| 14| `configure()`| `fw9369_fdt.c:182` | `0x05 / 0xFA` | `0x180C` | W | `0x0600` | None | ACK | Enable analog filter bypass. | VOLATILE_WRITE |
| 15| `configure()`| `fw9369_fdt.c:190` | `0x05 / 0xFA` | `0x1881` | W | Timing word | None | ACK | Program FDT poll rate and repeat interval. | VOLATILE_WRITE |
| 16| `configure()`| `fw9369_fdt.c:195` | `0x05 / 0xFA` | `0x1800` | W | `0x07FE` | None | ACK | Set acquisition matrix mode to 4-channel FDT. | VOLATILE_WRITE |
| 17| `configure()`| `fw9369_fdt.c:196` | `0x05 / 0xFA` | `0x1804` | W | `0x27C8` | None | ACK | Set integration time multiplier. | VOLATILE_WRITE |
| 18| `configure()`| `fw9369_fdt.c:199` | `0x05 / 0xFA` | `0x1808` | W | `0x0801` | None | ACK | Set FDT active sensing rows. | VOLATILE_WRITE |
| 19| `configure()`| `fw9369_fdt.c:201` | `0x05 / 0xFA` | `0x1887` | W | `0x0005` | None | ACK | Set channel decimation mode to 5. | VOLATILE_WRITE |
| 20| `configure()`| `fw9369_fdt.c:215` | `0x09 / 0xF6` | `0x9A` | W | `0x5A` | None | ACK | Unlock internal SRAM register bank. | VOLATILE_WRITE |
| 21| `configure()`| `fw9369_fdt.c:219` | `0x05 / 0xFA` | `0x00C0` | W | `0x0444` | None | ACK | Configure internal analog channel trim 1. | VOLATILE_WRITE |
| 22| `configure()`| `fw9369_fdt.c:220` | `0x05 / 0xFA` | `0x00C1` | W | `0x0021` | None | ACK | Configure internal analog channel trim 2. | VOLATILE_WRITE |
| 23| `configure()`| `fw9369_fdt.c:221` | `0x05 / 0xFA` | `0x00C2` | W | `0x00C0` | None | ACK | Configure internal analog channel trim 3. | VOLATILE_WRITE |
| 24| `configure()`| `fw9369_fdt.c:224` | `0x09 / 0xF6` | `0x9A` | W | `0x00` | None | ACK | Relock internal SRAM register bank. | VOLATILE_WRITE |
| 25| `configure()`| `fw9369_fdt.c:229` | `0x05 / 0xFA` | `0x1A8A` | W | `0x00FF` | None | ACK | Configure external IRQ output pin driver. | VOLATILE_WRITE |

### Phase 4: Finger Presence Arming (Entering Ready State)
| Seq | Function | Source Line | Opcode / Complement | Reg / Addr | W/R | Data Payload | Delay | Expected Return | Purpose | State Class |
| :---: | :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :--- | :---: |
| 26| `arm()` | `fw9369_startup.c:396` | `0x09 / 0xF6` | `0x9A` | W | `0x5A` | None | ACK | Unlock threshold SRAM bank. | VOLATILE_WRITE |
| 27| `arm()` | `fw9369_startup.c:404` | `0x05 / 0xFA` | `0x18E0` | W | 8 Words (16B) | None | ACK | Write calibrated threshold vector (`fdt_down`). | VOLATILE_WRITE |
| 28| `arm()` | `fw9369_startup.c:405` | `0x09 / 0xF6` | `0x9A` | W | `0x00` | None | ACK | Relock threshold SRAM bank. | VOLATILE_WRITE |
| 29| `arm()` | `fw9369_startup.c:408` | `0x05 / 0xFA` | `0x1881` | W | `(val & 0xFF7C) \| 0x81` | None | ACK | Enable touch detection mode & continuous poll. | VOLATILE_WRITE |
| 30| `arm()` | `fw9369_startup.c:413` | `0x05 / 0xFA` | `0x1A83` | W | `(val & 0xFF80) \| 0x16` | None | ACK | Enable external IRQ interrupt generation to Pad 229.| VOLATILE_WRITE |
| 31| `arm()` | `fw9369_startup.c:416` | `0x05 / 0xFA` | `0x1A84` | W | `0x002F` | None | ACK | Acknowledge & clear initial arm event flags. | VOLATILE_WRITE |
| 32| `arm()` | `fw9369_startup.c:417` | Strobe | `0xC2 0x3D`| W | `None` | 1 ms | None | **ARM STROBE:** Activate autonomous touch scanner. | VOLATILE_WRITE |
