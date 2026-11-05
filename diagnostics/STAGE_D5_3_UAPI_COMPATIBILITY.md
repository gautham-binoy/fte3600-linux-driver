# STAGE D5.3: UAPI COMPATIBILITY, ABI LAYOUT AND STATE REMAPPING AUDIT

## Document Information
- **Project:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`) Native Linux Fingerprint Driver
- **Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`, CS0)
- **Host Controller:** Intel LPSS SPI2 (`pxa2xx-spi.2`) on Linux `7.2.8-9-omarchy` (x86_64 SMP)
- **Auditor:** Independent Senior Kernel Auditor (Antigravity Agent)
- **Status:** COMPLETED INDEPENDENT AUDIT
- **Safety Boundary:** STRICTLY READ-ONLY VERIFICATION (Zero Hardware Communication, Zero Live Module Manipulation)

---

## 1. Executive Summary

This audit report provides an exhaustive, adversarial verification of the UAPI compatibility, memory layouts, structure offsets, and state mapping mechanisms in the FTE3600 Linux kernel driver following the Stage D5.2 source remediation.

### Key Audit Conclusions:
1. **Public State Space Strictly Bounded:** The UAPI contract bounds all public states returned by `FTE3600_IOC_GET_STATE` to the discrete range `[0..8]`. Internal state `FTE3600_STATE_DEVICE_REMOVAL` (13) is strictly mapped to `FTE3600_STATE_UNINITIALIZED` (0) with `FTE3600_FLAG_OFFLINE` asserted. Internal state leakage is completely eliminated.
2. **Client Compatibility Confirmed:** The production validation utility (`tools/fte3600-tool.c`) has an explicit array bounds check: `(snap->state <= 8) ? state_names[snap->state] : "INVALID"`. The remapping prevents `"INVALID"` state reporting, correctly outputting `State: 0 (UNINITIALIZED)` with `Flags: 0x00000001 [OFFLINE]`.
3. **`FTE3600_FLAG_OFFLINE` Historical Continuity:** Git history inspection confirms that `FTE3600_FLAG_OFFLINE` (`1U << 0`) has existed since the initial UAPI commit (`6661e605fb6a`). Its numeric value and semantic meaning are identical. Once set during driver removal, no code path can clear or overwrite this flag.
4. **Binary Layout Invariance (32 Bytes):** All three UAPI structures (`fte3600_state_snapshot`, `fte3600_init_params`, `fte3600_frame_buffer`) are exactly 32 bytes in size. Field offsets and alignments are bit-for-bit identical across 32-bit and 64-bit architectures, with zero padding holes.
5. **Information Disclosure Prevention:** The kernel driver guarantees `memset(&snap, 0, sizeof(snap))` prior to populating user-visible fields, ensuring that all 16 reserved bytes are strictly zeroed out and preventing kernel stack telemetry leakage.
6. **IOCTL Encoding Stability:** Command encodings for all four ioctls (`0x80204601`, `0x00004602`, `0xc0204603`, `0xc0204604`) remain 100% identical to the v1 specification.

---

## 2. Public Removal-State Mapping Verification

### 2.1 State Isolation Analysis
In Stage D5, `fte3600_remove()` set `data->state = FTE3600_STATE_DEVICE_REMOVAL` (13). When an existing open file descriptor issued `FTE3600_IOC_GET_STATE`, `data->state` was cast directly to `u32` and copied to userspace.

Stage D5.2 remedied this by introducing `fte3600_state_to_uapi()` in `kernel/fte3600.h:73-96`:

```c
#define FTE3600_MAX_PUBLIC_STATE     8

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
```

### 2.2 Driver IOCTL Integration
In `kernel/fte3600.c:419-432`, the GET_STATE handler was verified:
```c
	case FTE3600_IOC_GET_STATE: {
		struct fte3600_state_snapshot snap;

		memset(&snap, 0, sizeof(snap));
		snap.version = FTE3600_UAPI_VERSION;
		snap.state = fte3600_state_to_uapi(data->state);
		if (atomic_read(&data->offline) || data->state == FTE3600_STATE_DEVICE_REMOVAL)
			snap.flags |= FTE3600_FLAG_OFFLINE;
		if (data->state == FTE3600_STATE_RESET_SEQUENCE_ISSUED)
			snap.flags |= FTE3600_FLAG_RESET_SEQUENCE_ISSUED;
		snap.last_error = data->last_error;
		if (copy_to_user((void __user *)arg, &snap, sizeof(snap)))
			return -EFAULT;
		return 0;
	}
```

