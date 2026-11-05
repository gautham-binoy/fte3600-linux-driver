# STAGE D6.4: UAPI COMPATIBILITY AND ABI STABILITY AUDIT

**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Workspace:** `/home/titan/fte3600-linux-driver/`  
**Safety Status:** STRICTLY SOURCE-ONLY. Zero physical hardware communication.  

---

## 1. Executive Summary

A core requirement of Stage D6.4 is preserving 100% binary, ABI, and semantic compatibility for userspace applications interacting with `/dev/fte3600`. No public state values may be altered, no existing structure layouts may change size or member offsets, and no existing ioctl numbering or semantics may break.

This audit verifies all UAPI definitions, structure layouts, compile-time assertions, and conversion helpers against the current kernel header `kernel/fte3600.h`.

---

## 2. Public State Machine Compatibility

### 2.1 Canonical Public Values (0–8)
The public states defined in `enum fte3600_state` have retained their exact integer assignments since v1 UAPI introduction:

```c
enum fte3600_state {
	FTE3600_STATE_UNINITIALIZED         = 0,
	FTE3600_STATE_RESETTING             = 1,
	FTE3600_STATE_RESET_SEQUENCE_ISSUED = 2,
	FTE3600_STATE_INITIALIZING          = 3,
	FTE3600_STATE_READY                 = 4,
	FTE3600_STATE_CAPTURING             = 5,
	FTE3600_STATE_ERROR                 = 6,
	FTE3600_STATE_UNKNOWN               = 7,
	FTE3600_STATE_IDENTIFIED            = 8,

	/* Extended internal states (9..14) not exposed via public UAPI */
	FTE3600_STATE_WAITING_FOR_TOUCH         = 9,
	FTE3600_STATE_TOUCH_DETECTED            = 10,
	FTE3600_STATE_FRAME_READY               = 11,
	FTE3600_STATE_SUSPENDED                 = 12,
	FTE3600_STATE_DEVICE_REMOVAL            = 13,
	FTE3600_STATE_INITIALIZED_UNCALIBRATED  = 14,
};
```

### 2.2 UAPI State Isolation Guarantee
All public telemetry snapshots returned via `FTE3600_IOC_GET_STATE` pass through `fte3600_state_to_uapi()`. This function guarantees that:
1. Every internal state maps to a value strictly in the range `[0, FTE3600_MAX_PUBLIC_STATE]` (0 through 8).
2. `FTE3600_STATE_INITIALIZED_UNCALIBRATED` (14) maps truthfully to `FTE3600_STATE_IDENTIFIED` (8), strictly withholding `FTE3600_STATE_READY` (4).
3. `FTE3600_STATE_DEVICE_REMOVAL` (13) maps to `FTE3600_STATE_UNINITIALIZED` (0) accompanied by `FTE3600_FLAG_OFFLINE`.

---

## 3. ABI Data Structure Layout & Compile-Time Assertions

All three primary UAPI structures have fixed 32-byte footprints with explicit 64-bit alignment and reserved zero-padding:

### 3.1 `struct fte3600_state_snapshot` (32 Bytes)
```c
struct fte3600_state_snapshot {
	__u32 version;       /* Offset 0,  4 bytes */
	__u32 state;         /* Offset 4,  4 bytes */
	__u32 flags;         /* Offset 8,  4 bytes */
	__u32 last_error;    /* Offset 12, 4 bytes */
	__u32 reserved[4];   /* Offset 16, 16 bytes */
};                       /* Total: 32 bytes */
```

### 3.2 `struct fte3600_init_params` (32 Bytes)
```c
struct fte3600_init_params {
	__u32 version;       /* Offset 0,  4 bytes */
	__u32 flags;         /* Offset 4,  4 bytes */
	__u32 reserved[6];   /* Offset 8,  24 bytes */
};                       /* Total: 32 bytes */
```

### 3.3 `struct fte3600_frame_buffer` (32 Bytes)
```c
struct fte3600_frame_buffer {
	__u32 version;       /* Offset 0,  4 bytes */
	__u32 length;        /* Offset 4,  4 bytes */
	__u64 user_buffer;   /* Offset 8,  8 bytes */
	__u32 reserved[4];   /* Offset 16, 16 bytes */
};                       /* Total: 32 bytes */
```

### 3.4 Kernel and Userspace Compile-Time Assertions
In `kernel/fte3600.h`:
```c
#ifdef __KERNEL__
#include <linux/build_bug.h>
static_assert(sizeof(struct fte3600_state_snapshot) == 32, "struct fte3600_state_snapshot must be 32 bytes");
static_assert(sizeof(struct fte3600_init_params) == 32, "struct fte3600_init_params must be 32 bytes");
static_assert(sizeof(struct fte3600_frame_buffer) == 32, "struct fte3600_frame_buffer must be 32 bytes");
#endif
```

In `kernel/fte3600-test.c` (userspace test harness):
```c
_Static_assert(sizeof(struct fte3600_state_snapshot) == 32, "fte3600_state_snapshot size mismatch");
_Static_assert(sizeof(struct fte3600_init_params) == 32, "fte3600_init_params size mismatch");
_Static_assert(sizeof(struct fte3600_frame_buffer) == 32, "fte3600_frame_buffer size mismatch");
```

---

## 4. Status and Control Flags

The status flag bitfield layout in `fte3600_state_snapshot.flags` is formally specified as:

| Flag Name | Value | Bit | Description |
|---|---|---|---|
| `FTE3600_FLAG_OFFLINE` | `0x00000001U` | Bit 0 | Set when device unbind/removal occurs |
| `FTE3600_FLAG_RESET_SEQUENCE_ISSUED` | `0x00000002U` | Bit 1 | Set after GPIO reset pulse completion |
| `FTE3600_FLAG_STARTUP_PREFIX_COMPLETED` | `0x00000004U` | Bit 2 | Set after volatile startup prefix (6-step) succeeds |
| `FTE3600_FLAG_CALIBRATION_PENDING` | `0x00000008U` | Bit 3 | Set while sensor awaits analog/noise baseline calibration |

Initialization control flags in `fte3600_init_params.flags` are:

| Flag Name | Value | Bit | Description |
|---|---|---|---|
| `FTE3600_INIT_FLAG_IDENTIFY_ONLY` | `0x00000000U` | None | Identifies silicon only (Stage C / D5 legacy compatibility) |
| `FTE3600_INIT_FLAG_FULL_STARTUP` | `0x00000001U` | Bit 0 | Executes identify + volatile startup prefix sequence |

---

## 5. IOCTL Numbering and Encodings

| Macro | Magic | Nr | Direction | Size | Resulting Encoding |
|---|---|---|---|---|---|
| `FTE3600_IOC_GET_STATE` | `'F'` (0x46) | 0x01 | `_IOR` | 32 B | `0x80204601` |
| `FTE3600_IOC_RESET` | `'F'` (0x46) | 0x02 | `_IO` | 0 B | `0x00004602` |
| `FTE3600_IOC_INIT` | `'F'` (0x46) | 0x03 | `_IOWR` | 32 B | `0xC0204603` |
| `FTE3600_IOC_CAPTURE` | `'F'` (0x46) | 0x04 | `_IOWR` | 32 B | `0xC0204604` |

All command encodings, parameters, and directions are strictly identical to their initial definitions in v1 UAPI. Existing user-space tools (such as `bin/fw9369-hw-tool`) continue to function without modification or recompilation.
