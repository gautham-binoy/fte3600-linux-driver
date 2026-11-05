# Stage D7.4 — Proposed Calibration UAPI Compatibility & Encoding Audit

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source & Design Review  
**Deliverable:** `diagnostics/STAGE_D7_4_UAPI_COMPATIBILITY_AUDIT.md`

---

## 1. Executive Summary & Objective

In Linux kernel programming, ioctl ABI changes must maintain binary compatibility, prevent command number collisions, eliminate structure padding variations across architectures, and enforce defensive parameter validation.

This independent audit rigorously evaluates the proposed calibration ioctls (`0x20`–`0x25`):
1. Verifies collision-freedom against existing commands (`0x01`–`0x04`).
2. Validates Linux 32-bit ioctl macro encodings (`_IOWR`).
3. Confirms structure sizes (strictly 32 bytes) and 64-bit natural alignment.
4. Audits 32-bit userspace compatibility (`compat_ioctl` / `compat_ptr`).
5. Establishes uniform error code mappings across all commands.

---

## 2. Command Number & Namespace Verification

### 2.1. Collision Check against Core UAPI
The core driver defines four commands under magic character `'F'` (`0x46`) in `kernel/fte3600.h:160-163`:
- `0x01`: `FTE3600_IOC_GET_STATE` (`_IOR`)
- `0x02`: `FTE3600_IOC_RESET` (`_IO`)
- `0x03`: `FTE3600_IOC_INIT` (`_IOWR`)
- `0x04`: `FTE3600_IOC_CAPTURE` (`_IOWR`)

### 2.2. Proposed Calibration Command Allocation
The proposed calibration commands are assigned to the `0x20`–`0x25` range:
- `0x20`: `FTE3600_IOC_CALIB_BEGIN`
- `0x21`: `FTE3600_IOC_CALIB_FDT_SAMPLE`
- `0x22`: `FTE3600_IOC_CALIB_READ_FRAME`
- `0x23`: `FTE3600_IOC_CALIB_SET_THRESHOLDS`
- `0x24`: `FTE3600_IOC_CALIB_COMMIT`
- `0x25`: `FTE3600_IOC_CALIB_ABORT`

**Verification:**  
- The command sets are completely disjoint.
- A 27-number buffer (`0x05` through `0x1F`) is preserved for future core driver expansion.
- All proposed commands use magic `'F'` (`0x46`), preserving driver namespace consistency.

---

## 3. Linux IOCTL Macro Encoding & Bitfield Dissection

Under the standard Linux ioctl numbering scheme (`asm-generic/ioctl.h`), ioctl command numbers are 32-bit unsigned integers structured as:

$$\begin{array}{|c|c|c|c|}
\hline
\text{Bits 31..30 (2b)} & \text{Bits 29..16 (14b)} & \text{Bits 15..8 (8b)} & \text{Bits 7..0 (8b)} \\
\hline
\text{Direction (\_IOC\_DIR)} & \text{Payload Size (\_IOC\_SIZE)} & \text{Type / Magic (\_IOC\_TYPE)} & \text{Sequence Number (\_IOC\_NR)} \\
\hline
\end{array}$$

### 3.1. Complete Encoding Table

| Macro Name | Direction Bits | Payload Struct | Size (Bytes) | Magic | Nr | 32-Bit Hex Value |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `FTE3600_IOC_CALIB_BEGIN` | `_IOWR` (`11_2 = 3`) | `struct fte3600_calib_begin_params` | 32 (`0x0020`) | `'F'` (`0x46`) | `0x20` | `0xC0204620` |
| `FTE3600_IOC_CALIB_FDT_SAMPLE` | `_IOWR` (`11_2 = 3`) | `struct fte3600_calib_fdt_params` | 32 (`0x0020`) | `'F'` (`0x46`) | `0x21` | `0xC0204621` |
| `FTE3600_IOC_CALIB_READ_FRAME` | `_IOWR` (`11_2 = 3`) | `struct fte3600_calib_frame_params` | 32 (`0x0020`) | `'F'` (`0x46`) | `0x22` | `0xC0204622` |
| `FTE3600_IOC_CALIB_SET_THRESHOLDS`| `_IOWR` (`11_2 = 3`) | `struct fte3600_calib_threshold_params` | 32 (`0x0020`) | `'F'` (`0x46`) | `0x23` | `0xC0204623` |
| `FTE3600_IOC_CALIB_COMMIT` | `_IOWR` (`11_2 = 3`) | `struct fte3600_calib_commit_params` | 32 (`0x0020`) | `'F'` (`0x46`) | `0x24` | `0xC0204624` |
| `FTE3600_IOC_CALIB_ABORT` | `_IOWR` (`11_2 = 3`) | `struct fte3600_calib_abort_params` | 32 (`0x0020`) | `'F'` (`0x46`) | `0x25` | `0xC0204625` |

**Verification:**  
Every command number encodes exactly size 32 (`0x0020`) and direction `_IOWR`, guaranteeing uniform VFS copy-in/copy-out behavior.

