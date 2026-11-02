# Stage 2B — Hardware Risk Register & Safety Matrix

**Document ID:** `STAGE2B_RISK_REGISTER`  
**Date:** 2026-11-02  
**Target Device:** FocalTech FT9362 (FW9369 Architecture)  
**Host Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), Linux `7.2.8-9-omarchy`

---

## 1. Risk Evaluation Framework

Every hardware interaction with the FT9362 sensor carries potential risks ranging from software transaction failures to permanent silicon alteration. This Risk Register catalogues identified hardware, electrical, thermal, and protocol failure modes, evaluates their severity and probability, and establishes mandatory software mitigations.

Risk Severity Levels:
- **CRITICAL:** Permanent hardware damage, OTP/EFUSE corruption, motherboard lockup, or electrical short circuit.
- **HIGH:** System freeze, kernel panic, persistent sensor misconfiguration requiring cold power cycle, or battery drain.
- **MEDIUM:** Unreliable biometric matching, failed calibration, dropped image frames, or bus timeout.
- **LOW:** Harmless diagnostic warning or benign retry.

---

## 2. Comprehensive Risk Matrix

| Risk ID | Hazard Description | Severity | Probability | Impact | Mandatory Mitigation |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **RSK-01** | **OTP / Factory Calibration Corruption via Protected Register Unlock (`0x9A = 0x5A`)** | CRITICAL | LOW | Writing to incorrect register addresses while protected registers are unlocked could overwrite factory sensor calibration or trim parameters. | 1. Restrict unlocked writes strictly to approved registers (`0x00C0`..`0x00C2` and bank `0x80B0`/`0x80E0`).<br>2. Immediately re-lock registers with `write_byte(0x9A, 0x00)` within the same function.<br>3. Block all writes outside the approved whitelist. |
| **RSK-02** | **AFE DAC Saturation / Excessive Drive Current** | MEDIUM | LOW | Applying out-of-range DAC values during binary search could cause analog frontend saturation or excessive current draw. | 1. Hard-clamp all DAC candidate values to range `[0, 127]`.<br>2. Enforce binary search bounds `[20, 100]`.<br>3. Default to reference value `54`.<br>4. Abort calibration if ADC medians do not converge within 10 iterations. |
| **RSK-03** | **SPI Burst Buffer Overflow / Truncation (10,246 Bytes)** | HIGH | MEDIUM | Image FIFO transfer requires 10,246 continuous bytes. If host buffer capacity or `spidev.bufsiz` is exceeded, payload is truncated, causing kernel faults or corrupted frames. | 1. Inspect `/sys/module/spidev/parameters/bufsiz` at startup.<br>2. Allocate static DMA-aligned transfer buffers of >= 16,384 bytes.<br>3. Ensure `spi_ioc_transfer.len` exactly matches `10246` bytes.<br>4. Fail immediately if buffer capacity < 10,246 bytes. |
| **RSK-04** | **Electrical Contention on Reset Pad 14 (`GPP_B14`)** | HIGH | LOW | Incorrect GPIO direction or polarity inversion could drive Pad 14 high against an external open-drain or ground contention. | 1. Verify ACPI declaration (`IoRestrictionOutput`, `PullDefault`).<br>2. Enforce `GPIOD_OUT_LOW` active-low representation (logical 0 = physical High).<br>3. Never configure Pad 14 as input while sensor is active.<br>4. Rely on existing baseline (`0x84000201`) without toggling unless recovery is explicitly commanded. |
| **RSK-05** | **Interrupt Storm on PCH Pad 229 (`CPU_GP_0`)** | MEDIUM | MEDIUM | Failing to acknowledge status in register `0x1A84` leaves the sensor interrupt line asserted low, generating continuous edge/level interrupts and pegging a CPU core. | 1. Configure kernel IRQ handler as `IRQF_ONESHOT`.<br>2. Always write back the exact read status word to `0x1A84` to clear the sensor interrupt latch.<br>3. If interrupt cannot be cleared after 3 attempts, disable the IRQ line and log a recovery event. |
| **RSK-06** | **Un-gated Clock Thermal / Battery Drain during System Sleep** | MEDIUM | HIGH | Register `0xC6 = 0x01` un-gates internal sensor clocks. If left active across system suspend (S2idle/S3), the sensor continues drawing standby power. | 1. In device suspend hook, issue sensor `idle()` command sequence (`5A A5`, `C0 3F`, `A5 5A`).<br>2. Optionally deassert sensor power via ACPI `_PS3` or reset line assertion.<br>3. Re-initialize on resume via `_PS0` and Phase 1 wakeup. |
| **RSK-07** | **Host LPSS SPI Controller Lockup / Bus Desynchronization** | MEDIUM | LOW | If an SPI transaction is aborted mid-byte (e.g. signal interruption), the LPSS SPI FIFO and sensor SPI state machine lose byte alignment. | 1. Enforce atomic SPI message transfers (`SPI_IOC_MESSAGE(1)`).<br>2. Mask software signals during critical transfers.<br>3. In recovery handler, pulse CS high and send 32 dummy clocks (`idle()` sequence) to restore byte framing. |
| **RSK-08** | **Stale or Contaminated Image Baseline Acquisition** | LOW | MEDIUM | If the initial baseline frame is captured while a finger or foreign object rests on the sensor, all subsequent minutiae subtractions fail. | 1. Check baseline spread and variance before accepting: verify median is within `512 ± 50`.<br>2. Reject baseline if raw pixel standard deviation exceeds 150.<br>3. Request user to clean sensor surface if baseline validation fails. |

---

## 3. Specific Subsystem Safety Mitigations

### 3.1 Protected Register Safety (`0x9A` Unlock Gate)
The register `0x9A` acts as a hardware write-protect interlock. In `fw9369_startup.c:395` and `fw9369_fdt.c:205`:
```c
write_byte(op, 0x9A, 0x5A);  // Unlock
// Write only to approved registers (0x00C0-0x00C2 or 0x80B0 threshold bank)
write_byte(op, 0x9A, 0x00);  // Immediate Lock
```
**Safety Invariant:** Under no circumstances shall userspace or kernel code write to any register address other than the documented threshold bank while `0x9A` equals `0x5A`.

### 3.2 AFE Parameter Bounds
All analog front-end tuning values must be strictly validated before transmission to the sensor:
```c
static inline int validate_afe_params(uint8_t dac, uint8_t shift, uint16_t period) {
    if (dac > 127) return -EINVAL;       // 7-bit DAC maximum
    if (shift > 4) return -EINVAL;       // Shift limit
    if (period < 50 || period > 500) return -EINVAL; // Operational clock limits
    return 0;
}
```

### 3.3 Hardware Rollback and Safe State Invariant
Whenever any step in the initialization sequence encounters a timeout or transaction error:
1. Release Chip Select high immediately.
2. Send the Idle sequence (`0x5A 0xA5` / `0xC0 0x3F` / `0xA5 0x5A`).
3. Clear pending interrupt flags by writing `0xFFFF` to `0x1A84`.
4. Release the host character device and return a typed error.
