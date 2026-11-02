# Stage 2C.5B.1 — Timeout Semantics and Error Handling Audit

**Document ID:** `STAGE2C5B1_TIMEOUT_AND_ERROR_REVIEW`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Audit Scope:** Area C — Delay timers vs. transfer timeouts, blocking ioctl semantics, short transfer detection, and error propagation.  
**Hardware Authorization:** NONE (Software & Static Analysis Only).

---

## 1. Executive Summary

This audit evaluates the timing primitives, transaction completion checks, short transfer detection, and error handling paths in `tools/fw9369-mock/src/fw9369_transport_hw.c`.

Key findings of this review include:
1. **Delay vs. Timeout Distinction:** `nanosleep()` is used strictly as an inter-transaction physical settling delay (e.g., waiting 4 ms after writing `0xC6 = 0x01` to allow transceiver clock lock). It is **not** a transfer timeout.
2. **Blocking Ioctl Semantics:** The standard Linux `spidev` UAPI `ioctl(fd, SPI_IOC_MESSAGE(N), &tr)` is inherently synchronous and blocking. It has **no userspace timeout parameter**. The duration of an in-flight transfer is governed exclusively by the kernel SPI core and the underlying platform controller driver (`spi-pxa2xx`), which enforces an internal DMA/PIO timeout (typically 1,000 ms).
3. **Short Transfer Detection:** The implementation explicitly inspects the return code of `ioctl()`: if `ret < (int)len`, it flags a short transfer and converts the return value into `-EMSGSIZE`.
4. **Zero Uncontrolled Retries:** Transport-level errors (`-EIO`, `-EBUSY`, `-EMSGSIZE`, `-EPERM`) are immediately propagated upward to the state machine without automatic blind retries, preventing masked bus corruption.

---

## 2. Timing Semantics Analysis

### 2.1 Settling Delays via `nanosleep`
In `src/fw9369_transport_hw.c:106–124`:

```c
static void hw_delay_ms(void *context, uint32_t ms)
{
    (void)context;
    struct timespec req = {
        .tv_sec = ms / 1000,
        .tv_nsec = (long)(ms % 1000) * 1000000L
    };
    nanosleep(&req, NULL);
}

static void hw_delay_us(void *context, uint32_t us)
{
    (void)context;
    struct timespec req = {
        .tv_sec = us / 1000000,
        .tv_nsec = (long)(us % 1000000) * 1000L
    };
    nanosleep(&req, NULL);
}
```

#### Purpose and Behavior
- These functions satisfy the callback contracts of `struct fw9369_transport`: `delay_ms` and `delay_us`.
- They are invoked between discrete SPI transactions to satisfy hardware timing requirements mandated by the FocalTech FW9369 specification:
  - 4 ms delay after writing `0xC6 = 0x01` (transceiver PLL stabilization).
  - 1 ms delay after submitting command `0x5A 0xA5` (MCU idle transition window).
  - Polling interval (1 ms) during FDT ADC conversion checks on Register `0x1A82`.
- `nanosleep()` suspends the calling thread and allows other Linux scheduler tasks to execute.
- **Audit Assessment:** These delays are correctly decoupled from SPI transfer execution. They do not govern or abort in-flight SPI hardware clocks.

---

## 3. SPI Transfer Completion & Kernel Timeout Architecture

