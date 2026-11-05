# STAGE D5.2: ABI INTEGRITY AND STATE MACHINE REMEDIATION REPORT

## Document Information
- **Project:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`) Native Linux Fingerprint Driver
- **Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`, CS0)
- **Host Controller:** Intel LPSS SPI2 (`pxa2xx-spi.2`) on Linux `7.2.8-9-omarchy` (x86_64 SMP)
- **Author:** Lead Kernel Architect & ABI Specialist (Antigravity Agent)
- **Status:** COMPLETED ABI REMEDIATION
- **Safety Boundary:** STRICTLY SOURCE-ONLY VERIFICATION (Zero Hardware Operations)

---

## 1. Executive Summary

This report documents the architectural remediation of the driver state space and the verification of UAPI v1 binary compatibility.

The primary defect discovered in Stage D5.1—where internal state `FTE3600_STATE_DEVICE_REMOVAL` (13) leaked directly to userspace via `FTE3600_IOC_GET_STATE`—has been resolved. The driver now strictly bounds all public state telemetry to the range `[0..8]`, while retaining internal extended states for kernel lifecycle management.

---

## 2. Public State Isolation Architecture

### 2.1 The Two-Tier State Hierarchy

To decouple internal driver lifecycle events from external client expectations, the state space is explicitly partitioned:

```
+-------------------------------------------------------------------------------+
|                       PUBLIC UAPI STATES (Strictly 0..8)                       |
|  0: UNINITIALIZED              1: RESETTING          2: RESET_SEQUENCE_ISSUED  |
|  3: INITIALIZING               4: READY              5: CAPTURING              |
|  6: ERROR                      7: UNKNOWN            8: IDENTIFIED             |
+-------------------------------------------------------------------------------+
                                      ^
                                      | fte3600_state_to_uapi()
                                      |
+-------------------------------------------------------------------------------+
|                      INTERNAL DRIVER STATES (Extended 9..13)                   |
|  9: WAITING_FOR_TOUCH         10: TOUCH_DETECTED     11: FRAME_READY          |
| 12: SUSPENDED                 13: DEVICE_REMOVAL                              |
+-------------------------------------------------------------------------------+
```

### 2.2 Canonical Mapping Specification: `fte3600_state_to_uapi()`

Implemented in `kernel/fte3600.h`:
```c
static inline __u32 fte3600_state_to_uapi(enum fte3600_state state)
{
	switch (state) {
	case FTE3600_STATE_UNINITIALIZED:
	case FTE3600_STATE_RESETTING:
	case FTE3600_STATE_RESET_SEQUENCE_ISSUED:
	case FTE3600_STATE_INITIALIZING:
	case FTE3600_STATE_READY:
	case FTE3600_STATE_CAPTURING:
	case FTE3600_STATE_ERROR:
	case FTE3600_STATE_UNKNOWN:
	case FTE3600_STATE_IDENTIFIED:
		return (__u32)state;
	case FTE3600_STATE_WAITING_FOR_TOUCH:
	case FTE3600_STATE_TOUCH_DETECTED:
	case FTE3600_STATE_FRAME_READY:
		return (__u32)FTE3600_STATE_READY;
	case FTE3600_STATE_SUSPENDED:
		return (__u32)FTE3600_STATE_UNKNOWN;
	case FTE3600_STATE_DEVICE_REMOVAL:
	default:
		return (__u32)FTE3600_STATE_UNINITIALIZED;
	}
}
```

### 2.3 Justification for Removal Representation: `UNINITIALIZED` + `FLAG_OFFLINE`

When `fte3600_remove()` executes, why is `FTE3600_STATE_UNINITIALIZED` (0) paired with `FTE3600_FLAG_OFFLINE` the correct public representation?
1. **Preservation of Existing Invariants:** Prior to Stage D5, when the driver was unbound, `GET_STATE` returned `snap.state == FTE3600_STATE_UNINITIALIZED` with `snap.flags & FTE3600_FLAG_OFFLINE` set (as validated by baseline Scenario 5).
2. **Hardware Semantic Reality:** When unbound, the SPI slave and GPIO handles are detached and invalid. From userspace's perspective, the sensor silicon is uninitialized and inactive.
3. **Unambiguous Offline Flag:** The terminal condition is communicated cleanly through bit 0 of `snap.flags` (`FTE3600_FLAG_OFFLINE = 1U << 0`).
4. **Client Robustness:** Legacy daemons (e.g. libfprint or test scripts) branching on `snap.state` handle state 0 without entering unexpected error paths or throwing range assertion exceptions.

---

## 3. Comprehensive UAPI v1 Compatibility Audit

### 3.1 IOCTL Command Matrix

| IOCTL Symbolic Name | Command Macro | Direction | Size | Numeric Value | Status |
| :--- | :--- | :---: | :---: | :---: | :---: |
| `FTE3600_IOC_GET_STATE` | `_IOR('F', 0x01, struct fte3600_state_snapshot)` | Read | 32 B | `0x80204601` | **PRESERVED** |
| `FTE3600_IOC_RESET` | `_IO('F', 0x02)` | None | 0 B | `0x00004602` | **PRESERVED** |
| `FTE3600_IOC_INIT` | `_IOWR('F', 0x03, struct fte3600_init_params)` | Read/Write | 32 B | `0xC0204603` | **PRESERVED** |
| `FTE3600_IOC_CAPTURE` | `_IOWR('F', 0x04, struct fte3600_frame_buffer)` | Read/Write | 32 B | `0xC0204604` | **PRESERVED** |

### 3.2 UAPI Structure Layout & Member Offsets

All structures are verified by compiler `_Static_assert` and test suite offset checks:

#### `struct fte3600_state_snapshot` (32 Bytes)
- Offset `0` (`4` bytes): `__u32 version` (Must equal `1`)
- Offset `4` (`4` bytes): `__u32 state` (Bounded strictly `[0..8]`)
- Offset `8` (`4` bytes): `__u32 flags` (Bit 0: `OFFLINE`, Bit 1: `RESET_SEQUENCE_ISSUED`)
- Offset `12` (`4` bytes): `__u32 last_error`
- Offset `16` (`16` bytes): `__u32 reserved[4]` (Strictly zeroes)

#### `struct fte3600_init_params` (32 Bytes)
- Offset `0` (`4` bytes): `__u32 version` (Must equal `1`)
- Offset `4` (`4` bytes): `__u32 flags`
- Offset `8` (`24` bytes): `__u32 reserved[6]` (Must be passed as zeroes)

#### `struct fte3600_frame_buffer` (32 Bytes)
- Offset `0` (`4` bytes): `__u32 version` (Must equal `1`)
- Offset `4` (`4` bytes): `__u32 length` (Must equal `10240`)
- Offset `8` (`8` bytes): `__u64 buffer_ptr` (64-bit userspace pointer)
- Offset `16` (`16` bytes): `__u32 reserved[4]` (Must be passed as zeroes)

### 3.3 32-Bit / 64-Bit Compatibility
Because all structures use explicitly sized types (`__u32`, `__u64`) with natural alignment, structure sizes and member offsets are bit-for-bit identical across x86-32 and x86-64 architectures. The in-kernel `compat_ioctl` handler delegates directly to `fte3600_ioctl_common()`.

---

## 4. Verification Verdict

**PASS — ABI INTEGRITY AND STATE ISOLATION FULLY VERIFIED**
