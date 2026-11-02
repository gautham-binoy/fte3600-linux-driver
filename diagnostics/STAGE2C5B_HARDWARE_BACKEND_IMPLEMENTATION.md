# Stage 2C.5B — Hardware Backend Implementation Report

**Document ID:** `STAGE2C5B_HARDWARE_BACKEND_IMPLEMENTATION`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Implementation Files:** `tools/fw9369-mock/src/fw9369_transport_hw.c`, `tools/fw9369-mock/include/fw9369_transport_hw.h`  
**Execution Authorization:** NONE. Software implementation and offline unit testing only.

---

## 1. Executive Summary

This report documents the concrete implementation of the Linux `spidev` transport backend created in `tools/fw9369-mock/src/fw9369_transport_hw.c`.

The backend provides a safe, fail-closed interface to the Linux SPI character device subsystem (`/dev/spidev1.0`), incorporating:
- Strict path allowlists and parameter verification.
- Allowlist packet validation prior to system-call execution.
- Single continuous `SPI_IOC_MESSAGE(1)` transfers with `cs_change = 0`.
- Short-transfer and timeout detection.
- Fail-closed error handling with zero automatic retries.
- Offline test hooks (`struct fw9369_hw_ops`) allowing 100% test coverage without touching physical device nodes.

---

## 2. Interface Definitions & Configuration Limits

In `include/fw9369_transport_hw.h`:

```c
#define FW9369_HW_DEFAULT_DEV_PATH    "/dev/spidev1.0"
#define FW9369_HW_DEFAULT_SPEED_HZ    1000000u
#define FW9369_HW_DEFAULT_BITS        8
#define FW9369_HW_DEFAULT_MODE        0 /* SPI_MODE_0 */
#define FW9369_HW_MAX_TRANSFER_LEN    10246 /* FW9369_FIFO_TRANSFER_LEN */

struct fw9369_hw_config {
    const char *device_path;      /* Must be explicitly set to "/dev/spidev1.0" */
    uint32_t   speed_hz;         /* Maximum bus frequency (<= 1000000) */
    uint8_t    mode;             /* SPI mode: must be 0 (CPOL=0, CPHA=0) */
    uint8_t    bits_per_word;    /* Word length: must be 8 */
    uint8_t    lsb_first;        /* Bit order: must be 0 (MSB first) */
    size_t     max_transfer_len; /* Max transfer length (up to 10246) */
    bool       enable_logging;   /* Capture transaction telemetry */
};
```

---

## 3. Detailed Implementation Walkthrough

### 3.1 Offline Path & Configuration Validation
In `src/fw9369_transport_hw.c`:
- `fw9369_hw_validate_device_path(const char *path)`:
  - Validates `path != NULL`.
  - Rejects any device path other than `"/dev/spidev1.0"` with `-ENODEV`.
  - Prohibits fallbacks to `/dev/spidev0.0`, `/dev/spidev2.0`, or generic devices.
- `fw9369_hw_validate_config(const struct fw9369_hw_config *cfg)`:
  - Validates path via `fw9369_hw_validate_device_path()`.
  - Enforces speed bounds: $0 < speed\_hz \le 1,000,000$ Hz.
  - Enforces Mode 0: `cfg->mode == 0`.
  - Enforces 8 bits/word: `cfg->bits_per_word == 8`.
  - Enforces MSB-first: `cfg->lsb_first == 0`.
  - Enforces transfer length limit: $0 < max\_transfer\_len \le 10,246$.

### 3.2 Device Initialization via `spidev` IOCTLs
When `fw9369_hw_transport_create_with_ops()` executes:
1. Opens file descriptor with `O_RDWR | O_CLOEXEC`.
2. Sets and verifies SPI mode:
   - `ioctl(fd, SPI_IOC_WR_MODE, &mode)`
   - `ioctl(fd, SPI_IOC_RD_MODE, &mode_read)`
   - Verifies `mode_read == 0`. If mismatch, closes `fd` and returns `-EPROTO`.
3. Sets and verifies word size:
   - `ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits)`
   - `ioctl(fd, SPI_IOC_RD_BITS_PER_WORD, &bits_read)`
   - Verifies `bits_read == 8`.