---

## 4. Structure Layout & 32-Bit Compat Verification

### 4.1. Structure Sizing & Alignment
All 6 structures strictly utilize fixed-width types (`__u32`, `__u16`, `__u64`):
1. **`fte3600_calib_begin_params`:** $4 + 4 + 4 + 4 + 16 = 32\text{ bytes}$.
2. **`fte3600_calib_fdt_params`:** $4 + 4 + 8 + 4 + 12 = 32\text{ bytes}$.
3. **`fte3600_calib_frame_params`:** $4 + 4 + 4 + 4 + 8 + 8 = 32\text{ bytes}$.
   - Field `buffer_ptr` (`__u64`) is located at byte offset `16` (`0x10`).
   - Byte offset 16 is a multiple of 8, ensuring natural 64-bit alignment without hidden compiler padding.
4. **`fte3600_calib_threshold_params`:** $4 + 4 + 8 + 8 + 4 + 4 = 32\text{ bytes}$.
5. **`fte3600_calib_commit_params`:** $4 + 4 + 4 + 20 = 32\text{ bytes}$.
6. **`fte3600_calib_abort_params`:** $4 + 4 + 4 + 4 + 16 = 32\text{ bytes}$.

### 4.2. Compat Sycall Analysis
On x86_64 Linux, when a 32-bit userspace binary (e.g. `x86_32` process) executes an ioctl:
- Because every field is fixed-width and `__u64` is naturally placed at offset 16, the **struct layout, size, and ioctl numbers are identical on both 32-bit and 64-bit systems**.
- **No thunking structure translation is required.**
- In `kernel/fte3600.c`, `compat_ioctl` can simply point directly to `fte3600_cdev_ioctl`:
  ```c
  .compat_ioctl = fte3600_cdev_ioctl,
  ```
- The only necessary check is converting `params.buffer_ptr` via `compat_ptr` when `in_compat_syscall()` is true.

---

## 5. Defensive Validation Contract

Every proposed ioctl handler must execute the following validation steps in strict sequence before taking locks or touching the bus:

1. **Version Validation:**
   ```c
   if (params.version != FTE3600_UAPI_VERSION)
       return -EINVAL;
   ```
2. **Reserved Field Validation:**
   All reserved fields must be verified zero:
   ```c
   for (i = 0; i < ARRAY_SIZE(params.reserved); ++i) {
       if (params.reserved[i] != 0)
           return -EINVAL;
   }
   ```
3. **Unknown Flag Rejection:**
   Any undefined flag bits must return `-EINVAL`:
   ```c
   if (params.flags & ~FTE3600_CALIB_FLAGS_MASK)
       return -EINVAL;
   ```

---

## 6. Standardized Error Code Consistency

| Error Code | Symbolic Value | Precise Trigger Condition |
| :--- | :--- | :--- |
| **`-EINVAL`** | 22 | ABI version mismatch, non-zero reserved fields, invalid flags, out-of-order FDT index, or out-of-range thresholds ($T_{\text{touch}} > T_{\text{release}}$). |
| **`-EPERM`** | 1 | Driver state mismatch (not in `INITIALIZED_UNCALIBRATED`), caller not lease owner, invalid/stale `session_id`, or incomplete calibration stages during `COMMIT`. |
| **`-EBUSY`** | 16 | Calling `CALIB_BEGIN` while another calibration session is already active. |
| **`-ETIMEDOUT`**| 110 | Watchdog inactivity deadline (5.0s) expired, or hardware sensor status polling loop timeout. |
| **`-EFAULT`** | 14 | Memory access check (`access_ok`) failed, or `copy_to_user()` failed during baseline frame delivery. |
| **`-EIO`** | 5 | Physical SPI transfer failure, CRC mismatch, or unacknowledged FIFO read. |
| **`-ENODEV`** | 19 | Device unbound from SPI bus (`FTE3600_FLAG_OFFLINE`). |
| **`-EOPNOTSUPP`**| 95 | Capture attempted via `FTE3600_IOC_CAPTURE` (stub preserved). |

---

## 7. Evidence Classification

| Statement / Finding | Classification | Evidence Reference |
| :--- | :--- | :--- |
| Existing command numbers are `0x01`–`0x04` | `SOURCE-VERIFIED` | `kernel/fte3600.h:160-163` |
| Proposed numbers `0x20`–`0x25` are collision-free | `MATHEMATICALLY-DERIVED` | Proved via integer range disjointness |
| Linux `_IOWR` ioctl encoding bitfield format | `SOURCE-VERIFIED` | `include/uapi/asm-generic/ioctl.h` |
| All structs strictly 32 bytes wide | `MATHEMATICALLY-DERIVED` | Formally proven via field byte offsets |
| Offset 16 alignment eliminates 32-bit compat divergence | `MATHEMATICALLY-DERIVED` | Proved via ABI alignment rules |
| Error code mapping and defensive zero-reserved validation | `ARCHITECTURAL-PROPOSAL` | Specified in Sections 5 and 6 |
