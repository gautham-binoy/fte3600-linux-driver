# STAGE D6.4: CAPTURE GATING AND SAFETY RESTRICTION AUDIT

**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Workspace:** `/home/titan/fte3600-linux-driver/`  
**Safety Status:** STRICTLY SOURCE-ONLY. Zero physical hardware communication.  

---

## 1. Executive Summary

A critical objective of Stage D6.4 is to guarantee that the fingerprint capture path (`FTE3600_IOC_CAPTURE`) cannot be invoked prematurely, accidentally, or maliciously while the hardware is in an uncalibrated, uninitialized, offline, or fail-closed state.

This audit reviews every gate, check, lock, and condition protecting the capture path in `kernel/fte3600.c` and verifies them against the userspace simulation suite in `kernel/fte3600-test.c`.

---

## 2. Capture Path Source Implementation

The complete implementation of `FTE3600_IOC_CAPTURE` in `kernel/fte3600.c` (lines 747–791) is as follows:

```c
	case FTE3600_IOC_CAPTURE: {
		struct fte3600_frame_buffer fb;
		int i;

		if (copy_from_user(&fb, (void __user *)arg, sizeof(fb)))
			return -EFAULT;

		if (fb.version != FTE3600_UAPI_VERSION)
			return -EINVAL;

		if (fb.length != FTE3600_IMAGE_BYTES)
			return -EINVAL;

		for (i = 0; i < 4; i++) {
			if (fb.reserved[i] != 0)
				return -EINVAL;
		}

		if (mutex_lock_interruptible(&data->lock))
			return -ERESTARTSYS;

		/* Re-check offline status and SPI bus pointer under data->lock */
		if (atomic_read(&data->offline) || !data->spi) {
			mutex_unlock(&data->lock);
			return -ENODEV;
		}

		/*
		 * Capture-Gating Enforcement:
		 * Capture is strictly prohibited unless the device has completed all
		 * required initialization, analog baseline calibration, and touch arming stages.
		 * In the current driver milestone, volatile startup prefix has completed,
		 * but calibration (fdt_baseline, image_baseline) and touch arming (arm)
		 * are deferred to future authorized stages.
		 * Therefore, capture is rejected in all non-READY states with -EPERM.
		 */
		if (data->state != FTE3600_STATE_READY) {
			mutex_unlock(&data->lock);
			return -EPERM;
		}

		/* Physical FIFO capture deferred in Stage 2E.2 software milestone */
		mutex_unlock(&data->lock);
		return -EOPNOTSUPP;
	}
```

---

## 3. Systematic Path and Condition Analysis

### 3.1 Fast-Path Offline Rejection
At line 648 of `kernel/fte3600.c`:
```c
	if (atomic_read(&data->offline) && cmd != FTE3600_IOC_GET_STATE)
		return -ENODEV;
```
If the device has been unbound (`fte3600_remove()`) or shutdown, `atomic_read(&data->offline)` is 1. The ioctl handler aborts immediately and returns `-ENODEV` before touching the mutex, allocating kernel memory, or issuing bus operations.

### 3.2 Userspace Parameter Validation
Lines 751–763 enforce strict structural contracts:
- `copy_from_user`: Returns `-EFAULT` if memory address is invalid.
- `fb.version == FTE3600_UAPI_VERSION` (1): Prevents version skew; returns `-EINVAL`.
- `fb.length == FTE3600_IMAGE_BYTES` (10240): Prevents buffer underflow/overflow; returns `-EINVAL`.
- `fb.reserved[0..3] == 0`: Prevents uninitialized baggage or ABI abuse; returns `-EINVAL`.

### 3.3 Concurrency and Resource Invalidation Protection
Lines 765–772 acquire `data->lock` interruptibly and re-verify resource validity:
- If a signal interrupts the caller while waiting for `data->lock`, returns `-ERESTARTSYS`.
- If `fte3600_remove()` ran while the caller was waiting for `data->lock`, `atomic_read(&data->offline)` is 1 and `data->spi` is `NULL`. The check cleanly releases the lock and returns `-ENODEV`, preventing null pointer dereference on devres resources.

### 3.4 State Machine Capture-Gating
Lines 783–786 evaluate the gating predicate:
```c
	if (data->state != FTE3600_STATE_READY) {
		mutex_unlock(&data->lock);
		return -EPERM;
	}
```

The behavior across all driver states is summarized below:

