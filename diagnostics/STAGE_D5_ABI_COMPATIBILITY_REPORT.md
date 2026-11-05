# STAGE D5: ABI COMPATIBILITY AND UAPI VERIFICATION REPORT

## Document Information
- **Project:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`) Native Linux Fingerprint Driver
- **Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`, CS0)
- **Host Controller:** Intel LPSS SPI2 (`pxa2xx-spi.2`) on Linux `7.2.8-9-omarchy` (x86_64 SMP)
- **Author:** Lead Kernel Architect & Safety Auditor (Antigravity Agent)
- **Status:** COMPLETED ABI COMPATIBILITY AUDIT
- **Safety Boundary:** STRICTLY READ-ONLY AUDIT (Zero Hardware Communication)

---

## 1. Executive Summary

Stage D5 strictly mandates that all existing Application Binary Interface (ABI) contracts, data structures, ioctl numbers, and semantic behaviors remain 100% backward compatible with existing userspace binaries.

This report independently verifies every UAPI structure layout, offset, alignment, and command encoding across 32-bit and 64-bit architectures, and confirms the fail-safe behavior of existing v1 applications.

---

## 2. UAPI Structure Layout & Offset Audits

All public UAPI structures are fixed at exactly 32 bytes (256 bits) with zero implicit compiler padding holes.

### 2.1 `struct fte3600_state_snapshot` (Exact 32 Bytes)
```c
struct fte3600_state_snapshot {
    __u32 version;      /* [Offset  0, 4 bytes] ABI version (must equal 1) */
    __u32 state;        /* [Offset  4, 4 bytes] Driver state (enum fte3600_state) */
    __u32 flags;        /* [Offset  8, 4 bytes] Status bitmask (FTE3600_FLAG_*) */
    __u32 last_error;   /* [Offset 12, 4 bytes] Most recent errno or bus fault */
    __u32 reserved[4];  /* [Offset 16, 16 bytes] Reserved for future expansion */
}; /* Total Size: Exactly 32 bytes */
```

#### Field Offset Verification:
- `offsetof(struct fte3600_state_snapshot, version)    == 0`
- `offsetof(struct fte3600_state_snapshot, state)      == 4`
- `offsetof(struct fte3600_state_snapshot, flags)      == 8`
- `offsetof(struct fte3600_state_snapshot, last_error) == 12`
- `offsetof(struct fte3600_state_snapshot, reserved)   == 16`
- `sizeof(struct fte3600_state_snapshot)              == 32`

### 2.2 `struct fte3600_init_params` (Exact 32 Bytes)
```c
struct fte3600_init_params {
    __u32 version;      /* [Offset  0, 4 bytes] ABI version (must equal 1) */
    __u32 flags;        /* [Offset  4, 4 bytes] Initialization control flags */
    __u32 reserved[6];  /* [Offset  8, 24 bytes] Reserved (must be set to 0) */
}; /* Total Size: Exactly 32 bytes */
```

#### Field Offset Verification:
- `offsetof(struct fte3600_init_params, version)  == 0`
- `offsetof(struct fte3600_init_params, flags)    == 4`
- `offsetof(struct fte3600_init_params, reserved) == 8`
- `sizeof(struct fte3600_init_params)            == 32`

### 2.3 `struct fte3600_frame_buffer` (Exact 32 Bytes)
```c
struct fte3600_frame_buffer {
    __u32 version;      /* [Offset  0, 4 bytes] ABI version (must equal 1) */
    __u32 length;       /* [Offset  4, 4 bytes] Buffer length (10240 bytes) */
    __u64 buffer_ptr;   /* [Offset  8, 8 bytes] Userspace destination buffer */
    __u32 reserved[4];  /* [Offset 16, 16 bytes] Reserved (must be set to 0) */
}; /* Total Size: Exactly 32 bytes */
```

#### Field Offset Verification:
- `offsetof(struct fte3600_frame_buffer, version)    == 0`
- `offsetof(struct fte3600_frame_buffer, length)     == 4`
- `offsetof(struct fte3600_frame_buffer, buffer_ptr) == 8` (natural 8-byte alignment)
- `offsetof(struct fte3600_frame_buffer, reserved)   == 16`
- `sizeof(struct fte3600_frame_buffer)              == 32`

---

## 3. IOCTL Command Encodings & Invariants

All four standardized ioctl commands conform strictly to Linux kernel ioctl macros (`_IO`, `_IOR`, `_IOWR`):

| IOCTL Command Macro | Direction | Type | Nr | Size | Encoded Hex Integer | Operational Purpose |
| :--- | :---: | :---: | :---: | :---: | :---: | :--- |
| `FTE3600_IOC_GET_STATE` | `_IOR` (Read) | `'F'` (0x46) | `0x01` | 32 (`0x20`) | **`0x80204601`** | Query driver state snapshot |
| `FTE3600_IOC_RESET` | `_IO` (None) | `'F'` (0x46) | `0x02` | 0 (`0x00`) | **`0x00004602`** | Execute electrical reset pulse |
| `FTE3600_IOC_INIT` | `_IOWR` (Read/Write) | `'F'` (0x46) | `0x03` | 32 (`0x20`) | **`0xC0204603`** | Execute Stage C identification |
| `FTE3600_IOC_CAPTURE` | `_IOWR` (Read/Write) | `'F'` (0x46) | `0x04` | 32 (`0x20`) | **`0xC0204604`** | Image buffer capture |

