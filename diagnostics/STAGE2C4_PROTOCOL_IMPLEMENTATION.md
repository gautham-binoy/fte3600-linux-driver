# Stage 2C.4 — Protocol & CRC Implementation Review

**Document ID:** `STAGE2C4_PROTOCOL_IMPLEMENTATION`  
**Date:** 2026-11-02  
**Target Silicon:** FocalTech FT9362 / FW9369  
**Source Code Baseline:** `tools/fw9369-mock/` (`include/fw9369_protocol.h`, `src/fw9369_protocol.c`, `src/fw9369_crc.c`)  
**Scope:** Specification of packet encoding/decoding, address translation, opcode allowlist validation, and CRC implementations.  
**Execution Boundary:** **STRICTLY MOCK-ONLY.** Physical hardware execution is completely prohibited.

---

## 1. SPI Wire Protocol & Packet Framing

The FocalTech FW9369 SPI protocol uses complement-paired opcode framing to validate byte transfer integrity across the bus:

| Operation | Opcode Byte 0 | Opcode Byte 1 | Wire Format (MOSI) | Wire Format (MISO) | Total Bytes |
| :--- | :---: | :---: | :--- | :--- | :---: |
| **Byte Write** | `0x09` | `0xF6` | `[ 0x09, 0xF6, addr, val ]` | Dummy / Hi-Z | 4 |
| **Byte Read** | `0x08` | `0xF7` | `[ 0x08, 0xF7, addr, 0x00, 0x00 ]` | `[ 0, 0, 0, 0, val ]` | 5 |
| **Word Write** | `0x05` | `0xFA` | `[ 0x05, 0xFA, hi, lo, cnt_hi, cnt_lo, d_hi, d_lo ]`| Dummy / Hi-Z | 8 |
| **Word Read** | `0x04` | `0xFB` | `[ 0x04, 0xFB, hi, lo, cnt_hi, cnt_lo, 0, 0, ... ]` | `[ Preamble (6 B), Data (2N B), CRC (2 B) ]` | \(6 + 2N + 2\) |
| **Command** | `c1` | `c2` | `[ c1, c2, 0x00 ]` | Dummy / Hi-Z | 3 |
| **Burst FIFO**| `0x06` | `0xF9` | `[ 0x06, 0xF9, 0x9A, 0x05, 0x14, 0x00, ... ]` | `[ Dummy (6 B), 10,240 Pixel Bytes ]` | 10,246 |

---

## 2. Address Encoding & Decoding Rules

In the word read/write protocol, 16-bit register addresses are mapped onto the SPI bus wire as follows:
- **Wire Encoding:**
  ```c
  pkt[2] = (uint8_t)(0x80 | (addr >> 8));
  pkt[3] = (uint8_t)(addr & 0xFF);
  ```
- **Address Space Demultiplexing:**
  - `addr = 0x18xx`: `pkt[2] = 0x98`, `pkt[3] = lo`.
  - `addr = 0x1Axx`: `pkt[2] = 0x9A`, `pkt[3] = lo`.
  - `addr = 0x80Bx` (Thresholds & ADC): `pkt[2] = 0x80`, `pkt[3] = lo` (where $lo \in [0xB0, 0xBF]$).
  - `addr = 0x00Cx` (Protected Trims): `pkt[2] = 0x80`, `pkt[3] = lo` (where $lo \in [0xC0, 0xCF]$).

The implementation cleanly maps these representations without bit collisions.

---

## 3. Register & Command Allowlist Enforcement

In [src/fw9369_protocol.c](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_protocol.c):
1. **Allowlist Function `fw9369_is_register_allowed()`:**
   Enforces that only verified, documented registers from the canonical specification may be accessed:
   - Byte registers: `0xC6`, `0x80`, `0x8E`, `0x90`..`0x92`, `0x9A`, `0xF1`, `0xF4`.
   - Word registers: `0x1800`, `0x1801`, `0x1804`, `0x1807`, `0x1808`, `0x1881`, `0x1885`, `0x1887`, `0x1A82`, `0x1A83`, `0x1A84`, `0x1A8B`.
   - Protected trims: `0x00C0`..`0x00C2`.
   - Thresholds & ADC: `0x80B0`..`0x80B3`, `0x80B8`.
   - All other addresses (including OTP / flash address ranges) return `false` and are rejected.
2. **Command Allowlist `fw9369_is_command_allowed()`:**
   Restricted to exactly four command pairs:
   - Wake: `[ 0x5A, 0xA5 ]`
   - End: `[ 0xA5, 0x5A ]`
   - Exception: `[ 0xC0, 0x3F ]`
   - Arm: `[ 0xC2, 0x3D ]`

---

## 4. CRC Engines & Verified Mathematical Behavior

### 4.1 Hardware Wire CRC (`fw9369_crc_wire`)
- **Source:** [src/fw9369_crc.c:4-18](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_crc.c#L4-L18).
- **Format:** Byte-by-byte in big-endian wire transmission order.
- **Algorithm:**
  $$\text{poly} = 0x1021, \quad \text{seed} = 0xFFFF, \quad \text{XOR} = 0x0000, \quad \text{no reflection}$$
- **Verified Vector:**
  - `[ 0x93, 0x62 ]` $\rightarrow$ `0x1C53` (Exact match against Stage 2A physical hardware telemetry).

### 4.2 Host Memory Little-Endian CRC (`fw9369_crc_words_le`)
- **Source:** [src/fw9369_crc.c:20-37](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_crc.c#L20-L37).
- **Format:** Operates on host arrays of `uint16_t` words, serializing each word in little-endian order (`byte 0 = word & 0xFF`, `byte 1 = (word >> 8) & 0xFF`).
- **Verified Vectors:**
  - `[ 0, 0, 0, 0 ]` $\rightarrow$ `0x313E`.
  - `[ 512, 512, 512, 512 ]` $\rightarrow$ `0x5B7C`.
  - `[ 482, 482, 482, 482 ]` $\rightarrow$ `0xE4AA`.
- **Image Baseline Signature:**
  - Evaluates `count = 128` over `base->baseline`, generating a fast 16-bit signature over the first two rows (256 bytes).

---

## 5. Burst FIFO Protocol & Absence of Wire CRC

In [src/fw9369_protocol.c:70-82](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_protocol.c#L70-L82):
- Command prefix: `[ 0x06, 0xF9, 0x9A, 0x05, 0x14, 0x00 ]`.
- Followed by 10,240 clock bytes.
- The sensor transmits no trailing CRC words; the implementation does not append or expect any checksum bytes over the wire.
