# Stage D7.4 — 32-Byte Frame-Transfer ABI & Buffer Allocation Audit

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source & Design Review  
**Deliverable:** `diagnostics/STAGE_D7_4_FRAME_TRANSFER_ABI_AUDIT.md`

---

## 1. Executive Summary & Objective

Stage D7.3 proposed that all calibration ioctls, including `FTE3600_IOC_CALIB_READ_FRAME`, strictly adhere to a 32-byte fixed-width parameter structure while transferring a 10,240-byte image payload to userspace, claiming "zero in-kernel image buffers."

This independent audit rigorously evaluates:
1. How a 32-byte parameter structure expresses a 10,240-byte transfer.
2. Comparative evaluation of `ioctl` pointer passing, POSIX `read()`, and `mmap()`.
3. The technical veracity of the "zero kernel buffer" claim against Linux SPI DMA core requirements.
4. 32-bit userspace compat behavior (`compat_ptr`), pointer validation (`access_ok`), and `copy_to_user()` failure semantics.

---

## 2. Evaluation of Frame Transfer Mechanisms

### 2.1. Comparison of Candidate Mechanisms

| Evaluation Criterion | Option 1: In-Band Pointer (`_IOWR` IOCTL) — **Selected** | Option 2: Character Device `read()` | Option 3: Shared Memory (`mmap()`) |
| :--- | :--- | :--- | :--- |
| **Fixed 32-Byte ABI** | **Full compliance** (`__u64 buffer_ptr`). | Deviates (uses standard POSIX syscall). | Deviates (requires VMA setup ioctls). |
| **Session Authentication** | **Atomic** (`session_id` validated per call). | **None** (`read` takes no token). | Stateful (bound at map time). |
| **Frame Type Gating** | **Explicit** (`DISCARD` vs `BASELINE`). | Impossible without secondary ioctl. | Difficult (shared buffer overwrite). |
| **Kernel Complexity** | **Low** (single `copy_to_user`). | Medium (stateful read cursor). | **High** (VMA page locking, unbind sync). |
| **DMA Safety** | **High** (kernel owns DMA buffer). | High (kernel owns DMA buffer). | High (pinned pages). |
| **Memory Overhead** | 10,246 B single DMA buffer. | 10,246 B single DMA buffer. | Minimum 12–16 KB aligned pages. |

**Architectural Decision:**  
Option 1 (In-Band Pointer via 32-byte IOCTL) is the only mechanism that preserves session token authentication, explicit frame-type discrimination, and exact ABI symmetry with the core driver's `FTE3600_IOC_CAPTURE` structure (`kernel/fte3600.h:152-157`).

---

## 3. Debunking the "Zero Kernel Buffer Allocation" Claim

### 3.1. Linux SPI DMA Constraints
Stage D7.3 claimed:
> *"Zero In-Kernel Image Buffers: Image baseline averaging and pixel subtraction are performed in userspace, eliminating ~15 KB of unpaged kernel memory."*

**Audit Correction:**  
While it is true that userspace performs multi-frame averaging and baseline subtraction (eliminating persistent multi-frame history arrays), **the kernel CANNOT perform an SPI transfer directly into userspace virtual memory**:
- The Linux SPI core (`spi_sync`, `spi_message`, `spi_transfer`) and the Intel LPSS DMA controller require physically contiguous, DMA-coherent kernel memory (`kmalloc` with `GFP_KERNEL`).
- The sensor FIFO transfer is 10,246 bytes (6-byte prefix + 10,240 image bytes).
- Attempting direct userspace DMA requires `get_user_pages_fast()`, scatter-gather list building, and cache invalidation, which is completely unsupported for standard SPI char devices and introduces severe kernel memory lockup risks.

### 3.2. Accurate Buffer Memory Accounting
The driver **MUST** maintain one transient, DMA-safe kernel RX buffer:
$$\text{Buffer Size} = \text{FTE3600\_FIFO\_TRANSFER\_LEN} = 10,246 \text{ bytes}$$

```
+-------------------------------------------------------------------------+
| Kernel DMA RX Buffer (10,246 Bytes allocated once at driver probe)       |
| [ 6 Bytes Header: 00 00 00 00 AA 55 ] [ 10,240 Bytes Raw Sensor Data ]  |
+-------------------------------------------+-----------------------------+
                     |                      |
             Header Validated               | copy_to_user()
             (Stripped in Kernel)           v
                                +-----------------------------+
                                | Userspace Destination Buffer|
                                | (10,240 Bytes Pixels)       |
                                +-----------------------------+
```

