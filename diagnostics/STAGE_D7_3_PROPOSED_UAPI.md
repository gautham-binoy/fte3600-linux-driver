# Stage D7.3 — Proposed Calibration UAPI Specification

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Architectural UAPI Specification (Strictly Documentation Only — No Header Modifications)  
**Deliverable:** `diagnostics/STAGE_D7_3_PROPOSED_UAPI.md`

---

## 1. Architectural Context & Design Invariants

This specification defines the userspace API (UAPI) additions required to implement **Design C (Session-Gated Hybrid Architecture)** for the FocalTech FT9362 / FW9369 sensor calibration pipeline.

### Core UAPI Invariants
1. **Strict 32-Byte Structure Width:**  
   Every parameter structure is exactly 32 bytes (`sizeof(...) == 32`), maintaining the strict fixed-width standard established in Stage D6.4 (`fte3600_state_snapshot`, `fte3600_init_params`, `fte3600_frame_buffer`).
2. **Explicit ABI Version Enforcement:**  
   The first field of every structure is `__u32 version`. The driver strictly verifies `version == FTE3600_UAPI_VERSION` (1) and rejects any mismatch with `-EINVAL`.
3. **Strict Zero-Reserved Field Validation:**  
   All padding/reserved fields must be zero. If any bit in `reserved[]` is non-zero, the driver immediately rejects the call with `-EINVAL` before acquiring locks or issuing bus traffic.
4. **Natural Alignment:**  
   64-bit pointers (`__u64 buffer_ptr`) are positioned at 8-byte aligned offsets (offset 16) to avoid structure padding discrepancies between 32-bit and 64-bit compilers.
5. **Session Token Enforcement:**  
   Every transaction step following `CALIB_BEGIN` requires a matching 32-bit `session_id`. Stale or mismatched tokens are rejected with `-EPERM`.
6. **Bidirectional Parameter Passing:**  
   All proposed ioctls utilize `_IOWR` encoding (`_IOWR('F', 0x20..0x25, ...)`), allowing input parameter validation and in-place telemetry return.
7. **Strict Documentation Only:**  
   In compliance with Stage D7.3 safety constraints, this document defines the proposed header contents. **No header files in `kernel/` or `include/` are modified during this stage.**

---

## 2. Proposed C Header Definitions

