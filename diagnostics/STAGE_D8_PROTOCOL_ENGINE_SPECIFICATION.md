# Stage D8 — Protocol Engine Specification: Wire Framing, Register Semantics and Calibration

**Target Device:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Status:** FROZEN ARCHITECTURAL SPECIFICATION FOR STAGE D8  
**Safety Gate:** STRICTLY READ-ONLY DESIGN — ZERO HARDWARE ACCESS AUTHORIZED

---

## 1. Executive Summary & Purpose

This specification freezes the wire-level communication protocol for the FocalTech FT9362 / FW9369 SPI fingerprint sensor. It formalizes all packet structures, register addresses, command strobes, mathematical check bytes, CRC-16 checksums, and calibration algorithms.

All wire exchanges operate strictly under SPI Mode 0 (CPOL=0, CPHA=0), MSB first, at a nominal bus clock of 6.25 MHz (supported from 1.0 MHz to 12.5 MHz).

---

## 2. Command Strobe Wire Protocol

Every command strobe in the FT9362 SPI protocol adheres to a strict 3-byte framing rule:
$$\text{Frame} = [\text{CMD}, \sim\text{CMD}, 0\text{x}00]$$
where the second byte is the bitwise NOT of the command opcode ($\text{CMD} \oplus 0\text{xFF}$), and the third byte is a mandatory zero pad. Single-byte strobes are physically invalid and unacknowledged by the silicon command decoder.

### 2.1 Complete Strobe Command Table

| Functional Operation | Opcode (`CMD`) | Inversion (`~CMD`) | Pad | Wire Hex Bytes | Physical Action |
| :--- | :---: | :---: | :---: | :--- | :--- |
| **Wakeup 1 (`CMD_WAKE`)** | `0x5A` | `0xA5` | `0x00` | `{ 0x5A, 0xA5, 0x00 }` | Awakens SPI digital interface sequencer |
| **Wakeup 2 (`CMD_IDLE_ACK`)** | `0xA5` | `0x5A` | `0x00` | `{ 0xA5, 0x5A, 0x00 }` | Acknowledges transition to IDLE state |
| **Soft Reset / Abort (`CMD_ABORT`)** | `0xC0` | `0x3F` | `0x00` | `{ 0xC0, 0x3F, 0x00 }` | Forces hardware state machine back to IDLE |
| **Touch-Monitoring Arm (`CMD_ARM`)** | `0xC2` | `0x3D` | `0x00` | `{ 0xC2, 0x3D, 0x00 }` | Arms capacitive touch detection interrupt |
| **Image Trigger (`CMD_CAPTURE`)** | `0xC4` | `0x3B` | `0x00` | `{ 0xC4, 0x3B, 0x00 }` | Triggers matrix scanning and ADC conversion |

---

## 3. Register Access Framing

The FT9362 exposes two address spaces:
1. **8-bit Special Function Registers (SFR)**: Directly addressable internal registers ($0\text{x}00 \le \text{addr} \le 0\text{xFF}$).
2. **16-bit Configuration Registers**: Word-addressed matrix, AFE, and system control registers.

### 3.1 8-bit SFR Register Access

- **SFR Write (4 Bytes):**
  $$\text{TX} = [0\text{x}09, 0\text{xF6}, \text{addr}, \text{val}]$$
  Opcode is `0x09` ($\sim 0\text{x}09 = 0\text{xF6}$). The bus response is ignored (`RX` is NULL).
- **SFR Read (5 Bytes):**
  $$\text{TX} = [0\text{x}08, 0\text{xF7}, \text{addr}, 0\text{x}00, 0\text{x}00]$$
  Opcode is `0x08` ($\sim 0\text{x}08 = 0\text{xF7}$). The hardware returns the register content in byte 4:
  $$\text{Value} = \text{RX}[4]$$

### 3.2 16-bit Configuration Register Access

