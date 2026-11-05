# STAGE D6.5: UAPI FLAG COMPATIBILITY AND ABI STABILITY AUDIT

**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Workspace:** `/home/titan/fte3600-linux-driver/`  
**Safety Status:** STRICTLY READ-ONLY SOURCE AUDIT. Zero hardware communication.  

---

## 1. Executive Summary

Stage D6.4 introduced two new telemetry flags in `kernel/fte3600.h`:
- `FTE3600_FLAG_STARTUP_PREFIX_COMPLETED` (`1U << 2`, value `0x00000004U`)
- `FTE3600_FLAG_CALIBRATION_PENDING` (`1U << 3`, value `0x00000008U`)

This audit evaluates the historical evolution of the UAPI flags, tests for bit collisions, investigates whether legacy user-space applications are affected, and draws a formal distinction between **binary ABI compatibility** and **semantic behavioral compatibility**.

---

## 2. Historical Evolution and Bitfield Allocation

The status flags are exposed to userspace through `struct fte3600_state_snapshot.flags` via `ioctl(fd, FTE3600_IOC_GET_STATE, &snap)`. The historical evolution of this 32-bit field across driver stages is documented below:

| Stage / Milestone | Introduced Flags | Bit Value | Hex Representation | Purpose | Evidence Tag |
|---|---|---|---|---|---|
| **Stage 2E.2** (Commit `6661e60`) | `FTE3600_FLAG_OFFLINE` | `1U << 0` | `0x00000001U` | Terminal device removal / unbind signal | `[SOURCE-OBSERVED]` |
| **Stage 2E.4.2** | `FTE3600_FLAG_RESET_SEQUENCE_ISSUED`<br>`FTE3600_FLAG_RESET_COMPLETED` | `1U << 1`<br>(alias) | `0x00000002U` | Indicates hardware reset pulse completed | `[SOURCE-OBSERVED]` |
| **Stage D6.4** | `FTE3600_FLAG_STARTUP_PREFIX_COMPLETED` | `1U << 2` | `0x00000004U` | Indicates 6-step volatile prefix completed | `[SOURCE-OBSERVED]` |
| **Stage D6.4** | `FTE3600_FLAG_CALIBRATION_PENDING` | `1U << 3` | `0x00000008U` | Indicates analog/noise calibration required | `[SOURCE-OBSERVED]` |
| **Future Phases** | Reserved for IRQ, Touch, FIFO | Bits 4..31 | `0xFFFFFFF0U` | Unassigned / zero | `[SOURCE-OBSERVED]` |

### 2.1 Bit Overlap and Orthogonality Audit
- Bit 0 (`0x01`), Bit 1 (`0x02`), Bit 2 (`0x04`), and Bit 3 (`0x08`) occupy mutually disjoint power-of-two bit positions.
- **Result:** **ZERO BIT COLLISION**. All 4 defined flags are completely orthogonal.

### 2.2 Header Macro Redundancy Observation
In `kernel/fte3600.h`:
- Line 108: `#define FTE3600_FLAG_RESET_COMPLETED            FTE3600_FLAG_RESET_SEQUENCE_ISSUED`
- Line 111: `#define FTE3600_FLAG_RESET_COMPLETED        FTE3600_FLAG_RESET_SEQUENCE_ISSUED`
The compatibility alias `FTE3600_FLAG_RESET_COMPLETED` is defined twice within 4 lines. While this is benign in C preprocessor semantics (identical token replacement), it represents a source-level redundancy that should be cleaned up in a future maintenance pass.

---

## 3. Binary ABI Compatibility vs. Semantic Compatibility

### 3.1 Binary ABI Compatibility: 100% PRESERVED
Binary ABI compatibility requires that existing compiled binaries can invoke ioctls without memory corruption, stack misalignment, or parameter dislocation:
1. **Structure Size and Offsets:**  
   `struct fte3600_state_snapshot` is exactly 32 bytes in length:
   - `version`: offset 0 (4 bytes)
   - `state`: offset 4 (4 bytes)
   - `flags`: offset 8 (4 bytes)
   - `last_error`: offset 12 (4 bytes)
   - `reserved[4]`: offset 16 (16 bytes)
   Validated by compile-time `static_assert` in `kernel/fte3600.h:167` and `_Static_assert` in `kernel/fte3600-test.c:1842`.
2. **IOCTL Encodings:**  
   `FTE3600_IOC_GET_STATE` is defined as `_IOR('F', 0x01, struct fte3600_state_snapshot)` (`0x80204601`). The magic character (`'F'`), sequence number (`0x01`), direction (`_IOC_READ`), and payload size (`32`) are bit-for-bit identical to v1 UAPI introduction.

### 3.2 Semantic Compatibility: DOCUMENTED LIMITATIONS
Semantic compatibility requires that existing software observing the fields behaves identically or gracefully accommodates new semantics. Two specific semantic considerations were audited:

