/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_FTE3600_H
#define _UAPI_LINUX_FTE3600_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/build_bug.h>
#else
#include <sys/ioctl.h>
#include <linux/types.h>
#include <stdint.h>
#ifndef static_assert
#define static_assert _Static_assert
#endif
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

/* Session and Calibration Constants */
#define FTE3600_WATCHDOG_DEFAULT_MS    5000
#define FTE3600_THRESHOLD_BANK_LEN     22
#define FTE3600_CALIB_MAX_SAMPLES      31
#define FTE3600_MAX_DOMAINS            4

/* Explicit 14-State State Machine (v1 UAPI Public States: 0 through 8) */
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

	/* Extended Internal Driver States */
	FTE3600_STATE_WAITING_FOR_TOUCH         = 9,
	FTE3600_STATE_TOUCH_DETECTED            = 10,
	FTE3600_STATE_FRAME_READY               = 11,
	FTE3600_STATE_SUSPENDED                 = 12,
	FTE3600_STATE_DEVICE_REMOVAL            = 13,
	FTE3600_STATE_INITIALIZED_UNCALIBRATED  = 14,
};

/* Compatibility Aliases */
#define FTE3600_STATE_RESET_COMPLETE        FTE3600_STATE_RESET_SEQUENCE_ISSUED
#define FTE3600_STATE_PREFIX_COMPLETE       FTE3600_STATE_INITIALIZED_UNCALIBRATED
#define FTE3600_STATE_INITIALIZED           FTE3600_STATE_READY

/* Maximum valid public UAPI state value (strictly 0 through 8) */
#define FTE3600_MAX_PUBLIC_STATE            8

/**
 * fte3600_state_to_uapi - Map internal driver states to canonical public UAPI states
 * @state: Driver internal state
 *
 * Guaranteed to return a value in the range [0, FTE3600_MAX_PUBLIC_STATE].
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
#define FTE3600_FLAG_RESET_COMPLETED            FTE3600_FLAG_RESET_SEQUENCE_ISSUED
#define FTE3600_FLAG_STARTUP_PREFIX_COMPLETED   (1U << 2)
#define FTE3600_FLAG_CALIBRATION_PENDING        (1U << 3)
#define FTE3600_FLAG_SESSION_ACTIVE             (1U << 4)

/**
 * struct fte3600_state_snapshot - Telemetry and state query payload (32 bytes)
 */
struct fte3600_state_snapshot {
	__u32 version;      /* Offset 0:  Must equal FTE3600_UAPI_VERSION */
	__u32 state;        /* Offset 4:  Public state [0..8] */
	__u32 flags;        /* Offset 8:  Status bitmask (OFFLINE, CALIB_PENDING, etc.) */
	__u32 last_error;   /* Offset 12: Most recent errno */
	__u32 reserved[4];  /* Offset 16: Must be zero */
};

/* Initialization Control Flags */
#define FTE3600_INIT_FLAG_IDENTIFY_ONLY    0x00000000U
#define FTE3600_INIT_FLAG_FULL_STARTUP     (1U << 0)
#define FTE3600_INIT_FLAGS_MASK            (FTE3600_INIT_FLAG_FULL_STARTUP)

/**
 * struct fte3600_init_params - Sensor initialization parameters (32 bytes)
 */
struct fte3600_init_params {
	__u32 version;      /* Offset 0: Must equal FTE3600_UAPI_VERSION */
	__u32 flags;        /* Offset 4: Initialization control flags */
	__u32 reserved[6];  /* Offset 8: Must be zero */
};

/**
 * struct fte3600_frame_buffer - Image capture destination description (32 bytes)
 */
struct fte3600_frame_buffer {
	__u32 version;      /* Offset 0:  Must equal FTE3600_UAPI_VERSION */
	__u32 length;       /* Offset 4:  Must equal FTE3600_IMAGE_BYTES (10240) */
	__u64 buffer_ptr;   /* Offset 8:  Userspace virtual address (64-bit aligned) */
	__u32 reserved[4];  /* Offset 16: Must be zero */
};

/**
 * struct fte3600_session_begin - Allocate session lease and token (32 bytes)
 */
struct fte3600_session_begin {
	__u32 version;      /* Offset 0:  Must equal FTE3600_UAPI_VERSION */
	__u32 flags;        /* Offset 4:  Session flags (reserved, must be 0) */
	__u64 session_token;/* Offset 8:  Returned 64-bit session token */
	__u32 watchdog_ms;  /* Offset 16: Watchdog timeout (e.g. 5000 ms) */
	__u32 reserved[3];  /* Offset 20: Must be zero */
};

/**
 * struct fte3600_session_fdt - FDT stability measurement sample (64 bytes)
 */
struct fte3600_session_fdt {
	__u32 version;          /* Offset 0:  Must equal FTE3600_UAPI_VERSION */
	__u32 channel_mode;     /* Offset 4:  Channel mode (strictly 1 = 4 channels) */
	__u64 session_token;    /* Offset 8:  Session token */
	__u16 dac_val;          /* Offset 16: DAC search value (1..125) */
	__u16 status_flags;     /* Offset 18: Output status flags */
	__u16 adc_samples[8];   /* Offset 20: 8-channel ADC samples (channels 0..3 active) */
	__u16 wire_crc;         /* Offset 36: CRC-16 received over wire */
	__u16 computed_crc;     /* Offset 38: CRC-16 verified by driver */
	__u32 reserved[6];      /* Offset 40: Must be zero */
};