```c
/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * Proposed additions to kernel/fte3600.h for Stage D8 (Calibration Architecture)
 * DO NOT MODIFY LIVE HEADERS IN STAGE D7.3
 */

#ifndef _UAPI_LINUX_FTE3600_CALIB_H
#define _UAPI_LINUX_FTE3600_CALIB_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define FTE3600_IOC_MAGIC 'F'

/* Calibration Session Flags */
#define FTE3600_CALIB_FLAG_FORCE_RESET     (1U << 0) /* Request clean reset prior to session begin */
#define FTE3600_CALIB_FLAGS_MASK           (FTE3600_CALIB_FLAG_FORCE_RESET)

/* Calibration Frame Types */
#define FTE3600_CALIB_FRAME_DISCARD        0x00000000U /* Discard / dummy warm-up frame */
#define FTE3600_CALIB_FRAME_DARK_BASELINE  0x00000001U /* Dark baseline capture frame (10,240 bytes) */

/* Calibration Abort Reasons */
#define FTE3600_CALIB_ABORT_USER           0x00000001U /* Explicit user / application cancellation */
#define FTE3600_CALIB_ABORT_TIMEOUT        0x00000002U /* Step timeout exceeded in userspace */
#define FTE3600_CALIB_ABORT_ALGO_FAILURE   0x00000003U /* Numerical instability / variance threshold failure */
#define FTE3600_CALIB_ABORT_PROTOCOL_ERR   0x00000004U /* Unexpected frame header or payload corruption */

/*
 * Structure 1: Calibration Session Initialization
 * IOCTL: FTE3600_IOC_CALIB_BEGIN (_IOWR('F', 0x20, struct fte3600_calib_begin_params))
 */
struct fte3600_calib_begin_params {
	__u32 version;        /* [in]  Must equal FTE3600_UAPI_VERSION (1) */
	__u32 flags;          /* [in]  Calibration session flags (FTE3600_CALIB_FLAG_*) */
	__u32 session_id;     /* [out] Generated 32-bit non-zero cryptographically random token */
	__u32 timeout_ms;     /* [out] Driver-enforced inactivity watchdog (e.g. 5000 ms) */
	__u32 reserved[4];    /* [in]  Must be zero */
};

/*
 * Structure 2: FDT ADC Sampling
 * IOCTL: FTE3600_IOC_CALIB_FDT_SAMPLE (_IOWR('F', 0x21, struct fte3600_calib_fdt_params))
 */
struct fte3600_calib_fdt_params {
	__u32 version;        /* [in]  Must equal FTE3600_UAPI_VERSION (1) */
	__u32 session_id;     /* [in]  Must match active session_id */
	__u16 adc_samples[4]; /* [out] 4 ADC channel readings (CH0..CH3) from FDT register */
	__u32 sample_index;   /* [in]  Iteration counter (0..7) */
	__u32 reserved[3];    /* [in]  Must be zero */
};

/*
 * Structure 3: Baseline Frame Acquisition
 * IOCTL: FTE3600_IOC_CALIB_READ_FRAME (_IOWR('F', 0x22, struct fte3600_calib_frame_params))
 */
struct fte3600_calib_frame_params {
	__u32 version;        /* [in]  Must equal FTE3600_UAPI_VERSION (1) */
	__u32 session_id;     /* [in]  Must match active session_id */
	__u32 frame_type;     /* [in]  FTE3600_CALIB_FRAME_DISCARD or FTE3600_CALIB_FRAME_DARK_BASELINE */
	__u32 buffer_len;     /* [in]  Must equal FTE3600_IMAGE_BYTES (10,240) if dark baseline */
	__u64 buffer_ptr;     /* [in]  Userspace destination pointer (64-bit virtual address) */
	__u32 reserved[2];    /* [in]  Must be zero */
};

/*
 * Structure 4: FDT Touch/Release Threshold Configuration
 * IOCTL: FTE3600_IOC_CALIB_SET_THRESHOLDS (_IOWR('F', 0x23, struct fte3600_calib_threshold_params))
 */
struct fte3600_calib_threshold_params {
	__u32 version;                 /* [in] Must equal FTE3600_UAPI_VERSION (1) */
	__u32 session_id;              /* [in] Must match active session_id */
	__u16 touch_thresholds[4];     /* [in] 4 channel touch thresholds (CH0..CH3) */
	__u16 release_thresholds[4];   /* [in] 4 channel release thresholds (CH0..CH3) */
	__u32 flags;                   /* [in] Reserved control flags (must be 0) */
	__u32 reserved;                /* [in] Must be zero */
};

/*
 * Structure 5: Calibration Commit & Arming
 * IOCTL: FTE3600_IOC_CALIB_COMMIT (_IOWR('F', 0x24, struct fte3600_calib_commit_params))
 */
struct fte3600_calib_commit_params {
	__u32 version;        /* [in]  Must equal FTE3600_UAPI_VERSION (1) */
	__u32 session_id;     /* [in]  Must match active session_id */
	__u32 target_state;   /* [out] Driver state upon successful commit (FTE3600_STATE_READY) */
	__u32 reserved[5];    /* [in]  Must be zero */
};

/*
 * Structure 6: Calibration Session Abort
 * IOCTL: FTE3600_IOC_CALIB_ABORT (_IOWR('F', 0x25, struct fte3600_calib_abort_params))
 */
struct fte3600_calib_abort_params {
	__u32 version;         /* [in]  Must equal FTE3600_UAPI_VERSION (1) */
	__u32 session_id;      /* [in]  Must match active session_id */
	__u32 reason;          /* [in]  Abort reason code (FTE3600_CALIB_ABORT_*) */
	__u32 resulting_state; /* [out] Final driver state (always FTE3600_STATE_UNKNOWN) */
	__u32 reserved[4];     /* [in]  Must be zero */
};

/* IOCTL Command Definitions */
#define FTE3600_IOC_CALIB_BEGIN          _IOWR(FTE3600_IOC_MAGIC, 0x20, struct fte3600_calib_begin_params)
#define FTE3600_IOC_CALIB_FDT_SAMPLE     _IOWR(FTE3600_IOC_MAGIC, 0x21, struct fte3600_calib_fdt_params)
#define FTE3600_IOC_CALIB_READ_FRAME     _IOWR(FTE3600_IOC_MAGIC, 0x22, struct fte3600_calib_frame_params)
#define FTE3600_IOC_CALIB_SET_THRESHOLDS _IOWR(FTE3600_IOC_MAGIC, 0x23, struct fte3600_calib_threshold_params)
#define FTE3600_IOC_CALIB_COMMIT         _IOWR(FTE3600_IOC_MAGIC, 0x24, struct fte3600_calib_commit_params)
#define FTE3600_IOC_CALIB_ABORT          _IOWR(FTE3600_IOC_MAGIC, 0x25, struct fte3600_calib_abort_params)

#ifdef __KERNEL__
#include <linux/build_bug.h>
static_assert(sizeof(struct fte3600_calib_begin_params) == 32, "calib_begin_params must be 32 bytes");
static_assert(sizeof(struct fte3600_calib_fdt_params) == 32, "calib_fdt_params must be 32 bytes");
static_assert(sizeof(struct fte3600_calib_frame_params) == 32, "calib_frame_params must be 32 bytes");
static_assert(sizeof(struct fte3600_calib_threshold_params) == 32, "calib_threshold_params must be 32 bytes");
static_assert(sizeof(struct fte3600_calib_commit_params) == 32, "calib_commit_params must be 32 bytes");
static_assert(sizeof(struct fte3600_calib_abort_params) == 32, "calib_abort_params must be 32 bytes");
#endif

#endif /* _UAPI_LINUX_FTE3600_CALIB_H */
```