#### A. Consumer Flag Masking vs. Equality Checking
- **Bitmasking Consumers (`&`):**  
  Consumers such as `tools/fte3600-tool.c` (lines 27–28) inspect individual bits:
  ```c
  if (snap->flags & FTE3600_FLAG_OFFLINE) printf(" [OFFLINE]");
  if (snap->flags & FTE3600_FLAG_RESET_SEQUENCE_ISSUED) printf(" [RESET_ISSUED]");
  ```
  These consumers are completely unaffected by the presence of bits 2 and 3.
- **Strict Equality Consumers (`==`):**  
  If a legacy application asserts `snap->flags == 0` to deduce that no hardware sequencing has occurred, that assertion will evaluate to `false` when the driver enters `INITIALIZED_UNCALIBRATED` (`snap->flags == 0x0000000C`).  
  This is semantically correct (hardware sequencing *has* occurred), but represents a visible semantic shift compared to Stage C where `flags` remained `0x00` in state 8.

#### B. CAPTURE Error Code Semantic Shift
- Prior to Stage D6.4, calling `FTE3600_IOC_CAPTURE` returned `-EOPNOTSUPP` for valid parameters because capture gating had not yet been implemented.
- In `tools/fte3600-tool.c:81`:
  ```c
  if (ioctl(fd, FTE3600_IOC_CAPTURE, &fb) < 0) {
      if (errno == EOPNOTSUPP) {
          printf(">> Capture rejected as expected...\n");
      } else {
          fprintf(stderr, ">> Capture failed with unexpected errno: %s (errno=%d)\n", ...);
          return 1;
      }
  }
  ```
- Following Stage D6.4, capture gating enforces `data->state == FTE3600_STATE_READY`. Because the driver is in `INITIALIZED_UNCALIBRATED`, `FTE3600_IOC_CAPTURE` returns `-EPERM` (`errno == 1`), not `-EOPNOTSUPP` (`errno == 95`).
- **Result:** Running `fte3600-tool capture` against an uncalibrated driver produces exit code 1 because the utility hardcodes an expectation of `EOPNOTSUPP`. This is functionally safer for hardware protection, but is a confirmed semantic deviation from earlier tool mock expectations.

---

## 4. Input Flags in `struct fte3600_init_params`

`struct fte3600_init_params` defines a 32-bit `flags` field at offset 4:
```c
/* Initialization Control Flags (struct fte3600_init_params.flags) */
#define FTE3600_INIT_FLAG_IDENTIFY_ONLY    0x00000000U
#define FTE3600_INIT_FLAG_FULL_STARTUP     (1U << 0)
```

### 4.1 Reserved Array Validation vs. Flag Masking
In `kernel/fte3600.c:704-710`:
```c
if (params.version != FTE3600_UAPI_VERSION)
    return -EINVAL;

for (i = 0; i < 6; i++) {
    if (params.reserved[i] != 0)
        return -EINVAL;
}
```
- `params.version` is verified equal to 1 (`-EINVAL` on mismatch).
- `params.reserved[0..5]` are strictly verified to be zero (`-EINVAL` if non-zero).
- **Audit Observation:** `params.flags` is **not verified** against undefined bits (`params.flags & ~FTE3600_INIT_FLAG_FULL_STARTUP`). Passing `flags = 0xFFFFFFFE` does not return `-EINVAL`. At line 733, the driver checks `if (params.flags & FTE3600_INIT_FLAG_FULL_STARTUP)`: if bit 0 is 0, it silently executes identify-only; if bit 0 is 1, it executes full startup.
- **Classification:** `[SOURCE-OBSERVED]` Non-fatal input leniency. Does not violate ABI stability, but represents an unmasked input field.

---

## 5. Summary Compatibility Matrix

| Feature / Artifact | Binary ABI Compatibility | Semantic Compatibility | Limiting Conditions / Caveats | Evidence Tag |
|---|---|---|---|---|
| `fte3600_state_snapshot` (32 B) | **100% Compatible** | **Compatible** | Offset 8 (`flags`) contains bits 2 & 3 in state 14 | `[SOURCE-OBSERVED]` |
| `fte3600_init_params` (32 B) | **100% Compatible** | **Compatible** | Undefined upper bits in `flags` not rejected with `-EINVAL` | `[SOURCE-OBSERVED]` |
| `fte3600_frame_buffer` (32 B) | **100% Compatible** | **Compatible** | Fixed 32-byte layout; 64-bit aligned user buffer | `[SOURCE-OBSERVED]` |
| `FTE3600_IOC_GET_STATE` | **100% Compatible** | **Compatible** | Returns states `[0..8]`; state 14 maps to public 8 | `[SOURCE-OBSERVED]` |
| `FTE3600_IOC_RESET` | **100% Compatible** | **Compatible** | Active-low GPIO pulse on Pin 14 unchanged | `[SOURCE-OBSERVED]` |
| `FTE3600_IOC_INIT` (zero flags) | **100% Compatible** | **Compatible** | Identifies chip ID 0x9362 without volatile prefix | `[SOURCE-OBSERVED]` |
| `FTE3600_IOC_CAPTURE` | **100% Compatible** | **Changed Errno** | Returns `-EPERM` instead of `-EOPNOTSUPP` in non-READY states | `[SOURCE-OBSERVED]` |
