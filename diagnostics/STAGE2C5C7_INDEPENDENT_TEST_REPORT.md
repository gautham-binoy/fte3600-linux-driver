# Stage 2C.5C.7 — Independent Offline Test and Sanitizer Verification Report

**Target Silicon:** FocalTech FT9362 / FW9369  
**Execution Binary:** `bin/fw9369-tests`  
**Host Environment:** Linux 6.8 x86_64 / GCC 11  
**Audit Date:** 2026-11-02  
**Evaluation Mode:** Strictly In-Memory Mock Testing (Zero Physical Hardware Access)

---

## 1. Test Suite Execution Results

All 14 test suites were executed independently under three distinct toolchain configurations:
1. Standard C11 Release Build (`-O2 -pthread`)
2. AddressSanitizer Instrumentation (`-fsanitize=address`)
3. ThreadSanitizer Instrumentation (`-fsanitize=thread`)

### 1.1 Complete Test Matrix

| Suite # | Suite Focus | Assertions / Tests | Standard Result | ASan Result | TSan Result |
|---|---|---|---|---|---|
| **Suite 1** | Wire and LE CRC-CCITT Implementations | 5 | 5 / 5 PASS | 5 / 5 PASS | 5 / 5 PASS |
| **Suite 2** | Framing, Opcode & Directional Allowlist | 15 | 15 / 15 PASS | 15 / 15 PASS | 15 / 15 PASS |
| **Suite 3** | Mock Transport Invariants & Safety | 7 | 7 / 7 PASS | 7 / 7 PASS | 7 / 7 PASS |
| **Suite 4** | FDT Calibration Search & Thresholds | 7 | 7 / 7 PASS | 7 / 7 PASS | 7 / 7 PASS |
| **Suite 5** | Image Baseline Capture & Single-Frame Copy | 5 | 5 / 5 PASS | 5 / 5 PASS | 5 / 5 PASS |
| **Suite 6** | 5-Phase Pipeline & Protected Bank Cleanup | 8 | 8 / 8 PASS | 8 / 8 PASS | 8 / 8 PASS |
| **Suite 7** | Deterministic Failure Injection Scenarios | 14 | 14 / 14 PASS | 14 / 14 PASS | 14 / 14 PASS |
| **Suite 8** | Sysfs Identity Verification | 7 | 7 / 7 PASS | 7 / 7 PASS | 7 / 7 PASS |
| **Suite 9** | Device Path & Parameter Validation | 8 | 8 / 8 PASS | 8 / 8 PASS | 8 / 8 PASS |
| **Suite 10** | Hardware Transport ioctl Mechanics & Limits | 15 | 15 / 15 PASS | 15 / 15 PASS | 15 / 15 PASS |
| **Suite 11** | Adversarial Hardening & Opcode Complements | 30 | 30 / 30 PASS | 30 / 30 PASS | 30 / 30 PASS |
| **Suite 12** | Independent Lock-State Transition Audit | 10 | 10 / 10 PASS | 10 / 10 PASS | 10 / 10 PASS |
| **Suite 13** | Fail-Closed UNKNOWN-State Hardening | 19 | 19 / 19 PASS | 19 / 19 PASS | 19 / 19 PASS |
| **Suite 14** | API, Mutex & Concurrency Hardening | 77 | 77 / 77 PASS | 77 / 77 PASS | 77 / 77 PASS |
| **TOTAL** | **All 14 Test Suites** | **227** | **227 / 227 (100%)** | **227 / 227 (100%)** | **227 / 227 (100%)** |

**Execution Metrics:**
- **Tests Skipped:** 0
- **Tests Timed Out:** 0
- **Tests Incomplete:** 0
- **Tests Failed:** 0
- **Overall Execution Duration:** < 1.0 second per full run

---

## 2. Independent Inspection of Claimed Edge Cases

To verify that tests actually validate real code mechanics rather than emitting superficial `PASS` lines, the following tests were audited at the source code level:

### 2.1 Copy-Out Initialization Consistency (Suite 14 Test 6)
- Evaluated `fw9369_hw_get_last_tx()` and `fw9369_mock_get_last_tx()` when called with uninitialized stack variables (`snap_len = 999; m_len = 999;`).
- Verified that on `-ENODATA` and `-EINVAL`, `out_len` was explicitly written to 0.
- Verified that on `-ENOBUFS`, `out_len` was populated with the required transfer length (4).
- Verified that on successful copy, caller buffer matched transmitted payload byte-for-byte and was immutable against subsequent log updates.

### 2.2 Strict Normal Mutex Serialization (Suite 14 Test 1 & Test 4)
- Test 1 ran concurrent threads through `s14_ioctl_hook_mutex_check` to assert that `s14_in_ioctl_count` strictly never exceeded 1.
- Test 4 used an independent test-control synchronization harness (`test4_ctrl`) with timed condition variables to assert that a concurrent call to `fw9369_hw_get_lock_state()` blocked until the in-flight transfer released `ctx->lock`.

### 2.3 Error-Path Lock Release (Suite 14 Test 7)
- Verified all 5 discrete error branches (`-EPERM`, `-EIO`, `-EMSGSIZE`, `-EPROTO`, `-ESHUTDOWN`).
- Confirmed that after each error was triggered, a subsequent transfer on the same context immediately acquired the mutex and succeeded, proving that none of the error branches leaked a held mutex.

---

## 3. Sanitizer Analysis

### 3.1 AddressSanitizer (ASan) Audit
- **Command:** `make CFLAGS="... -fsanitize=address" LDFLAGS="-fsanitize=address" test`
- **Results:**
  - Heap Use-After-Free: 0
  - Buffer Overflows: 0
  - Stack / Global Misalignment: 0
  - Memory Leaks: 0
  - Exit Code: 0 (Clean)

### 3.2 ThreadSanitizer (TSan) Audit
- **Command:** `make CFLAGS="... -fsanitize=thread" LDFLAGS="-fsanitize=thread" test`
- **Results:**
  - Data Races Detected: 0
  - Deadlocks Detected: 0
  - Lock-Order Inversions: 0
  - Thread Leaks: 0
  - Exit Code: 0 (Clean)

---

## 4. Build Targets and Binary Isolation

1. **`bin/fw9369-tool` (Mock Production CLI):**
   - Successfully compiled and linked.
   - Verified that `nm -g bin/fw9369-tool | grep "fw9369_hw_"` produces **0 symbols**.
2. **`bin/fw9369-tests` (Unit Test Runner):**
   - Successfully compiled and linked. Contains both mock and hardware test harnesses.
3. **`bin/fw9369-hw-tool` (Hardware Transport CLI):**
   - Successfully compiled and linked.
   - Execution test: returns exit code **1** unconditionally with fail-closed safety alert.

**Audit Status:** **PASS (100% verified across all targets and sanitizers)**.
