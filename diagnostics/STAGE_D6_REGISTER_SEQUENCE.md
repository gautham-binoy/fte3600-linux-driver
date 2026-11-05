# Stage D6 — FT9362 Register Sequence and Wire Protocol Specification

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Controller  
**Document Ref:** `diagnostics/STAGE_D6_REGISTER_SEQUENCE.md`  

---

## 1. Complete FT9362 Volatile Startup Sequence

The volatile initialization sequence prepares the FT9362 controller and analog front-end for operational sensing after power-on reset or hardware reset deassertion.

| Step | Operation Name | Opcode / Wire Payload (Hex) | Bytes | Dir | Purpose & Semantics |
| :--- | :--- | :--- | :---: | :---: | :--- |
| **0** | **Electrical Reset Pulse** | GPIO Pin Low 10 ms, High 50 ms | — | OUT | Hardware electrical reset (handled by `FTE3600_IOC_RESET`). |
| **1** | **Clear Initial Interrupts** | `05 FA 9A 84 00 01 FF FF` | 8 | OUT | Write `0xFFFF` to 16-bit register `0x1A84`. Clears power-on interrupt latches. |
| **2** | **Wake Strobe 1** | `5A A5` | 2 | OUT | `CMD_WAKE`: Strobes controller out of ultra-low-power sleep into active state. |
| **3** | **Stabilization Wait** | `usleep_range(1000, 1500)` | — | — | 1 ms minimum wait time for internal power rails and oscillator to settle. |
| **4a** | **Poll Status 0x80** | `08 F7 80 00 00` | 5 | I/O | Read SFR `0x80`. Returns `rx[4]`. Expected idle value is `0x50`. |
| **4b** | **Abort Strobe (Fallback)** | `C0 3F` | 2 | OUT | If `rx[4] != 0x50`, issue `CMD_ABORT` followed by 1 ms delay and retry read of `0x80`. |
| **5** | **Wake Strobe 2** | `A5 5A` | 2 | OUT | `CMD_IDLE_ACK`: Acknowledges idle readiness and arms internal register bus. |
| **6a** | **Interrogation Loop 0** | `09 F6 F1 03`<br>`09 F6 F4 C0`<br>`09 F6 F4 C1`<br>`09 F6 F4 C0`<br>`09 F6 F4 C0`<br>`08 F7 F3 00 00` | 4<br>4<br>4<br>4<br>4<br>5 | OUT<br>OUT<br>OUT<br>OUT<br>OUT<br>I/O | Set sense-amp index `0x03` on `0xF1`.<br>Strobe `0xC0` on `0xF4`.<br>Strobe `0xC1` on `0xF4`.<br>Strobe `0xC0` on `0xF4`.<br>Strobe `0xC0` on `0xF4`.<br>Dummy read SFR `0xF3` to latch configuration. |
| **6b** | **Interrogation Loop 1** | `09 F6 F1 13`<br>`09 F6 F4 C0`<br>`09 F6 F4 C1`<br>`09 F6 F4 C0`<br>`09 F6 F4 C0`<br>`08 F7 F3 00 00` | 4<br>4<br>4<br>4<br>4<br>5 | OUT<br>OUT<br>OUT<br>OUT<br>OUT<br>I/O | Set sense-amp index `0x13` on `0xF1`.<br>Strobe `0xC0` on `0xF4`.<br>Strobe `0xC1` on `0xF4`.<br>Strobe `0xC0` on `0xF4`.<br>Strobe `0xC0` on `0xF4`.<br>Dummy read SFR `0xF3` to latch configuration. |
| **7** | **Read Foundry Code** | `08 F7 9B 00 00` | 5 | I/O | Read SFR `0x9B` (up to 10 attempts).<br>If `(rx[4] >> 2) == 0x13`, Fab is SMIC Fab AA (`0xAA`) $\rightarrow$ Line Period = 150 µs.<br>Else Line Period = 200 µs. |
| **8** | **Clear Final Interrupts** | `05 FA 9A 84 00 01 FF FF` | 8 | OUT | Write `0xFFFF` to 16-bit register `0x1A84`. Clears any spurious latches from interrogation. |

---

## 2. SPI Protocol Framing Details

### 2.1 Opcode Encoding
The FT9362 controller employs specific SPI header structures:

1. **Command Strobe (2 Bytes):**
   - Format: `{ Opcode, Complement }` where `Complement == ~Opcode`.
   - Examples:
     * Wake Strobe 1: `{ 0x5A, 0xA5 }`
     * Wake Strobe 2: `{ 0xA5, 0x5A }`
     * Abort Strobe: `{ 0xC0, 0x3F }`
     * Arm Touch Strobe: `{ 0xC2, 0x3D }` *(Blocked in Stage D6)*

2. **8-bit SFR Direct Access:**
   - **Write (4 Bytes):** `{ 0x09, 0xF6, Address, Value }`
     * `0x09` = Write SFR opcode.
     * `0xF6` = SFR write subcode / bank indicator.
   - **Read (5 Bytes):** `{ 0x08, 0xF7, Address, 0x00, 0x00 }`
     * `0x08` = Read SFR opcode.
     * `0xF7` = SFR read subcode.
     * Returned data is at offset byte index 4 (`rx[4]`).

3. **16-bit Configuration Register Access:**
   - **Write (8 Bytes):**
     * Payload: `{ 0x05, 0xFA, 0x80 | (Addr >> 8), Addr & 0xFF, 0x00, 0x01, Val >> 8, Val & 0xFF }`
     * `0x05` = Write 16-bit register opcode.
     * `0xFA` = 16-bit write subcode.
     * The address high byte is framed as `0x80 | (Addr >> 8)`. For register `0x1A84`, `Addr >> 8` is `0x1A`, producing wire byte `0x9A`.
     * Data is MSB-first: `0x00, 0x01` indicates length 1 word, followed by high byte and low byte.
   - **Read (12 Bytes):**
     * Payload: `{ 0x04, 0xFB, 0x80 | (Addr >> 8), Addr & 0xFF, 0x00, 0x01, 0, 0, 0, 0, 0, 0 }`
     * Returned word is at offset byte index 6 and 7 (`rx[6..7]`).

---

## 3. Wire Address Reconciliation for Threshold Bank

In Stage D1/D2 vendor references, threshold programming references `write_reg16(0x18E0, thresh)`.

**Critical Wire Framing Finding:**
- The 16-bit register write helper encodes byte 2 as `0x80 | (Addr >> 8)`.
- If passed `Addr = 0x18E0`, byte 2 would be `0x80 | 0x18 = 0x98`, transmitting wire address `0x98E0`.
- However, the threshold memory resides in internal SRAM bank 0 at offset `0x00E0`.
- The reference driver's `write_bank` helper explicitly set the high wire byte to `0x80`, which corresponds to address `0x00E0` (`0x80 | 0x00 = 0x80`).
- Therefore, the true wire address for the threshold bank is `0x00E0`, not `0x18E0`.
- This operation remains strictly **BLOCKED** in Stage D6 pending userspace calibration data.