| Driver Internal State | Numeric | Description | Capture Gating Decision | Returned Errno |
|---|---|---|---|---|
| `FTE3600_STATE_UNINITIALIZED` | 0 | Probe default | Rejected | `-EPERM` |
| `FTE3600_STATE_RESETTING` | 1 | Reset pulse active | Rejected | `-EPERM` |
| `FTE3600_STATE_RESET_SEQUENCE_ISSUED` | 2 | Reset pulse complete | Rejected | `-EPERM` |
| `FTE3600_STATE_INITIALIZING` | 3 | Identify/startup in flight | Rejected | `-EPERM` |
| `FTE3600_STATE_CAPTURING` | 5 | Concurrent capture attempt | Rejected | `-EPERM` |
| `FTE3600_STATE_ERROR` | 6 | Hardware error latch | Rejected | `-EPERM` |
| `FTE3600_STATE_UNKNOWN` | 7 | Bus timeout / fail-closed | Rejected | `-EPERM` |
| `FTE3600_STATE_IDENTIFIED` | 8 | Chip ID verified only | Rejected | `-EPERM` |
| `FTE3600_STATE_WAITING_FOR_TOUCH` | 9 | Touch IRQ wait (future) | Rejected | `-EPERM` |
| `FTE3600_STATE_TOUCH_DETECTED` | 10 | Touch detected (future) | Rejected | `-EPERM` |
| `FTE3600_STATE_FRAME_READY` | 11 | Frame in FIFO (future) | Rejected | `-EPERM` |
| `FTE3600_STATE_SUSPENDED` | 12 | Power management suspended | Rejected | `-EPERM` |
| `FTE3600_STATE_DEVICE_REMOVAL` | 13 | Unbound / offline | Rejected (Offline check) | `-ENODEV` |
| `FTE3600_STATE_INITIALIZED_UNCALIBRATED` | 14 | Startup prefix completed | **Rejected** | **`-EPERM`** |
| `FTE3600_STATE_READY` | 4 | Fully calibrated & armed | **Admitted to capture path** | `-EOPNOTSUPP` (Milestone stub) |

---

## 4. Reassessment of Interrupt and NVM Storage Claims

### 4.1 Interrupt Clear Register `0x1A84`
- **Source Observation:** In the vendor reference driver (`fw9369_startup.c:368`), the sequence writes `0xFFFF` to 16-bit register `0x1A84` twice (`05 FA 9A 84 00 01 FF FF`).
- **Audit Finding:** The FT9362 hardware specification is proprietary and unpublished. The exact bitfield layout, latching semantics, clear-on-read vs. write-1-to-clear behaviors of register `0x1A84` remain **NOT ESTABLISHED**.
- **Architectural Policy:** In Stage D6.2, D6.3, and D6.4, register writes to `0x1A84` are strictly excluded from the baseline initialization path. Comment blocks in `kernel/fte3600.c` have been updated to explicitly record that silicon semantics are unverified, and no speculative replacement writes are introduced.

### 4.2 OTP / eFuse Storage
- **Source Observation:** Vendor code references sense-amp calibration registers and OTP read facilities (`fw9369_fdt.c`).
- **Audit Finding:** OTP read facilities are source-observed, but persistent non-volatile OTP/eFuse programming capabilities, timing tolerances, and high-voltage burn requirements are **NOT ESTABLISHED**.
- **Architectural Policy:** No OTP or eFuse programming operations exist or are permitted in the kernel driver. The driver maintains strictly volatile operation, ensuring zero risk of irreversible silicon modification.

---

## 5. Verification Matrix

The gating logic was verified across Test Suite 9 scenarios in `kernel/fte3600-test.c`:
1. `test_d6_4_prefix_completion_withholds_ready`: Confirms prefix completion yields `INITIALIZED_UNCALIBRATED`, withholding `READY`.
2. `test_d6_4_capture_rejected_in_identified`: Confirms `IDENTIFIED` rejects capture with `-EPERM`.
3. `test_d6_4_capture_rejected_in_uncalibrated`: Confirms `INITIALIZED_UNCALIBRATED` rejects capture with `-EPERM`.
4. `test_d6_4_capture_rejected_in_unknown`: Confirms `UNKNOWN` rejects capture with `-EPERM`.
5. `test_d6_4_capture_rejected_when_offline`: Confirms offline state rejects capture with `-ENODEV`.
6. `test_d6_4_capture_rejected_after_failed_init`: Confirms failed initialization leaves state `UNKNOWN` and rejects capture with `-EPERM`.
7. `test_d6_4_capture_admitted_only_in_ready`: Confirms only `STATE_READY` admits capture to the FIFO handler.

**Audit Conclusion:** The capture path is completely and strictly gated against all premature execution paths.