/**
 * struct fte3600_session_frame - Calibration baseline frame read (32 bytes)
 */
struct fte3600_session_frame {
	__u32 version;          /* Offset 0:  Must equal FTE3600_UAPI_VERSION */
	__u32 length;           /* Offset 4:  Must equal FTE3600_IMAGE_BYTES (10240) */
	__u64 session_token;    /* Offset 8:  Session token */
	__u64 buffer_ptr;       /* Offset 16: 64-bit userspace destination buffer */
	__u32 frame_index;      /* Offset 24: Frame sequence number */
	__u32 reserved;         /* Offset 28: Must be zero */
};

/**
 * struct fte3600_session_thresholds - Write 22-byte threshold bank (48 bytes)
 */
struct fte3600_session_thresholds {
	__u32 version;          /* Offset 0:  Must equal FTE3600_UAPI_VERSION */
	__u32 bank_len;         /* Offset 4:  Must equal FTE3600_THRESHOLD_BANK_LEN (22) */
	__u64 session_token;    /* Offset 8:  Session token */
	__u8  threshold_bank[24];/* Offset 16: 22 bytes bank + 2 bytes padding */
	__u32 reserved[2];      /* Offset 40: Must be zero */
};

/**
 * struct fte3600_session_commit - Commit calibration and arm sensor (32 bytes)
 */
struct fte3600_session_commit {
	__u32 version;          /* Offset 0:  Must equal FTE3600_UAPI_VERSION */
	__u32 flags;            /* Offset 4:  Commit flags (reserved, must be 0) */
	__u64 session_token;    /* Offset 8:  Session token */
	__u32 final_state;      /* Offset 16: Returned state (FTE3600_STATE_READY) */
	__u32 reserved[3];      /* Offset 20: Must be zero */
};

/**
 * struct fte3600_session_abort - Explicitly abort session and fail closed (32 bytes)
 */
struct fte3600_session_abort {
	__u32 version;          /* Offset 0:  Must equal FTE3600_UAPI_VERSION */
	__u32 abort_reason;     /* Offset 4:  Userspace abort code */
	__u64 session_token;    /* Offset 8:  Session token */
	__u32 reserved[4];      /* Offset 16: Must be zero */
};

/* Standardized Transactional IOCTL Interface */
/* Legacy Commands (Stage C Compatibility) */
#define FTE3600_IOC_GET_STATE              _IOR(FTE3600_IOC_MAGIC,  0x01, struct fte3600_state_snapshot)
#define FTE3600_IOC_RESET                  _IO(FTE3600_IOC_MAGIC,   0x02)
#define FTE3600_IOC_INIT                   _IOWR(FTE3600_IOC_MAGIC, 0x03, struct fte3600_init_params)
#define FTE3600_IOC_CAPTURE                _IOWR(FTE3600_IOC_MAGIC, 0x04, struct fte3600_frame_buffer)

/* Transactional Session Commands (Stage D8+ Calibration Architecture) */
#define FTE3600_IOC_SESSION_BEGIN          _IOWR(FTE3600_IOC_MAGIC, 0x20, struct fte3600_session_begin)
#define FTE3600_IOC_SESSION_FDT_SAMPLE     _IOWR(FTE3600_IOC_MAGIC, 0x21, struct fte3600_session_fdt)
#define FTE3600_IOC_SESSION_READ_FRAME     _IOWR(FTE3600_IOC_MAGIC, 0x22, struct fte3600_session_frame)
#define FTE3600_IOC_SESSION_SET_THRESHOLDS _IOW(FTE3600_IOC_MAGIC,  0x23, struct fte3600_session_thresholds)
#define FTE3600_IOC_SESSION_COMMIT         _IOWR(FTE3600_IOC_MAGIC, 0x24, struct fte3600_session_commit)
#define FTE3600_IOC_SESSION_ABORT          _IOW(FTE3600_IOC_MAGIC,  0x25, struct fte3600_session_abort)

/* Compile-Time Size and Alignment Assertions */
static_assert(sizeof(struct fte3600_state_snapshot) == 32, "struct fte3600_state_snapshot must be 32 bytes");
static_assert(sizeof(struct fte3600_init_params) == 32, "struct fte3600_init_params must be 32 bytes");
static_assert(sizeof(struct fte3600_frame_buffer) == 32, "struct fte3600_frame_buffer must be 32 bytes");
static_assert(sizeof(struct fte3600_session_begin) == 32, "struct fte3600_session_begin must be 32 bytes");
static_assert(sizeof(struct fte3600_session_fdt) == 64, "struct fte3600_session_fdt must be 64 bytes");
static_assert(sizeof(struct fte3600_session_frame) == 32, "struct fte3600_session_frame must be 32 bytes");
static_assert(sizeof(struct fte3600_session_thresholds) == 48, "struct fte3600_session_thresholds must be 48 bytes");
static_assert(sizeof(struct fte3600_session_commit) == 32, "struct fte3600_session_commit must be 32 bytes");
static_assert(sizeof(struct fte3600_session_abort) == 32, "struct fte3600_session_abort must be 32 bytes");

#endif /* _UAPI_LINUX_FTE3600_H */