### 3.1 Absence of Userspace Timeout in `spidev`
The Linux SPI userspace API structure `struct spi_ioc_transfer` includes:
```c
struct spi_ioc_transfer {
    __u64 tx_buf;
    __u64 rx_buf;
    __u32 len;
    __u32 speed_hz;
    __u16 delay_usecs;
    __u8 bits_per_word;
    __u8 cs_change;
    __u8 tx_nbits;
    __u8 rx_nbits;
    __u8 word_delay_usecs;
    __u8 pad;
};
```
- There is **no field for a transfer timeout** in `struct spi_ioc_transfer`.
- When userspace issues `ioctl(fd, SPI_IOC_MESSAGE(1), &tr)`, control transfers synchronously to `spidev_ioctl()` in `drivers/spi/spidev.c`.
- In the kernel, `spi_sync()` submits the transfer to the SPI controller driver (`drivers/spi/spi-pxa2xx.c`) and blocks until the controller issues an interrupt or DMA completion callback.
- The `spi-pxa2xx` controller driver utilizes an internal kernel completion timer (typically `wait_for_completion_timeout(&drv_data->done, msecs_to_jiffies(1000))`).
- If the hardware hangs (e.g. SCLK line grounded, DMA engine frozen), the kernel driver will abort the transfer after 1,000 ms and return `-ETIMEDOUT` or `-EIO` to userspace.
- Userspace `fw9369_transport_hw.c` correctly relies on the kernel's completion handling and receives standard POSIX error codes.

---

## 4. Short Transfer Detection and Handling

### 4.1 Incomplete Byte Shift Detection
In SPI full-duplex communication, the master shifts out $N$ bits on MOSI while simultaneously shifting in $N$ bits on MISO. If a transfer completes successfully, the kernel returns the exact number of bytes transferred: `ret == len`.

In `src/fw9369_transport_hw.c:207–215`:

```c
    int ret = ctx->ops.ioctl_fn(ctx->fd, SPI_IOC_MESSAGE(1), &tr);
    int status = 0;

    if (ret < 0) {
        status = -errno;
    } else if ((size_t)ret < len) {
        /* Short transfer detection: incomplete byte shift */
        status = -EMSGSIZE;
    }
```

### 4.2 Audit Analysis
- If `ret < 0`, `status` is set to `-errno` (e.g., `-EFAULT`, `-EBADF`, `-EINTR`, `-EIO`).
- If `ret >= 0` but `(size_t)ret < len`, the driver recognizes that fewer bytes were shifted across the bus than requested by the protocol framing.
- In SPI protocol exchanges (especially 10,246-byte FIFO burst reads or 8-byte word reads with CRC), a short transfer means that the remaining bytes were not clocked out, leaving the sensor's internal pointer desynchronized.
- Converting this condition to `-EMSGSIZE` ensures that upper layers treat the packet as corrupted and execute an immediate abort sequence rather than parsing partial payloads.

---

## 5. Error Propagation and Fail-Closed Discipline

### 5.1 Zero Masked Retries
A major source of hardware instability in custom Linux drivers is automated retry loops around failed register writes. If a hardware bus fault occurs (such as an electrical short or clock gating failure), looping indefinitely or retrying immediately can overheat the sensor or lock up the host controller.

In `fw9369_transport_hw.c`:
- `hw_transfer()` contains **zero retry loops**.
- Any failure reported by `ops->ioctl_fn` or the allowlist validator immediately terminates the function and returns the negative error code directly to the caller.
- Upper layers (specifically `fw9369_pipeline_run` in `src/fw9369_state.c`) catch the error, immediately set `state->phase = FW9369_PHASE_ABORTED`, and execute the mandatory cleanup handler to relock protected registers if they were unlocked.

---

## 6. Audit Assessment Summary

| Review Criteria | Verification Result | Status |
| :--- | :--- | :---: |
| **Delay vs Timeout Distinction** | `nanosleep()` is used strictly for physical settling delays | **PASSED** |
| **Blocking Ioctl Understanding** | Acknowledges kernel governs transfer timeouts (no userspace param) | **PASSED** |
| **Short Transfer Detection** | Explicitly checks `(size_t)ret < len` and returns `-EMSGSIZE` | **PASSED** |
| **Error Code Propagation** | Preserves `-errno` without swallowing or mutating POSIX codes | **PASSED** |
| **Retry Suppression** | No transport-level retries; fail-closed termination on error | **PASSED** |

**Conclusion:** The timeout semantics and error handling mechanisms in `fw9369_transport_hw.c` adhere to strict fail-closed safety and correctly interface with the Linux kernel SPI subsystem.
