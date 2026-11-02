# Stage 2C.3 — FW9369 CRC Implementations & Reconciliation

**Document ID:** `STAGE2C3_CRC_RECONCILIATION`  
**Date:** 2026-11-02  
**Target Silicon:** FocalTech FT9362 / FW9369  
**Source Locations:** `research/libfprint-fte3600/libfprint/drivers/fte3600/` (`ft93xx.c`, `fw9369_startup.c`, `fw9369_release.c`, `capture.c`)  
**Scope:** Investigation 2 — Dissection of all wire and host CRC algorithms, polynomials, boundaries, byte orders, and test vectors.  
**Execution Boundary:** Static analysis only. Zero hardware access.

---

## 1. Executive Summary

FW9369 and its reference driver architecture utilize two distinct categories of CRC:
1. **Hardware-Calculated Wire CRCs:** Generated autonomously by the FT9362 internal silicon state machine and transmitted over the SPI MISO line to validate register telemetry.
2. **Host-Calculated Memory CRCs:** Computed in software by the Linux host driver over volatile RAM structures to detect buffer corruption or trigger recalibration.

Crucially, **there is NO CRC on the raw 10,246-byte image FIFO transfer**. The reported "128-word image CRC" is purely a software signature computed in host memory over the first 128 words of the baseline image buffer.

---

## 2. Comparative Analysis of All CRC Implementations

| Parameter | 1. Chip ID Wire CRC | 2. FDT Wire Telemetry CRC | 3. Host FDT Threshold CRC | 4. Host Image Baseline CRC | 5. Image FIFO Wire Transfer |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Category** | Hardware Wire CRC | Hardware Wire CRC | Host Software Check | Host Software Signature | **NO CRC PRESENT** |
| **Source File** | `ft93xx.c:211-220` | `ft93xx.c:1044-1055, 1113` | `fw9369_startup.c:116-128` | `fw9369_startup.c:306` | `capture.c:191-205` |
| **Function Name** | Inline in probe | `fdt_crc()` | `crc()` | `crc()` | N/A |
| **Input Buffer** | `buffer[6..7]` (SPI RX) | `buffer[6..13]` (SPI RX) | `state->fdt_up[0..3]` | `state->baseline[0..127]` | N/A |
| **Boundary** | 2 bytes (Chip ID) | 8 bytes (4 channels) | 4 words (8 bytes) | 128 words (256 bytes) | N/A |
| **Polynomial** | `0x1021` (CRC-CCITT) | `0x1021` (CRC-CCITT) | `0x1021` (CRC-CCITT) | `0x1021` (CRC-CCITT) | N/A |
| **Initial Seed** | `0xFFFF` | `0xFFFF` | `0xFFFF` | `0xFFFF` | N/A |
| **Reflection** | None (MSB-first) | None (MSB-first) | None (MSB-first) | None (MSB-first) | N/A |
| **Final XOR** | `0x0000` | `0x0000` | `0x0000` | `0x0000` | N/A |
| **Byte Ordering** | Big-Endian (wire order) | Big-Endian (wire order) | **Little-Endian per word** | **Little-Endian per word** | N/A |
| **Coverage** | Data only (ID bytes) | Data only (ADC channels)| Threshold words only | First 2 rows (128 words) | N/A |
| **Expected Value** | Matches `buffer[8..9]` | Matches `buffer[14..15]` | Verified on reload | Verified on reload | N/A |

---

## 3. Dissection of Each CRC Mechanism