### 2.3 Verification of `FTE3600_FLAG_OFFLINE`
- **History & Provenance:**
  Git log inspection confirms `FTE3600_FLAG_OFFLINE` was introduced in commit `6661e605fb6a307654d146cf5cb69be40fc2f4ad` (Author: Gautham Binoy, Nov 3 2026):
  ```c
  #define FTE3600_FLAG_OFFLINE         (1U << 0)
  ```
- **Numeric Stability:** The value remains `(1U << 0)` (0x00000001). No bit shifting or renumbering has occurred.
- **Immutability After Removal:** In `fte3600_remove()`, `atomic_set(&data->offline, 1)` and `data->state = FTE3600_STATE_DEVICE_REMOVAL` are executed under `data->lock`. Once set, no driver function can reset `offline` to 0 or change `state` back to an online state.
- **Distinguishability:**
  Userspace clients distinguish genuine uninitialized state from device removal cleanly:
  * **Genuine Initial State:** `snap.state == FTE3600_STATE_UNINITIALIZED` (0), `(snap.flags & FTE3600_FLAG_OFFLINE) == 0`.
  * **Post-Removal Teardown State:** `snap.state == FTE3600_STATE_UNINITIALIZED` (0), `(snap.flags & FTE3600_FLAG_OFFLINE) == 1`.

### 2.4 Client Utility Verification (`tools/fte3600-tool.c`)
Inspection of `tools/fte3600-tool.c:14-32` shows:
```c
static void print_state(const struct fte3600_state_snapshot *snap)
{
	const char *state_names[] = {
		"UNINITIALIZED", "RESETTING", "RESET_SEQUENCE_ISSUED",
		"INITIALIZING", "READY", "CAPTURING", "ERROR", "UNKNOWN",
		"IDENTIFIED"
	};
	const char *name = (snap->state <= 8) ? state_names[snap->state] : "INVALID";

	printf(">> FTE3600 State Snapshot:\n");
	printf("   Version:    %u\n", snap->version);
	printf("   State:      %u (%s)\n", snap->state, name);
	printf("   Flags:      0x%08X", snap->flags);
	if (snap->flags & FTE3600_FLAG_OFFLINE) printf(" [OFFLINE]");
	if (snap->flags & FTE3600_FLAG_RESET_SEQUENCE_ISSUED) printf(" [RESET_ISSUED]");
	printf("\n");
	printf("   Last Error: %u (%s)\n", snap->last_error,
	       snap->last_error ? strerror(snap->last_error) : "None");
}
```
- **Prior to D5.2:** `snap->state` was `13`. `snap->state <= 8` evaluated to `false`, causing `name` to resolve to `"INVALID"`.
- **Under D5.2:** `snap->state` is `0`. `snap->state <= 8` evaluates to `true`, resolving to `state_names[0]` (`"UNINITIALIZED"`). Concurrently, `snap->flags & FTE3600_FLAG_OFFLINE` evaluates to true, outputting `[OFFLINE]`.
- **Result:** The output is completely formatted, accurate, and informative:
  ```text
  >> FTE3600 State Snapshot:
     Version:    1
     State:      0 (UNINITIALIZED)
     Flags:      0x00000001 [OFFLINE]
     Last Error: 0 (None)
  ```

---

## 3. Structure Layouts, Field Offsets and Padding Analysis

All UAPI structures are defined with fixed-width integers (`__u32`, `__u64`) to guarantee deterministic memory layouts across compiler versions and architectures.

### 3.1 `struct fte3600_state_snapshot` (Total Size: 32 Bytes)

```c
struct fte3600_state_snapshot {
	__u32 version;       /* Offset 0,  Size 4 */
	__u32 state;         /* Offset 4,  Size 4 */
	__u32 flags;         /* Offset 8,  Size 4 */
	__u32 last_error;    /* Offset 12, Size 4 */
	__u32 reserved[4];   /* Offset 16, Size 16 */
};
```