---

## 3. Structure Layout & Field Specifications

### 3.1. `struct fte3600_calib_begin_params` (0x20)
*Total Size:* 32 bytes.  
*Direction:* Bidirectional (`_IOWR`).

| Offset (Bytes) | Field Name | Type | Direction | Description / Constraints |
| :--- | :--- | :--- | :--- | :--- |
| `0x00 - 0x03` | `version` | `__u32` | In | Must equal `FTE3600_UAPI_VERSION` (1). |
| `0x04 - 0x07` | `flags` | `__u32` | In | Bitmask of `FTE3600_CALIB_FLAG_*`. Unknown bits return `-EINVAL`. |
| `0x08 - 0x0B` | `session_id` | `__u32` | Out | Driver-generated non-zero random token identifying active lease. |
| `0x0C - 0x0F` | `timeout_ms` | `__u32` | Out | Driver inactivity watchdog timeout (fixed at `5000` ms). |
| `0x10 - 0x1F` | `reserved[4]` | `__u32[4]` | In | Reserved for future ABI extension. Must all be zero (`0x00000000`). |

#### Execution Semantics:
- **Preconditions:**  
  `data->state == FTE3600_STATE_INITIALIZED_UNCALIBRATED` (14). If driver is in any other state, returns `-EPERM`.  
  No calibration session currently active (`data->calib_session.active == false`). If another session is active, returns `-EBUSY`.
- **Driver Actions:**  
  Generates `session_id = get_random_u32(); while (!session_id) session_id = get_random_u32();`.  
  Binds session to `filp->private_data`.  
  Initializes `session->jiffies_deadline = jiffies + msecs_to_jiffies(5000)`.  
  Clears session progression bitmap (`session->progress = 0`).
