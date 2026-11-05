# STAGE D6.5: INDEPENDENT STATE-MACHINE, UAPI AND CAPTURE-GATING FINAL VERDICT

**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Workspace:** `/home/titan/fte3600-linux-driver/`  
**Safety Status:** STRICTLY READ-ONLY SOURCE AUDIT. Zero hardware communication.  

---

## 1. Audit Overview & Methodology

Stage D6.5 conducted an independent, adversarial source audit of the state-machine, UAPI, and capture-gating remediation implemented in Stage D6.4. The audit inspected the actual Git diff, kernel source files (`kernel/fte3600.h`, `kernel/fte3600.c`), simulation test harnesses (`kernel/fte3600-test.c`, `kernel/fte3600-policy-test.c`), and user-space tooling (`tools/fte3600-tool.c`).

Every claim from Stage D6.4 was independently tested against the actual code. Zero physical hardware communication was performed, and the live host system was audited strictly through passive kernel interfaces.

---

## 2. Invariant Verification Checklist

| Requirement / Invariant | Status | Line Reference | Audit Finding | Evidence Tag |
|---|---|---|---|---|
| **No Premature READY Assignment** | **VERIFIED** | `fte3600.c:591` | `data->state = INITIALIZED_UNCALIBRATED`. `STATE_READY` is never assigned. | `[SOURCE-OBSERVED]` |
| **Public UAPI Range Bounded [0..8]** | **VERIFIED** | `fte3600.h:78-103` | `fte3600_state_to_uapi()` strictly returns values in range `[0..8]`. | `[SOURCE-OBSERVED]` |
| **Internal State 14 Maps Truthfully** | **VERIFIED** | `fte3600.h:91-92` | Maps internal 14 to public 8 (`IDENTIFIED`); strictly withholds 4 (`READY`). | `[SOURCE-OBSERVED]` |
| **Fixed 32-Byte Structure Footprints** | **VERIFIED** | `fte3600.h:167-169` | `snapshot`, `init_params`, and `frame_buffer` assert exact 32-byte size. | `[SOURCE-OBSERVED]` |
| **Status Flags Orthogonality** | **VERIFIED** | `fte3600.h:106-110` | Bits 0, 1, 2, and 3 are disjoint power-of-two flags; zero bit collision. | `[SOURCE-OBSERVED]` |
| **Strict Capture-Gating Precondition** | **VERIFIED** | `fte3600.c:783-786` | Rejects capture in all non-`READY` states under `data->lock` with `-EPERM`. | `[SOURCE-OBSERVED]` |
| **Device Removal Concurrency Guard** | **VERIFIED** | `fte3600.c:769-772` | Offline check under `data->lock` prevents NULL pointer dereference. | `[SOURCE-OBSERVED]` |
| **Clean Lock Release on All Exits** | **VERIFIED** | `fte3600.c:750-790` | Every exit path (`-EFAULT`, `-EINVAL`, `-ERESTARTSYS`, `-ENODEV`, `-EPERM`) unlocks. | `[SOURCE-OBSERVED]` |
| **Test Provenance (60/60 Tests)** | **VERIFIED** | `fte3600-test.c:2640` | All 60 unit tests pass (100%); exit code 2 explained and resolved. | `[TEST-OBSERVED]` |
| **Passive Invariant Preservation** | **VERIFIED** | Live host system | `fte3600` module loaded, transfers=3, bytes=19, `/dev/fte3600` 511,0. | `[SOURCE-OBSERVED]` |

---

## 3. Detailed Audit Findings & Documented Semantic Limitations

The source code implementation is technically robust and satisfies all primary safety goals. In accordance with strict adversarial audit standards, the following semantic and interface characteristics are formally documented:

### 3.1 Documented Limitation 1: Capture Errno Semantic Shift (`-EPERM` vs `-EOPNOTSUPP`)
- **Observation:** In earlier software milestones (Stage 2E.2), `FTE3600_IOC_CAPTURE` returned `-EOPNOTSUPP` whenever valid parameters were passed, because state gating was not yet active.
- **Remediation Effect:** In Stage D6.4, capture gating enforces `data->state == FTE3600_STATE_READY`. Because the driver is in `INITIALIZED_UNCALIBRATED`, capture returns `-EPERM` (`errno == 1`), not `-EOPNOTSUPP` (`errno == 95`).
- **Impact:** User-space test utilities (such as `tools/fte3600-tool.c:81`) that explicitly assert `if (errno == EOPNOTSUPP)` will report a failure because `-EPERM` is returned.
- **Assessment:** While `-EPERM` is the semantically correct error code for an uncalibrated permission violation, this constitutes a documented semantic change in returned errno.

### 3.2 Documented Limitation 2: Unmasked Upper Bits in `init_params.flags`
- **Observation:** In `kernel/fte3600.c:704-710`, `params.reserved[0..5]` are strictly checked to be zero (`-EINVAL`). However, `params.flags` is not checked with `if (params.flags & ~FTE3600_INIT_FLAG_FULL_STARTUP) return -EINVAL;`.
- **Impact:** Passing undefined bits (e.g. `flags = 0x02` or `0x80`) does not produce an error; the driver evaluates `params.flags & FTE3600_INIT_FLAG_FULL_STARTUP` and treats any value where bit 0 is cleared as identification-only.
- **Assessment:** This is a non-fatal input leniency, but represents an unmasked input field.

### 3.3 Documented Limitation 3: Header Macro Redundancy
- **Observation:** In `kernel/fte3600.h`, `#define FTE3600_FLAG_RESET_COMPLETED FTE3600_FLAG_RESET_SEQUENCE_ISSUED` is declared twice (line 108 and line 111).
- **Assessment:** Benign preprocessor redundancy with zero functional impact, but noted for cleanup.

---

## 4. Host System Hardware Invariant Verification

Passive inspection of the host system confirms strict preservation of Option 1:

```bash
$ grep fte3600 /proc/modules
fte3600 20480 0 - Live 0x0000000000000000 (OE)

$ cat /sys/bus/spi/devices/spi-FTE3600:00/statistics/transfers
3

$ cat /sys/bus/spi/devices/spi-FTE3600:00/statistics/bytes
19

$ ls -l /dev/fte3600
crw------- 1 root root 511, 0 Nov  4 09:32 /dev/fte3600
```

1. The currently loaded `fte3600.ko` module remains untouched and bound to `spi-FTE3600:00`.
2. SPI hardware statistics remain exactly at `transfers = 3` and `bytes = 19`, identical to the post-Stage C baseline.
3. No newly compiled module has been installed or loaded into the live kernel.
4. Zero ioctl or SPI operations were executed on physical hardware.

---

## 5. Final Classification

```
CONDITIONAL PASS — state machine and capture gating are fully sound; semantic UAPI compatibility limitations are documented
```

### Audit Conclusion
The Stage D6.4 implementation successfully eliminates phantom readiness, enforces truthful state representation, and unconditionally gates fingerprint capture behind complete calibration. The classification is rendered as **CONDITIONAL PASS** solely due to the documented semantic errno change on capture (`-EPERM` vs `-EOPNOTSUPP`) and unmasked upper bits in `init_params.flags`. No blocking state, capture, lifetime, or memory defects exist.
