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

/* Explicit 9-State State Machine */
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
};

/* Compatibility alias: RESET_COMPLETE indicates strictly that the electrical reset pulse sequence was issued */
#define FTE3600_STATE_RESET_COMPLETE FTE3600_STATE_RESET_SEQUENCE_ISSUED

/* Status Flags */
#define FTE3600_FLAG_OFFLINE                (1U << 0)
#define FTE3600_FLAG_RESET_SEQUENCE_ISSUED  (1U << 1)
#define FTE3600_FLAG_RESET_COMPLETED        FTE3600_FLAG_RESET_SEQUENCE_ISSUED

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

#endif /* _UAPI_LINUX_FTE3600_H */