| Field Name | Type | Offset (Bytes) | Size (Bytes) | Alignment Requirement | Padding Following |
| :--- | :---: | :---: | :---: | :---: | :---: |
| `version` | `__u32` | 0 | 4 | 4-byte | None (0) |
| `state` | `__u32` | 4 | 4 | 4-byte | None (0) |
| `flags` | `__u32` | 8 | 4 | 4-byte | None (0) |
| `last_error` | `__u32` | 12 | 4 | 4-byte | None (0) |
| `reserved[0]` | `__u32` | 16 | 4 | 4-byte | None (0) |
| `reserved[1]` | `__u32` | 20 | 4 | 4-byte | None (0) |
| `reserved[2]` | `__u32` | 24 | 4 | 4-byte | None (0) |
| `reserved[3]` | `__u32` | 28 | 4 | 4-byte | None (0) |
| **Total** | | | **32** | **4-byte** | **0 bytes** |

### 3.2 `struct fte3600_init_params` (Total Size: 32 Bytes)

```c
struct fte3600_init_params {
	__u32 version;       /* Offset 0, Size 4 */
	__u32 flags;         /* Offset 4, Size 4 */
	__u32 reserved[6];   /* Offset 8, Size 24 */
};
```

| Field Name | Type | Offset (Bytes) | Size (Bytes) | Alignment Requirement | Padding Following |
| :--- | :---: | :---: | :---: | :---: | :---: |
| `version` | `__u32` | 0 | 4 | 4-byte | None (0) |
| `flags` | `__u32` | 4 | 4 | 4-byte | None (0) |
| `reserved[0]` | `__u32` | 8 | 4 | 4-byte | None (0) |
| `reserved[1]` | `__u32` | 12 | 4 | 4-byte | None (0) |
| `reserved[2]` | `__u32` | 16 | 4 | 4-byte | None (0) |
| `reserved[3]` | `__u32` | 20 | 4 | 4-byte | None (0) |
| `reserved[4]` | `__u32` | 24 | 4 | 4-byte | None (0) |
| `reserved[5]` | `__u32` | 28 | 4 | 4-byte | None (0) |
| **Total** | | | **32** | **4-byte** | **0 bytes** |

### 3.3 `struct fte3600_frame_buffer` (Total Size: 32 Bytes)

```c
struct fte3600_frame_buffer {
	__u32 version;       /* Offset 0,  Size 4 */
	__u32 length;        /* Offset 4,  Size 4 */
	__u64 buffer_ptr;    /* Offset 8,  Size 8 */
	__u32 reserved[4];   /* Offset 16, Size 16 */
};
```

| Field Name | Type | Offset (Bytes) | Size (Bytes) | Alignment Requirement | Padding Following |
| :--- | :---: | :---: | :---: | :---: | :---: |
| `version` | `__u32` | 0 | 4 | 4-byte | None (0) |
| `length` | `__u32` | 4 | 4 | 4-byte | None (0) |
| `buffer_ptr` | `__u64` | 8 | 8 | 8-byte | None (0) |
| `reserved[0]` | `__u32` | 16 | 4 | 4-byte | None (0) |
| `reserved[1]` | `__u32` | 20 | 4 | 4-byte | None (0) |
| `reserved[2]` | `__u32` | 24 | 4 | 4-byte | None (0) |
| `reserved[3]` | `__u32` | 28 | 4 | 4-byte | None (0) |
| **Total** | | | **32** | **8-byte** | **0 bytes** |

### 3.4 32-bit vs 64-bit Architecture Parity
In 32-bit x86 environments (`compat_ioctl` or 32-bit user binaries on 32-bit kernels):
- `version` and `length` occupy bytes 0..7.
- `buffer_ptr` starts at byte 8, which is naturally both 4-byte and 8-byte aligned.
- The trailing 16 bytes of `reserved[4]` bring the total structure size to exactly 32 bytes (a multiple of both 4 and 8).
- **Result:** Struct packing, member offsets, and size are identical on 32-bit (x86, ARM32) and 64-bit (x86_64, ARM64) architectures. No compat thunks or packing adjustments are needed.

### 3.5 Verification via Static Assertions and Unit Tests
- **Kernel Compile-Time Asserts (`kernel/fte3600.h:153-155`):**
  ```c
  static_assert(sizeof(struct fte3600_state_snapshot) == 32, "struct fte3600_state_snapshot must be 32 bytes");
  static_assert(sizeof(struct fte3600_init_params) == 32, "struct fte3600_init_params must be 32 bytes");
  static_assert(sizeof(struct fte3600_frame_buffer) == 32, "struct fte3600_frame_buffer must be 32 bytes");
  ```