16-bit register addresses (`addr`) are mapped using word framing where bit 7 of the high address byte is set to 1 ($0\text{x}80 \mid (\text{addr} \gg 8)$):
- **16-bit Register Write (8 Bytes):**
  $$\text{TX} = [0\text{x}05, 0\text{x}FA, 0\text{x}80 \mid (\text{addr} \gg 8), \text{addr} \ \& \ 0\text{xFF}, 0\text{x}00, 0\text{x}01, \text{val} \gg 8, \text{val} \ \& \ 0\text{xFF}]$$
  Opcode is `0x05` ($\sim 0\text{x}05 = 0\text{x}FA$). Word count is $0\text{x}0001$ (1 word). Payload is big-endian 16-bit.
- **16-bit Register Read (12 Bytes):**
  $$\text{TX} = [0\text{x}04, 0\text{x}FB, 0\text{x}80 \mid (\text{addr} \gg 8), \text{addr} \ \& \ 0\text{xFF}, 0\text{x}00, 0\text{x}01, 0, 0, 0, 0, 0, 0]$$
  Opcode is `0x04` ($\sim 0\text{x}04 = 0\text{x}FB$). The sensor clocks out the 16-bit word at `RX[6..7]` followed by wire CRC-16 at `RX[8..9]`:
  $$\text{Value} = (\text{RX}[6] \ll 8) \mid \text{RX}[7], \quad \text{CRC} = (\text{RX}[8] \ll 8) \mid \text{RX}[9]$$

---

## 4. Hardware Verification and Wire CRC-16 Specification

### 4.1 CRC-16 CCITT Algorithm
The FT9362 uses standard CRC-16 CCITT with polynomial $0\text{x}1021$ ($x^{16} + x^{12} + x^5 + 1$), seed $0\text{xFFFF}$, MSB-first bit order, unreflected input and output, and XOR sum $0\text{x}0000$.

```c
static u16 fte3600_crc16_wire(const u8 *bytes, size_t len)
{
    u16 crc = 0xFFFF;
    size_t i;
    unsigned int bit;

    if (!bytes || len == 0)
        return 0;

    for (i = 0; i < len; ++i) {
        crc ^= ((u16)bytes[i] << 8);
        for (bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021u) : (crc << 1);
        }
    }
    return crc;
}
```

### 4.2 Known Test Vectors
- Chip ID bytes `[0x93, 0x62]` yield strictly `0x1C53`.
- Any CRC mismatch immediately transitions the driver to `FTE3600_STATE_UNKNOWN` and returns `-EBADMSG`.

---

## 5. Capacitive Touch Detection (FDT) & Calibration

### 5.1 FDT Search Parameters & Invariants
1. **Channel Mode:** Strictly `channel_mode == 1` (4 active differential channels). Mode 0 (8 channels) is unsupported.
2. **DAC Operating Range:** $1 \le \text{DAC} \le 125$. Starting DAC is strictly **27**.
3. **Maximum Search Iterations:** 31 iterations (`FTE3600_CALIBRATION_MAX_SAMPLES`).
4. **Convergence Criterion:** Two adjacent DAC steps crossing the midpoint threshold of $512$ counts:
   $$\text{cross} = (\text{DAC} = \text{prev\_DAC} + 1 \land \text{prev\_meas} \le 512 < \text{meas}) \lor (\text{prev\_DAC} = \text{DAC} + 1 \land \text{meas} \le 512 < \text{prev\_meas})$$

### 5.2 10-Sample Stability Loop
After finding the baseline DAC, the driver verifies analog settling:
- A shared loop executes up to 10 measurement samples across all 4 channels.
- Stability requires **three consecutive passes** where every active channel varies by $\le \pm 5$ counts from the preceding sample:
  $$\max_{c \in [0..3]} |\text{ADC}_c(k) - \text{ADC}_c(k-1)| \le 5$$
- The earliest possible exit occurs at Sample 2 (3 passes: 0, 1, 2). If not stabilized after 10 samples, calibration fails closed with `-ETIMEDOUT`.

### 5.3 Threshold Calculation and Hardware Bank Programming
1. **Threshold Formula:**
   $$\text{Threshold}_c = \max(0, \min_{k}(\text{ADC}_{c,k}) - 30)$$
   where $\min(\text{ADC}_{c})$ must fall within $[300, 700]$ counts; otherwise, calibration is aborted.
