# Stage D7.3 — Failure, Recovery & Abort Matrix

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source Architecture Review (Zero Hardware Operations)  
**Deliverable:** `diagnostics/STAGE_D7_3_FAILURE_AND_RECOVERY_MATRIX.md`  

---

## 1. Executive Summary

This report establishes the comprehensive Failure, Recovery, and Abort Matrix for the proposed hybrid calibration architecture.

It enforces a strict **fail-closed security and stability policy**:
1. Any intermediate hardware or communication failure during calibration immediately aborts the active session and transitions the internal driver state to `FTE3600_STATE_UNKNOWN` (7).
2. Because the vendor protocol features zero hardware rollback, software aborts **never assume** the physical silicon state has returned to a baseline.
3. Verification of `kernel/fte3600.c:734–742` confirms that from `FTE3600_STATE_UNKNOWN`, `FTE3600_IOC_INIT` is rejected with `-EPERM`. The only permissible recovery vector is an active-low hardware reset pulse (`FTE3600_IOC_RESET`).

---

## 2. Comprehensive Failure & Recovery Matrix

| Failure Mode | Point of Occurrence | Returned Errno | Internal State Post-Failure | Session Active? | ABORT Permitted? | Hardware RESET Required? | Re-Calibration Permitted? | READY Possible? |
|---|---|---|---|---|---|---|---|---|
| **SPI Transfer Error** | Any step (`spi_sync` fails) | `-EIO` | `UNKNOWN` (7) | False | No (auto-aborted)| **YES (Mandatory)** | Only after RESET + INIT | **NO (Gated)** |
| **Unexpected Sensor Status** | `0x80 != 0x54` on image poll | `-ETIMEDOUT` / `-EIO` | `UNKNOWN` (7) | False | No (auto-aborted)| **YES (Mandatory)** | Only after RESET + INIT | **NO (Gated)** |
| **ADC Scan Timeout** | `0x1A82` bit 3 fails to assert | `-ETIMEDOUT` | `UNKNOWN` (7) | False | No (auto-aborted)| **YES (Mandatory)** | Only after RESET + INIT | **NO (Gated)** |
| **DAC Search Exhaustion** | FDT or Image DAC hits limit (1 or 125) | `-ERANGE` | `UNKNOWN` (7) | False | No (auto-aborted)| **YES (Mandatory)** | Only after RESET + INIT | **NO (Gated)** |
| **Out-of-Range Minima** | $minima[c] \notin [300, 700]$ | `-EDOM` | `UNKNOWN` (7) | False | No (auto-aborted)| **YES (Mandatory)** | Only after RESET + INIT | **NO (Gated)** |
| **Invalid Threshold Bounds** | Threshold $<270$ or $>670$ | `-EINVAL` | `UNKNOWN` (7) | False | No (auto-aborted)| **YES (Mandatory)** | Only after RESET + INIT | **NO (Gated)** |
| **FIFO Read Error** | Image burst read truncated/corrupt | `-EIO` | `UNKNOWN` (7) | False | No (auto-aborted)| **YES (Mandatory)** | Only after RESET + INIT | **NO (Gated)** |
| **Incomplete Dark Frame** | Helper fails to cache frame 2 | `-EBADSLT` | `UNKNOWN` (7) | False | No (auto-aborted)| **YES (Mandatory)** | Only after RESET + INIT | **NO (Gated)** |
| **Threshold Bank Write Error**| SPI write to `0x80B0` fails | `-EIO` | `UNKNOWN` (7) | False | No (auto-aborted)| **YES (Mandatory)** | Only after RESET + INIT | **NO (Gated)** |
| **Touch Arm Strobe Error** | Strobe `0xC2 0x3D` fails | `-EIO` | `UNKNOWN` (7) | False | No (auto-aborted)| **YES (Mandatory)** | Only after RESET + INIT | **NO (Gated)** |
| **Incomplete Phase Commit** | Calling `COMMIT` before all steps | `-EPERM` | `UNKNOWN` (7) | False | No (auto-aborted)| **YES (Mandatory)** | Only after RESET + INIT | **NO (Gated)** |
| **Userspace Helper Crash** | Process dies (`SIGKILL`, segfault) | N/A (VFS close) | `UNKNOWN` (7) | False | Closed by kernel | **YES (Mandatory)** | Only after RESET + INIT | **NO (Gated)** |
| **Session Inactivity Timeout** | No step for $>5.0\text{ s}$ | `-ETIMEDOUT` | `UNKNOWN` (7) | False | No (auto-aborted)| **YES (Mandatory)** | Only after RESET + INIT | **NO (Gated)** |
| **Invalid Session Token** | Stale / mismatched `session_id` | `-EACCES` | Unchanged | Retained | Yes | No (if session valid) | Yes (with valid token) | No |
| **Concurrent IOCTL Attempt**| Another thread calls `INIT`/`RESET` | `-EBUSY` | Unchanged | Retained | Yes | No | Unaffected | No |
| **Device Removal / Unbind** | Module unloaded or sysfs unbind | `-ENODEV` | `DEVICE_REMOVAL` (13)| False | No (driver offline) | N/A (Unbound) | No (Device gone) | **NO (Offline)** |

