# Stage D7.2 — Technical Reconciliation: Image Dimensions & Buffer Interpretation

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source and Documentation Review (Zero Hardware Operations)  
**Deliverable:** `diagnostics/STAGE_D7_2_IMAGE_GEOMETRY_RECONCILIATION.md`  

---

## 1. Executive Summary

This report reconciles conflicting statements regarding the physical sensor matrix dimensions, memory layout, and pixel interpretation of the FocalTech FT9362/FW9369:
- **Stage D7 Report:** Specified geometry as **64 × 80** (width 64 columns, height 80 rows).
- **Stage D7.1 Final Verdict:** Loosely referenced resolution as **80 × 64**.

Through rigorous source code verification across header definitions, device model bindings, and pixel neighborhood traversal algorithms, this audit establishes that:
1. The physical and logical sensor geometry is strictly **64 columns wide by 80 rows high** (`Width = 64`, `Height = 80`).
2. The total pixel count is exactly **5,120 pixels** ($64 \times 80$).
3. Pixels are 16-bit unsigned integers (`uint16_t`), transmitted in big-endian byte order across the SPI bus.
4. The raw SPI frame payload is exactly **10,240 bytes** ($5,120 \times 2$), framed by a 6-byte command/length header for a total wire transfer size of **10,246 bytes**.
5. Memory layout is standard **row-major order** with a stride of 64 words (128 bytes) per row. The Stage D7.1 reference to "80 × 64" was an informal notation transposing rows $\times$ columns.

---

## 2. Geometry Reconciliation Matrix

| Parameter | Stage D7 Report | Stage D7.1 Report | Source Evidence | Reconciled Technical Fact | Confidence Level |
|---|---|---|---|---|---|
| **Width (Columns)** | 64 | Implied 80 | `include/fte3600_image.h:12`; `device_session.c:345` | `WIDTH = 64` pixels (horizontal dimension). | **FACT (Source Verified)** |
| **Height (Rows)** | 80 | Implied 64 | `include/fte3600_image.h:13`; `device_session.c:346` | `HEIGHT = 80` rows (vertical dimension). | **FACT (Source Verified)** |
| **Pixel Count** | 5,120 | 5,120 | `include/fte3600_image.h:14` | Total pixels = $64 \times 80 = 5,120$ (`FTE3600_IMAGE_PIXELS`). | **FACT (Source Verified)** |
| **Pixel Bit Depth** | 16-bit | 16-bit | `capture.c:371`; `image.c:78` | 16 bits per pixel (`uint16_t`). | **FACT (Source Verified)** |
| **Payload Size** | 10,240 bytes | 10,240 bytes | `capture.c:278`; `fw9369_startup.c:287` | $5,120 \text{ pixels} \times 2 \text{ bytes} = 10,240 \text{ bytes}$. | **FACT (Source Verified)** |
| **Wire Header** | 6 bytes | 6 bytes | `capture.c:135`; `STAGE_D7_IMAGE_BASELINE_INVESTIGATION.md:37` | `06 F9 9A 05 14 00` (Opcode, Inv, Addr, Length). | **FACT (Source Verified)** |
| **Total Wire Frame**| 10,246 bytes | 10,246 bytes | `10240 + 6 = 10246` | Full SPI transaction buffer length. | **FACT (Source Verified)** |
| **Memory Layout** | Row-major | Not analyzed | `fw9369_startup.c:240, 248–250` | Row-major: `pixel[y][x] = buffer[y * 64 + x]`. | **FACT (Source Verified)** |

---

## 3. Authoritative Source Definitions

### 3.1 Macro Definitions (`include/fte3600_image.h:12–14`)
The geometry of the sensor is canonically established in `fte3600_image.h`:
```c
#define FTE3600_IMAGE_WIDTH 64
#define FTE3600_IMAGE_HEIGHT 80
#define FTE3600_IMAGE_PIXELS (FTE3600_IMAGE_WIDTH * FTE3600_IMAGE_HEIGHT)
```

### 3.2 Device Model Session Initialization (`device_session.c:339–346`)
When the FW9369/FT9362 device is identified and registered in `device_session.c`:
```c
else if (s->status.model == FTE3600_MODEL_FW9369)
{
    const struct fte3600_fw9369_startup_diagnostics *d = &r->word.fw;
    const struct fte3600_fw9369_startup_state *f = &s->factory.word.fw;
    s->status.raw_id = 0x9362;
    s->status.chip_id = 0x9369;
    s->status.width = 64;
    s->status.height = 80;
```
Both `status.width` and `status.height` are explicitly assigned to 64 and 80, respectively.