- **Userspace Unit Test Suite 7 (`kernel/fte3600-test.c:929-953`):**
  Independently asserts that `sizeof` is 32, `offsetof(reserved)` matches expectations, and `reserved` values written by `GET_STATE` are 0x00000000. All assertions pass 100%.

---

## 4. Output Zeroing and Information Leakage Prevention

Kernel information disclosure vulnerabilities often occur when stack variables containing sensitive kernel data are copied to userspace without complete initialization.

### 4.1 Audit of GET_STATE Stack Initialization
In `kernel/fte3600.c:420`:
```c
struct fte3600_state_snapshot snap;

memset(&snap, 0, sizeof(snap));
```
1. `memset(&snap, 0, sizeof(snap))` wipes all 32 bytes of the structure on the kernel stack to zero.
2. `snap.version = FTE3600_UAPI_VERSION;` (4 bytes written at offset 0).
3. `snap.state = fte3600_state_to_uapi(data->state);` (4 bytes written at offset 4).
4. `snap.flags` populated according to atomic offline flag and reset issued status (4 bytes written at offset 8).
5. `snap.last_error = data->last_error;` (4 bytes written at offset 12).
6. Bytes 16 through 31 (`snap.reserved[0..3]`) remain strictly 0x00000000.
7. `copy_to_user()` copies all 32 bytes cleanly.
- **Verdict:** Zero information leakage risk. No kernel stack addresses or uninitialized bytes reach userspace.

---

## 5. IOCTL Command Encodings Audit

The Linux `_IOC` macro encodes direction, magic number, sequence number, and payload size:
`_IOC(dir, type, nr, size)` where:
- `dir`: 0 = `_IO`, 1 = `_IOW`, 2 = `_IOR`, 3 = `_IOWR`
- `type`: `'F'` = 0x46
- `size`: 32 = 0x0020 (for snapshot, init, capture)

| IOCTL Macro | Type / Magic | Number | Direction | Payload Size | Computed Encoding | Encoding Hex Value |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| `FTE3600_IOC_GET_STATE` | `'F'` (0x46) | 0x01 | `_IOR` (2) | 32 (0x0020) | `(2 << 30) \| (32 << 16) \| (0x46 << 8) \| 0x01` | `0x80204601` |
| `FTE3600_IOC_RESET` | `'F'` (0x46) | 0x02 | `_IO` (0) | 0 (0x0000) | `(0 << 30) \| (0 << 16) \| (0x46 << 8) \| 0x02` | `0x00004602` |
| `FTE3600_IOC_INIT` | `'F'` (0x46) | 0x03 | `_IOWR` (3) | 32 (0x0020) | `(3 << 30) \| (32 << 16) \| (0x46 << 8) \| 0x03` | `0xc0204603` |
| `FTE3600_IOC_CAPTURE` | `'F'` (0x46) | 0x04 | `_IOWR` (3) | 32 (0x0020) | `(3 << 30) \| (32 << 16) \| (0x46 << 8) \| 0x04` | `0xc0204604` |

All four encodings were compared against historical git revisions. No encoding changes have occurred.

---

## 6. Safety Gates and Live Hardware Boundary Audit

1. **Hardware Communication Prohibited:** Stage D5.2 was strictly confined to source files (`kernel/fte3600.h`, `kernel/fte3600.c`, `kernel/fte3600-test.c`).
2. **Safety Gates Intact:** `bin/fw9369-hw-tool` and all safety preconditions in scripts remain intact and untouched.
3. **Live Driver Preserved:** Passive checks confirm `fte3600.ko` remains loaded and bound to `spi-FTE3600:00`, state machine remains in `8 (FTE3600_STATE_IDENTIFIED)`, and SPI transfer count remains exactly 3 transfers / 19 bytes.
4. **Rebuilt Module Isolated:** The newly built module resulting from `make -C kernel all` remains in `kernel/` workspace directory and was NOT loaded into the live kernel.

---

## 7. Audit Sign-Off

The Stage D5.2 remediation has been thoroughly inspected and verified across all UAPI contracts, ABI layouts, binary structures, memory alignments, and client utilities. 

**Audit Classification: PASS — D5.2 INDEPENDENTLY VERIFIED**
