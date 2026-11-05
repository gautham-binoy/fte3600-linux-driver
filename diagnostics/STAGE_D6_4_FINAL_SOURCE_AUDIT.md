# STAGE D6.4: FINAL SOURCE AND SAFETY AUDIT REPORT

**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Workspace:** `/home/titan/fte3600-linux-driver/`  
**Safety Status:** STRICTLY SOURCE-ONLY. Zero physical hardware communication.  

---

## 1. Executive Summary

Stage D6.4 was conducted as a source-only engineering stage to correct initialization state semantics and implement strict capture gating in the FTE3600 / FT9362 Linux kernel driver.

The audit verified:
1. Volatile startup prefix completion no longer asserts `FTE3600_STATE_READY` prematurely; it transitions truthfully to internal state `FTE3600_STATE_INITIALIZED_UNCALIBRATED` (14).
2. Public UAPI reports `FTE3600_STATE_IDENTIFIED` (8) with status flags `FTE3600_FLAG_STARTUP_PREFIX_COMPLETED` and `FTE3600_FLAG_CALIBRATION_PENDING`, strictly isolating internal states from userspace ABI.
3. Fingerprint capture (`FTE3600_IOC_CAPTURE`) is unconditionally locked and gated: any state other than `FTE3600_STATE_READY` is rejected with `-EPERM` (or `-ENODEV` if offline).
4. Code comments regarding interrupt latch register `0x1A84` and OTP/eFuse storage accurately reflect that silicon semantics and non-volatile burn capabilities remain unverified.
5. All 60 unit tests in `kernel/fte3600-test.c` and 9 policy groups pass cleanly.
6. The live kernel module and physical device invariants remain strictly untouched under Option 1 (`fte3600` loaded and bound, transfers=3, bytes=19).

---

## 2. Source Code Modifications and Line Citations

### 2.1 Header: `kernel/fte3600.h`
- **Lines 40–53:** Extended `enum fte3600_state` with internal states `9..14`:
  - `FTE3600_STATE_WAITING_FOR_TOUCH = 9`
  - `FTE3600_STATE_TOUCH_DETECTED = 10`
  - `FTE3600_STATE_FRAME_READY = 11`
  - `FTE3600_STATE_SUSPENDED = 12`
  - `FTE3600_STATE_DEVICE_REMOVAL = 13`
  - `FTE3600_STATE_INITIALIZED_UNCALIBRATED = 14`
  - `#define FTE3600_STATE_PREFIX_COMPLETE FTE3600_STATE_INITIALIZED_UNCALIBRATED`
  - `#define FTE3600_MAX_PUBLIC_STATE 8`
- **Lines 55–102:** Implemented inline helper `fte3600_state_to_uapi()`:
  - Maps `INITIALIZED_UNCALIBRATED` (14) -> `FTE3600_STATE_IDENTIFIED` (8).
  - Maps `DEVICE_REMOVAL` (13) -> `FTE3600_STATE_UNINITIALIZED` (0).
  - Preserves states `0..8` identically.
  - Bounded return range `[0..8]`.
- **Lines 105–112:** Defined status flags:
  - `FTE3600_FLAG_STARTUP_PREFIX_COMPLETED (1U << 2)`
  - `FTE3600_FLAG_CALIBRATION_PENDING (1U << 3)`
- **Lines 129–132:** Defined initialization control flags:
  - `FTE3600_INIT_FLAG_IDENTIFY_ONLY 0x00000000U`
  - `FTE3600_INIT_FLAG_FULL_STARTUP (1U << 0)`
- **Lines 165–171:** Static assertions verifying exact 32-byte layout of `fte3600_state_snapshot`, `fte3600_init_params`, and `fte3600_frame_buffer`.