### Compatibility Invariant:
- `FTE3600_IOC_INIT` evaluates to `0xC0204603` identically across all kernel and userspace compilation environments.
- The command encoding is completely unchanged.

---

## 4. Preservation of Version 1 Application Semantics

### 4.1 Strict Reserved Output Zeroing
A major concern raised in Stages D4.1 and D4.2 was the **Client B hazard**: applications that zero `reserved` fields before calling `FTE3600_IOC_GET_STATE` and subsequently assert that `reserved` remains all zeroes.

- **Kernel Guarantee:** In `kernel/fte3600.c:421`:
  ```c
  memset(&snap, 0, sizeof(snap));
  snap.version = FTE3600_UAPI_VERSION;
  snap.state = (u32)data->state;
  ...
  copy_to_user((void __user *)arg, &snap, sizeof(snap));
  ```
  `memset(&snap, 0, sizeof(snap))` explicitly zeroes all 16 bytes of `snap.reserved[4]` before copying to userspace.
- **Client B Safety:** All reserved output fields are guaranteed to remain strictly `0x00`. Client B assertions never fail.

### 4.2 `FTE3600_IOC_INIT` Input-Only Semantics
- Source inspection of `kernel/fte3600.c:458-486` confirmed that `FTE3600_IOC_INIT` executes `copy_from_user(&params, ...)`, validates `params.version == 1` and `params.reserved[i] == 0`, and executes `fte3600_hw_identify(data)`.
- It **never executes `copy_to_user()`**. It does not overwrite userspace buffers.
- Userspace queries device identification and driver state exclusively through `FTE3600_IOC_GET_STATE`.

### 4.3 Proposed `FTE3600_IOC_IDENTIFY` Status
- **Strict Compliance Decision:** In accordance with Section 3 of the Stage D5 prompt ("Do not introduce FTE3600_IOC_IDENTIFY in this stage unless it is strictly necessary... document it separately as a proposal and do not activate it"), `FTE3600_IOC_IDENTIFY` has **NOT been added to `kernel/fte3600.h`**.
- It remains documented strictly as an architectural proposal for future non-breaking telemetry expansion.

---

## 5. 32-Bit Userspace Compatibility (x86_32 on x86_64)

### 5.1 Natural Alignment Analysis
- In `struct fte3600_frame_buffer`, the `__u64 buffer_ptr` field is positioned at byte offset 8.
- On both x86_32 and x86_64, an 8-byte integer at offset 8 is naturally aligned on an 8-byte boundary.
- There are no compiler-inserted padding differences between 32-bit and 64-bit builds.

### 5.2 Compat IOCTL Handler
In `kernel/fte3600.c`:
```c
#ifdef CONFIG_COMPAT
static long fte3600_cdev_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    return fte3600_ioctl_common(filp, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static const struct file_operations fte3600_fops = {
    .owner          = THIS_MODULE,
    .open           = fte3600_cdev_open,
    .release        = fte3600_cdev_release,
    .unlocked_ioctl = fte3600_cdev_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl   = fte3600_cdev_compat_ioctl,
#endif
    .poll           = fte3600_cdev_poll,
};
```
Because the binary layout of all ioctl structures is identical in 32-bit and 64-bit compilation, `compat_ptr(arg)` seamlessly converts 32-bit userspace pointers without structure thunking overhead.

---

## 6. State Enum Backward Compatibility

| Public State Enum | Integer Value | Description | UAPI v1 Status |
| :--- | :---: | :--- | :---: |
| `FTE3600_STATE_UNINITIALIZED` | 0 | Probe default state | **Unchanged** |
| `FTE3600_STATE_RESETTING` | 1 | Hardware reset pulse active | **Unchanged** |
| `FTE3600_STATE_RESET_SEQUENCE_ISSUED` | 2 | Reset pulse complete | **Unchanged** |
| `FTE3600_STATE_INITIALIZING` | 3 | Calibration in progress | **Unchanged** |
| `FTE3600_STATE_READY` | 4 | Standby mode (canonical) | **Unchanged** |
| `FTE3600_STATE_CAPTURING` | 5 | Image exposure and FIFO read | **Unchanged** |
| `FTE3600_STATE_ERROR` | 6 | Hardware or bus fault | **Unchanged** |
| `FTE3600_STATE_UNKNOWN` | 7 | Fail-closed invalid state | **Unchanged** |
| `FTE3600_STATE_IDENTIFIED` | 8 | Stage C identification verified | **Unchanged** |

### Legacy Aliases:
- `#define FTE3600_STATE_RESET_COMPLETE FTE3600_STATE_RESET_SEQUENCE_ISSUED` (Value 2)
- `#define FTE3600_STATE_INITIALIZED    FTE3600_STATE_READY` (Value 4)

Existing binaries querying state snapshots observe identical integer values.

---

## 7. Audit Verdict

**PASS — ABI COMPATIBILITY 100% PRESERVED**
