# Stage D3 — FT9362 Kernel UAPI Specification (v2)

**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`)  
**Host Controller:** Intel LPSS SPI2 (`pxa2xx-spi.2`, CS0) on Linux `7.2.8-9-omarchy`  
**Document Purpose:** Complete specification of the versioned Linux Userspace API (UAPI), ioctl definitions, data structures, binary layout, 32/64-bit compatibility guarantees, and input validation invariants.  
**Safety Classification:** Strictly UAPI Design and Architecture Specification. Zero Hardware Operations Authorized.

---

## 1. Architectural Philosophy and Design Invariants

1. **Explicit Transactional Semantics:**
   Every UAPI command is atomic and deterministic. Userspace cannot trigger arbitrary SPI transfers or bypass the internal kernel state machine.
2. **Fixed Binary Representation & Architecture Independence:**
   All structures use explicit fixed-width Linux types (`__u8`, `__u16`, `__u32`, `__u64`). User virtual addresses are transmitted as `__u64` values to ensure identical binary layouts between 32-bit userspace (x86) and 64-bit kernels (x86_64), eliminating the need for translation thunks in `compat_ioctl`.
3. **Strict Versioning and Forward Compatibility:**
   Every struct begins with `__u32 version`. The driver strictly verifies `version == FTE3600_UAPI_VERSION`. All structs include 64-bit aligned reserved fields that must be zero-initialized by userspace; non-zero values return `-EINVAL`.
4. **Single-Client Lease Enforcement:**
   The character device `/dev/fte3600` enforces single-client exclusive lease via atomic comparison (`atomic_cmpxchg(&data->is_open, 0, 1)`). Concurrent open attempts fail immediately with `-EBUSY`.

---

## 2. Master IOCTL Inventory

```
+------------------------------------------------------------------------------------------------------------------------------------+
| IOCTL Symbol                 | Direction | Code | Payload Type                   | Size | Hardware Operation Executed              |
+------------------------------+-----------+------+--------------------------------+------+------------------------------------------+
| FTE3600_IOC_GET_STATE        | _IOR      | 0x01 | struct fte3600_state_snapshot  | 32 B | None (Passive kernel telemetry read)     |
| FTE3600_IOC_RESET            | _IO       | 0x02 | None (void)                    |  0 B | Pad 14 active-low 10/20/10 ms pulse      |
| FTE3600_IOC_INIT             | _IOWR     | 0x03 | struct fte3600_init_params     | 32 B | Clock ungate, ID read, or full post-ID   |
| FTE3600_IOC_CAPTURE          | _IOWR     | 0x04 | struct fte3600_frame_buffer    | 32 B | Matrix scan trigger & 10,246B FIFO read  |
| FTE3600_IOC_ARM_TOUCH        | _IOWR     | 0x05 | struct fte3600_touch_params    | 32 B | Bank 0x00E0 threshold write & Arm strobe|
| FTE3600_IOC_GET_FRAME_INFO   | _IOR      | 0x06 | struct fte3600_frame_info      | 48 B | None (Queries matrix dimensions & seq)   |
+------------------------------------------------------------------------------------------------------------------------------------+
```

---

## 3. Data Structure Specifications & Binary Layouts

### 3.1 `struct fte3600_state_snapshot` (Opcode `0x01`)
```c
struct fte3600_state_snapshot {
    __u32 version;       /* Offset 0:  Must equal FTE3600_UAPI_VERSION */
    __u32 state;         /* Offset 4:  Current kernel state (enum fte3600_state) */
    __u32 flags;         /* Offset 8:  Status bitmask (OFFLINE, TOUCH_DETECTED) */
    __u32 last_error;    /* Offset 12: Last errno or bus error code */
    __u32 spi_transfers; /* Offset 16: Cumulative hardware SPI transactions */
    __u32 reserved[3];   /* Offset 20: Reserved, must be 0 */
};                       /* Total Size: 32 bytes (Natural 64-bit alignment) */
```
- **Direction:** `_IOR('F', 0x01, struct fte3600_state_snapshot)` -> Numeric: `0x80204601`
- **Prerequisites:** Device node open. No state prerequisite.
- **Hardware Operations:** **None.** Reads cached kernel driver fields under `data->lock`.
- **Validation:** `version == FTE3600_UAPI_VERSION`, `reserved == 0`.
- **Error Codes:** `-EFAULT` (copy to user failed), `-EINVAL` (version or reserved mismatch).

---

### 3.2 `FTE3600_IOC_RESET` (Opcode `0x02`)
```c
#define FTE3600_IOC_RESET _IO('F', 0x02) /* Numeric: 0x00004602 */
```
- **Direction:** `_IO` (No payload).
- **Prerequisites:** Device node open, driver online.
- **Hardware Operations:** Active-low hardware reset pulse on Intel PCH Pad 14:
  1. Set Pad 14 logical 0 (physical HIGH / inactive) for 10 ms.
  2. Set Pad 14 logical 1 (physical LOW / asserted) for 20 ms.
  3. Set Pad 14 logical 0 (physical HIGH / inactive) for 10 ms.
- **State Transition:** Enters `FTE3600_STATE_RESETTING` during pulse; transitions to `FTE3600_STATE_RESET_SEQUENCE_ISSUED` on completion.
- **Error Codes:** `-ENODEV` (driver offline / unbinding), `-ERESTARTSYS` (signal interrupted while acquiring mutex).

---

### 3.3 `struct fte3600_init_params` (Opcode `0x03`)
```c
#define FTE3600_INIT_FLAG_IDENTIFY_ONLY 0x00000000
#define FTE3600_INIT_FLAG_FULL_INIT     0x00000001

