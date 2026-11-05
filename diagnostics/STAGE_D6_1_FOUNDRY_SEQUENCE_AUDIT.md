# Stage D6.1 — Audit of Foundry-Interrogation Sequence & Command Strobing

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Hardware:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`, Intel LPSS SPI2)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Document Ref:** `diagnostics/STAGE_D6_1_FOUNDRY_SEQUENCE_AUDIT.md`  

---

## 1. Executive Summary

This audit independently reviews every SPI transaction in the foundry interrogation sequence (Steps 5 through 7) implemented in `kernel/fte3600.c` against the original factory reference driver (`research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c`).

Two major findings were identified:
1. **Command Strobe Length Defect:** In `kernel/fte3600.c`, `fte3600_spi_cmd_strobe()` transmits **2 bytes** (`{ cmd, complement }`). However, every factory reference driver file (`fw9369_startup.c:65`, `fw9369_fdt.c:61`, `capture.c:66`, `ft93xx.c:177`) and the reference test mock (`test-fw9369-backend.c:308, 347, 359`) transmit **3 bytes** (`{ cmd, complement, 0x00 }`) and explicitly assert `length == 3`.
2. **OTP Sense-Amplifier Cycling Semantics:** The sequence writes to `0xF1` (index `0x03` and `0x13`), clocks `0xF4` four times with `0xC0/0xC1`, and dummy-reads `0xF3` to latch manufacturing data into `0x9B`. While the software intention is read-only wafer identification, the registers interface directly with on-chip OTP macros, leaving silicon persistence unverified by official documentation.

---

## 2. Byte-for-Byte Audit of the Foundry Interrogation Loop

In `kernel/fte3600.c:538-560`, the interrogation sequence runs 2 iterations (`i = 0` and `i = 1`):

```c
for (i = 0; i < 2; i++) {
    u8 idx = (i == 0) ? 0x03 : 0x13;

    ret = fte3600_spi_write_sfr(data, 0xF1, idx);
    if (ret) goto err;
    ret = fte3600_spi_write_sfr(data, 0xF4, 0xC0);
    if (ret) goto err;
    ret = fte3600_spi_write_sfr(data, 0xF4, 0xC1);
    if (ret) goto err;
    ret = fte3600_spi_write_sfr(data, 0xF4, 0xC0);
    if (ret) goto err;
    ret = fte3600_spi_write_sfr(data, 0xF4, 0xC0);
    if (ret) goto err;
    ret = fte3600_spi_read_sfr(data, 0xF3, &dummy);
    if (ret) goto err;
}
```

### 2.1 Chronological Transaction Inventory

| Seq | Target SFR | Operation | Transmitted Wire Bytes (Hex) | Bytes | Expected Response | Delay / Timeout | Factory Source Ref |
| :---: | :---: | :---: | :--- | :---: | :--- | :---: | :--- |
| **Iter 0.1** | `0xF1` | Write SFR | `09 F6 F1 03` | 4 | SPI ACK (`ret == 0`) | None | `fw9369_startup.c:349` |
| **Iter 0.2** | `0xF4` | Write SFR | `09 F6 F4 C0` | 4 | SPI ACK (`ret == 0`) | None | `fw9369_startup.c:350` |
| **Iter 0.3** | `0xF4` | Write SFR | `09 F6 F4 C1` | 4 | SPI ACK (`ret == 0`) | None | `fw9369_startup.c:351` |
| **Iter 0.4** | `0xF4` | Write SFR | `09 F6 F4 C0` | 4 | SPI ACK (`ret == 0`) | None | `fw9369_startup.c:352` |
| **Iter 0.5** | `0xF4` | Write SFR | `09 F6 F4 C0` | 4 | SPI ACK (`ret == 0`) | None | `fw9369_startup.c:353` |
| **Iter 0.6** | `0xF3` | Read SFR  | `08 F7 F3 00 00` | 5 | Returns `rx[4]`, discarded | None | `fw9369_startup.c:354` |
| **Iter 1.1** | `0xF1` | Write SFR | `09 F6 F1 13` | 4 | SPI ACK (`ret == 0`) | None | `fw9369_startup.c:349` |
| **Iter 1.2** | `0xF4` | Write SFR | `09 F6 F4 C0` | 4 | SPI ACK (`ret == 0`) | None | `fw9369_startup.c:350` |
| **Iter 1.3** | `0xF4` | Write SFR | `09 F6 F4 C1` | 4 | SPI ACK (`ret == 0`) | None | `fw9369_startup.c:351` |
| **Iter 1.4** | `0xF4` | Write SFR | `09 F6 F4 C0` | 4 | SPI ACK (`ret == 0`) | None | `fw9369_startup.c:352` |
| **Iter 1.5** | `0xF4` | Write SFR | `09 F6 F4 C0` | 4 | SPI ACK (`ret == 0`) | None | `fw9369_startup.c:353` |
| **Iter 1.6** | `0xF3` | Read SFR  | `08 F7 F3 00 00` | 5 | Returns `rx[4]`, discarded | None | `fw9369_startup.c:354` |
| **Post.1**   | `0x9B` | Read SFR  | `08 F7 9B 00 00` | 5 | Up to 10 polls: `rx[4]` | None | `fw9369_startup.c:357-369` |

### 2.2 Wire Framing and Register Parsing Comparison
1. **Register `0xF1` (Sense-Amp Row Pointer):**
   - Transmitted payload: `0x09, 0xF6, 0xF1, 0x03` (iteration 0) and `0x09, 0xF6, 0xF1, 0x13` (iteration 1).
   - Factory match: Exact byte match with `write_byte(op, 0xf1, i ? 0x13 : 3)`.
2. **Register `0xF4` (Sense-Amp Clock Strobe):**
   - Four writes per iteration: `0xC0` $\rightarrow$ `0xC1` $\rightarrow$ `0xC0` $\rightarrow$ `0xC0`.
   - Bit 0 high (`0xC1`) asserts the sense clock; `0xC0` deasserts it.
   - Factory match: Exact byte match with `write_byte(op, 0xf4, ...)`.
3. **Register `0xF3` (Sense-Amp Output Latch):**
   - Read 5 bytes: `0x08, 0xF7, 0xF3, 0x00, 0x00`.
   - Factory code: `(void) read_byte(op, 0xf3);`.
   - Purpose: Discarded dummy read to advance internal serial shift register.
4. **Register `0x9B` (Foundry Identity Shadow Register):**
   - Read 5 bytes: `0x08, 0xF7, 0x9B, 0x00, 0x00`.
   - Factory code parses: `code = (read_byte(op, 0x9b) >> 2)`.
   - Decoding:
     * If `code == 0x13` (wire byte `0x4C`): SMIC Fab AA (`foundry = 0xAA`, `line_period = 150` µs).
     * If `code == 0`: Default fab (`foundry = 0`, `line_period = 200` µs).
   - Kernel implementation matches factory source parsing bit-for-bit.

---

## 3. Discrepancy: Command Strobe Wire Length (2 vs 3 Bytes)

A critical discrepancy was discovered during this audit between `kernel/fte3600.c` and all reference materials:

### 3.1 Kernel Driver Implementation in Stage D6
In `kernel/fte3600.c:241-246`:
```c
static int fte3600_spi_cmd_strobe(struct fte3600_data *data, u8 cmd, u8 complement)
{
	data->tx_buf[0] = cmd;
	data->tx_buf[1] = complement;
	return fte3600_spi_sync_transfer(data, data->tx_buf, NULL, 2);
}
```
This helper transmits **2 bytes** over SPI (`len = 2`).

### 3.2 Factory Driver Reference Code
Inspection of the original factory source files reveals that **every single reference file defines command strobes as 3 bytes**:
1. `fw9369_startup.c:63-68`:
   ```c
   static void command (struct startup *op, uint8_t a, uint8_t b) {
       uint8_t bytes[3] = {a, b, 0};
       (void) transfer (op, bytes, sizeof (bytes));
   }
   ```
2. `fw9369_fdt.c:59-64`:
   ```c
   static void command (const struct fdt_operation *op, uint8_t a, uint8_t b) {
       uint8_t buffer[3] = {a, b, 0};
       (void) transfer (op, buffer, sizeof (buffer));
   }
   ```
3. `capture.c:64-69`:
   ```c
   static void command (const struct operation *op, uint8_t first, uint8_t second) {
       uint8_t buffer[3] = {first, second, 0};
       (void) transfer (op, buffer, sizeof (buffer));
   }
   ```
4. `ft93xx.c:174-180`:
   ```c
   static int command (const struct operation *op, uint8_t command_byte) {
       uint8_t buffer[3] = {command_byte, (uint8_t) ~command_byte, 0};
       return transfer (op, buffer, sizeof (buffer));
   }
   ```

### 3.3 Factory Test Mock Enforcement
In `research/libfprint-fte3600/tests/test-fw9369-backend.c:308, 347, 359`:
```c
308:   g_assert_cmpuint (length, >=, 3);
309:   g_assert_cmpuint (tx[0] ^ tx[1], ==, 0xff);
...
347:   case 0xc0:
348:       g_assert_cmpuint (length, ==, 3);
...
359:   case 0x5a:
360:       g_assert_cmpuint (length, ==, 3);
```
The test harness explicitly fails with an assertion failure if any command strobe is submitted with `length != 3`.

### 3.4 Hardware Risk Analysis
The FT9362 SPI slave receiver requires the 3rd byte (`0x00`) to provide trailing clock pulses while CS is asserted, allowing the internal synchronous state machine to decode the 2-byte command and latch the transition. Transmitting only 2 bytes risks:
- Command strobe rejection by physical silicon.
- Partial byte framing offset on subsequent transfers.
- Inability to transition out of deep sleep or recover from non-idle states.

---

## 4. Demonstrable Volatility Evaluation

- **Register `0xF1` & `0xF4`:** These registers interface with the on-chip OTP / eFuse macro.
- While the sequence operates in differential sense-amplifier mode (reading out wafer lot data), no silicon documentation proves that repeated execution or power interruptions during clocking cannot induce unintended state changes.
- Therefore, the operation is classified as **`SOURCE-OBSERVED BUT HARDWARE SEMANTICS UNVERIFIED`**.
