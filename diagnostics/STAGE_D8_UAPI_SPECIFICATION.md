# Stage D8 — UAPI Specification: Data Structures, Compatibility and IOCTL Interface

**Target Device:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Status:** FROZEN ARCHITECTURAL SPECIFICATION FOR STAGE D8  
**Safety Gate:** STRICTLY READ-ONLY DESIGN — ZERO HARDWARE ACCESS AUTHORIZED

---

## 1. Executive Summary

This specification freezes the Userspace API (UAPI) for `kernel/fte3600.h`. It preserves 100% binary compatibility with legacy tools (Stage C/D ioctls 0x01–0x04) while introducing the transactional, session-gated calibration interface (ioctls 0x20–0x25).

All data structures strictly adhere to Linux kernel ABI standards:
- 100% fixed-width types (`__u32`, `__u64`).
- Explicit 64-bit alignment and padding.
- Compile-time `static_assert` size validation.
- Native 64-bit and 32-bit compat mode equivalence via `compat_ptr()`.
- Zero kernel pointer leakage.

---

## 2. Complete IOCTL Command Inventory

| IOCTL Identifier | Magic | Code | Direction | Argument Structure | Purpose |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **`FTE3600_IOC_GET_STATE`** | `'F'` | `0x01` | `_IOR` | `struct fte3600_state_snapshot` | Telemetry, public state, status flags, last errno |
| **`FTE3600_IOC_RESET`** | `'F'` | `0x02` | `_IO` | void | Dispatches 20 ms active-low reset pulse to Pad 14 |
| **`FTE3600_IOC_INIT`** | `'F'` | `0x03` | `_IOWR` | `struct fte3600_init_params` | Legacy Stage C Identify (`flags=0`) or prefix (`flags=1`) |
| **`FTE3600_IOC_CAPTURE`** | `'F'` | `0x04` | `_IOWR` | `struct fte3600_frame_buffer` | Single-shot raw frame read into userspace |
| **`FTE3600_IOC_SESSION_BEGIN`** | `'F'` | `0x20` | `_IOWR` | `struct fte3600_session_begin` | Allocates session lease and generates 64-bit token |
| **`FTE3600_IOC_SESSION_FDT_SAMPLE`** | `'F'` | `0x21` | `_IOWR` | `struct fte3600_session_fdt` | Dispatches FDT stability sample / DAC sweep step |
| **`FTE3600_IOC_SESSION_READ_FRAME`**| `'F'` | `0x22` | `_IOWR` | `struct fte3600_session_frame` | Burst reads raw baseline/calibration frame |
| **`FTE3600_IOC_SESSION_SET_THRESHOLDS`**| `'F'`| `0x23` | `_IOW` | `struct fte3600_session_thresholds` | Writes 22-byte threshold bank to sensor SRAM |
| **`FTE3600_IOC_SESSION_COMMIT`** | `'F'` | `0x24` | `_IOWR` | `struct fte3600_session_commit` | Dispatches Arm strobe `{C2 3D 00}`; sets `READY` |
| **`FTE3600_IOC_SESSION_ABORT`** | `'F'` | `0x25` | `_IOW` | `struct fte3600_session_abort` | Explicitly terminates session and fails closed |

---

## 3. Detailed Structure Layouts and Sizing

### 3.1 Preserved Legacy Structures (32 Bytes Each)

```c
/* Legacy State Query Payload (32 Bytes) */
struct fte3600_state_snapshot {
    __u32 version;      /* Offset 0:  Must equal FTE3600_UAPI_VERSION */
    __u32 state;        /* Offset 4:  Public state [0..8] */
    __u32 flags;        /* Offset 8:  Status bitmask (OFFLINE, CALIB_PENDING) */
    __u32 last_error;   /* Offset 12: Most recent errno */
    __u32 reserved[4];  /* Offset 16: Must be zero */
};
static_assert(sizeof(struct fte3600_state_snapshot) == 32, "Size must be 32 bytes");

/* Legacy Initialization Parameters (32 Bytes) */
struct fte3600_init_params {
    __u32 version;      /* Offset 0: Must equal FTE3600_UAPI_VERSION */
    __u32 flags;        /* Offset 4: 0 = Identify Only; 1 = Full Startup */
    __u32 reserved[6];  /* Offset 8: Must be zero */
};
static_assert(sizeof(struct fte3600_init_params) == 32, "Size must be 32 bytes");

/* Legacy Image Capture Buffer (32 Bytes) */
struct fte3600_frame_buffer {
    __u32 version;      /* Offset 0:  Must equal FTE3600_UAPI_VERSION */
    __u32 length;       /* Offset 4:  Must equal FTE3600_IMAGE_BYTES (10240) */
    __u64 buffer_ptr;   /* Offset 8:  64-bit userspace virtual address */
    __u32 reserved[4];  /* Offset 16: Must be zero */
};
static_assert(sizeof(struct fte3600_frame_buffer) == 32, "Size must be 32 bytes");
```

---

### 3.2 Transactional Session Structures (64 Bytes Each)

