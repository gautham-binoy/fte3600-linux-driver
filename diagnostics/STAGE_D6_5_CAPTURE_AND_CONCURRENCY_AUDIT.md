# STAGE D6.5: CAPTURE GATING AND CONCURRENCY AUDIT

**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Workspace:** `/home/titan/fte3600-linux-driver/`  
**Safety Status:** STRICTLY READ-ONLY SOURCE AUDIT. Zero hardware communication.  

---

## 1. Executive Summary

Fingerprint capture is the highest-risk operational phase of biometric silicon. Calling capture on an uncalibrated matrix can produce saturated ADC readouts, incorrect bias currents, or electrical stress on the analog sensing array.

This audit evaluates the locking discipline, state gating, concurrency guards, and failure modes of `FTE3600_IOC_CAPTURE` in `kernel/fte3600.c` (lines 747–791).

### Key Audit Findings
1. **Serialization Discipline:** `[SOURCE-OBSERVED]` The capture path is fully protected by `data->lock` (`struct mutex`). All state evaluations, offline checks, and resource dereferences occur under this lock.
2. **State Atomicity:** `[SOURCE-OBSERVED]` Driver internal state cannot mutate between validation and execution because all mutators (`RESET`, `INIT`, `remove`) serialize on the same `data->lock`.
3. **Capture Gating Invariant:** `[SOURCE-OBSERVED]` Capture is impossible from `IDENTIFIED` (8), `INITIALIZED_UNCALIBRATED` (14), `UNKNOWN` (7), `UNINITIALIZED` (0), and `OFFLINE` (13). Any state other than `FTE3600_STATE_READY` is rejected with `-EPERM`.
4. **Current Implementation is a Stub:** `[SOURCE-OBSERVED]` The capture handler does not implement physical SPI FIFO streaming. When `READY` is asserted, it returns `-EOPNOTSUPP`.
5. **Clean Lock Release:** `[SOURCE-OBSERVED]` Every exit path (`-ERESTARTSYS`, `-ENODEV`, `-EPERM`, `-EOPNOTSUPP`) strictly releases `data->lock`.

---

## 2. Source-Level Path Inspection

The complete implementation of `FTE3600_IOC_CAPTURE` in `kernel/fte3600.c:747-791` is audited below:

```c
747:	case FTE3600_IOC_CAPTURE: {
748:		struct fte3600_frame_buffer fb;
749:		int i;
750:
751:		if (copy_from_user(&fb, (void __user *)arg, sizeof(fb)))
752:			return -EFAULT;
753:
754:		if (fb.version != FTE3600_UAPI_VERSION)
755:			return -EINVAL;
756:
757:		if (fb.length != FTE3600_IMAGE_BYTES)
758:			return -EINVAL;
759:
760:		for (i = 0; i < 4; i++) {
761:			if (fb.reserved[i] != 0)
762:				return -EINVAL;
763:		}
764:
765:		if (mutex_lock_interruptible(&data->lock))
766:			return -ERESTARTSYS;
767:
768:		/* Re-check offline status and SPI bus pointer under data->lock */
769:		if (atomic_read(&data->offline) || !data->spi) {
770:			mutex_unlock(&data->lock);
771:			return -ENODEV;
772:		}
773:
774:		/*
775:		 * Capture-Gating Enforcement:
776:		 * Capture is strictly prohibited unless the device has completed all
777:		 * required initialization, analog baseline calibration, and touch arming stages.
778:		 * In the current driver milestone, volatile startup prefix has completed,
779:		 * but calibration (fdt_baseline, image_baseline) and touch arming (arm)
780:		 * are deferred to future authorized stages.
781:		 * Therefore, capture is rejected in all non-READY states with -EPERM.
782:		 */
783:		if (data->state != FTE3600_STATE_READY) {
784:			mutex_unlock(&data->lock);
785:			return -EPERM;
786:		}
787:
788:		/* Physical FIFO capture deferred in Stage 2E.2 software milestone */
789:		mutex_unlock(&data->lock);
790:		return -EOPNOTSUPP;
791:	}
```

---

## 3. Concurrency and Race Condition Audit

### 3.1 Device Removal Racing with Capture
A critical failure mode in character device drivers is a race between userspace invoking ioctl on an open file descriptor and kernel driver core unbinding the SPI hardware device (`fte3600_remove()`).

#### Timing Scenario A: Capture acquires lock before `remove()`
1. Thread A (`ioctl(CAPTURE)`) enters `FTE3600_IOC_CAPTURE` and acquires `data->lock` (line 765).
2. Thread B (Kernel SPI bus worker) invokes `fte3600_remove(spi)`.
3. In `fte3600_remove()`, line 952 executes `mutex_lock(&data->lock)`: Thread B blocks, waiting for Thread A to complete.
4. Thread A evaluates `data->state` (state 14 != 4), unlocks `data->lock`, and returns `-EPERM`.
5. Thread B unblocks, acquires `data->lock`, asserts `offline = 1`, assigns `data->state = FTE3600_STATE_DEVICE_REMOVAL`, nullifies `data->spi` and `data->reset_gpiod`, and releases `data->lock`.
6. Thread A closes file descriptor: `cdev_release` calls `put_device(&data->dev)`. The object is freed cleanly only after all references drop.
- **Audit Verdict:** **SAFE**. No invalid dereferences; memory lifecycle preserved. `[SOURCE-OBSERVED]`