4. Sets and verifies clock speed:
   - `ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed)`
   - `ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, &speed_read)`
   - Verifies `speed_read <= speed`.
5. Sets MSB-first bit order:
   - `ioctl(fd, SPI_IOC_WR_LSB_FIRST, &lsb)`

### 3.3 Allowlist Enforcement at Transport Layer
Before preparing the `spi_ioc_transfer` structure, `hw_transfer()` inspects the outgoing buffer:
1. **Command Packets (`len == 3 && tx[2] == 0x00`):**
   - Invokes `fw9369_is_command_allowed(tx[0], tx[1])`.
   - Rejects unauthorized opcodes with `-EPERM`.
2. **FIFO Burst Read:**
   - Detects `tx[0] == 0x06 && tx[1] == 0xF9`.
   - Verifies `len == 10246`. Rejects segmented reads with `-EMSGSIZE`.
3. **Byte & Word Read/Write Operations:**
   - Decodes target register address from packet bytes.
   - Invokes `fw9369_is_register_allowed(reg, is_write)`.
   - Rejects unauthorized registers with `-EPERM`.

### 3.4 Transfer Execution & Continuous Chip Select
The SPI transaction is submitted as a single full-duplex message:
```c
struct spi_ioc_transfer tr;
memset(&tr, 0, sizeof(tr));
tr.tx_buf = (unsigned long)tx;
tr.rx_buf = (unsigned long)rx;
tr.len = (uint32_t)len;
tr.speed_hz = ctx->config.speed_hz;
tr.delay_usecs = 0;
tr.bits_per_word = ctx->config.bits_per_word;
tr.cs_change = 0;

int ret = ctx->ops.ioctl_fn(ctx->fd, SPI_IOC_MESSAGE(1), &tr);
```
- **`cs_change = 0`:** Guarantees that the PXA2xx controller holds Chip Select (CS#) active-low continuously throughout the transfer.

### 3.5 Short-Transfer Detection & Error Unwinding
- If `ret < 0`:
  `status = -errno;`
  Execution returns the negative system error code immediately.
- If `ret >= 0 && (size_t)ret < len`:
  `status = -EMSGSIZE;`
  Detects partial byte shifts and treats them as fatal communication faults.
- **Zero Automatic Retries:** The function never attempts to re-send failed packets automatically.

### 3.6 Microsecond Real-Time Delays
Unlike the mock backend which uses zero-delay no-ops, the hardware backend provides real physical timing via POSIX `nanosleep()`:
- `hw_delay_ms(context, ms)`: Sleeps exact milliseconds.
- `hw_delay_us(context, us)`: Sleeps exact microseconds.

### 3.7 Deterministic Resource Destruction
- `hw_close()` calls `ops.close_fn(ctx->fd)` and resets `ctx->fd = -1`.
- `fw9369_hw_transport_destroy()` cleans up context and transport structures on heap, leaving no dangling file descriptors or memory leaks.

---

## 4. Hardware Safety Gate in Tool Binary

The standalone tool binary `bin/fw9369-hw-tool` (compiled from `src/fw9369_main_hw.c`) enforces a fail-closed gate:
```text
===================================================================
 FocalTech FW9369 Hardware Transport Tool (Stage 2C.5B)
 Safety Boundary: Physical hardware execution is STRICTLY PROHIBITED
 Target Device:   /dev/spidev1.0
 Target Speed:    1000000 Hz
===================================================================

[PASS] Offline configuration validation: OK (Mode 0, 8-bit, MSB-first, <= 1 MHz)

[SECURITY ALERT] Stage 2C.5B is static preparation only.
[SAFETY GATE PASS] Physical device access is blocked by policy.
To verify hardware transport mechanics offline, execute bin/fw9369-tests.
```
Any execution without `--validate-config` immediately halts with exit code 1.

---

## 5. Audit Conclusion

The hardware transport backend in `src/fw9369_transport_hw.c` satisfies all technical criteria:
- Complete parameter validation.
- UAPI compliance with Linux `spidev`.
- Transport-level allowlist guard.
- Continuous Chip Select enforcement (`cs_change = 0`).
- Strict isolation from mock tooling.
- Hardware transport implementation is **VERIFIED AND READY FOR STATIC REVIEW**.