2. **Hardware Register Bank Write (22 Bytes):**
   - **Unlock Key:** Write SFR $0\text{x}9A = 0\text{x}5A$.
   - **Bank Burst Write (22 Bytes):**
     $$\text{TX}[0..5] = [0\text{x}05, 0\text{x}FA, 0\text{x}80, (\text{foundry} == 0\text{x}AA \ ? \ 0\text{xE0} : 0\text{xB0}), 0\text{x}00, 0\text{x}08]$$
     $$\text{TX}[6..13] = [\text{Threshold}_0 \dots \text{Threshold}_3] \quad (\text{Big-Endian, 8 bytes total})$$
     $$\text{TX}[14..21] = [0\text{x}00 \dots 0\text{x}00] \quad (\text{Zero pad channels 4..7, 8 bytes})$$
   - **Lock Key:** Write SFR $0\text{x}9A = 0\text{x}00$.

### 5.4 Operational Arming Sequence
Upon threshold commitment, the driver transitions to `READY` via:
1. Configure sensor mode: Read $0\text{x}1881$, set bit 7 and bit 0 ($(val \ \& \ 0\text{xFF7C}) \mid 0\text{x}0081$), write $0\text{x}1881$.
2. Configure detection filter: Read $0\text{x}1A83$, mask $(val \ \& \ 0\text{xFF80}) \mid 0\text{x}0016$, write $0\text{x}1A83$.
3. Clear interrupt latch: Write $0\text{x}1A84 = 0\text{x}002F$.
4. Dispatch Arm strobe: Transmit `{ 0xC2, 0x3D, 0x00 }`.
5. Stabilization wait: 1 ms.
6. Driver sets `state = FTE3600_STATE_READY`.

---

## 6. FIFO Image Frame Burst Acquisition

### 6.1 Dimensions and Framing
- **Matrix Geometry:** $64 \times 80$ pixels ($5,120$ pixels).
- **Pixel Encoding:** 16 bits per pixel, big-endian unsigned integer ($10,240$ bytes total).
- **Burst Header (6 Bytes):**
  $$\text{TX}[0..5] = [0\text{x}06, 0\text{xF9}, 0\text{x}9A, 0\text{x}05, 0\text{x}14, 0\text{x}00]$$
  where $0\text{x}06$ is burst read opcode ($\sim 0\text{x}06 = 0\text{xF9}$), $0\text{x}9A05$ is FIFO memory address, and $0\text{x}1400$ ($5,120$) is word count.
- **Total SPI Transaction Length:** $6 + 10,240 = 10,246$ bytes.

### 6.2 Capture Trigger & Readout Pipeline
1. Check controller status: Poll SFR $0\text{x}80 == 0\text{x}50$.
2. Dispatch trigger strobe: Transmit `{ 0xC4, 0x3B, 0x00 }`.
3. Poll capture completion: Poll SFR $0\text{x}80 == 0\text{x}54$ (up to 10 ms).
4. Enable FIFO output: Set bit 0 in register $0\text{x}1800$.
5. Submit burst transfer: 10,246 bytes via asynchronous SPI engine.
6. Acknowledge interrupt: Write register $0\text{x}1A84 = 0\text{x}0020$.
7. Extract pixel payload: Discard 6-byte header, copy $10,240$ bytes to userspace.

---

## 7. Transport Abstraction Interface (Hardware vs Mock)

To enable 100% test coverage and fault-injection testing without touching physical hardware, the protocol engine dispatches all SPI operations through an abstract transport interface:

```c
struct fte3600_transport_ops {
    int (*transfer)(struct fte3600_data *data, const u8 *tx, u8 *rx, size_t len);
    int (*reset_pulse)(struct fte3600_data *data);
    void (*delay_us)(unsigned int min_us, unsigned int max_us);
};
```
In kernel production builds, `transfer` dispatches directly to `spi_async()` with DMA quarantine protection. In userspace test harnesses and mock simulation suites, `transfer` dispatches to deterministic synthetic test fixtures.

---

## 8. Conclusion and Sign-Off

The protocol engine wire framing, register semantics, FDT calibration, and FIFO acquisition specifications are formally frozen for Stage D8.
