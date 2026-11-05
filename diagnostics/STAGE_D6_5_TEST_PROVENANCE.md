# STAGE D6.5: TEST PROVENANCE AND BUILD VERIFICATION REPORT

**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Workspace:** `/home/titan/fte3600-linux-driver/`  
**Safety Status:** STRICTLY READ-ONLY SOURCE AUDIT. Zero hardware communication.  

---

## 1. Executive Summary

This report establishes the build provenance, compiler diagnostics, and execution verification of the test harnesses in `/home/titan/fte3600-linux-driver/kernel/`.

All 60 unit tests across Test Suites 1 through 9 and all 9 glue policy groups executed successfully with 100% pass rates. The transient compilation failure (exit code 2) observed during the initial Stage D6.4 development cycle is investigated, explained, and confirmed resolved.

---

## 2. Investigation of Transient Exit-Code-2 Compiler Failure

### 2.1 Failure Manifestation
During the Stage D6.4 development sequence, an intermediate build attempt via `make -C kernel check` failed with exit code 2:

```
make: Entering directory '/home/titan/fte3600-linux-driver/kernel'
cc -std=c11 -Wall -Wextra -Werror -O2 -pthread -o fte3600-test fte3600-test.c
fte3600-test.c:417:12: error: ‘sim_driver_ioctl_capture’ defined but not used [-Werror=unused-function]
  417 | static int sim_driver_ioctl_capture(sim_context_t *ctx, const struct fte3600_frame_buffer *fb)
      |            ^~~~~~~~~~~~~~~~~~~~~~~~
cc1: all warnings being treated as errors
make: *** [Makefile:22: fte3600-test] Error 1
make: Leaving directory '/home/titan/fte3600-linux-driver/kernel'
```

### 2.2 Root Cause Analysis
1. In `kernel/Makefile`, the test target is compiled with strict flags: `CFLAGS = -std=c11 -Wall -Wextra -Werror -O2 -pthread`.
2. The remediation plan for Stage D6.4 required implementing capture gating in both the kernel driver (`kernel/fte3600.c`) and the test simulation harness (`kernel/fte3600-test.c`).
3. In the initial editing pass on `kernel/fte3600-test.c`, the simulation helper `sim_driver_ioctl_capture()` was inserted at line 417.
4. However, the 10 scenarios comprising Test Suite 9 (which invoke `sim_driver_ioctl_capture()`) had not yet been appended to the file.
5. The C compiler detected an unused static function and raised `-Wunused-function`. Because `-Werror` was active, the warning was escalated to an error, halting compilation with exit code 2.

### 2.3 Resolution and Final Verification
Once Test Suite 9 was appended and wired into `main()`, `sim_driver_ioctl_capture()` was actively invoked by 7 distinct test scenarios (Suite 9 Scenarios 2, 3, 4, 5, 6, 7, and 10). Recompilation succeeded with zero warnings and zero errors, and all tests passed.

---

## 3. Test Suite Structure and Execution Coverage

The standalone test harness `kernel/fte3600-test.c` comprises 2,716 lines of code organized into 9 distinct test suites:

| Suite | Focus Area | Scenarios | Pass Rate | Key Invariants Tested | Evidence Tag |
|---|---|---|---|---|---|
| **Suite 1** | UAPI Layout & Alignment | 3 | 3 / 3 (100%) | Exact 32-byte struct sizes; offsets; magic `'F'`; ioctl direction & sizes | `[TEST-OBSERVED]` |
| **Suite 2** | CRC-16 Verification | 1 | 1 / 1 (100%) | Wire CRC (`0x1C53`) and Word LE CRC (`0x313E`, `0x5B7C`, `0xE4AA`) | `[TEST-OBSERVED]` |
| **Suite 3** | State Machine Logic | 1 | 1 / 1 (100%) | 9-state canonical model; fail-closed `UNKNOWN`; reset pulse recovery | `[TEST-OBSERVED]` |
| **Suite 4** | Lifecycle & Concurrency | 11 | 11 / 11 (100%) | Open/release; unbind racing with ioctl; single-client lease; refcounting | `[TEST-OBSERVED]` |
| **Suite 5** | Parameter Rejections | 1 | 1 / 1 (100%) | Invalid magic; version mismatch; non-zero reserved fields; `-EINVAL` | `[TEST-OBSERVED]` |
| **Suite 6** | Chip Identification | 17 | 17 / 17 (100%) | Wake verification `0xC6`; Chip ID `0x9362`; short transfers; repeated INIT | `[TEST-OBSERVED]` |
| **Suite 7** | UAPI & State Isolation | 4 | 4 / 4 (100%) | State range `[0..8]`; state 13 never leaks; poll offline terminal condition | `[TEST-OBSERVED]` |
| **Suite 8** | Volatile Startup Prefix | 12 | 12 / 12 (100%) | 6-step prefix; 3-byte command strobes; foundry loops; zero `0x1A84` writes | `[TEST-OBSERVED]` |
| **Suite 9** | State Semantics & Capture | 10 | 10 / 10 (100%) | State 14 withholds READY; capture rejected in all non-READY states; UAPI flags | `[TEST-OBSERVED]` |
| **Total** | **All Unit Tests** | **60** | **60 / 60 (100%)** | **Complete driver synchronization, UAPI, and protocol test coverage** | `[TEST-OBSERVED]` |

### 3.1 Glue Policy Verification
The companion harness `kernel/fte3600-policy-test` verifies architecture and glue layer invariants without hardware access:
- ACPI reset and IRQ GPIO bindings (9 policy groups passed).

---

## 4. Kernel Module Build Provenance

The kernel driver was built against the host kernel headers:
- **Kernel Version:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)
- **Compiler:** `gcc (GCC) 15.2.1 20250813`
- **Build Command:** `make -C kernel all`
- **Build Artifacts:**
  - `kernel/fte3600.o` (object)
  - `kernel/fte3600.mod.o` (module object)
  - `kernel/fte3600.ko` (kernel module binary)
- **Compiler Output:** Clean build with **0 compiler warnings, 0 errors**.
- **Hardware Boundary:** In strict adherence to Option 1, the newly built module was **NOT installed or loaded** into the live kernel.

---

## 5. Scope Boundary: Mocks vs. Physical Silicon

**FORMAL PROVENANCE DISCLAIMER:**
1. **Scope of Mock Assertions:**  
   The tests in `kernel/fte3600-test.c` execute in a userspace process utilizing simulated hardware abstractions (`mock_spi_profile_t`, simulated GPIO pointers, and pthread mutexes). They prove that the driver source code implements the specified state transitions, locking disciplines, parameter validations, and error paths.
2. **Silicon Reality:**  
   The mock tests do **NOT** validate physical silicon behavior, physical SPI controller clock generation, analog pixel matrix health, or interrupt line latching. Physical silicon operation has not been executed in this stage.