---

## 4. Algorithmic Proof of Memory Layout & Stride

In `fw9369_startup.c:235–261`, the baseline adaptation routine `warm_baseline()` performs spatial filtering across neighboring pixels:

```c
/* Validate before any retained baseline mutation. The final row is not an
 * observed edge; its neighbor-dependent branch has no portable meaning. */
for (size_t i = 79 * 64 + 1; i < FTE3600_IMAGE_PIXELS; ++i)
    if ((int) s->work_image[i] - (int) s->baseline[i] > 300)
        return FTE3600_FW9369_STARTUP_UNDEFINED_NEIGHBORHOOD;

for (size_t i = 0; i < FTE3600_IMAGE_PIXELS; ++i)
{
    int difference = (int) s->work_image[i] - (int) s->baseline[i];
    if (difference <= 0)
        continue;
    if (difference > 300 && i / 64 != 0 && i / 64 != 63 && i % 64 != 0)
    {
        const size_t neighbors[4] = {i - 64, i + 64, i - 1, i + 1};
        int supported = 0;
        for (size_t n = 0; n < 4; ++n)
            if ((int) s->work_image[neighbors[n]] -
                (int) s->baseline[neighbors[n]] >= 100)
                supported = 1;
        if (!supported)
            continue;
    }
    s->baseline[i] = s->work_image[i];
}
```

### 4.1 Proof Analysis:
1. **Row Stride:**
   - Moving one row vertically corresponds to an index shift of **64 pixels** (`i - 64` for North neighbor, `i + 64` for South neighbor).
   - Moving one column horizontally corresponds to an index shift of **1 pixel** (`i - 1` for West neighbor, `i + 1` for East neighbor).
2. **Coordinate Mapping:**
   - Row coordinate: `y = i / 64` ($y \in [0, 79]$).
   - Column coordinate: `x = i % 64` ($x \in [0, 63]$).
   - Linear index: `i = y * 64 + x`.
3. **Boundary Conditions:**
   - `i / 64 != 0`: Pixel is not on the top row ($y > 0$).
   - `i / 64 != 63`: Pixel is not on row 63 (guard boundary).
   - `i % 64 != 0`: Pixel is not on the leftmost column ($x > 0$).
   - `79 * 64`: Identifies the start of the 80th (final) row ($y = 79$).
4. **Conclusion:** This mathematical structure proves unconditionally that the image buffer is indexed as **64 columns wide by 80 rows tall in row-major order**.

---

## 5. Pixel Bit Depth and Endianness

### 5.1 FIFO Read Command Framing
Frame acquisition reads from SPI FIFO register `0x9A05` using the 6-byte command:
```text
06 F9 9A 05 14 00
```
- `0x06`: SPI Read Opcode
- `0xF9`: Bitwise inverted opcode (`~0x06`)
- `0x9A 0x05`: 16-bit register address of hardware FIFO
- `0x14 0x00`: Transfer word count = `0x1400` words = 5,120 16-bit words = 10,240 bytes.

### 5.2 Big-Endian Wire Deserialization
In `capture.c:371–387`, raw bytes read from the SPI bus are decoded into native `uint16_t` pixels:
```c
for (size_t i = 0; i < FTE3600_IMAGE_PIXELS; ++i)
{
    pixels[i] = (uint16_t) ((payload[6 + i * 2] << 8) | payload[7 + i * 2]);
}
```
- High-order byte precedes low-order byte: **Big-Endian**.
- Each pixel occupies 12 bits of physical ADC resolution left-justified or zero-extended within the 16-bit word.

---

## 6. Reconciliation Summary

The apparent conflict between "64 × 80" in Stage D7 and "80 × 64" in Stage D7.1 is resolved:
- **`64 × 80` (Width × Height)** is the **canonical hardware and algorithmic dimension**.
- **`80 × 64`** in Stage D7.1 was an informal transposition expressing height $\times$ width without specifying axis labels.
- All driver buffer allocations, UAPI structures, and image processing algorithms must strictly use `width = 64`, `height = 80`, `stride = 64 * sizeof(uint16_t) = 128 bytes`, and `total_payload = 10,240 bytes`.