- **Mutex Lock Duration:** < 1 ms (pure in-memory assignment).
- **Return Codes:**  
  `0`: Success. Session established.  
  `-EINVAL`: Version mismatch, invalid flag bit, or non-zero reserved fields.  
  `-EPERM`: Driver not in `INITIALIZED_UNCALIBRATED` state.  
  `-EBUSY`: Calibration session already active on device.

---

### 3.2. `struct fte3600_calib_fdt_params` (0x21)
*Total Size:* 32 bytes.  
*Direction:* Bidirectional (`_IOWR`).

| Offset (Bytes) | Field Name | Type | Direction | Description / Constraints |
| :--- | :--- | :--- | :--- | :--- |
| `0x00 - 0x03` | `version` | `__u32` | In | Must equal `FTE3600_UAPI_VERSION` (1). |
| `0x04 - 0x07` | `session_id` | `__u32` | In | Must match active session token returned by `CALIB_BEGIN`. |
| `0x08 - 0x0F` | `adc_samples[4]` | `__u16[4]` | Out | ADC channel readings (CH0, CH1, CH2, CH3) sampled via SPI. |
| `0x10 - 0x13` | `sample_index` | `__u32` | In | Sample iteration sequence number (`0` through `7`). |
| `0x14 - 0x1F` | `reserved[3]` | `__u32[3]` | In | Reserved for future ABI extension. Must all be zero (`0x00000000`). |

#### Execution Semantics:
- **Preconditions:**  
  Session active (`session->active == true`), `session->id == params.session_id`, `filp == session->owner_filp`.  
  Watchdog deadline not expired (`time_before(jiffies, session->jiffies_deadline)`).  
  `sample_index` must equal `session->expected_sample_index`.
- **Driver Actions:**  
  Acquires `data->lock`.  
  Refreshes watchdog deadline (`jiffies + 5000ms`).  
  Transfers 2 SPI commands:  
  1. Trigger/Read FDT register: `05 FA 1B 00 00 04 ...` (read 8 bytes of ADC data).  
  2. Parse 4 `__u16` channel values into `adc_samples[4]`.  
  Updates `session->expected_sample_index++`.  
  Releases `data->lock`.
- **Mutex Lock Duration:** ~10 ms (SPI transaction).
- **Return Codes:**  
  `0`: Success. ADC samples populated.  
  `-EINVAL`: Version mismatch, reserved fields non-zero, or out-of-order `sample_index`.  
  `-EPERM`: Invalid/stale `session_id` or mismatched caller `filp`.  
  `-ETIMEDOUT`: Inactivity watchdog expired (transitions state to `UNKNOWN`).  
  `-EIO`: Hardware SPI transfer error.

---

### 3.3. `struct fte3600_calib_frame_params` (0x22)
*Total Size:* 32 bytes.  
*Direction:* Bidirectional (`_IOWR`).

| Offset (Bytes) | Field Name | Type | Direction | Description / Constraints |
| :--- | :--- | :--- | :--- | :--- |
| `0x00 - 0x03` | `version` | `__u32` | In | Must equal `FTE3600_UAPI_VERSION` (1). |
| `0x04 - 0x07` | `session_id` | `__u32` | In | Must match active session token returned by `CALIB_BEGIN`. |
| `0x08 - 0x0B` | `frame_type` | `__u32` | In | `0` = Discard frame, `1` = Dark baseline frame. |
| `0x0C - 0x0F` | `buffer_len` | `__u32` | In | If type=1: must equal `10240`. If type=0: ignored (may be 0). |
| `0x10 - 0x17` | `buffer_ptr` | `__u64` | In | 64-bit user address. If type=1: must be valid writable memory. |
| `0x18 - 0x1F` | `reserved[2]` | `__u32[2]` | In | Reserved for future ABI extension. Must all be zero (`0x00000000`). |

