# Stage D6.6 — Final Source, ABI, and Concurrency Audit Report

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Environment:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Safety Gate:** Active and Untouched (`Option 1` strictly enforced)  
**Stage Classification:** `PASS — all identified source and userspace semantic issues corrected`  

---

## 1. Executive Summary

Stage D6.6 was executed as a source-only and documentation-only remediation task addressing:
1. Strict `FTE3600_IOC_INIT` flag validation (`FTE3600_INIT_FLAGS_MASK`).
2. CAPTURE errno reconciliation across the kernel driver, user tools, tests, and documentation (`-ENODEV` -> `-EPERM` -> `-EOPNOTSUPP`).
3. OFFLINE UAPI state mapping clarification, client contract rules, and non-revival guarantees.
4. Header cleanup eliminating duplicate macro definitions.
5. Absolute hardware boundary preservation under Option 1.

---

## 2. Source-Level Code Changes & Diff Audit

### 2.1 Header Cleanup and Flag Mask (`kernel/fte3600.h`)

- **Duplicate Macro Removal:**
  The redundant `#define FTE3600_FLAG_RESET_COMPLETED` definition was cleanly removed. The canonical definition at line 108 is preserved with complete documentation.
- **Initialization Flag Mask:**
  Added:
  ```c
  #define FTE3600_INIT_FLAG_IDENTIFY_ONLY    0x00000000U
  #define FTE3600_INIT_FLAG_FULL_STARTUP     (1U << 0)
  #define FTE3600_INIT_FLAGS_MASK            (FTE3600_INIT_FLAG_FULL_STARTUP)
  ```
- **ABI Structural Integrity:**
  Compile-time static assertions confirm zero layout changes:
  ```c
  static_assert(sizeof(struct fte3600_state_snapshot) == 32, "struct fte3600_state_snapshot must be 32 bytes");
  static_assert(sizeof(struct fte3600_init_params) == 32, "struct fte3600_init_params must be 32 bytes");
  static_assert(sizeof(struct fte3600_frame_buffer) == 32, "struct fte3600_frame_buffer must be 32 bytes");
  ```

### 2.2 Kernel Driver Flag Validation (`kernel/fte3600.c`)

In `fte3600_ioctl_common()`, the `FTE3600_IOC_INIT` case was updated:
```c
		if (params.reserved[0] || params.reserved[1] ||
		    params.reserved[2] || params.reserved[3])
			return -EINVAL;

		if (params.flags & ~FTE3600_INIT_FLAGS_MASK)
			return -EINVAL;
```
Validation occurs immediately after userspace copying, before `mutex_lock_interruptible(&data->lock)` and before any SPI bus access.

### 2.3 Userspace Utility (`tools/fte3600-tool.c`)

- Added support for both `init` (`FTE3600_INIT_FLAG_IDENTIFY_ONLY`) and `init-full` (`FTE3600_INIT_FLAG_FULL_STARTUP`).
- Added state flag decoding for `[PREFIX_COMPLETED]` and `[CALIBRATION_PENDING]`.
- Updated `capture` error handling to correctly report `-EPERM` (not capture ready), `-EOPNOTSUPP` (feature unimplemented), and `-ENODEV` (device offline).

### 2.4 Test Suite Harness (`kernel/fte3600-test.c`)

- Updated mock ioctl validator to enforce `params.flags & ~FTE3600_INIT_FLAGS_MASK`.
- Added Test Suite 10 (10 comprehensive scenarios covering all 31 invalid flag bits, compound bits, reserved fields, capture errnos, offline contract, and reset non-revival).
- All 70 simulation tests pass (100%).

---

## 3. ABI and System Stability Audit

| Parameter | Baseline (D5.0 / D6.0) | Stage D6.6 | Status |
|---|---|---|---|
| `sizeof(struct fte3600_state_snapshot)` | 32 Bytes | 32 Bytes | **Exact Match** |
| `sizeof(struct fte3600_init_params)` | 32 Bytes | 32 Bytes | **Exact Match** |
| `sizeof(struct fte3600_frame_buffer)` | 32 Bytes | 32 Bytes | **Exact Match** |
| `FTE3600_IOC_MAGIC` | `'F'` (0x46) | `'F'` (0x46) | **Exact Match** |
| `FTE3600_IOC_RESET` | `_IO('F', 0x01)` | `_IO('F', 0x01)` | **Exact Match** |
| `FTE3600_IOC_GET_STATE` | `_IOR('F', 0x02, struct fte3600_state_snapshot)` | `_IOR('F', 0x02, ...)` | **Exact Match** |
| `FTE3600_IOC_INIT` | `_IOWR('F', 0x03, struct fte3600_init_params)` | `_IOWR('F', 0x03, ...)` | **Exact Match** |
| `FTE3600_IOC_CAPTURE` | `_IOWR('F', 0x04, struct fte3600_frame_buffer)` | `_IOWR('F', 0x04, ...)` | **Exact Match** |
| Public State Range | `0 .. 8` | `0 .. 8` | **Exact Match** |
| Public Telemetry Flags | `OFFLINE (0x1)`, `RESET (0x2)` | `OFFLINE (0x1)`, `RESET (0x2)`, `PREFIX (0x4)`, `CALIB_PENDING (0x8)` | **Backward-Compatible** |

---

## 4. Hardware Safety & Option 1 Preservation

Throughout Stage D6.6, zero hardware-facing commands or module reloads were executed.

Passive invariant verification confirms:
- `/proc/modules`: `fte3600 20480 0 - Live 0x0000000000000000 (OE)`
- `/sys/bus/spi/devices/spi-FTE3600:00/statistics/transfers`: `3`
- `/sys/bus/spi/devices/spi-FTE3600:00/statistics/bytes`: `19`
- `/dev/fte3600`: `crw------- 1 root root 511, 0 Nov  4 09:32`
- Sensor physical state: Unperturbed since Stage 2E.15.

---

## 5. Final Stage Classification

**Classification:**
`PASS — all identified source and userspace semantic issues corrected`