---

## 3. Verification of Hardware Reset Transition Rules

A critical requirement of this design is verifying that the kernel driver actually enforces hardware reset recovery from state `UNKNOWN` (7).

### 3.1 Driver Source Verification (`kernel/fte3600.c:725–742`)
In `fte3600_ioctl_common()`:
```c
if (data->state == FTE3600_STATE_RESET_SEQUENCE_ISSUED) {
    ret = fte3600_hw_identify(data);
    if (ret == 0 && (params.flags & FTE3600_INIT_FLAG_FULL_STARTUP))
        ret = fte3600_hw_startup(data);
} else if (data->state == FTE3600_STATE_IDENTIFIED) {
    ret = fte3600_hw_startup(data);
} else {
    ret = -EPERM; // Lines 740-741: Rejects all other states!
}
```
### 3.2 Proof of Enforcement:
1. When `data->state == FTE3600_STATE_UNKNOWN` (7), any call to `FTE3600_IOC_INIT` lands in the `else` branch and is **strictly rejected with `-EPERM`**.
2. Calling `FTE3600_IOC_CAPTURE` in `UNKNOWN` is rejected with `-EPERM` (`lines 786–789`).
3. Calling calibration ioctls in `UNKNOWN` is rejected with `-ENODEV` or `-EPERM` (session requires `INITIALIZED_UNCALIBRATED`).
4. In `FTE3600_IOC_RESET` (`lines 675–695`), reset pulsing is **unconditionally permitted** from any state as long as `reset_gpiod` is valid.
5. `fte3600_hw_reset_pulse()` drives Pad 14 low for 10 ms, restoring hardware flip-flops, and transitions `data->state` to `FTE3600_STATE_RESET_SEQUENCE_ISSUED` (2).
6. From state 2, `FTE3600_IOC_INIT` is once again permitted.
7. **Conclusion:** The source code rigorously guarantees that a device entering `UNKNOWN` can **never resume operation without a full physical hardware reset pulse**.

---

## 4. Why Software Abort Cannot Restore Silicon State

In reference code (`fw9369_startup.c`, `fw9369_release.c`), the vendor implements zero rollback logic:
- An aborted FDT calibration leaves bias DAC register `0x1801` modified.
- An aborted image calibration leaves exposure register `0x1800` set to mode `0xA3` and FIFO clocking enabled (`0x1800 |= 1`).
- Setting threshold SRAM bank (`0x80B0` under `0x9A=0x5A`) cannot be undone without known good baseline values.
- Software commands alone cannot reliably restore analog comparator bias circuits without risking latchup or thermal drift.

Therefore, the driver architecture adopts the principle: **A failed calibration session is electrically dirty; only physical active-low reset assertion (Pad 14) can restore cold baseline silicon flip-flops.**
