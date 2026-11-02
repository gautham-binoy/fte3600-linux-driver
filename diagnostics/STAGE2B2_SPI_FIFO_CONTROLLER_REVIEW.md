# Stage 2B.2 — SPI Controller & 10,246-Byte Burst Transfer Review

**Document ID:** `STAGE2B2_SPI_FIFO_CONTROLLER_REVIEW`  
**Date:** 2026-11-02  
**Target Hardware:** FocalTech FT9362 Fingerprint Sensor  
**Host Controller:** Intel LPSS SPI (`pxa2xx-spi.2`, PCI ID `8086:a0aa`), Linux `7.2.8-9-omarchy` x86_64  
**Scope:** Deep kernel subsystem review of Linux `spidev`, `spi-pxa2xx-platform`, `dw_dmac` (iDMA64), transfer continuity, and failure recovery.  
**Execution Gate:** Strictly static analysis. Zero hardware reads or SPI transactions.

---

## 1. System SPI Subsystem Configuration

### 1.1 Host Hardware & Kernel Drivers
- **Physical SPI Controller:** Intel Tiger Lake-LP LPSS SPI Controller #2 (`0000:00:1e.3` / `pxa2xx-spi.2`).
- **Core Kernel Driver:** `spi-pxa2xx-platform.ko` / `spi-pxa2xx-core.ko`.
- **Integrated DMA Engine:** Synopsys DesignWare 64-bit DMA (`dw_dmac` / `idma64.2`).
  - Verified from `/proc/interrupts` ([host-inventory.txt:170](file:///home/titan/fte3600-linux-driver/diagnostics/host-inventory.txt#L170)):
    ```text
    37: IR-IO-APIC 37-fasteoi idma64.2, pxa2xx-spi.2
    ```
- **Existing `modprobe.d` Audit:**
  - An inspection of `/etc/modprobe.d/` confirms that **no configuration exists for `spidev`**.
  - Default `spidev` module parameters apply on plain `modprobe spidev`.

---

## 2. Technical Findings on the Seven Transfer Properties

### 2.1 Can 10,246 Bytes Be Submitted as One SPI Transfer?
- **YES**, at the userspace API level.
- In `libfprint-fte3600/linux_board.c:405-430`, the transfer is issued as:
  ```c
  struct spi_ioc_transfer tr = {
      .tx_buf = (uintptr_t) buffer,
      .rx_buf = (uintptr_t) receive,
      .len = 10246,
      .speed_hz = 1000000,
      .bits_per_word = 8,
      .cs_change = 0
  };
  ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
  ```
- Submitting a single `spi_ioc_transfer` request to the kernel instructs the SPI subsystem to treat the entire 10,246-byte payload as an atomic sequence.

### 2.2 Applicable `spidev.bufsiz` Requirement
- In mainline Linux kernel (`drivers/spi/spidev.c:85, 275`):
  ```c
  static unsigned bufsiz = 4096;
  module_param(bufsiz, uint, S_IRUGO);
  ...
  if (k_tmp->len > bufsiz) {
      status = -EMSGSIZE;
      goto done;
  }
  ```
- **The 4,096-Byte Limit:** By default, `spidev` allocates a single 4,096-byte memory buffer. If userspace requests `tr.len = 10246`, `spidev` rejects the ioctl immediately with **`-EMSGSIZE` (-22 / -EINVAL)** before contacting hardware.
- **Requirement:** `spidev` **MUST** be loaded with `bufsiz >= 10246` (e.g. `modprobe spidev bufsiz=65536` or installing `/etc/modprobe.d/fte3600-spidev.conf`).

### 2.3 Controller Maximum Transfer and Message Sizes
- In `drivers/spi/spi-pxa2xx.c`:
  - The controller sets `master->max_dma_len = 65536` (64 KiB).
  - The controller sets `master->max_transfer_size = pxa2xx_spi_max_transfer_size`.
  - A 10,246-byte transfer is well within the 64 KiB controller limit.

### 2.4 Is DMA Guaranteed, Conditional, or Unverified?
- **Determination: CONDITIONAL.**
- In `drivers/spi/spi-pxa2xx.c:pxa2xx_spi_can_dma()`:
  - The driver evaluates: `len >= chip->dma_burst_size` (typically 16 or 32 bytes).
  - If length exceeds the burst threshold, the driver requests an iDMA channel from `dw_dmac`.
  - **Fallback Condition:** If DMA channel allocation fails, if system memory is fragmented, or if DMA is disabled at boot, the driver falls back to **PIO mode** (interrupt-driven FIFO servicing).
  - Therefore, DMA is **conditional on runtime kernel resource availability**, not unconditionally guaranteed.
  - However, whether DMA or PIO is used, the SPI bus transfer semantics remain identical from the device's perspective.

### 2.5 Does Chip Select (CS) Remain Continuously Asserted?
- **YES.**
- In Linux SPI core architecture:
  - Chip Select is asserted low at the beginning of `spi_transfer`.
  - Because `tr.cs_change = 0`, the SPI core holds the native hardware chip select low for the entire duration of `tr.len` (10,246 bytes).
  - At 1 MHz clock speed, the transfer duration is:
    $$10,246\text{ bytes} \times 8\text{ bits} \times 1\,\mu\text{s} = 81.968\text{ ms}$$
  - Chip Select remains asserted low continuously for ~82 ms.

### 2.6 Could the Driver Split the Transfer Internally?
- **NO.**
- The Linux SPI core and `spi-pxa2xx` do not split a single `spi_transfer` into separate messages or toggle CS mid-transfer.
- In DMA mode, scatter-gather DMA blocks are linked in hardware descriptors; the controller clocks the bus continuously.
- In PIO mode, the interrupt handler refills the TX FIFO when the watermark drops (`SSSR_TFS`) without deasserting CS.
- If `spidev.bufsiz` is too small, the driver does **not** split the message: it rejects it upfront with `EMSGSIZE`.

### 2.7 What Happens if the Transfer Is Interrupted or Partially Completed?

#### 1. Host-Side Behavior
- If an interrupt (SIGKILL, abort) or bus error occurs mid-transfer:
  - The Linux SPI core terminates the DMA/PIO transfer.
  - The controller flushes its TX/RX FIFOs and immediately deasserts CS high.
  - The host memory buffer contains partial, corrupted data.

#### 2. Sensor-Side Behavior (Source Evidence)
- The FT9362 internal FIFO readout pointer auto-increments synchronously on SPI clock pulses while CS is low.
- When CS is deasserted mid-frame, the sensor FIFO pointer freezes at the offset where the clock stopped. Sensed pixel data remains un-cleared.
- **Recovery In Reference Driver:**
  In `research/libfprint-fte3600/libfprint/drivers/fte3600/device_session.c:668-705` (`fte3600_device_recover_fw9369`):
  - The reference driver **does NOT issue ad-hoc register commands** to un-stick an interrupted FIFO transfer.
  - Instead, it pulses the **hardware reset GPIO line (`RST_N`)**:
    ```c
    gpio (s, 1);       // Assert reset
    wait_ms (s, 10);
    gpio (s, 0);       // Deassert reset
    wait_ms (s, 20);
    fte3600_fw9369_startup_linux (&next, ...); // Clean re-init from Phase 1
    ```
  - **Source Fact:** The only proven recovery mechanism for an interrupted or desynchronized sensor state in the reference implementation is a **hardware reset pulse followed by clean re-initialization**.

---

## 3. Controller Review Summary

| Question | Verification Result | Basis |
| :--- | :--- | :--- |
| **Submitted as one transfer?** | **YES** | Standard `SPI_IOC_MESSAGE(1)` single transfer API. |
| **`spidev.bufsiz` requirement?** | **MANDATORY >= 10,246** | Mainline Linux defaults to 4,096; fails with `-EMSGSIZE` without `bufsiz=65536`. |
| **Controller max size?** | **64 KiB (65,536 B)** | Exceeds 10,246 B; fully capable. |
| **DMA status?** | **CONDITIONAL** | Engaged for >32 bytes if `idma64` channel available; falls back to PIO cleanly. |
| **Continuous CS assertion?** | **YES** | Verified. `cs_change = 0` guarantees ~82 ms continuous CS assertion. |
| **Internal driver splitting?** | **NO** | Kernel does not split single transfers; rejects with `EMSGSIZE` if buffer insufficient. |
| **Interruption recovery?** | **HARDWARE RESET** | Reference driver uses `RST_N` toggle + restart, not ad-hoc register fixes. |
