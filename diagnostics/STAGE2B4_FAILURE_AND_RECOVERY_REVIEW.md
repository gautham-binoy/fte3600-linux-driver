# Stage 2B.4 — Failure Path & Hardware Rollback Analysis

**Document ID:** `STAGE2B4_FAILURE_AND_RECOVERY_REVIEW`  
**Date:** 2026-11-02  
**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FT9362 / FW9369  
**Status:** COMPLETE — STATIC FAILURE PATH AUDIT  
**Hardware Execution Authorization:** NONE (Static Audit Only)

---

## 1. Executive Summary

This investigation analyzes all error conditions, interruption scenarios, failure modes, and recovery limitations associated with the proposed Sub-Stage 2C.1B operation.

The audit establishes that:
1. **Protected Register Bank `0x9A` Immunity:** Because Sub-Stage 2C.1B does not access register `0x9A` at any point, the well-documented reference driver vulnerability (leaving `0x9A` unlocked on error) is completely avoided. Register `0x9A` remains locked (`0x00`) at all times.
2. **Immediate Abort Rule:** If status register `0x80` fails to report `0x50` after opcode `0x5A`, the reference driver attempts recovery via `command(0xC0, 0x3F)`. For our safety harness, this recovery strobe is **prohibited**. The harness must abort immediately on any status mismatch to prevent sending unverified recovery sequences.
3. **Host Isolation vs. Silicon Restoration:** Host cleanup (unbinding `spi-FTE3600:00`, restoring `driver_override`, unloading `spidev`) guarantees complete host-side isolation and releases the SPI bus (CS high). However, host rollback cannot restore the internal volatile state of the sensor's microcontroller or un-gate register `0xC6`. Cold power cycling remains the only proven method to restore the sensor's physical silicon baseline.

---

## 2. Failure Mode & Error Path Analysis

| Failure Scenario | Detection Mechanism | Immediate Harness Action | Sensor-Internal Consequence | Reference Driver Behavior |
| :--- | :--- | :--- | :--- | :--- |
| **SPI IOCTL Transport Error** | `ioctl(fd, SPI_IOC_MESSAGE(1))` returns `< 0` or `!= len` | Log `errno`; close `fd`; abort with specific exit code | Bus clock halted; CS released high; transaction truncated | Loops up to 32 times with 10 ms delay (`fw9369_startup.c:50-60`) |
| **Wakeup Mismatch (`0xC6 != 0x01`)** | Readback buffer contains `rx[4] != 0x01` | Log observed byte; close `fd`; abort | Transceiver clock gating may be partial or un-gated | Ignores mismatch or loops up to 32 times |
| **MCU Handshake Timeout / Mismatch (`0x80 != 0x50`)** | Readback buffer contains `rx[4] != 0x50` | Log exact status code; close `fd`; abort. **DO NOT send `C0 3F`** | MCU remains in pre-idle or busy state | Issues exception strobe `command(0xC0, 0x3F)` (`fw9369_startup.c:110`) |
| **Partial Execution (Failure between `0x5A` and `0xA5`)** | Error detected during `0x80` read | Immediate abort; initiate host rollback | MCU remains in transitional idle-request state | Fallback to `C0 3F` strobe |
| **Signal Interruption (`SIGINT` / `SIGTERM`)** | Shell trap handler invoked | Deferred until critical SPI transaction completes; then execute `cleanup()` | Same as completion or partial failure | Upstream driver does not mask signals; prone to early exit |
| **Abnormal Termination (`SIGKILL` / OOM)** | Kernel process destruction | Kernel automatically closes open `fd`, deasserting CS high; trap triggers rollback | CS deasserts; sensor registers remain in last written state | Process killed; state unmanaged |

---

## 3. Deep Dive: Protected Register Bank 0x9A Status

In Stage 2B.1 and 2B.2 (`STAGE2B2_PROTECTED_REGISTER_REVIEW.md`), we uncovered severe defects in the upstream reference driver:
- In `fw9369_fdt.c:215-224`, failure of an intermediate write triggers an early `return 0` that skips locking `0x9A = 0x00`.
- In `fw9369_release.c:272`, short-circuit logical OR evaluation skips locking if the bank write fails.

### Safety Guarantee for Stage 2C.1B
- Sub-Stage 2C.1B operates strictly within Phase 1 (transceiver un-gating) and Phase 2 (MCU idle handshake).
- **Register `0x9A` is NEVER accessed** in Sub-Stage 2C.1B.
- Neither unlock key `0x5A` nor lock key `0x00` is written to address `0x9A`.
- Therefore, register `0x9A` remains in its cold-boot hardware locked default (`0x00`) across all execution and failure paths.

---

## 4. Immediate Abort vs. Upstream Recovery Strobes

In `fw9369_startup.c:108-112`, the upstream driver implements recovery:
```c
/* fw9369_startup.c:108-112 */
if (read_byte (op, 0x80) != 0x50)
  {
    command (op, 0xc0, 0x3f);
    wait_ms (op, 1);
  }
```
In `ft93xx.c:268-277`, the driver loops up to 5 times sending `command(op, 0xc0)` whenever `0x80 != 0x50`.

### Architectural Policy for 2C.1B
- Upstream recovery loops are intended for end-user operation where retrying is preferred to failure.
- In a safety-critical bring-up harness, **automatic recovery writes are PROHIBITED**.
- If register `0x80` returns any value other than `0x50` (e.g. `0x00`, `0x54`, `0x58`, `0xFF`), the harness must:
  1. Record the exact byte returned for telemetry.
  2. Refuse to transmit `[ 0xC0, 0x3F, 0x00 ]`.
  3. Close the SPI file descriptor immediately.
  4. Invoke host-side rollback.
- This prevents the harness from sending unverified abort strobes into an indeterminate silicon state.

---

## 5. Host Cleanup vs. Sensor-Internal Restoration

Host cleanup and sensor-internal state restoration must never be conflated:

### What Host-Side Rollback Guarantees
1. File descriptor closure: The SPI bus clock halts, MOSI goes idle, and Chip-Select (CS) deasserts high.
2. Device unbinding: Device `spi-FTE3600:00` is detached from the `spidev` driver.
3. Override clearance: `/sys/bus/spi/devices/spi-FTE3600:00/driver_override` is restored to `(null)`.
4. Driver removal: Module `spidev` is unloaded from kernel space.
5. Character device cleanup: All `/dev/spidev*` nodes are deleted by the kernel.
6. Baseline isolation: The host is fully decoupled from the sensor.

### What Host-Side Rollback Cannot Guarantee
1. Internal Transceiver State: Register `0xC6` remains set to `0x01` (transceiver clocks un-gated) in volatile sensor RAM. No host command exists in reference code to return `0xC6` to sleep.
2. Microcontroller State: The embedded MCU remains in whatever execution state it reached (IDLE `0x50` or boot loop).
3. Physical Silicon Reset: Only a full cold reboot or power cycle (Host Reset through S5) drops sensor power and restores true hardware baseline.
