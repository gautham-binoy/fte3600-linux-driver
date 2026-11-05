/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_FTE3600_H
#define _UAPI_LINUX_FTE3600_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <sys/ioctl.h>
#include <linux/types.h>
#include <stdint.h>
#endif

#define FTE3600_UAPI_VERSION      1
#define FTE3600_IOC_MAGIC         'F'

/* Hardware and Protocol Constants */
#define FTE3600_EXPECTED_CHIP_ID   0x9362
#define FTE3600_EXPECTED_CHIP_CRC  0x1C53

/* Image and Transfer Dimensions (Verified by Protocol Reference) */
#define FTE3600_IMAGE_WIDTH        64
#define FTE3600_IMAGE_HEIGHT       80
#define FTE3600_IMAGE_PIXELS       (FTE3600_IMAGE_WIDTH * FTE3600_IMAGE_HEIGHT) /* 5120 */
#define FTE3600_IMAGE_BYTES        (FTE3600_IMAGE_PIXELS * 2)                  /* 10240 */
#define FTE3600_FIFO_PREFIX_LEN    6
#define FTE3600_FIFO_TRANSFER_LEN  (FTE3600_FIFO_PREFIX_LEN + FTE3600_IMAGE_BYTES) /* 10246 */
#define FTE3600_MAX_SPI_TRANSFER_LEN FTE3600_FIFO_TRANSFER_LEN

/* Explicit 9-State State Machine (v1 UAPI Public States: 0 through 8) */
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

	/*
	 * Extended Internal Driver States (Architectural proposals for future phases)
	 * These states govern internal kernel synchronization (IRQ, waitqueue, capture, PM).
	 * They are internal to driver operation and not exposed via public v1 UAPI snapshots.
	 */
	FTE3600_STATE_WAITING_FOR_TOUCH         = 9,
	FTE3600_STATE_TOUCH_DETECTED            = 10,
	FTE3600_STATE_FRAME_READY               = 11,
	FTE3600_STATE_SUSPENDED                 = 12,
	FTE3600_STATE_DEVICE_REMOVAL            = 13,
	FTE3600_STATE_INITIALIZED_UNCALIBRATED  = 14,
};

/* Compatibility alias: RESET_COMPLETE indicates strictly that the electrical reset pulse sequence was issued */
#define FTE3600_STATE_RESET_COMPLETE        FTE3600_STATE_RESET_SEQUENCE_ISSUED

/* Compatibility alias: PREFIX_COMPLETE represents completion of volatile startup prefix */
#define FTE3600_STATE_PREFIX_COMPLETE       FTE3600_STATE_INITIALIZED_UNCALIBRATED

/* Compatibility alias: INITIALIZED retained for legacy compatibility (maps to READY) */
#define FTE3600_STATE_INITIALIZED           FTE3600_STATE_READY

/* Maximum valid public UAPI state value (strictly 0 through 8) */
#define FTE3600_MAX_PUBLIC_STATE            8

/**
 * fte3600_state_to_uapi - Map internal driver states to canonical public UAPI states
 * @state: Driver internal state
 *
 * Guaranteed to return a value in the range [0, FTE3600_MAX_PUBLIC_STATE].
 * Extended internal states (9..14) are mapped to compatible public representations:
 * - FTE3600_STATE_INITIALIZED_UNCALIBRATED (14) maps to FTE3600_STATE_IDENTIFIED (8)
 * - FTE3600_STATE_DEVICE_REMOVAL (13) maps to FTE3600_STATE_UNINITIALIZED (0)
 * - Waiting/Touch/Frame internal states (9..11) map to FTE3600_STATE_READY (4)
 * - Suspended (12) maps to FTE3600_STATE_UNKNOWN (7)
 */
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
	case FTE3600_STATE_INITIALIZED_UNCALIBRATED:
		return (__u32)FTE3600_STATE_IDENTIFIED;
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

/* Status Flags */
#define FTE3600_FLAG_OFFLINE                    (1U << 0)
#define FTE3600_FLAG_RESET_SEQUENCE_ISSUED      (1U << 1)
#define FTE3600_FLAG_RESET_COMPLETED            FTE3600_FLAG_RESET_SEQUENCE_ISSUED /* Backward compatibility alias */
#define FTE3600_FLAG_STARTUP_PREFIX_COMPLETED   (1U << 2)
#define FTE3600_FLAG_CALIBRATION_PENDING        (1U << 3)

/**
 * struct fte3600_state_snapshot - Telemetry and state query payload
 * @version: ABI version, must equal FTE3600_UAPI_VERSION
 * @state: Current driver state (enum fte3600_state)
 * @flags: Status bitmask (e.g. FTE3600_FLAG_OFFLINE)
 * @last_error: Most recent errno or transport error
 * @reserved: Reserved for future expansion, must be set to 0
 */
struct fte3600_state_snapshot {
	__u32 version;
	__u32 state;
	__u32 flags;
	__u32 last_error;
	__u32 reserved[4];
};

/* Initialization Control Flags (struct fte3600_init_params.flags) */
#define FTE3600_INIT_FLAG_IDENTIFY_ONLY    0x00000000U /* Execute identification only (Stage C compatibility) */
#define FTE3600_INIT_FLAG_FULL_STARTUP     (1U << 0)   /* Execute complete volatile startup sequence */
#define FTE3600_INIT_FLAGS_MASK            (FTE3600_INIT_FLAG_FULL_STARTUP)

/**
 * struct fte3600_init_params - Sensor initialization parameters
 * @version: ABI version, must equal FTE3600_UAPI_VERSION
 * @flags: Initialization control flags
 * @reserved: Reserved for future expansion, must be set to 0
 */
struct fte3600_init_params {
	__u32 version;
	__u32 flags;
	__u32 reserved[6];
};

/**
 * struct fte3600_frame_buffer - Image capture destination description
 * @version: ABI version, must equal FTE3600_UAPI_VERSION
 * @length: Length of userspace buffer in bytes (must equal FTE3600_IMAGE_BYTES)
 * @buffer_ptr: Userspace virtual address of destination buffer (64-bit aligned)
 * @reserved: Reserved for future expansion, must be set to 0
 */
struct fte3600_frame_buffer {
	__u32 version;
	__u32 length;
	__u64 buffer_ptr;
	__u32 reserved[4];
};

/* Standardized Transactional IOCTL Interface */
#define FTE3600_IOC_GET_STATE _IOR(FTE3600_IOC_MAGIC, 0x01, struct fte3600_state_snapshot)
#define FTE3600_IOC_RESET     _IO(FTE3600_IOC_MAGIC, 0x02)
#define FTE3600_IOC_INIT      _IOWR(FTE3600_IOC_MAGIC, 0x03, struct fte3600_init_params)
#define FTE3600_IOC_CAPTURE   _IOWR(FTE3600_IOC_MAGIC, 0x04, struct fte3600_frame_buffer)

#ifdef __KERNEL__
#include <linux/build_bug.h>
static_assert(sizeof(struct fte3600_state_snapshot) == 32, "struct fte3600_state_snapshot must be 32 bytes");
static_assert(sizeof(struct fte3600_init_params) == 32, "struct fte3600_init_params must be 32 bytes");
static_assert(sizeof(struct fte3600_frame_buffer) == 32, "struct fte3600_frame_buffer must be 32 bytes");
#endif

#endif /* _UAPI_LINUX_FTE3600_H */
