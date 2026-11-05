# Stage D6.6 — INIT Flag Validation & Userspace Semantic Specification

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Environment:** Linux `7.2.8-9-omarchy` (x86_64)  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Driver State:** Stage D6.6 Source-Only Remediation  

---

## 1. Executive Summary

In Stage D6.5, an independent audit identified that while `struct fte3600_init_params.reserved` fields were strictly validated, undefined bits within `params.flags` were not rejected. An application passing undefined flag bits (e.g. `0xFFFFFFFF` or arbitrary future feature bits) could execute initialization paths without error, creating ABI forward-compatibility hazards and ambiguous driver states.

In Stage D6.6, strict flag validation has been designed, implemented, and verified in `kernel/fte3600.c`, UAPI header `kernel/fte3600.h`, userspace utility `tools/fte3600-tool.c`, and the simulation test harness `kernel/fte3600-test.c`.

---

## 2. Specification & UAPI Definitions

### 2.1 Flag Definitions and Valid Mask

In `kernel/fte3600.h`, the initialization flags are explicitly defined with their semantic meanings:

```c
/* Initialization Control Flags (struct fte3600_init_params.flags) */
#define FTE3600_INIT_FLAG_IDENTIFY_ONLY    0x00000000U /* Execute identification only (Stage C compatibility) */
#define FTE3600_INIT_FLAG_FULL_STARTUP     (1U << 0)   /* Execute complete volatile startup sequence */
#define FTE3600_INIT_FLAGS_MASK            (FTE3600_INIT_FLAG_FULL_STARTUP)
```

- **`FTE3600_INIT_FLAG_IDENTIFY_ONLY` (`0x00000000U`)**:
  Requests only the wake strobe, status read, and 16-bit chip ID read over SPI. Validates sensor identity (`0x9362`) and leaves hardware in `FTE3600_STATE_IDENTIFIED` (internal state 8).
- **`FTE3600_INIT_FLAG_FULL_STARTUP` (`1U << 0`)**:
  Requests the full volatile startup sequence (wake strobe, chip ID verification, abort/status check, SMIC AA foundry interrogation, 0x1A00/0x1A02 clock configuration, and clock stabilization delay). Advances hardware state to `FTE3600_STATE_UNCALIBRATED` (internal state 14).
- **`FTE3600_INIT_FLAGS_MASK` (`FTE3600_INIT_FLAG_FULL_STARTUP`)**:
  Represents the exact bitmask of all currently supported initialization flags. Any bit outside this mask is strictly rejected.

### 2.2 Strict Rejection Logic in `FTE3600_IOC_INIT`

In `kernel/fte3600.c`, validation is performed immediately after copy from userspace and version verification, before acquiring `data->lock` and before performing any SPI bus communication:

```c
	case FTE3600_IOC_INIT: {
		struct fte3600_init_params params;

		if (copy_from_user(&params, (void __user *)arg, sizeof(params)))
			return -EFAULT;

		if (params.version != FTE3600_UAPI_VERSION)
			return -EINVAL;

		if (params.reserved[0] || params.reserved[1] ||
		    params.reserved[2] || params.reserved[3])
			return -EINVAL;

		if (params.flags & ~FTE3600_INIT_FLAGS_MASK)
			return -EINVAL;
...
```

### 2.3 Guarantees of the Flag Validation Precondition

1. **Pre-Lock, Pre-Hardware Execution:**
   Invalid flags fail immediately at the top of the ioctl handler with `-EINVAL`. The driver never touches SPI registers, issues no SPI transfers, and does not contend for `data->lock`.
2. **Deterministic Error Reporting:**
   Userspace passing unrecognized flags receives `-EINVAL` consistently across all kernel versions supporting this ABI.
3. **Forward ABI Safety:**
   Future features or extensions (e.g. calibration flags, baseline modes) can safely allocate bit 1..31 knowing that legacy drivers will cleanly reject unknown configurations rather than silently misbehaving.

---

## 3. Test Coverage & Verification

Test Suite 10 was added to `kernel/fte3600-test.c` to rigorously verify the flag validation logic:

| Scenario | Input Condition | Expected Result | Verified Result | Accesses |
|---|---|---|---|---|
| **Scenario 1** | `params.flags = 0` (`FTE3600_INIT_FLAG_IDENTIFY_ONLY`) | Accepted (0), advances to IDENTIFIED | PASS | 4 SPI ops |
| **Scenario 2** | `params.flags = 1` (`FTE3600_INIT_FLAG_FULL_STARTUP`) | Accepted (0), advances to UNCALIBRATED | PASS | 20 SPI ops |
| **Scenario 3** | Individual unsupported bits `1U << 1` through `1U << 31` (31 distinct checks) | Each rejected with `-EINVAL` | PASS | 0 SPI ops |
| **Scenario 4** | Combinations of unsupported bits (`0x00000006`, `0x80000000`, `0xFFFFFFFE`, etc.) | Each rejected with `-EINVAL` | PASS | 0 SPI ops |
| **Scenario 5** | `FULL_STARTUP` bit combined with invalid bits (`0x00000003`, `0x80000001`, `0xFFFFFFFF`) | Each rejected with `-EINVAL` | PASS | 0 SPI ops |
| **Scenario 6** | `params.reserved[0..3]` non-zero | Each rejected with `-EINVAL` | PASS | 0 SPI ops |

All 6 validation scenarios passed with 100% success rate under `-Werror -Wall -Wextra`.