struct fte3600_init_params {
    __u32 version;       /* Offset 0:  Must equal FTE3600_UAPI_VERSION */
    __u32 flags;         /* Offset 4:  IDENTIFY_ONLY or FULL_INIT */
    __u16 chip_id;       /* Offset 8:  Returned hardwired ID (expected 0x9362) */
    __u16 wire_crc;      /* Offset 10: Returned wire CRC (expected 0x1C53) */
    __u8  foundry_code;  /* Offset 12: Returned fab code (0xAA) */
    __u8  calibrated_dac;/* Offset 13: Initial or calibrated DAC (nominal 27/56) */
    __u16 reserved1;     /* Offset 14: Reserved for alignment, must be 0 */
    __u32 reserved2[4];  /* Offset 16: Reserved, must be 0 */
};                       /* Total Size: 32 bytes (Natural 64-bit alignment) */
```
- **Direction:** `_IOWR('F', 0x03, struct fte3600_init_params)` -> Numeric: `0xC0204603`
- **Prerequisites:**
  * If `flags == IDENTIFY_ONLY`: Driver must be in `RESET_SEQUENCE_ISSUED`.
  * If `flags == FULL_INIT`: Driver must be in `RESET_SEQUENCE_ISSUED` or `IDENTIFIED`.
- **Hardware Operations:**
  * Mode A (`IDENTIFY_ONLY`): Writes `0xC6 = 0x01`, 4 ms delay, reads back `0xC6`, reads `0x1A8B`, validates CRC `0x1C53`. Transitions to `FTE3600_STATE_IDENTIFIED`.
  * Mode B (`FULL_INIT`): Executes complete 32-step post-ID sequence: wake handshake `{0x5A, 0xA5}`, foundry interrogation (`0xF1 = 3/0x13`, `0xF4`, `0x9B`), sets initial AFE baseline `0x1801`, unlocks bank `0x9A = 0x5A`, writes trims `0x00C0`, relocks bank. Transitions to `FTE3600_STATE_INITIALIZED`.
- **Error Codes:** `-EPERM` (invalid state prerequisite), `-EBADMSG` (CRC mismatch), `-EIO` (readback mismatch), `-ETIMEDOUT` (controller unresponsive).

---

### 3.4 `struct fte3600_touch_params` (Opcode `0x05`)
```c
#define FTE3600_TOUCH_MODE_DISARM  0x00000000
#define FTE3600_TOUCH_MODE_ARM     0x00000001