### 2.2 Kernel Driver Implementation: `kernel/fte3600.c`
- **Lines 44–46:** Added `foundry_code` and `line_period_us` fields to `struct fte3600_data`.
- **Lines 450–600 (`fte3600_hw_startup()`):**
  - Updated docstring recording that `0x1A84` silicon semantics and OTP non-volatile programming remain unverified.
  - Success transition: `data->state = FTE3600_STATE_INITIALIZED_UNCALIBRATED;` (withholding READY).
  - Error path transition: `data->state = FTE3600_STATE_UNKNOWN;`.
- **Lines 652–674 (`FTE3600_IOC_GET_STATE`):**
  - Calls `fte3600_state_to_uapi(data->state)` to populate `snap.state`.
  - Sets `FTE3600_FLAG_STARTUP_PREFIX_COMPLETED | FTE3600_FLAG_CALIBRATION_PENDING` when in `INITIALIZED_UNCALIBRATED`.
  - Sets `FTE3600_FLAG_OFFLINE` when offline or in `DEVICE_REMOVAL`.
- **Lines 720–745 (`FTE3600_IOC_INIT`):**
  - Case 1: `RESET_SEQUENCE_ISSUED`: runs identify. If `FTE3600_INIT_FLAG_FULL_STARTUP` set, proceeds through startup prefix to `INITIALIZED_UNCALIBRATED`.
  - Case 2: `IDENTIFIED`: proceeds through startup prefix to `INITIALIZED_UNCALIBRATED`.
  - Any other state (including `INITIALIZED_UNCALIBRATED`, `READY`, `UNKNOWN`, `UNINITIALIZED`): rejects with `-EPERM`.
- **Lines 747–791 (`FTE3600_IOC_CAPTURE`):**
  - Fast-path and mutex-held offline check returns `-ENODEV`.
  - Precondition gating: `if (data->state != FTE3600_STATE_READY) return -EPERM;`.
  - Milestone stub: returns `-EOPNOTSUPP` if `STATE_READY`.

### 2.3 Standalone Test Suite: `kernel/fte3600-test.c`
- **Lines 400–450:** Updated `sim_driver_ioctl_get_state()` and implemented `sim_driver_ioctl_capture()`.
- **Lines 1547–1566:** Updated Scenario 13 of Suite 6 to verify repeated INIT transitions to `INITIALIZED_UNCALIBRATED` and rejects third INIT with `-EPERM`.
- **Lines 1964–2040:** Updated Suite 8 Scenarios 1 and 2 assertions and descriptions to `INITIALIZED_UNCALIBRATED`.
- **Lines 2288–2580:** Implemented Suite 9 (10 comprehensive test scenarios covering state semantics, capture rejection across all states, legacy zero flags, UAPI constants, and D5.4 lifetime).
- **Lines 2640–2670:** Updated runner in `main()` reporting 60 / 60 tests passed (100.0%).

---

## 3. Physical Hardware and Live Invariant Audit

Passive inspection of the host system confirms strict adherence to Option 1:

```bash
$ lsmod | grep fte3600
fte3600                20480  0

$ cat /sys/bus/spi/devices/spi-FTE3600:00/statistics/transfers
3

$ cat /sys/bus/spi/devices/spi-FTE3600:00/statistics/bytes
19

$ ls -l /dev/fte3600
crw------- 1 root root 511, 0 Nov  4 09:32 /dev/fte3600
```

1. The currently loaded `fte3600.ko` module remains untouched and bound to `spi-FTE3600:00`.
2. SPI hardware statistics remain exactly at `transfers = 3` and `bytes = 19`, identical to the post-Stage C state.
3. No newly compiled module has been installed or loaded into the live kernel.
4. No ioctls or SPI transfers were executed against the physical fingerprint sensor.

---

## 4. Final Classification

```
PASS — state semantics corrected and capture safely gated
```

All requirements of Stage D6.4 have been fully satisfied. State semantics are completely truthful, capture is strictly gated against premature invocation, UAPI compatibility is 100% preserved, and physical hardware invariants remain untouched.