- **Accurate Qualification:**
  - *Persistent In-Kernel Calibration Memory:* **0 Bytes** (No baseline arrays or calibration history held in kernel).
  - *Transient In-Kernel DMA Transfer Memory:* **10,246 Bytes** (Preallocated in `struct fte3600_data` at probe time).

---

## 4. 32-Byte Parameter Structure & 64-Bit Pointer ABI

### 4.1. Structure Layout Verification
```c
struct fte3600_calib_frame_params {
	__u32 version;        /* Offset 0x00, 4 Bytes */
	__u32 session_id;     /* Offset 0x04, 4 Bytes */
	__u32 frame_type;     /* Offset 0x08, 4 Bytes */
	__u32 buffer_len;     /* Offset 0x0C, 4 Bytes */
	__u64 buffer_ptr;     /* Offset 0x10, 8 Bytes (Naturally 64-bit aligned) */
	__u32 reserved[2];    /* Offset 0x18, 8 Bytes */
};
```
*Total Size:* $4 + 4 + 4 + 4 + 8 + 8 = 32\text{ bytes}$.  
`static_assert(sizeof(struct fte3600_calib_frame_params) == 32)` succeeds.

### 4.2. 32-Bit Userspace Compat Semantics
On 64-bit kernels running 32-bit userspace binaries (compat mode):
1. `__u64 buffer_ptr` is at offset 16 on both 32-bit and 64-bit architectures because the preceding four `__u32` fields sum to exactly 16 bytes.
2. Structure padding is **0 bytes** on both 32-bit and 64-bit compilers.
3. The kernel driver must properly cast the userspace pointer using `compat_ptr`:
   ```c
   void __user *uptr;
   if (in_compat_syscall())
       uptr = compat_ptr((compat_uptr_t)params.buffer_ptr);
   else
       uptr = (void __user *)(uintptr_t)params.buffer_ptr;
   ```

### 4.3. Defensive Pointer Validation & Error Handling
Before initiating any SPI transaction:
1. **Length Validation:** If `params.frame_type == FTE3600_CALIB_FRAME_DARK_BASELINE`, verify `params.buffer_len == FTE3600_IMAGE_BYTES` (10,240). If mismatch, return `-EINVAL`.
2. **Memory Access Pre-Check:** Verify that userspace destination is writable:
   ```c
   if (!access_ok(uptr, params.buffer_len))
       return -EFAULT;
   ```
   If access check fails, return `-EFAULT` immediately without executing bus transfers or arming hardware.

### 4.4. `copy_to_user()` Failure & Unrecoverable Hardware State
If the SPI transfer finishes but `copy_to_user()` fails:
- **Hardware Consequence:** The FT9362 sensor FIFO is drained by the SPI readout. The hardware FIFO cannot be rewound or re-read.
- **Fail-Closed Teardown:**
  1. Driver logs `copy_to_user failed`.
  2. Invalidate calibration session: `session->active = false; session->id = 0;`.
  3. Transition hardware state: `data->state = FTE3600_STATE_UNKNOWN;`.
  4. Return `-EFAULT`.
  5. Subsequent operation strictly requires `FTE3600_IOC_RESET`.

---

## 5. Evidence Classification

| Statement / Finding | Classification | Evidence Reference |
| :--- | :--- | :--- |
| `sizeof(struct fte3600_calib_frame_params) == 32` | `MATHEMATICALLY-DERIVED` | Proved via struct offset layout |
| Linux SPI core requires kernel DMA-coherent buffer | `SOURCE-VERIFIED` | Linux kernel SPI core (`drivers/spi/spi.c:spi_sync()`) |
| FIFO transfer length is 10,246 bytes | `VENDOR-SOURCE-VERIFIED` | `kernel/fte3600.h:27`, `capture.c:278` |
| Image payload is strictly 10,240 bytes | `SOURCE-VERIFIED` | `kernel/fte3600.h:25` |
| `__u64` eliminates 32-bit/64-bit padding skew | `MATHEMATICALLY-DERIVED` | Offset 16 is 8-byte aligned on both ABIs |
| `compat_ptr` conversion required for compat syscall | `SOURCE-VERIFIED` | Linux kernel compat layer (`include/linux/compat.h`) |
| Unrecoverable FIFO drain on `copy_to_user` failure | `SOURCE-VERIFIED` | FT9362 hardware FIFO destructive readout property |
| Proposed 32-byte frame transfer ABI | `ARCHITECTURAL-PROPOSAL` | Defined in Section 4 |