#### Timing Scenario B: `remove()` runs while capture waits for lock
1. Thread B (`fte3600_remove()`) acquires `data->lock` (line 952), asserts `offline = 1`, nullifies `data->spi = NULL`, and releases `data->lock`.
2. Thread A (`ioctl(CAPTURE)`), which was waiting at line 765, acquires `data->lock`.
3. Thread A executes line 769:
   ```c
   if (atomic_read(&data->offline) || !data->spi) {
       mutex_unlock(&data->lock);
       return -ENODEV;
   }
   ```
4. Thread A immediately unlocks `data->lock` and returns `-ENODEV`.
5. `data->spi` is **never dereferenced**.
- **Audit Verdict:** **SAFE**. Zero NULL pointer dereferences; zero invalid hardware operations. `[SOURCE-OBSERVED]`

---

## 4. Gating Across All Driver States

The table below traces the exact evaluation of the capture gate across every internal state:

| Internal Driver State | Numeric Value | State Gating Check (`state == READY`) | Resulting Action | Returned Error Code | Evidence Tag |
|---|---|---|---|---|---|
| `FTE3600_STATE_UNINITIALIZED` | 0 | `0 == 4` -> False | Reject | `-EPERM` | `[SOURCE-OBSERVED]` |
| `FTE3600_STATE_RESETTING` | 1 | `1 == 4` -> False | Reject | `-EPERM` | `[SOURCE-OBSERVED]` |
| `FTE3600_STATE_RESET_SEQUENCE_ISSUED` | 2 | `2 == 4` -> False | Reject | `-EPERM` | `[SOURCE-OBSERVED]` |
| `FTE3600_STATE_INITIALIZING` | 3 | `3 == 4` -> False | Reject | `-EPERM` | `[SOURCE-OBSERVED]` |
| `FTE3600_STATE_READY` | 4 | `4 == 4` -> True | Admit to stub | `-EOPNOTSUPP` | `[SOURCE-OBSERVED]` |
| `FTE3600_STATE_CAPTURING` | 5 | `5 == 4` -> False | Reject | `-EPERM` | `[SOURCE-OBSERVED]` |
| `FTE3600_STATE_ERROR` | 6 | `6 == 4` -> False | Reject | `-EPERM` | `[SOURCE-OBSERVED]` |
| `FTE3600_STATE_UNKNOWN` | 7 | `7 == 4` -> False | Reject | `-EPERM` | `[SOURCE-OBSERVED]` |
| `FTE3600_STATE_IDENTIFIED` | 8 | `8 == 4` -> False | Reject | `-EPERM` | `[SOURCE-OBSERVED]` |
| `FTE3600_STATE_WAITING_FOR_TOUCH` | 9 | `9 == 4` -> False | Reject | `-EPERM` | `[SOURCE-OBSERVED]` |
| `FTE3600_STATE_TOUCH_DETECTED` | 10 | `10 == 4` -> False | Reject | `-EPERM` | `[SOURCE-OBSERVED]` |
| `FTE3600_STATE_FRAME_READY` | 11 | `11 == 4` -> False | Reject | `-EPERM` | `[SOURCE-OBSERVED]` |
| `FTE3600_STATE_SUSPENDED` | 12 | `12 == 4` -> False | Reject | `-EPERM` | `[SOURCE-OBSERVED]` |
| `FTE3600_STATE_DEVICE_REMOVAL` | 13 | Checked at line 769 | Offline check | `-ENODEV` | `[SOURCE-OBSERVED]` |
| `FTE3600_STATE_INITIALIZED_UNCALIBRATED` | 14 | `14 == 4` -> False | Reject | `-EPERM` | `[SOURCE-OBSERVED]` |

---

## 5. Mock Scope vs. Kernel Silicon Safety

**CRITICAL METHODOLOGICAL DISTINCTION:**
- The unit tests in `kernel/fte3600-test.c` (Test Suite 9 Scenarios 2 through 7) simulate the capture path by exercising `sim_driver_ioctl_capture()`.
- **What is proven (`[TEST-OBSERVED]`):** The userspace simulation confirms that the conditional logic, mutex locking patterns, and errno mappings correctly reject capture in all simulated non-READY states and admit capture only when state is explicitly forced to `READY`.
- **What is NOT proven (`[MOCK-ONLY]`):** The simulation does not prove that physical SPI FIFO burst transfers work, that DMA buffers do not overflow under hardware line speeds, or that physical silicon does not assert unexpected interrupt signals.
- **Current Operational Reality (`[SOURCE-OBSERVED]`):** Physical FIFO capture is deferred. Because `READY` is never assigned in the kernel driver, physical capture is completely blocked at the software level.