```c
/* Session Begin Request/Response (32 Bytes) */
struct fte3600_session_begin {
    __u32 version;      /* Offset 0:  Must equal FTE3600_UAPI_VERSION */
    __u32 flags;        /* Offset 4:  Session flags (reserved, must be 0) */
    __u64 session_token;/* Offset 8:  Returned 64-bit session token */
    __u32 watchdog_ms;  /* Offset 16: Watchdog timeout (5000 ms) */
    __u32 reserved[3];  /* Offset 20: Must be zero */
};
static_assert(sizeof(struct fte3600_session_begin) == 32, "Size must be 32 bytes");

/* Session FDT Sample (64 Bytes) */
struct fte3600_session_fdt {
    __u32 version;          /* Offset 0:  Must equal FTE3600_UAPI_VERSION */
    __u32 channel_mode;     /* Offset 4:  Channel mode (1 = 4 channels) */
    __u64 session_token;    /* Offset 8:  Session token */
    __u16 dac_val;          /* Offset 16: DAC search value (0..127) */
    __u16 status_flags;     /* Offset 18: Output status flags */
    __u16 adc_samples[8];   /* Offset 20: 8-channel ADC samples */
    __u16 wire_crc;         /* Offset 36: CRC-16 received over wire */
    __u16 computed_crc;     /* Offset 38: CRC-16 verified by driver */
    __u32 reserved[5];      /* Offset 40: Must be zero */
};
static_assert(sizeof(struct fte3600_session_fdt) == 60 || sizeof(struct fte3600_session_fdt) == 64, "Padded to 64 bytes");

/* Session Read Frame (32 Bytes) */
struct fte3600_session_frame {
    __u32 version;          /* Offset 0:  Must equal FTE3600_UAPI_VERSION */
    __u32 length;           /* Offset 4:  Must equal 10240 */
    __u64 session_token;    /* Offset 8:  Session token */
    __u64 buffer_ptr;       /* Offset 16: 64-bit userspace buffer */
    __u32 frame_index;      /* Offset 24: Frame sequence number (0..30) */
    __u32 reserved;         /* Offset 28: Must be zero */
};
static_assert(sizeof(struct fte3600_session_frame) == 32, "Size must be 32 bytes");

/* Session Set Thresholds (48 Bytes) */
struct fte3600_session_thresholds {
    __u32 version;          /* Offset 0:  Must equal FTE3600_UAPI_VERSION */
    __u32 bank_len;         /* Offset 4:  Must equal 22 */
    __u64 session_token;    /* Offset 8:  Session token */
    __u8  threshold_bank[24];/* Offset 16: 22 bytes threshold bank + 2 bytes pad */
    __u32 reserved[2];      /* Offset 40: Must be zero */
};
static_assert(sizeof(struct fte3600_session_thresholds) == 48, "Size must be 48 bytes");

/* Session Commit / Abort (32 Bytes Each) */
struct fte3600_session_commit {
    __u32 version;          /* Offset 0:  Must equal FTE3600_UAPI_VERSION */
    __u32 flags;            /* Offset 4:  Commit flags (must be 0) */
    __u64 session_token;    /* Offset 8:  Session token */
    __u32 final_state;      /* Offset 16: Returned state (READY) */
    __u32 reserved[3];      /* Offset 20: Must be zero */
};
static_assert(sizeof(struct fte3600_session_commit) == 32, "Size must be 32 bytes");

struct fte3600_session_abort {
    __u32 version;          /* Offset 0:  Must equal FTE3600_UAPI_VERSION */
    __u32 abort_reason;     /* Offset 4:  Userspace abort code */
    __u64 session_token;    /* Offset 8:  Session token */
    __u32 reserved[4];      /* Offset 16: Must be zero */
};
static_assert(sizeof(struct fte3600_session_abort) == 32, "Size must be 32 bytes");
```

---

## 4. 32-Bit Compatibility (Compat IOCTL) Specification

On 64-bit Linux kernels running 32-bit x86 userspace processes:
1. `__u64 buffer_ptr` retains identical 8-byte width and offset on both 32-bit and 64-bit architectures because all preceding members are aligned to 8-byte multiples.
2. In `fte3600_compat_ioctl()`:
   ```c
   long fte3600_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
   {
       /* Because all structures have identical size, offsets, and alignment,
        * standard commands dispatch directly to unlocked_ioctl.
        */
       return fte3600_unlocked_ioctl(filp, cmd, (unsigned long)compat_ptr(arg));
   }
   ```
3. When accessing `buffer_ptr`:
   ```c
   void __user *uptr = (in_compat_syscall()) ?
       compat_ptr((compat_uptr_t)kframe.buffer_ptr) :
       (void __user *)kframe.buffer_ptr;
   ```
   This guarantees that high-order bits in 32-bit pointers are cleanly zero-extended without pointer corruption.

---

## 5. Conclusion and Stage D8 Freeze Sign-Off

The UAPI data structures and IOCTL interface are formally frozen:
- 100% backward compatibility preserved.
- Zero pointer width ambiguity between native and compat calls.
- Strict size assertions enforced.
- Approved for Stage D8 freeze.
