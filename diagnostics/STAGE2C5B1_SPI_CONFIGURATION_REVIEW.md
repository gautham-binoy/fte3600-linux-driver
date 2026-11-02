# Stage 2C.5B.1 — SPI Configuration & Controller Setup Audit

**Document ID:** `STAGE2C5B1_SPI_CONFIGURATION_REVIEW`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Audit Scope:** Area B — SPI configuration ioctls, readback verification, parameter safety bounds, and controller vs. sensor semantics.  
**Hardware Authorization:** NONE (Software & Static Analysis Only).

---

## 1. Executive Summary

This audit independently reviews the SPI bus parameter configuration routines implemented in `tools/fw9369-mock/src/fw9369_transport_hw.c`. 

The audit confirms that:
1. SPI Mode 0 (`CPOL=0, CPHA=0`), 8 bits per word, MSB-first bit order, and a conservative speed of 1,000,000 Hz are correctly requested through standard Linux `spidev` UAPI ioctls.
2. The implementation executes symmetric readback ioctls (`SPI_IOC_RD_*`) after every write ioctl (`SPI_IOC_WR_*`) to assert that the host kernel controller accepted the requested configuration without clamping or error.
3. Crucially, the implementation correctly recognizes the boundary between host-side controller configuration and physical sensor acknowledgement: configuring host controller registers via ioctl generates **zero bus transactions** on the physical SPI lines and does not imply that the sensor is awake, connected, or responding.

---

## 2. Code Inspection: Configuration Validation and Ioctl Sequence

### 2.1 Configuration Validation (`fw9369_hw_validate_config`)
The offline configuration validation routine in `src/fw9369_transport_hw.c:74–104` checks:

```c
int fw9369_hw_validate_config(const struct fw9369_hw_config *cfg)
{
    if (!cfg)
        return -EINVAL;

    int ret = fw9369_hw_validate_device_path(cfg->device_path);
    if (ret < 0)
        return ret;

    if (cfg->speed_hz == 0 || cfg->speed_hz > FW9369_HW_MAX_SPEED_HZ)
        return -EINVAL;

    if (cfg->spi_mode != FW9369_HW_SPI_MODE)
        return -EINVAL;

    if (cfg->bits_per_word != FW9369_HW_BITS_PER_WORD)
        return -EINVAL;

    if (cfg->lsb_first != 0)
        return -EINVAL;

    if (cfg->max_transfer_len == 0 || cfg->max_transfer_len > FW9369_HW_MAX_TRANSFER_LEN)
        return -EINVAL;

    return 0;
}
```

#### Constants Audit
- `FW9369_HW_MAX_SPEED_HZ`: Defined as `1000000` (1.0 MHz). This matches the proven safe speed from Stage 2A and stays well within the sensor's verified clock capabilities.
- `FW9369_HW_SPI_MODE`: Defined as `0` (`SPI_MODE_0`).
- `FW9369_HW_BITS_PER_WORD`: Defined as `8`.
- `cfg->lsb_first`: Enforced as `0` (MSB-first).
- `FW9369_HW_MAX_TRANSFER_LEN`: Defined as `10246` (matches exact FIFO burst size).

### 2.2 Host Controller Setup Sequence (`fw9369_hw_transport_create_with_ops`)
Lines 252–305 execute the four standard `spidev` parameter setup steps:

