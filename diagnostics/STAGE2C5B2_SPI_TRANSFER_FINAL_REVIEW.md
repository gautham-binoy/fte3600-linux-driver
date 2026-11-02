# Stage 2C.5B.2 — SPI Transfer Correctness & Bus Mechanics Final Review

**Document ID:** `STAGE2C5B2_SPI_TRANSFER_FINAL_REVIEW`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Audit Scope:** SPI bus configuration correctness, full-duplex message framing, returned byte count validation, and physical chip-select integrity.  
**Hardware Authorization:** NONE (Software & Static Analysis Only).

---

## 1. Executive Summary

This report delivers the final architectural audit of the SPI bus parameter configuration and message transfer mechanics in `tools/fw9369-mock/src/fw9369_transport_hw.c`.

The audit verifies that all low-level bus parameters (Mode 0, 8-bit word size, MSB-first bit ordering, $\le 1$ MHz clock frequency) strictly match the hardware parameters proven in Stage 2A and verified across Stage 2B/2C research. Furthermore, the transfer execution loop enforces exact returned byte count validation, rejects transfer chunking, and avoids invalid assumptions regarding software-to-hardware chip select propagation.

---

## 2. SPI Parameter Verification Matrix

| Bus Parameter | Target Specification | Implementation Check | Readback Verification | Status |
| :--- | :---: | :---: | :---: | :---: |
| **Clock Polarity & Phase** | Mode 0 (`CPOL=0, CPHA=0`) | `cfg->mode == 0` | Verified via `SPI_IOC_RD_MODE == 0` | **VERIFIED** |
| **Word Length** | 8 bits per word | `cfg->bits_per_word == 8` | Verified via `SPI_IOC_RD_BITS_PER_WORD == 8`| **VERIFIED** |
| **Bit Order** | MSB First (`lsb_first = 0`) | `cfg->lsb_first == 0` | Verified via `SPI_IOC_RD_LSB_FIRST == 0` | **VERIFIED** |
| **Clock Frequency** | 1,000,000 Hz (1.0 MHz) | `cfg->speed_hz <= 1,000,000` | Verified via `SPI_IOC_RD_MAX_SPEED_HZ <= 1M` | **VERIFIED** |
| **Maximum Transfer Length** | 10,246 bytes (FIFO burst) | `len <= 10,246` | Upper bound checked in `hw_transfer()` | **VERIFIED** |

### 2.1 Parameter Clamping Defense
In Linux `spidev`, requested frequencies may be clamped by the kernel's platform clock dividers.
- In `fw9369_hw_transport_create_with_ops()`:
  Every write ioctl (`SPI_IOC_WR_*`) is followed immediately by a symmetric readback ioctl (`SPI_IOC_RD_*`).
  If `mode_read != 0`, `bits_read != 8`, `speed_read > cfg->speed_hz`, or `lsb_read != 0`, the driver aborts creation, cleans up the file descriptor, and returns `NULL` with `errno = EPROTO`.
- This guarantees deterministic bus clocking.

---

## 3. Returned Byte Count & Message Length Validation

In SPI full-duplex transactions, the master clocks out $N$ bits on MOSI while simultaneously clocking in $N$ bits on MISO.

In `src/fw9369_transport_hw.c:355–367`:

```c
    int ret = ctx->ops.ioctl_fn(ctx->fd, SPI_IOC_MESSAGE(1), &tr);
    int status = 0;

    if (ret < 0) {
        status = -errno;
    } else if ((size_t)ret < len) {
        /* Short transfer detection: incomplete byte shift */
        status = -EMSGSIZE;
    } else if ((size_t)ret > len) {
        /* Kernel returned more bytes than requested */
        status = -EPROTO;
    }
```

### 3.1 Strict Byte Count Equality (`ret == len`)
- If `ret < 0`: the negative errno is captured directly.
- If `ret < len`: a short transfer occurred (fewer bytes shifted than requested). This is flagged as `-EMSGSIZE`.
- If `ret > len`: an anomalous kernel return occurred (more bytes reported than buffer capacity). This is flagged as `-EPROTO`.
- Only when `ret == (int)len` is the transfer classified as successful (`status == 0`).

---

## 4. Transfer Non-Segmentation & Single-Burst Discipline

### 4.1 Strict Rejection of FIFO Segmentation
The FocalTech FW9369 sensor's raw frame FIFO buffer requires continuous reading:
- De-asserting CS# resets the internal FIFO read pointer to zero.
- If software chunks a 10,246-byte frame into $3 \times 4,096$-byte reads, the sensor resets on each chunk, repeating the first 4,096 bytes and corrupting the captured image.
- In `src/fw9369_transport_hw.c`:
  - `hw_transfer()` only accepts `len == FW9369_FIFO_TRANSFER_LEN` (10,246 bytes) for `FW9369_OP_FIFO_READ`.
  - It contains **zero chunking loops**.
  - Any attempt to issue a chunked FIFO read (e.g. `len = 4096`) is rejected at the allowlist boundary with `-EPERM`.

---

## 5. Software CS vs. Physical Wire Reality

The implementation enforces `tr.cs_change = 0` on every `struct spi_ioc_transfer`.

### 5.1 Clear Boundary Acknowledgment
- **What `cs_change = 0` guarantees:** It communicates an explicit directive to the Linux kernel SPI subsystem not to toggle CS# during or between transfer buffers.
- **What `cs_change = 0` does NOT guarantee:** It does **not** prove that the physical electrical CS# line on PCH Pad 229 remains glitch-free on silicon. Hardware FIFO refills, DMA burst boundaries, or LPSS interrupt latency in the `spi-pxa2xx` controller could theoretically cause microsecond glitches if DMA is improperly configured.
- **Project Position:** The codebase correctly documents that continuous physical CS# remains an operational requirement to be qualified during live hardware readout, not an assumed physical fact.

---

## 6. Audit Assessment Summary

| Review Category | Requirement | Finding |
| :--- | :--- | :---: |
| **SPI Mode 0** | CPOL=0, CPHA=0 enforced and read back | **PASSED** |
| **8 Bits per Word** | 8-bit word size enforced and read back | **PASSED** |
| **MSB-First Order** | `lsb_first = 0` enforced and read back | **PASSED** |
| **$\le 1$ MHz Clock** | Speed upper bound strictly checked | **PASSED** |
| **Exact Completion** | `ret == len` strictly enforced (`< len` -> `-EMSGSIZE`, `> len` -> `-EPROTO`) | **PASSED** |
| **Non-Segmentation** | Single 10,246 B burst enforced; chunking prohibited | **PASSED** |
| **CS# Qualification Boundary** | Correctly acknowledges software CS does not prove wire behavior | **PASSED** |

**Conclusion:** The SPI transfer mechanics are fully hardened, compliant with kernel spidev standards, and architecturally sound.
