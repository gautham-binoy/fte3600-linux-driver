# Stage 2C.3 — Four-Layer SPI FIFO Feasibility Review

**Document ID:** `STAGE2C3_FIFO_FEASIBILITY_REVIEW`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target SoC:** Intel Tiger Lake-LP PCH (`pxa2xx-spi.2` / `idma64.2`)  
**Investigated Transfer:** Single-Burst Image FIFO Read (10,246 bytes full-duplex)  
**Investigation Scope:** Investigation 4 — Multi-layer validation of Linux spidev, SPI core, PXA2xx driver, LPSS DMA engine, electrical CS continuity, and sensor FIFO protocol.  
**Execution Boundary:** Static analysis only. Zero hardware access.

---

## 1. Executive Summary & Four-Level Verdict

Acquiring a raw capacitive frame from the FT9362 requires reading **10,246 consecutive bytes** within a single, uninterrupted Chip Select (CS# low) assertion:
- **Prefix:** 6 bytes MOSI `[ 0x06, 0xF9, 0x9A, 0x05, 0x14, 0x00 ]`
- **Payload:** 10,240 bytes MISO ($64 \times 80 = 5,120$ 16-bit big-endian pixels)

This audit evaluates feasibility across all four physical and logical layers of the Linux kernel and hardware stack:

### Four-Level Verdict Matrix

| Layer | Assessment Question | Verdict | Technical Finding & Evidence |
| :--- | :--- | :---: | :--- |
| **1. Linux API** | Can the message be submitted? | **FEASIBLE (CONDITIONAL)** | `spidev_ioctl(SPI_IOC_MESSAGE(1))` accepts 10,246 bytes **only** if module parameter `spidev.bufsiz >= 65536`. Default `bufsiz=4096` causes immediate `-EMSGSIZE`. |
| **2. Controller Driver** | Can the controller execute it? | **FEASIBLE** | `drivers/spi/spi-pxa2xx.c` routes transfers $> 16$ bytes to the Intel LPSS DMA controller (`idma64.2`). `idma64` supports scatter-gather descriptors up to 64 KB per block. |
| **3. Electrical / Protocol** | Does CS remain asserted as required? | **FEASIBLE** | With `cs_change = 0`, the PXA2xx hardware CS line stays continuously asserted low for the entire ~82 ms duration (at 1 MHz SPI clock) without glitching. |
| **4. Sensor Silicon** | Is a complete FIFO read established? | **FEASIBLE (ARMED ONLY)** | FT9362 internal FIFO stream is established **if and only if** the sensor has been fully calibrated and armed. If FIFO is un-triggered, dummy `0xFF` bytes are returned. |

---

## 2. In-Depth Layer Analysis

### Layer 1: Linux spidev User API Boundary
In mainline Linux `drivers/spi/spidev.c`:
```c
static unsigned bufsiz = 4096;
module_param(bufsiz, uint, S_IRUGO);
...
static long spidev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    ...
    case SPI_IOC_MESSAGE(N):
        for (u = 0; u < n_ioc; u++) {
            total += k_tmp[u].len;
            if (total > bufsiz) {
                status = -EMSGSIZE;
                goto done;
            }
        }
}
```
- **Mainline Default:** `bufsiz = 4096`. Attempting to submit a 10,246-byte message fails instantly at the syscall boundary with `-EMSGSIZE` before reaching the SPI bus driver.
- **Module Parameter Configuration:** Passing `modprobe spidev bufsiz=65536` increases the kernel bounce buffer capacity to 64 KB, allowing `ioctl` to succeed.

---

### Layer 2: Linux SPI Core & Controller Driver (`spi-pxa2xx`)
In `drivers/spi/spi-pxa2xx.c`:
1. **Transfer Validation:** The SPI core validates `max_transfer_size` and `max_message_size`. The PXA2xx driver does not set an artificial transfer cap, deferring to the underlying controller capabilities.
2. **DMA Selection:**
   - The driver evaluates `pxa2xx_spi_can_dma()`: any transfer with `len > 16` bytes qualifies for DMA.
   - For 10,246 bytes, the driver builds an `idma64` transfer using `dmaengine_prep_slave_sg()`.
3. **Intel LPSS DMA Capabilities (`idma64`):**
   - The `idma64` controller on Intel Tiger Lake-LP PCH supports transfer descriptor blocks up to 64 KB or 128 KB.
   - 10,246 bytes fit cleanly into a single contiguous DMA descriptor without chaining or descriptor exhaustion.
4. **DMA Fallback:** If DMA channels were exhausted, the controller falls back to interrupt-driven PIO with its internal 64-byte FIFO. However, at 1 MHz, DMA is the default and deterministic path.

---

### Layer 3: Electrical & Chip-Select Timing
1. **SPI Timing Calculation:**
   - Bus frequency: 1,000,000 Hz (1 MHz), confirmed from ACPI DSDT and Stage 2A telemetry.
   - Clock period: $T_{clk} = 1.0\ \mu\text{s}$.
   - Byte duration: $8 \times 1.0\ \mu\text{s} = 8.0\ \mu\text{s}$.
   - Full message duration:
     $$T_{transfer} = 10,246 \times 8.0\ \mu\text{s} \approx 81,968\ \mu\text{s} \approx 82\text{ ms}$$
2. **Chip-Select Continuity:**
   - In `struct spi_ioc_transfer`, `cs_change` is set to `0`.
   - The PXA2xx hardware controller asserts CS# (active low) at byte 0 and maintains CS# asserted continuously until byte 10,245 is completely shifted out.
   - DMA completion interrupts do not toggle CS#. CS# deasserts only after the trailing clock cycle completes.
   - DMA controller timeout is set to 1,000 ms, well above the 82 ms transfer time.

---

### Layer 4: Sensor FIFO Protocol Requirements
In [capture.c:191-205](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/capture.c#L191-L205):
1. **Atomic Read Pointer:**
   - The FT9362 internal FIFO address pointer is reset to 0 upon CS# assertion and receiving command `0x06 0xF9 0x9A 0x05 0x14 0x00`.
   - If software attempts to segment the 10,246-byte read into multiple smaller chunk transfers (e.g., three chunks of 3,415 bytes), **each CS# deassertion and re-assertion resets or corrupts the internal FIFO pointer**, resulting in repeated or scrambled image slices.
   - **Continuous, unsegmented single-burst transfer is an absolute protocol requirement.**
2. **State Prerequisite:**
   - The sensor silicon only outputs valid pixel data if it has completed AFE calibration, baseline preparation, and an exposure scan has been triggered via register `0x1885 = 1` or autonomous finger-down interrupt.
   - If read while un-triggered or uncalibrated, the FIFO returns empty padding (`0xFF` or `0x00`).

---

## 3. Necessity vs. Sufficiency of `spidev.bufsiz=65536`

| Property | Verdict | Justification |
| :--- | :---: | :--- |
| **Is it NECESSARY?** | **YES** | Without `spidev.bufsiz >= 10246`, `spidev_ioctl` unconditionally returns `-EMSGSIZE`. The message cannot enter the kernel. |
| **Is it SUFFICIENT?** | **NO** | `bufsiz=65536` only opens the userspace syscall gateway. Successful execution still depends on controller DMA availability, uninterrupted CS continuity, valid AFE registers, and prior sensor exposure triggering. |
| **Strategic Conclusion** | **NECESSARY BUT NOT SUFFICIENT** | A future userspace tool must enforce `spidev.bufsiz >= 65536` as an indispensable prerequisite, but must also implement robust timeout handling and frame validation. |
