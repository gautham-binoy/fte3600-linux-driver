# Stage D3 — FT9362 Capture Architecture Specification

**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`)  
**Host Controller:** Intel LPSS SPI2 (`pxa2xx-spi.2`, CS0) on Linux `7.2.8-9-omarchy`  
**Document Purpose:** In-depth technical specification of the frame acquisition pipeline, SPI burst FIFO readout protocol, 10,246-byte DMA buffer accounting, and strict boundary separation between kernel transport and userspace biometric processing.  
**Safety Classification:** Strictly Capture Architecture Design and Protocol Specification. Zero Hardware Operations Authorized.

---

## 1. Frame Geometry, FIFO Arithmetic, and Buffer Layout

### 1.1 Sensor Matrix Dimensions
- **Matrix Width (Columns):** $64\,\text{pixels}$.
- **Matrix Height (Rows):** $80\,\text{pixels}$.
- **Total Pixels per Frame:** $64 \times 80 = 5,120\,\text{pixels}$.
- **Resolution / Pixel Depth:** $16\,\text{bits}$ ($2\,\text{bytes}$ unsigned big-endian differential capacitive charge).
- **Nominal Frame Byte Size:** $5,120 \times 2 = 10,240\,\text{bytes}$.

### 1.2 Burst FIFO SPI Command Header Accounting
The FT9362 internal FIFO memory is accessed via hardware burst read opcode `0x06`:
```
Byte 0: 0x06 (Opcode Burst Read)
Byte 1: 0xF9 (Opcode One's Complement: ~0x06)
Byte 2: 0x9A (Address MSB: 0x80 | 0x1A = 0x9A)
Byte 3: 0x05 (Address LSB: Register 0x1A05)
Byte 4: 0x14 (Word Count MSB: 0x14 = 20)
Byte 5: 0x00 (Word Count LSB: 0x00)
Total Words Requested: 0x1400 = 5,120 16-bit words = 10,240 bytes.
```

### 1.3 Exact 10,246-Byte SPI Transfer & Buffer Mapping

$$\text{Total SPI Wire Transfer Length} = 6 \text{ (Command Header)} + 10,240 \text{ (Payload)} = 10,246\,\text{bytes}$$

```
+----------------------------------------------------------------------------------------------------+
| TX Buffer (Host to Sensor - MOSI): Total 10,246 Bytes                                             |
| [ Byte 0 ] [ Byte 1 ] [ Byte 2 ] [ Byte 3 ] [ Byte 4 ] [ Byte 5 ] [ Bytes 6 .. 10245             ] |
|   0x06       0xF9       0x9A       0x05       0x14       0x00       0x00 ... 0x00 (Dummy Clocks)  |
+----------------------------------------------------------------------------------------------------+
                                                  |
                                                  v  (Full Duplex SPI Clocking over Intel LPSS SPI2)
+----------------------------------------------------------------------------------------------------+
| RX Buffer (Sensor to Host - MISO): Total 10,246 Bytes                                             |
| [ Byte 0 ] [ Byte 1 ] [ Byte 2 ] [ Byte 3 ] [ Byte 4 ] [ Byte 5 ] [ Bytes 6 .. 10245             ] |
|   0xFF       0xFF       0xFF       0xFF       0xFF       0xFF     [ Pixel 0 MSB ] [ Pixel 0 LSB ] ...|
| <---------- 6 Turnaround Dummy Bytes (Discarded) ---------> | <---- 10,240 Raw Image Bytes -----> |
+----------------------------------------------------------------------------------------------------+
                                                  |
                                                  v  (copy_to_user strips 6-byte header)
+----------------------------------------------------------------------------------------------------+
| Userspace Destination Buffer (libfprint): Exactly 10,240 Bytes                                     |
| [ Bytes 0 .. 10239 ]: 5,120 consecutive 16-bit big-endian raw differential capacitive pixels       |
+----------------------------------------------------------------------------------------------------+
```

- **Reconciliation Proof:** The 6-byte difference between nominal frame payload ($10,240$) and SPI transfer size ($10,246$) is exactly the SPI command header. The actual frame data begins at byte index 6 of the kernel RX buffer.

---

## 2. Intel LPSS SPI2 Controller Capability & DMA Transport

1. **Controller Hardware:** Intel LPSS SPI2 (`pxa2xx-spi.2`) connected to Intel 64-bit iDMA controller (`idma64`).
2. **Maximum Single Transfer Size:**
   The `pxa2xx-spi` driver supports transfers up to the maximum DMA block size of $65,535$ bytes. A single $10,246$-byte transfer is well within controller hardware and driver limits.
3. **Chip-Select Assertion Invariant:**
   `t.cs_change = 0`. The Linux SPI core keeps `CS0` asserted continuously active-low from byte 0 through byte 10,245. The transfer must **not** be fragmented into smaller messages, as deasserting `CS0` prematurely terminates the internal burst address counter on the FT9362 silicon.
4. **DMA-Safe Buffer Allocation:**
   In `kernel/fte3600.c`, `struct fte3600_data` will allocate dedicated DMA-safe buffers:
   ```c
   u8 *fifo_tx_buf; /* Allocated via kmalloc(10246, GFP_KERNEL | GFP_DMA32) */
   u8 *fifo_rx_buf; /* Allocated via kmalloc(10246, GFP_KERNEL | GFP_DMA32) */
   ```
   Both buffers are allocated at driver probe and freed at device release, avoiding runtime memory allocation latency during biometric capture.

---

## 3. End-to-End Acquisition Pipeline Specification

```
+-----------------------------------------------------------------------------------+
| 1. Touch Event Triggered (State: TOUCH_DETECTED or INITIALIZED)                   |
+-----------------------------------------------------------------------------------+
                                         |
                                         v
+-----------------------------------------------------------------------------------+
| 2. Mode Entry & Timing Configuration                                              |
| - Write 0x1801 = 0xFC80 | dac (Calibrated baseline DAC)                           |
| - Write 0x1800 = 0x4FFE (Acquisition mode: Frame Capture)                         |
| - Write 0x1804 = 0x27C8 (Integration time multiplier)                             |
| - Modify 0x1806, 0x180A, 0x180B (Sample-and-hold phase timing)                    |
| - Write 0x1807 = 0x1521 (Line period timing: 150 µs for Fab AA)                   |
| - Write 0x1887 = 0x0002 (Decimation mode)                                         |
| - Write Prescalers: 0x8E=0xF4, 0x90=0x00, 0x91=0x07, 0x92=0xD0, 0x90=0x01         |
+-----------------------------------------------------------------------------------+
                                         |
                                         v
+-----------------------------------------------------------------------------------+
| 3. Physical Acquisition Initiation                                                |
| - Host transmits Command Strobe {0xC4, 0x3B} (CMD_TRIGGER)                        |
| - State transitions to FTE3600_STATE_CAPTURING                                    |
| - Host waits 1 ms for matrix integration                                          |
+-----------------------------------------------------------------------------------+
                                         |
                                         v
+-----------------------------------------------------------------------------------+
| 4. Completion Verification & Readiness Polling                                    |
| - Poll Status Register 0x80 up to 10 iterations (1 ms delay per iteration)        |
| - Success condition: Register 0x80 returns 0x54 (HW_STATUS_CAPTURE_READY)         |
| - If 10 ms elapse without 0x54: issue Abort Strobe {0xC0, 0x3F}, return -ETIMEDOUT|
+-----------------------------------------------------------------------------------+
                                         |
                                         v
+-----------------------------------------------------------------------------------+
| 5. FIFO Readout Enable                                                            |
| - Read Register 0x1800 into value                                                 |
| - Write Register 0x1800 = value | 0x0001 (Asserts FIFO bus enable bit 0)          |
| - Host waits 1 ms for FIFO bus driver stabilization                               |
+-----------------------------------------------------------------------------------+
                                         |
                                         v
+-----------------------------------------------------------------------------------+
| 6. Single Burst SPI DMA Readout (10,246 Bytes)                                    |
| - Format TX: {0x06, 0xF9, 0x9A, 0x05, 0x14, 0x00} + 10,240 zero bytes             |
| - Execute spi_sync() with CS0 held continuously active-low                        |
| - Hardware transfer duration: ~82 ms at 1 MHz (or ~20.5 ms at 4 MHz)              |
+-----------------------------------------------------------------------------------+
                                         |
                                         v
+-----------------------------------------------------------------------------------+
| 7. Latch Acknowledgment & State Transition                                        |
| - Write Register 0x1A84 = 0x0020 (Clears capture complete event flag)             |
| - State transitions to FTE3600_STATE_FRAME_READY                                  |
+-----------------------------------------------------------------------------------+
                                         |
                                         v
+-----------------------------------------------------------------------------------+
| 8. Delivery to Userspace                                                          |
| - copy_to_user(buffer_ptr, &fifo_rx_buf[6], 10240)                                |
| - State restores to FTE3600_STATE_INITIALIZED                                     |
| - ioctl(FTE3600_IOC_CAPTURE) returns 0 (Success)                                  |
+-----------------------------------------------------------------------------------+
```

---

## 4. Kernel vs Userspace Boundary Architecture

```
+-----------------------------------------------------------------------------------+
|                               USERSPACE (libfprint)                               |
|                                                                                   |
|  * IEEE-754 Floating-Point Arithmetic (float / double / fegetround)               |
|  * Volatile Baseline Calibration (Binary Search across DAC 1..125)                |
|  * Dynamic Baseline Subtraction: difference = baseline[i] - raw[i]                |
|  * Border Artifact Filtering (zeroing outermost die edges)                        |
|  * White-Point Median Outlier Filtering                                           |
|  * Grayscale Histogram Equalization (16-bit raw -> 8-bit [0..255] grayscale)      |
|  * NBIS / Bozorth Minutiae Feature Extraction & Template Matching                 |
+-----------------------------------------------------------------------------------+
                                         ^
                                         | Clean 10,240-Byte Raw Frames
                                         | via /dev/fte3600 (ioctl FTE3600_IOC_CAPTURE)
                                         v
+-----------------------------------------------------------------------------------+
|                            KERNEL DRIVER (fte3600.ko)                             |
|                                                                                   |
|  * Intel LPSS SPI2 Hardware Bus Master Management                                 |
|  * Pad 14 Electrical Reset Pulse Generation                                       |
|  * Pad 229 Threaded IRQ Handling & Event Wakeups                                  |
|  * Safe Command Strobes ({0xC4, 0x3B}, {0xC0, 0x3F})                              |
|  * Volatile Register Sequencing & Parameter Bank Locking                          |
|  * 10,246-Byte DMA Burst FIFO Readout Engine                                      |
|  * Fail-Closed Error Enforcement & State Tracking                                 |
|  * Zero Floating-Point Math; Zero Biometric Processing                            |
+-----------------------------------------------------------------------------------+
```

### Architectural Rationale
1. **Kernel Floating-Point Prohibition:** Grayscale conversion ([`fw9369_grayscale.c`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_grayscale.c)) uses floating-point math, which is strictly prohibited in kernel space.
2. **Minimal Kernel Attack Surface:** By delivering raw pixel buffers, the kernel driver remains simple, testable, and robust against memory corruption.
3. **Biometric Agility:** Algorithms for contrast enhancement and minutiae extraction can be updated in `libfprint` without requiring kernel module re-compilations.