#### Execution Semantics:
- **Preconditions:**  
  Session active and authenticated.  
  If `frame_type == FTE3600_CALIB_FRAME_DARK_BASELINE`: `access_ok((void __user *)buffer_ptr, buffer_len)` must succeed.  
  `session->fdt_completed == true` (FDT sampling phase must have completed).
- **Driver Actions:**  
  Acquires `data->lock`.  
  Refreshes watchdog deadline.  
  Issues single-byte trigger strobe `0xA0`.  
  Waits bounded polling loop for frame ready interrupt/status (~50 ms max).  
  Issues bulk SPI read (10,246 bytes: 6-byte header + 10,240 pixel data).  
  Validates 6-byte header.  
  If `frame_type == FTE3600_CALIB_FRAME_DARK_BASELINE`: streams 10,240 bytes directly to `(void __user *)buffer_ptr` via `copy_to_user()`.  
  If `frame_type == FTE3600_CALIB_FRAME_DISCARD`: skips userspace copy.  
  Releases `data->lock`.
- **Mutex Lock Duration:** ~86 ms (strobe + sensor integration wait + SPI 10,246-byte transfer).
- **Return Codes:**  
  `0`: Success. Frame captured (and copied to userspace if dark baseline).  
  `-EINVAL`: Version mismatch, invalid `frame_type`, buffer length != 10240, or reserved non-zero.  
  `-EFAULT`: `copy_to_user()` failed (bad userspace pointer).  
  `-EPERM`: Invalid/stale `session_id` or FDT sampling not completed.  
  `-ETIMEDOUT`: Sensor integration timeout or watchdog expired (state $\rightarrow$ `UNKNOWN`).  
  `-EIO`: Protocol error, corrupt FIFO header, or SPI bus failure.

---

### 3.4. `struct fte3600_calib_threshold_params` (0x23)
*Total Size:* 32 bytes.  
*Direction:* Bidirectional (`_IOWR`).

| Offset (Bytes) | Field Name | Type | Direction | Description / Constraints |
| :--- | :--- | :--- | :--- | :--- |
| `0x00 - 0x03` | `version` | `__u32` | In | Must equal `FTE3600_UAPI_VERSION` (1). |
| `0x04 - 0x07` | `session_id` | `__u32` | In | Must match active session token returned by `CALIB_BEGIN`. |
| `0x08 - 0x0F` | `touch_thresholds[4]` | `__u16[4]` | In | 4 channel touch thresholds. Range: `100 <= val <= 3500`. |
| `0x10 - 0x17` | `release_thresholds[4]` | `__u16[4]` | In | 4 channel release thresholds. Must be `>= touch_thresholds[i]`. |
| `0x18 - 0x1B` | `flags` | `__u32` | In | Reserved control flags (must be `0x00000000`). |
| `0x1C - 0x1F` | `reserved` | `__u32` | In | Reserved for future ABI extension. Must be zero (`0x00000000`). |

#### Execution Semantics:
- **Preconditions:**  
  Session active and authenticated.  
  FDT sampling completed and dark baseline frame acquired.  
  Sanity check thresholds: For each channel $i \in [0..3]$, $100 \le T_{\text{touch}}[i] \le T_{\text{release}}[i] \le 3500$.
- **Driver Actions:**  
  Acquires `data->lock`.  
  Refreshes watchdog deadline.  
  Formats 16-byte write register command for touch/release thresholds (`0x05 0xFA 0x1B ...`).  
  Executes SPI write transfer.  
  Records `session->thresholds_configured = true`.  
  Caches threshold values in driver state context.  
  Releases `data->lock`.
- **Mutex Lock Duration:** ~5 ms (SPI write transfer).
- **Return Codes:**  
  `0`: Success. Thresholds programmed to hardware registers.  
  `-EINVAL`: Version mismatch, reserved non-zero, out-of-range thresholds, or release < touch.  
  `-EPERM`: Invalid/stale `session_id` or prerequisite frames not captured.  
  `-EIO`: Hardware SPI transfer error.

---