```c
    /* 1. SPI Mode (Mode 0: CPOL=0, CPHA=0) */
    uint8_t mode = cfg->spi_mode;
    if (ops->ioctl_fn(fd, SPI_IOC_WR_MODE, &mode) < 0) goto err_cleanup;
    uint8_t mode_rb = 0;
    if (ops->ioctl_fn(fd, SPI_IOC_RD_MODE, &mode_rb) < 0 || mode_rb != mode) goto err_cleanup;

    /* 2. Bits per word (8 bits) */
    uint8_t bits = cfg->bits_per_word;
    if (ops->ioctl_fn(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) goto err_cleanup;
    uint8_t bits_rb = 0;
    if (ops->ioctl_fn(fd, SPI_IOC_RD_BITS_PER_WORD, &bits_rb) < 0 || bits_rb != bits) goto err_cleanup;

    /* 3. Max Speed (1 MHz) */
    uint32_t speed = cfg->speed_hz;
    if (ops->ioctl_fn(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) goto err_cleanup;
    uint32_t speed_rb = 0;
    if (ops->ioctl_fn(fd, SPI_IOC_RD_MAX_SPEED_HZ, &speed_rb) < 0 || speed_rb != speed) goto err_cleanup;

    /* 4. Bit Justification (MSB First: lsb_first = 0) */
    uint8_t lsb = cfg->lsb_first;
    if (ops->ioctl_fn(fd, SPI_IOC_WR_LSB_FIRST, &lsb) < 0) goto err_cleanup;
    uint8_t lsb_rb = 0xFF;
    if (ops->ioctl_fn(fd, SPI_IOC_RD_LSB_FIRST, &lsb_rb) < 0 || lsb_rb != lsb) goto err_cleanup;
```

---

## 3. Analysis of Critical Configuration Areas

### 3.1 Readback Verification Rigor
In Linux `spidev`, drivers frequently clamp requested parameters to the hardware limits of the underlying SPI controller. For instance, if an application requests 1,000,000 Hz, but the controller clock divider can only produce 960,000 Hz or 1,200,000 Hz, `SPI_IOC_WR_MAX_SPEED_HZ` may succeed, but subsequent transactions run at the clamped rate.
- In `fw9369_transport_hw.c`, every configuration ioctl is followed immediately by an `SPI_IOC_RD_*` ioctl.
- If the read back value does not exactly match the requested value (`mode_rb != mode`, `bits_rb != bits`, `speed_rb != speed`, or `lsb_rb != lsb`), the routine aborts immediately (`goto err_cleanup`), closes the file descriptor, and returns `NULL` with `errno = EIO`.
- This ensures deterministic clocking and timing characteristics.

### 3.2 Controller-Side vs. Sensor-Side Semantics
A common architectural error in embedded driver design is assuming that a successful `SPI_IOC_WR_*` ioctl indicates that the sensor is present, healthy, or communicating.

**Static Verification:**
- SPI is a master-slave, de-asserted idle bus. In SPI Mode 0, SCLK is low, CS# is high, and MOSI/MISO are idle.
- Executing `ioctl(fd, SPI_IOC_WR_MODE, ...)` instructs the Linux kernel driver `spi-pxa2xx` to update internal registers in the Intel LPSS SPI controller (`0000:00:1e.2`).
- CS# remains de-asserted high throughout the entire configuration sequence.
- Zero clock pulses are generated; zero bits are transmitted over the physical wire.
- Therefore, successful completion of `fw9369_hw_transport_create()` proves **only** that the host kernel controller is initialized; it does **not** indicate sensor presence or responsiveness.
- The `fw9369-mock` codebase correctly respects this distinction: actual hardware presence is interrogated only through explicit byte/word/CRC exchanges during Phase 1 (Register `0xC6` write/readback and Register `0x1A8B` Chip ID read).

---

## 4. Assessment Summary

| Requirement | Implementation Finding | Status |
| :--- | :--- | :---: |
| **SPI Mode 0 Request** | Requested via `SPI_IOC_WR_MODE` with value `0` | **VERIFIED** |
| **8 Bits/Word Request** | Requested via `SPI_IOC_WR_BITS_PER_WORD` with value `8` | **VERIFIED** |
| **MSB-First Bit Order** | Requested via `SPI_IOC_WR_LSB_FIRST` with value `0` | **VERIFIED** |
| **Speed $\le 1$ MHz** | Requested via `SPI_IOC_WR_MAX_SPEED_HZ` with `1,000,000` | **VERIFIED** |
| **Readback Verification** | All 4 settings verified symmetrically with `SPI_IOC_RD_*` | **VERIFIED** |
| **Host vs. Sensor Boundary** | Correctly treats ioctls as host controller config only | **VERIFIED** |

**Conclusion:** The SPI configuration logic is architecturally sound, conforms strictly to Linux spidev conventions, and safely bounds controller parameters.
