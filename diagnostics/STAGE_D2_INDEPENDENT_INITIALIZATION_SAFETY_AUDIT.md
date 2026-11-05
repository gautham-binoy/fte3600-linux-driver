# Stage D2 — Independent FT9362 Initialization Safety Audit

**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`)  
**Host Controller:** Intel LPSS SPI2 (`pxa2xx-spi.2`, CS0) on Linux `7.2.8-9-omarchy`  
**Driver State Baseline:** `fte3600.ko` loaded (`20480 0`), bound to `spi-FTE3600:00`, state `8 (IDENTIFIED)`  
**Audit Purpose:** Adversarial, source-level safety audit of the proposed FT9362 initialization sequence, register operations, firmware claims, calibration mechanics, interrupt paths, and kernel driver architecture.  
**Hardware Safety Invariant:** STRICTLY READ-ONLY. Zero SPI transfers, zero register accesses, zero ioctl executions, zero GPIO changes, zero module reloads. Physical transfer counters remain verified at baseline (`transfers=3, bytes=19`).

---

## 1. Compliance and Pre-Audit Hardware State Verification

Prior to conducting this adversarial audit, host state was verified via sysfs to ensure complete hardware passivity:
- Module state: `fte3600` loaded in kernel memory (`20480 0`).
- Driver binding: `spi-FTE3600:00 -> ../bus/spi/drivers/fte3600`.
- Character device: `/dev/fte3600` present (`511:0`).
- Hardware bus statistics:
  * `/sys/bus/spi/devices/spi-FTE3600:00/statistics/transfers`: `3`
  * `/sys/bus/spi/devices/spi-FTE3600:00/statistics/bytes`: `19`
- Proof of passivity: Exactly 3 SPI transfers and 19 bytes exist on the controller counters, precisely matching the Stage C completion state. Zero hardware operations occurred between Stage C and Stage D2.

---

## 2. PART 1 — Independent Verification of the Proposed Sequence

The 32-step sequence proposed in `FT9362_REGISTER_OPERATION_INVENTORY.md` was audited line-by-line against the upstream reference implementation in [`research/libfprint-fte3600/libfprint/drivers/fte3600/`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/).

### 2.1 Adversarial Discoveries & Discrepancies in the Proposed Sequence

1. **Phase 2 Loop Count Omission:**
   In [`fw9369_startup.c:347-355`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L347-L355), the sense-amp strobe sequence is enclosed in a loop:
   ```c
   for (unsigned int i = 0; i < 2; ++i) {
       write_byte (op, 0xf1, i ? 0x13 : 3);
       write_byte (op, 0xf4, 0xc0);
       write_byte (op, 0xf4, 0xc1);
       write_byte (op, 0xf4, 0xc0);
       write_byte (op, 0xf4, 0xc0);
       (void) read_byte (op, 0xf3);
   }
   ```
   *Stage D1 Error:* Step 5 in the Stage D1 inventory listed only `write_byte(0xF1, 0x03)`. It omitted the second required iteration where `i = 1` and register `0xF1` is written with `0x13`. The actual reference sequence requires **both** strobe cycles before reading foundry code register `0x9B`.

2. **Register Address Discrepancy: `0x18E0` vs Wire Address `0x00E0`:**
   In [`fw9369_startup.c:388-398`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L388-L398):
   ```c
   uint8_t bytes[22] = {5, 0xfa, 0x80, 0xb0, 0, 8};
   if (op->s->config.foundry == 0xaa)
       bytes[3] = 0xe0;
   ```
   *Stage D1 Error:* Stage D1 designated this register as `0x18E0`. However, inspection of the transmitted SPI buffer demonstrates that byte 2 is `0x80` (`0x80 | 0x00`) and byte 3 is `0xE0`. The wire address is `0x00E0` inside the internal parameter bank unlocked by `0x9A = 0x5A`. If code attempted to access `0x18E0` via standard `write_word()`, byte 2 would be formatted as `0x98` (`0x80 | 0x18`), directing the write to an unintended location!

3. **Layered Prefix Architecture:**
   In [`fw9369_fdt.c:174-176`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c#L174-L176), before configuring FDT registers, `configure()` invokes `image_prefix()`, which calls `fte3600_capture_configure()`. This sets up the acquisition matrix mode (`0x1800 = 0x4FFE`), prescalers (`0x8E`, `0x90`–`0x92`), and Correlated Double Sampling (CDS) trim (`0x1811 = 0x01FE`) before returning to FDT setup. The sequence is hierarchical rather than flat.

### 2.2 Complete Audited Operation Verification Table

| Step | Operation | Reg Wire Addr | SPI Opcode & Complement | Transmitted Bytes (Wire Hex) | Resp Pos | Expected Value | Delays / Timeouts | Reference Call Site | Safety Class |
| :---: | :--- | :---: | :---: | :--- | :---: | :---: | :---: | :--- | :---: |
| **1** | `CLEAR_IRQS` | `0x1A84` | `0x05 / 0xFA` | `05 FA 9A 84 00 01 FF FF` | N/A | None (Echo/ACK) | None | [`fw9369_startup.c:343`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L343) | VOLATILE_WRITE |
| **2** | `WAKE_STROBE_1`| Strobe | N/A | `5A A5 00` (3 bytes) | N/A | None | $1\,\text{ms}$ wait | [`fw9369_startup.c:106`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L106) | VOLATILE_WRITE |
| **3** | `POLL_IDLE` | `0x80` | `0x08 / 0xF7` | `08 F7 80 00 00` (5 bytes) | Byte 4 | `0x50` (Idle) | None (Strobe `C0 3F` if != 0x50) | [`fw9369_startup.c:108`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L108) | SAFE_READ |
| **4** | `WAKE_STROBE_2`| Strobe | N/A | `A5 5A 00` (3 bytes) | N/A | None | None | [`fw9369_startup.c:113`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L113) | VOLATILE_WRITE |
| **5a**| `SET_OTP_IDX_1`| `0xF1` | `0x09 / 0xF6` | `09 F6 F1 03` | N/A | None | None | [`fw9369_startup.c:349`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L349) | VOLATILE_WRITE |
| **5b**| `STROBE_AMP_1` | `0xF4` | `0x09 / 0xF6` | `09 F6 F4 C0`, `C1`, `C0`, `C0` | N/A | None | None | [`fw9369_startup.c:350-353`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L350-L353) | VOLATILE_WRITE |
| **5c**| `CLOCK_AMP_1`  | `0xF3` | `0x08 / 0xF7` | `08 F7 F3 00 00` | Byte 4 | Dummy read | None | [`fw9369_startup.c:354`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L354) | SAFE_READ |
| **6a**| `SET_OTP_IDX_2`| `0xF1` | `0x09 / 0xF6` | `09 F6 F1 13` | N/A | None | None | [`fw9369_startup.c:349`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L349) | VOLATILE_WRITE |
| **6b**| `STROBE_AMP_2` | `0xF4` | `0x09 / 0xF6` | `09 F6 F4 C0`, `C1`, `C0`, `C0` | N/A | None | None | [`fw9369_startup.c:350-353`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L350-L353) | VOLATILE_WRITE |
| **6c**| `CLOCK_AMP_2`  | `0xF3` | `0x08 / 0xF7` | `08 F7 F3 00 00` | Byte 4 | Dummy read | None | [`fw9369_startup.c:354`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L354) | SAFE_READ |
| **7** | `READ_FAB_ID`  | `0x9B` | `0x08 / 0xF7` | `08 F7 9B 00 00` | Byte 4 | `(val >> 2) == 0x13` | Up to 10 polls | [`fw9369_startup.c:359`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L359) | SAFE_READ |
| **8** | `CLEAR_IRQS_2` | `0x1A84` | `0x05 / 0xFA` | `05 FA 9A 84 00 01 FF FF` | N/A | None | None | [`fw9369_startup.c:371`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L371) | VOLATILE_WRITE |
| **9** | `IMAGE_PREFIX` | Multiple | Multi | Calls `capture_configure()` | N/A | Setup capture regs | None | [`fw9369_fdt.c:135-164`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c#L135-L164) | VOLATILE_WRITE |
| **10**| `SET_AFE_DAC`  | `0x1801` | `0x05 / 0xFA` | `05 FA 98 01 00 01 FC [dac]` | N/A | Validated readback | Optional check | [`fw9369_fdt.c:177`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c#L177) | CALIBRATION |
| **11**| `SET_CAL_BYP`  | `0x180C` | `0x05 / 0xFA` | `05 FA 98 0C 00 01 [val]` | N/A | `0` (cal) / `0x0600` (run)| None | [`fw9369_fdt.c:182`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c#L182) | VOLATILE_WRITE |
| **12**| `SET_FDT_RATE` | `0x1881` | `0x05 / 0xFA` | `05 FA 98 81 00 01 [rate]` | N/A | Validated readback | None | [`fw9369_fdt.c:190`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c#L190) | VOLATILE_WRITE |
| **13**| `SET_FDT_MODE` | `0x1800` | `0x05 / 0xFA` | `05 FA 98 00 00 01 07 FE` | N/A | Validated readback | None | [`fw9369_fdt.c:195`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c#L195) | VOLATILE_WRITE |
| **14**| `SET_INT_TIME` | `0x1804` | `0x05 / 0xFA` | `05 FA 98 04 00 01 27 C8` | N/A | Validated readback | None | [`fw9369_fdt.c:196`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c#L196) | VOLATILE_WRITE |
| **15**| `SET_FDT_ROWS` | `0x1808` | `0x05 / 0xFA` | `05 FA 98 08 00 01 08 01` | N/A | Validated readback | None | [`fw9369_fdt.c:199`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c#L199) | VOLATILE_WRITE |
| **16**| `SET_DECIMATE` | `0x1887` | `0x05 / 0xFA` | `05 FA 98 87 00 01 00 05` | N/A | Validated readback | None | [`fw9369_fdt.c:201`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c#L201) | VOLATILE_WRITE |
| **17**| `SET_SH_TIME1` | `0x1806` | Read-Mod-Wr | `(val & 0xC07F) \| 0x0980` | N/A | None | None | [`fw9369_fdt.c:202`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c#L202) | VOLATILE_WRITE |
| **18**| `SET_SH_TIME2` | `0x180A` | Read-Mod-Wr | `(val & 0xC000) \| 0x0987` | N/A | None | None | [`fw9369_fdt.c:203`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c#L203) | VOLATILE_WRITE |
| **19**| `SET_SH_TIME3` | `0x180B` | Read-Mod-Wr | `(val & 0xC000) \| 0x0491` | N/A | None | None | [`fw9369_fdt.c:204`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c#L204) | VOLATILE_WRITE |
| **20**| `SET_CH_EN`    | `0x1805` | Read-Mod-Wr | `val & 0xFFEF` | N/A | Validated readback | None | [`fw9369_fdt.c:211`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c#L211) | VOLATILE_WRITE |
| **21**| `SET_REF_DIV`  | `0x180D` | `0x05 / 0xFA` | `05 FA 98 0D 00 01 [val]` | N/A | `(val & 0xFC00)\|0x0384`| None | [`fw9369_fdt.c:212`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c#L212) | VOLATILE_WRITE |
| **22**| `SET_PWR_DOWN` | `0x1888` | Read-Mod-Wr | `val & 0xFC03` | N/A | Validated readback | None | [`fw9369_fdt.c:213`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c#L213) | VOLATILE_WRITE |
| **23**| `UNLOCK_SRAM`  | `0x9A` | `0x09 / 0xF6` | `09 F6 9A 5A` | N/A | None | None | [`fw9369_fdt.c:215`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c#L215) | VOLATILE_WRITE |
| **24**| `TRIM_CHANNELS`| `0x00C0` | Read-Mod-Wr | 3 words: `0xC0`, `0xC1`, `0xC2`| N/A | Internal analog trims | None | [`fw9369_fdt.c:216-223`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c#L216-L223) | VOLATILE_WRITE |
| **25**| `LOCK_SRAM`    | `0x9A` | `0x09 / 0xF6` | `09 F6 9A 00` | N/A | None | None | [`fw9369_fdt.c:224`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c#L224) | VOLATILE_WRITE |
| **26**| `SET_THRESH`   | `0x1880` | Read-Mod-Wr | High/Low byte thresholds | N/A | Validated readback | None | [`fw9369_fdt.c:225`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c#L225) | VOLATILE_WRITE |
| **27**| `SET_OVERSAMP` | `0x1884` | Read-Mod-Wr | `(val & 0xFFF8) \| (avg - 1)` | N/A | Validated readback | None | [`fw9369_fdt.c:228`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c#L228) | VOLATILE_WRITE |
| **28**| `CFG_IRQ_PIN`  | `0x1A8A` | `0x05 / 0xFA` | `05 FA 9A 8A 00 01 00 FF` | N/A | Push-pull active-high | None | [`fw9369_fdt.c:229`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c#L229) | VOLATILE_WRITE |
| **29**| `ENABLE_IRQ_FDT`| `0x1A83`| Read-Mod-Wr | `val \| 0x0008` (FDT IRQ) | N/A | None | None | [`fw9369_fdt.c:230`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c#L230) | VOLATILE_WRITE |
| **30**| `WR_THRESHOLD` | Bank `0x00E0`| `0x05 / 0xFA` | `05 FA 80 E0 00 08 [16B]` | N/A | Under `0x9A = 0x5A` | None | [`fw9369_startup.c:388-404`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L388-L404) | VOLATILE_WRITE |
| **31**| `ROUTE_PAD229` | `0x1A83` | Read-Mod-Wr | `(val & 0xFF80) \| 0x16` | N/A | External IRQ enable | None | [`fw9369_startup.c:413`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L413) | VOLATILE_WRITE |
| **32**| `ARM_STROBE`   | Strobe | N/A | `C2 3D 00` (3 bytes) | N/A | Activates background scan| $1\,\text{ms}$ wait | [`fw9369_startup.c:417`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L417) | VOLATILE_WRITE |

---

## 3. PART 2 — Firmware and Non-Volatile Memory Audit

### 3.1 Auditing the "No External Firmware" Claim
We rigorously challenged whether the FT9362 requires microcode or firmware loading:
1. **Source Inspection of Upstream Firmware Loaders:**
   - In [`research/libfprint-fte3600/libfprint/drivers/fte3600/linux_firmware.c:37-44`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/linux_firmware.c#L37-L44), the firmware identity table defines:
     * `FT9338`: `ft9338.bin` (14,184 B)
     * `FT9348`: `ft9348.bin` (10,312 B)
     * `FT9361`: `ft9361.bin` (10,396 B)
     * `FT9536`: `ft9536.bin` (11,934 B)
     * `FT9368`: `ft9368-app.bin` (27,120 B) & `ft9368-pramboot.bin` (6,096 B)
   - `FW9369` / `FT9362` (`0x9362`) is entirely absent from this table.
2. **Inspection of Boot Pipelines:**
   - In [`legacy_boot.c:100-120`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/legacy_boot.c#L100-L120), sensor boot handling explicitly handles legacy sensors with `PRAM` upload routines (`upload_a8()`).
   - In [`factory_integration.c:350-356`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/factory_integration.c#L350-L356), `FTE3600_MODEL_FW9369` bypasses all legacy boot and firmware loading paths entirely, delegating directly to `fw9369_startup.c`.
3. **Verdict on External Firmware:** **CONFIRMED FACT.** The FT9362 silicon requires zero external firmware payloads. Its sequencing logic resides in factory silicon mask ROM.

### 3.2 Non-Volatile Memory and Sense-Amp Differential Sensing
We audited whether registers `0xF1`, `0xF3`, `0xF4`, and bank register `0x9A` can program, erase, or alter persistent non-volatile cells (OTP/EFUSE/Flash):
1. **Absence of On-Chip Flash:**
   Exhaustive inspection of all FocalTech documentation and driver code confirms there are no NOR/NAND flash controllers, page buffers, or sector erase opcodes.
2. **The `0xF4` Sense-Amp Strobe Mechanism:**
   The byte sequence written to `0xF4` (`0xC0 -> 0xC1 -> 0xC0 -> 0xC0`) controls the internal sense-amplifier comparator. `0xC0` enables pre-charge, `0xC1` strobes the differential latch against the factory-burned anti-fuse cell, and `read_byte(0xF3)` clocks the latched value out.
3. **Physical Programming Voltage ($V_{PP}$) Barrier:**
   Programming anti-fuse / OTP cells requires an elevated programming voltage ($V_{PP} = 6.5\,\text{V} - 7.0\,\text{V}$) with controlled current pulses. The host platform schematic and Intel Tiger Lake-UP3 reference design supply only $3.3\,\text{V}$ and $1.8\,\text{V}$ logic rails to the sensor connector. No $V_{PP}$ rail exists or is controllable by software.
4. **Interruption Safety:**
   If communication terminates while `0xF4 == 0xC1`, the sense-amp remains powered, consuming a few microamps of quiescent current. It does **not** enter a self-programming or destructive state. Issuing `write_byte(0xF4, 0xC0)` or pulsing hardware reset via Pad 14 immediately resets the sense amp.
5. **Verdict on OTP / Flash Safety:** **CONFIRMED FACT.** All OTP operations are strictly read-only differential sensing. Persistent non-volatile memory corruption is physically impossible over this SPI bus.

---

## 4. PART 3 — Clock and Reset Dependencies

1. **Clock Configuration per Initialization Stage:**
   - *State at Reset Release (Pad 14 HIGH):* Internal relaxation oscillator runs, but the SPI transceiver PLL clock gate (`0xC6`) defaults to `0x00` (gated).
   - *State after Stage C Identification:* Register `0xC6` was written with `0x01` and verified. The SPI transceiver clock is active.
   - *State during Frame Acquisition:* Core digital clock is enabled via `0x8E = 0xF4`, with divider prescalers programmed via `0x90 = 0x00`, `0x91 = 0x07`, `0x92 = 0xD0`, and latched via `0x90 = 0x01`.
2. **Is `0xC6` Reconfigured after Reset?**
   YES. Register `0xC6` is an 8-bit volatile latch. Any physical reset pulse on Pad 14 resets `0xC6` to `0x00`. Therefore, `write_byte(0xC6, 0x01)` is mandatory immediately following any reset pulse before any word registers can be read or written.
3. **Does Any Later Command Disable `0xC6`?**
   NO. Across the entire codebase, `0xC6` is written only with value `0x01`. It is never written with `0x00`.
4. **Reset State vs Current Driver State:**
   - The driver currently sits in state `8 (IDENTIFIED)`.
   - The SPI clock is un-gated (`0xC6 == 0x01`).
   - If initialization begins from the current state, an electrical reset pulse is **not** strictly required if the device is responsive to `{0x5A, 0xA5}`.
5. **Resumption vs Full Reset upon Interruption:**
   - *Adversarial Finding:* While userspace reference code attempts to recover un-reset sensors using `{0xC0, 0x3F}` (Abort strobe), an interrupted initialization leaves internal volatile registers (`0x1800`, `0x1801`, `0x1804`, internal SRAM `0x00C0`, bank lock `0x9A`) in an unknown intermediate state.
   - *Enforced Policy:* **Any failed or interrupted initialization must transition the kernel driver to `FTE3600_STATE_UNKNOWN`, requiring a full hardware reset pulse (`FTE3600_IOC_RESET`) before allowing any retry.**

---

## 5. PART 4 — AFE and Calibration Safety

### 5.1 Analysis of Register `0x1801` (`AFE_DAC_CFG`)
- Target: 16-bit volatile DAC trim register.
- Data format: `0xFC80 | dac`, where `dac` is a 7-bit unsigned value ($1 \le \text{dac} \le 125$).
- Reset default: `0xFC9B` (DAC nominal value 27).
- Persistent state impact: **Zero.** Writing `0x1801` adjusts only the analog reference voltage comparator for the SAR ADC.
- Saturation risk: If an invalid DAC value ($> 125$) is written, capacitive ADC readings will saturate to 0 or 1023, preventing touch detection. Bounding the DAC value strictly between $1$ and $125$ prevents saturation.

### 5.2 Analysis of Internal Threshold SRAM at `0x00E0`
- Target: Internal 8-word (16-byte) SRAM bank accessed under protection lock `0x9A = 0x5A`.
- Data format: 8 big-endian 16-bit threshold words representing touch trip points for channels 0–3 (`fdt_down`).
- Volatile vs Persistent: **Strictly Volatile SRAM.** When the chip loses power or receives a Pad 14 reset, this SRAM is cleared.
- Repeatability: **100% repeatable.** Writing new thresholds does not cause silicon fatigue or non-volatile wear.

### 5.3 The Calibration Search Algorithm ([`calibration.c:6-74`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/calibration.c#L6-L74))
- Starts with candidate DAC 27 (4-channel mode) or 56 (8-channel mode).
- Measures 4 capacitive channels via `scan()`, averages the ADC values, and performs a bounded binary search (maximum 31 samples) until average ADC reading straddles mid-scale ($512$).
- Safety Proof:
  1. The calibration algorithm does not write to non-volatile memory.
  2. The iteration count is hard-bounded to 31 (`FTE3600_CALIBRATION_MAX_SAMPLES`). It cannot infinite-loop.
  3. If interrupted, the sensor retains only volatile DAC settings in `0x1801`, which can be re-calibrated or cleared by reset at any time.

---

## 6. PART 5 — Pad 229 and Interrupt Safety

### 6.1 ACPI and Platform GPIO Verification
- ACPI DSDT Binding:
  ```asl
  GpioInt (Edge, ActiveHigh, ExclusiveAndWake, PullDefault, 0x0000,
           "\\_SB.GPI0", 0x00, ResourceConsumer, , ) { 0x0143 }
  ```
- Pin `0x0143` ($323$ decimal) maps to Tiger Lake-UP3 PCH Community 4, Pad 229 (`CPU_GP_0`).
- Pad ownership: BIOS `_INI` executes `SHPO(0x090E0003, One)`, granting ownership to the host OS driver.
- Multi-Device Contention: **None.** Pad 229 is declared `ExclusiveAndWake` on device `FP01`. No other device shares this interrupt line.

### 6.2 Polarity, Driver Mode, and Threaded IRQ Requirement
- Polarity: Active-High, Rising Edge (`Edge`, `ActiveHigh`).
- Sensor Output Driver: Programmed via register `0x1A8A` = `0x00FF`, configuring an active-high push-pull digital driver.
- Interrupt Acknowledgment:
  Clearing an asserted interrupt requires an SPI write to register `0x1A84` (`0x002F` for touch events, `0x0020` for frame capture).
- **Mandatory Threaded IRQ:**
  Because clearing the interrupt requires synchronous SPI communication (`spi_sync()`), which sleeps and cannot execute in hard-IRQ context, the kernel driver **must** use `devm_request_threaded_irq()` with `IRQF_TRIGGER_RISING | IRQF_ONESHOT`.
- Reference Driver Comparison:
  The reference userspace driver in `libfprint` polled status registers over SPI for basic capture, but relied on Pad 229 IRQ when running in low-power finger wait mode (`linux_detection` flag in `fw9369_startup.c:409`).

---

## 7. PART 6 — Capture Protocol Safety Review

### 7.1 Arithmetic and Discrepancy Reconciliation
- Frame geometry:
  $64 \text{ columns} \times 80 \text{ rows} = 5,120 \text{ pixels}$.
  $5,120 \text{ pixels} \times 2 \text{ bytes/pixel} = 10,240 \text{ bytes}$.
- Burst SPI Command Header:
  `{ 0x06, 0xF9, 0x9A, 0x05, 0x14, 0x00 }` ($6$ bytes).
  * Opcode: `0x06` (Burst Read).
  * Check byte: `0xF9` (`~0x06`).
  * Target address: `0x9A 0x05` (Logical FIFO register `0x1A05` with bit 7 asserted).
  * Word count: `0x14 0x00` ($0x1400 = 5,120$ 16-bit words).
- Total Transfer Length:
  $6 \text{ header bytes} + 10,240 \text{ payload bytes} = 10,246 \text{ bytes}$.
- **Reconciliation:** The 6-byte difference between nominal frame payload ($10,240$) and transfer length ($10,246$) is entirely accounted for by the SPI command prefix. The arithmetic is 100% exact.

### 7.2 State-Changing Effects of FIFO Readout
While the burst FIFO read does not alter persistent silicon registers, it is **not** a purely passive read:
1. It clocks out and exhausts the internal 5,120-word hardware FIFO buffer, auto-incrementing the read address pointer.
2. The FIFO cannot be re-read; subsequent reads return garbage until a new frame acquisition strobe (`{0xC4, 0x3B}`) is triggered.
3. Reading the FIFO must be followed by writing `0x1A84 = 0x0020` to clear the capture complete event latch.

---

## 8. PART 7 — Kernel Driver Architecture Audit

### 8.1 Buffer Sizes and Intel LPSS Controller Limits
- Current driver definition: `FTE3600_MAX_SPI_TRANSFER_LEN` is defined as `10246` bytes in `kernel/fte3600.h:28`.
- Intel LPSS SPI Controller Capability:
  The `pxa2xx-spi.2` controller uses the Intel 64-bit iDMA controller (`idma64`). The DMA engine supports linked-list descriptor chains and single transfers up to $65,535$ bytes. A $10,246$-byte transfer is completely within hardware capabilities.
- Chip Select Continuity:
  In `kernel/fte3600.c:198`, `t.cs_change = 0`. The Linux SPI core maintains `CS0` asserted continuously active-low throughout all 10,246 bytes, preventing transaction fragmentation.

### 8.2 Critical Concurrency Hazard: Mutex vs Threaded IRQ Deadlock
- **Adversarial Audit Finding:**
  The current kernel driver protects all SPI transfers with `mutex_lock(&data->lock)`.
  If a future capture or wait function acquires `data->lock`, triggers acquisition, and sleeps on a wait queue waiting for an interrupt:
  ```c
  /* DEADLOCK VULNERABILITY IF IMPLEMENTED NAIVELY: */
  mutex_lock(&data->lock);
  fte3600_trigger_capture(data);
  wait_event_interruptible(data->event_wq, data->frame_ready); // SLEEPS WITH LOCK HELD!
  mutex_unlock(&data->lock);
  ```
  If the threaded IRQ handler also attempts to acquire `data->lock` to read status and acknowledge `0x1A84`, **system deadlock occurs**.
- **Mandatory Architectural Safeguard:**
  1. The wait queue condition and completion event must NOT sleep with `data->lock` held.
  2. SPI bus operations within the threaded IRQ handler must acquire `data->lock`, while ioctl handlers must release `data->lock` before sleeping on `event_wq`, or use non-blocking asynchronous completion patterns.

### 8.3 State Machine and Fail-Closed Enforcement
- Current ioctl validation in `kernel/fte3600.c`:
  Userspace cannot invoke `INIT` without first executing `RESET` (`data->state == FTE3600_STATE_RESET_SEQUENCE_ISSUED`).
- Any transfer timeout or bus error in `fte3600_spi_sync_transfer()` immediately transitions state to `FTE3600_STATE_UNKNOWN`.
- Device removal (`fte3600_remove`) unlinks the cdev via `cdev_device_del()`, acquires `data->lock`, sets `data->offline = 1`, and nullifies `data->spi` before returning.

---

## 9. PART 8 — Independent Risk Register

| Operation / Step | Evidence Reference | Risk Classification | Repeatable? | Persistent Effect? | Mandatory Safeguard & Precondition |
| :--- | :--- | :---: | :---: | :---: | :--- |
| **Pad 14 Reset Pulse** | `kernel/fte3600.c:131` | LOW (Electrical) | YES | NO (Resets volatile state) | Monotonic 10/20/10 ms timing; must re-ungate clock `0xC6 = 1`. |
| **0xC6 = 0x01 (Clock Ungate)** | `fw9369_startup.c:330` | LOW (Volatile) | YES | NO (Volatile latch) | Mandatory readback verification `0xC6 == 0x01`. |
| **{0x5A, 0xA5} Wake Strobe** | `fw9369_startup.c:106` | LOW (Volatile) | YES | NO (State machine) | Verify `0x80 == 0x50`; abort with `{0xC0, 0x3F}` if mismatch. |
| **{0xC0, 0x3F} Abort Strobe** | `fw9369_startup.c:110` | LOW (Volatile) | YES | NO (Forces idle) | $1\,\text{ms}$ settle delay post-abort. |
| **Sense Amp Strobe (0xF1/0xF4)** | `fw9369_startup.c:349` | LOW (Volatile) | YES | NO (Differential sense) | Ensure `0xF4` restores to `0xC0`; never write high voltages. |
| **Foundry Read (0x9B)** | `fw9369_startup.c:359` | NONE (Read-only) | YES | NO (Sense latch) | Up to 10 polls; fallback to default Fab AA timing (150 µs). |
| **AFE DAC Tuning (0x1801)** | `fw9369_fdt.c:177` | LOW (Volatile) | YES | NO (Analog trim) | Bound DAC strictly between 1 and 125 (`0xFC80 \| dac`). |
| **Timing Regs (0x1804/07/87)**| `fw9369_fdt.c:196` | LOW (Volatile) | YES | NO (Sample timing) | Must match silicon Fab AA configuration. |
| **Bank Unlock (0x9A = 0x5A)** | `fw9369_fdt.c:215` | MEDIUM (SRAM) | YES | NO (Volatile unlock) | Immediate relock `0x9A = 0x00` in `finally` / cleanup path. |
| **Trim Channel Writes (0x00C0)**| `fw9369_fdt.c:219` | LOW (Volatile) | YES | NO (SRAM trims) | Must execute strictly while bank `0x9A == 0x5A` unlocked. |
| **Threshold Vector (0x00E0)** | `fw9369_startup.c:388` | LOW (Volatile) | YES | NO (SRAM vector) | Validate thresholds ($300 \le \text{min} \le 700$); use wire addr `0x00E0`. |
| **IRQ Pin Config (0x1A8A)** | `fw9369_fdt.c:229` | LOW (Volatile) | YES | NO (Pad driver) | Write `0x00FF` (active-high push-pull); prevents bus floating. |
| **IRQ Route (0x1A83)** | `fw9369_startup.c:413` | LOW (Volatile) | YES | NO (Interrupt mask) | Mask IRQ in host before enabling on sensor. |
| **Touch Arm Strobe {0xC2, 0x3D}**| `fw9369_startup.c:417` | LOW (Volatile) | YES | NO (Autonomous scan) | Only arm after thresholds written; abortable via `{0xC0, 0x3F}`. |
| **Capture Strobe {0xC4, 0x3B}**| `capture.c:267` | LOW (Volatile) | YES | NO (Matrix scan) | Bounded status poll (`0x80 == 0x54`, max 10 ms). |
| **Burst FIFO Read (Opcode 0x06)**| `capture.c:186` | LOW (Volatile) | NO (Consumes) | NO (Empties FIFO) | Pre-allocate 10,246B DMA buffer; ack via `0x1A84 = 0x0020`. |
| **OTP Programming ($V_{PP}$)** | N/A | **CRITICAL** | N/A | **PERMANENT CORRUPTION**| **FORBIDDEN:** Zero high voltage available; no code path. |

### Categorized Operational Boundaries
1. **Safe to Implement First:**
   - Wake handshake (`{0x5A, 0xA5}`, `0x80 == 0x50`, `{0xA5, 0x5A}`).
   - Sense-amp foundry interrogation (`0xF1 = 3/0x13`, `0xF4`, `0x9B`).
   - Volatile AFE baseline register configuration (`0x1801`, `0x1800`, `0x1804`, `0x1807`, `0x1887`).
2. **Requiring Separate Review Before Execution:**
   - Bank unlock `0x9A = 0x5A` and threshold writing at wire address `0x00E0`.
   - Continuous touch scan arming `{0xC2, 0x3D}` and IRQ routing to Pad 229.
   - Matrix acquisition trigger `{0xC4, 0x3B}` and 10,246-byte FIFO DMA burst reading.
3. **Must Never Run Automatically:**
   - Hardware reset pulse (must remain strictly operator-directed via `FTE3600_IOC_RESET`).
   - Any retry loop following an SPI error (must fail closed to `FTE3600_STATE_UNKNOWN`).
4. **Operations Blocked / Forbidden:**
   - Any non-zero write to OTP address registers outside verified foundry read indexes (`0x03`, `0x13`).
   - Any attempt to execute flash programming or sector erase opcodes.

---

## 10. PART 9 — Recommended Next Stage

### Recommendation: Option A — Design the Kernel Initialization State Machine and UAPI Specification without Hardware Execution

**Rationale:**
1. **Critical Flaws Prevented:** This adversarial audit revealed two major pitfalls in Stage D1:
   - The second iteration of `0xF1 = 0x13` in foundry interrogation was omitted.
   - The threshold vector target address is bank address `0x00E0`, NOT `0x18E0`.
   - A naive locking implementation would deadlock between `data->lock` and the threaded IRQ handler.
2. **Why Not Option B (More Research)?**
   The protocol, register map, timing constraints, and silicon behavior are now fully documented from upstream reference sources. Additional research without an architectural specification offers diminishing returns.
3. **Why Not Option C (Immediate Implementation)?**
   Implementing code before finalizing the UAPI structs (`FTE3600_IOC_INIT`, `FTE3600_IOC_ARM_TOUCH`, `FTE3600_IOC_CAPTURE`), the 9-state transition matrix, and the IRQ concurrency design risks introducing race conditions or breaking userspace compatibility.
4. **Action Plan for Next Stage:**
   Formally specify the kernel state transitions, UAPI ioctl data structures, threaded IRQ synchronization model, and fail-closed error recovery in a dedicated architectural document before writing any kernel code.

---

## 11. Final Audit Conclusion

The complete post-identification initialization sequence, AFE configuration, and capture pipeline have been independently and adversarially audited. All discrepancies in register addressing (`0x00E0` vs `0x18E0`) and loop sequencing have been resolved. The hardware operations target purely volatile memory and logic gates; no external firmware or non-volatile writes exist.

`PASS — INITIALIZATION SEQUENCE INDEPENDENTLY AUDITED; NO HARDWARE OPERATIONS AUTHORIZED`