### 3.5. `struct fte3600_calib_commit_params` (0x24)
*Total Size:* 32 bytes.  
*Direction:* Bidirectional (`_IOWR`).

| Offset (Bytes) | Field Name | Type | Direction | Description / Constraints |
| :--- | :--- | :--- | :--- | :--- |
| `0x00 - 0x03` | `version` | `__u32` | In | Must equal `FTE3600_UAPI_VERSION` (1). |
| `0x04 - 0x07` | `session_id` | `__u32` | In | Must match active session token returned by `CALIB_BEGIN`. |
| `0x08 - 0x0B` | `target_state` | `__u32` | Out | State upon commit (`FTE3600_STATE_READY` = 4). |
| `0x0C - 0x1F` | `reserved[5]` | `__u32[5]` | In | Reserved for future ABI extension. Must all be zero (`0x00000000`). |

#### Execution Semantics:
- **Preconditions:**  
  Session active and authenticated.  
  All 5 prerequisites satisfied:  
  1. Prefix sequence completed (`INITIALIZED_UNCALIBRATED`).  
  2. FDT samples collected (8 iterations).  
  3. Dark baseline frame captured and delivered.  
  4. Hardware thresholds verified and committed to sensor registers.  
  5. Touch monitoring mode armed (`0xA2` strobe).
- **Driver Actions:**  
  Acquires `data->lock`.  
  Issues touch-monitoring arm strobe: `0xA2` (1 byte).  
  Terminates calibration session (`session->active = false`, `session->id = 0`).  
  Sets `data->state = FTE3600_STATE_READY` (4).  
  Populates `target_state = FTE3600_STATE_READY`.  
  Releases `data->lock`.  
  Wakes up any pending telemetry waitqueues.
- **Mutex Lock Duration:** ~2 ms (1-byte strobe + state assignment).
- **Return Codes:**  
  `0`: Success. Hardware is armed; driver transitioned to `READY`.  
  `-EINVAL`: Version mismatch or non-zero reserved fields.  
  `-EPERM`: Incomplete session prerequisites or invalid/stale `session_id`.  
  `-EIO`: Hardware SPI strobe error (transitions state to `UNKNOWN`).

---

### 3.6. `struct fte3600_calib_abort_params` (0x25)
*Total Size:* 32 bytes.  
*Direction:* Bidirectional (`_IOWR`).

| Offset (Bytes) | Field Name | Type | Direction | Description / Constraints |
| :--- | :--- | :--- | :--- | :--- |
| `0x00 - 0x03` | `version` | `__u32` | In | Must equal `FTE3600_UAPI_VERSION` (1). |
| `0x04 - 0x07` | `session_id` | `__u32` | In | Must match active session token. |
| `0x08 - 0x0B` | `reason` | `__u32` | In | Abort reason code (`FTE3600_CALIB_ABORT_*`). |
| `0x0C - 0x0F` | `resulting_state` | `__u32` | Out | Resulting driver state (always `FTE3600_STATE_UNKNOWN` = 7). |
| `0x10 - 0x1F` | `reserved[4]` | `__u32[4]` | In | Reserved for future ABI extension. Must all be zero (`0x00000000`). |

#### Execution Semantics:
- **Preconditions:**  
  Session active and authenticated (`filp == session->owner_filp`).
- **Driver Actions:**  
  Acquires `data->lock`.  
  Logs abort notice to `dmesg` with `reason` and caller PID.  
  Clears session structure: `session->active = false`, `session->id = 0`.  
  Transitions device state: `data->state = FTE3600_STATE_UNKNOWN` (7).  
  Populates `resulting_state = FTE3600_STATE_UNKNOWN`.  
  Releases `data->lock`.  
  *Note:* Because state is now `UNKNOWN`, subsequent capture or calibration is blocked until userspace issues a physical hardware reset pulse via `FTE3600_IOC_RESET`.