struct fte3600_touch_params {
    __u32 version;       /* Offset 0:  Must equal FTE3600_UAPI_VERSION */
    __u32 mode;          /* Offset 4:  DISARM (0) or ARM (1) */
    __u16 thresholds[8]; /* Offset 8:  Calibrated touch threshold vector (16B) */
    __u32 timeout_ms;    /* Offset 24: Max wait timeout (0 = non-blocking) */
    __u32 reserved;      /* Offset 28: Reserved, must be 0 */
};                       /* Total Size: 32 bytes (Natural 64-bit alignment) */
```
- **Direction:** `_IOWR('F', 0x05, struct fte3600_touch_params)` -> Numeric: `0xC0204605`
- **Prerequisites:** Driver must be in `FTE3600_STATE_INITIALIZED`.
- **Hardware Operations:**
  * If `mode == ARM`: Unlocks bank `0x9A = 0x5A`, writes 8 threshold words to bank address `0x00E0`, relocks bank `0x9A = 0x00`, programs touch rate `0x1881`, enables Pad 229 IRQ `0x1A83 = val | 0x16`, clears latches `0x1A84 = 0x002F`, transmits Arm strobe `{0xC2, 0x3D}`. Transitions to `FTE3600_STATE_WAITING_FOR_TOUCH`.
  * If `mode == DISARM`: Transmits Abort strobe `{0xC0, 0x3F}`, disables IRQ routing, restores `FTE3600_STATE_INITIALIZED`.
- **Error Codes:** `-EPERM` (not in INITIALIZED), `-EINVAL` (thresholds out of range $[300..700]$).

---

### 3.5 `struct fte3600_frame_buffer` (Opcode `0x04`)
```c
struct fte3600_frame_buffer {
    __u32 version;       /* Offset 0:  Must equal FTE3600_UAPI_VERSION */
    __u32 length;        /* Offset 4:  Must equal FTE3600_IMAGE_BYTES (10240) */
    __u64 buffer_ptr;    /* Offset 8:  Userspace pointer to 10,240-byte buffer */
    __u32 flags;         /* Offset 16: Capture flags (e.g. non-blocking) */
    __u32 reserved[3];   /* Offset 20: Reserved, must be 0 */
};                       /* Total Size: 32 bytes (Natural 64-bit alignment) */
```
- **Direction:** `_IOWR('F', 0x04, struct fte3600_frame_buffer)` -> Numeric: `0xC0204604`
- **Prerequisites:** Driver must be in `FTE3600_STATE_INITIALIZED` or `FTE3600_STATE_TOUCH_DETECTED`.
- **Hardware Operations:**
  1. Writes acquisition mode `0x1800 = 0x4FFE` and prescaler dividers.
  2. Transmits Trigger strobe `{0xC4, 0x3B}` (`FTE3600_STATE_CAPTURING`).
  3. Polls status register `0x80 == 0x54` (max 10 iterations, $1\,\text{ms}$ delay) or awaits frame IRQ.
  4. Enables FIFO readout: `0x1800 |= 0x0001`.
  5. Executes 10,246-byte burst SPI transfer: `{0x06, 0xF9, 0x9A, 0x05, 0x14, 0x00}` + dummy clocks.
  6. Acknowledges capture complete latch: `write_word(0x1A84, 0x0020)`.
  7. Copies 10,240 payload bytes to `buffer_ptr` via `copy_to_user()`.
  8. Transitions to `FTE3600_STATE_INITIALIZED`.
- **Error Codes:** `-EPERM` (not ready), `-EINVAL` (length != 10240), `-EFAULT` (bad user pointer), `-ETIMEDOUT` (matrix scan timed out).

---

### 3.6 `struct fte3600_frame_info` (Opcode `0x06`)
```c
struct fte3600_frame_info {
    __u32 version;       /* Offset 0:  Must equal FTE3600_UAPI_VERSION */
    __u32 width;         /* Offset 4:  Matrix columns (64) */
    __u32 height;        /* Offset 8:  Matrix rows (80) */
    __u32 bits_per_pixel;/* Offset 12: Pixel resolution (16 bits) */
    __u32 frame_bytes;   /* Offset 16: Byte payload (10240) */
    __u32 sequence_nr;   /* Offset 20: Monotonic frame counter */
    __u64 timestamp_ns;  /* Offset 24: CLOCK_MONOTONIC acquisition timestamp */
    __u32 reserved[4];   /* Offset 32: Reserved, must be 0 */
};                       /* Total Size: 48 bytes (Natural 64-bit alignment) */
```
- **Direction:** `_IOR('F', 0x06, struct fte3600_frame_info)` -> Numeric: `0x80304606`
- **Prerequisites:** Device node open.
- **Hardware Operations:** **None.** Returns immutable silicon matrix geometry and telemetry.
- **Error Codes:** `-EFAULT`, `-EINVAL`.

---

## 4. 32-bit Compatibility and Kernel-Userspace ABI Safety

1. **Natural Alignment Guarantee:**
   All 64-bit members (`buffer_ptr`, `timestamp_ns`) are positioned at 8-byte offsets from the start of the structure. No implicit padding bytes exist. A 32-bit compiled binary (e.g. GCC `-m32`) produces the identical struct layout and sizeof value as a 64-bit binary.
2. **`compat_ioctl` Direct Mapping:**
   Because all structures have identical binary layouts on 32-bit and 64-bit platforms, `fte3600_cdev_compat_ioctl()` can directly invoke `fte3600_ioctl_common()` without struct re-packing or pointer translation thunks:
   ```c
   #ifdef CONFIG_COMPAT
   static long fte3600_cdev_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
   {
       return fte3600_ioctl_common(filp, cmd, (unsigned long)compat_ptr(arg));
   }
   #endif
   ```
3. **Defense Against Malformed Inputs:**
   Every ioctl validates:
   - Kernel memory boundary: `access_ok(arg, sizeof(struct ...))`.
   - Structural version: `params.version == FTE3600_UAPI_VERSION`.
   - Strict zero-reserved validation: Loop over `reserved[]` fields; any non-zero value returns `-EINVAL`.
   - Length verification: Captures must request exactly `10240` bytes.
   - Pointers: Userspace destination pointers are checked with `copy_to_user()`.