### 1. Chip Identification Wire CRC
- **Source:** [ft93xx.c:211-220](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/ft93xx.c#L211-L220).
- **Protocol Command:** Read 10 bytes from `0x1A8B`: `[ 0x04, 0xFB, 0x9A, 0x8B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 ]`.
- **MISO Response Layout:**
  - `buffer[0..5]`: SPI transport preamble.
  - `buffer[6..7]`: 16-bit Chip ID (`0x9362`).
  - `buffer[8..9]`: 16-bit hardware-generated CRC (`0x1C53`).
- **Mathematical Definition:**
  Starts with `crc = 0xFFFF`. For each byte $b \in \{buffer[6], buffer[7]\}$:
  $$crc \leftarrow crc \oplus (b \ll 8)$$
  For each bit $0..7$:
  $$crc \leftarrow (crc \ll 1) \oplus ((crc \ \& \ 0x8000) \ ? \ 0x1021 : 0)$$
- **Hardware Test Vector (Verified from Stage 2A Physical Execution):**
  - Input: `[ 0x93, 0x62 ]`
  - Output: `0x1C53` (Exact match against physical silicon).

---

### 2. FDT Wire Telemetry CRC
- **Source:** [ft93xx.c:1044-1055, 1113-1114](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/ft93xx.c#L1044-L1055).
- **Protocol Command:** Read 16 bytes from register `0x80B8` (or `0x80E8` depending on chip/foundry):
  `[ 0x04, 0xFB, 0x80, 0xB8, 0x00, 0x03, 0x00, ... ]`
- **MISO Response Layout:**
  - `buffer[0..5]`: Preamble / address echo.
  - `buffer[6..13]`: 4 channels \(\times\) 16-bit big-endian ADC readings (8 bytes).
  - `buffer[14..15]`: 16-bit hardware CRC.
- **Mathematical Definition:** Identical byte-by-byte CRC-CCITT (MSB-first, poly `0x1021`, seed `0xFFFF`, no reflection, no final XOR) calculated over `buffer[6..13]`.

---

### 3. Host FDT Threshold CRC (Four-Word CRC)
- **Source:** [fw9369_startup.c:116-128](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L116-L128), [fw9369_release.c:80-92](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_release.c#L80-L92).
- **Function Call:**
  ```c
  op->s->fdt_down_crc = op->s->fdt_up_crc = crc (op->s->fdt_up, 4);
  ```
- **Serialization Quirk:**
  Notice the word-to-byte expansion in `crc()`:
  ```c
  for (size_t i = 0; i < count; ++i)
    for (unsigned int byte = 0; byte < 2; ++byte)
      {
        result ^= (uint16_t) ((uint16_t) (uint8_t) (words[i] >> (8 * byte)) << 8);
        for (unsigned int bit = 0; bit < 8; ++bit)
          result = (uint16_t) ((result << 1) ^ ((result & 0x8000) ? 0x1021 : 0));
      }
  ```
  When `byte == 0`, `(words[i] >> 0) & 0xFF` extracts the **LOW BYTE (LSB)**.  
  When `byte == 1`, `(words[i] >> 8) & 0xFF` extracts the **HIGH BYTE (MSB)**.  
  **The words are fed in LITTLE-ENDIAN byte order**, unlike the big-endian wire order!
- **Test Vectors (Independently Computed):**
  - Input: `words = [ 0x0000, 0x0000, 0x0000, 0x0000 ]` (8 zero bytes)  
    `CRC = 0x313E`
  - Input: `words = [ 512, 512, 512, 512 ]` = `[ 0x0200, 0x0200, 0x0200, 0x0200 ]`  
    Byte stream fed to CRC: `[ 0x00, 0x02, 0x00, 0x02, 0x00, 0x02, 0x00, 0x02 ]`  
    `CRC = 0x5B7C`
  - Input: `words = [ 482, 482, 482, 482 ]` (calibrated thresholds if minima were 512)  
    `482 = 0x01E2`. Byte stream: `[ 0xE2, 0x01, 0xE2, 0x01, 0xE2, 0x01, 0xE2, 0x01 ]`  
    `CRC = 0x081D`

---

### 4. Host Image Baseline CRC (128-Word CRC)
- **Source:** [fw9369_startup.c:306](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L306).
- **Function Call:**
  ```c
  s->baseline_crc = crc (s->baseline, 128);
  ```
- **Clarification:**
  - The baseline image has \(64 \times 80 = 5,120\) 16-bit words (10,240 bytes).
  - The CRC routine is called with `count = 128`.
  - It processes **exactly the first 128 words** (256 bytes), which corresponds to the first two horizontal rows of the sensor ($2 \times 64 = 128$ pixels).
  - It uses the exact same little-endian serialization per word as the 4-word FDT threshold CRC.
  - Its purpose is to serve as a fast fingerprint signature of the baseline buffer to detect inadvertent memory overwrite, NOT a complete cryptographic or transmission hash.

---

### 5. Absence of Image Frame FIFO Wire CRC
- **Source:** [capture.c:191-205](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/capture.c#L191-L205).
- In `fifo()`:
  - Command prefix: `[ 0x06, 0xF9, 0x9A, 0x05, 0x14, 0x00 ]`.
  - Total SPI transfer: 10,246 bytes full-duplex.
  - Received data: Bytes `[6..10245]` are copied directly into the image buffer as 5,120 16-bit pixels.
  - **There are zero CRC bytes appended to the FIFO stream.**
  - Transmission integrity must rely entirely on the electrical integrity of the SPI bus lines and the Linux LPSS DMA engine.