- **Mutex Lock Duration:** < 1 ms (pure in-memory state transition).
- **Return Codes:**  
  `0`: Success. Session cleanly aborted; hardware placed in `UNKNOWN`.  
  `-EINVAL`: Version mismatch or non-zero reserved fields.  
  `-EPERM`: Invalid/stale `session_id` or caller not session owner.

---

## 4. Backward Compatibility & ABI Verification

### 4.1. Existing IOCTL Coexistence
The proposed calibration ioctls are assigned to command indices `0x20` through `0x25` under magic character `'F'`, completely avoiding conflicts with existing driver commands:

| Command Name | Macro | Type | Direction | Data Struct |
| :--- | :--- | :--- | :--- | :--- |
| `FTE3600_IOC_GET_STATE` | `_IOR('F', 0x01, struct fte3600_state_snapshot)` | Core | Read | 32 Bytes |
| `FTE3600_IOC_RESET` | `_IO('F', 0x02)` | Core | None | Void (0 Bytes) |
| `FTE3600_IOC_INIT` | `_IOWR('F', 0x03, struct fte3600_init_params)` | Core | Read/Write | 32 Bytes |
| `FTE3600_IOC_CAPTURE` | `_IOWR('F', 0x04, struct fte3600_frame_buffer)` | Core | Read/Write | 32 Bytes |
| **`FTE3600_IOC_CALIB_BEGIN`** | `_IOWR('F', 0x20, struct fte3600_calib_begin_params)` | Calib | Read/Write | 32 Bytes |
| **`FTE3600_IOC_CALIB_FDT_SAMPLE`** | `_IOWR('F', 0x21, struct fte3600_calib_fdt_params)` | Calib | Read/Write | 32 Bytes |
| **`FTE3600_IOC_CALIB_READ_FRAME`** | `_IOWR('F', 0x22, struct fte3600_calib_frame_params)` | Calib | Read/Write | 32 Bytes |
| **`FTE3600_IOC_CALIB_SET_THRESHOLDS`** | `_IOWR('F', 0x23, struct fte3600_calib_threshold_params)`| Calib | Read/Write | 32 Bytes |
| **`FTE3600_IOC_CALIB_COMMIT`** | `_IOWR('F', 0x24, struct fte3600_calib_commit_params)` | Calib | Read/Write | 32 Bytes |
| **`FTE3600_IOC_CALIB_ABORT`** | `_IOWR('F', 0x25, struct fte3600_calib_abort_params)` | Calib | Read/Write | 32 Bytes |

### 4.2. Compile-Time Alignment Verification
When implemented in a future stage, the kernel driver will enforce the 32-byte layout with static build-time assertions:

```c
static_assert(sizeof(struct fte3600_calib_begin_params) == 32, "calib_begin_params must be 32 bytes");
static_assert(sizeof(struct fte3600_calib_fdt_params) == 32, "calib_fdt_params must be 32 bytes");
static_assert(sizeof(struct fte3600_calib_frame_params) == 32, "calib_frame_params must be 32 bytes");
static_assert(sizeof(struct fte3600_calib_threshold_params) == 32, "calib_threshold_params must be 32 bytes");
static_assert(sizeof(struct fte3600_calib_commit_params) == 32, "calib_commit_params must be 32 bytes");
static_assert(sizeof(struct fte3600_calib_abort_params) == 32, "calib_abort_params must be 32 bytes");
```

---

## 5. Summary & Implementation Readiness

The proposed UAPI design provides:
1. **Mathematical Certainty:** Every structure is strictly 32 bytes wide, preventing kernel/userspace ABI skew across 32-bit and 64-bit architectures.
2. **Defensive Validation:** Strict zero-reserved checking and version enforcement protect against uninitialized userspace memory and future protocol changes.
3. **Fail-Closed Session Gating:** Random 32-bit tokens prevent cross-process or orphaned execution.
4. **Clean Decoupling:** Userspace orchestrates algorithm heuristics (variance checks, dark frame subtractions, threshold selections) while kernel enforces protocol safety, hardware timing, and physical state transitions.
