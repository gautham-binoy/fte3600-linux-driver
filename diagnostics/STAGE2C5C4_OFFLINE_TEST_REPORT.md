# Stage 2C.5C.4 — Offline Test Report

## Executive Summary & Build Verification

The comprehensive offline test suite for the FW9369 mock and hardware transport backend was compiled and executed in `/home/titan/fte3600-linux-driver/tools/fw9369-mock/`.

- **Target Binary:** `../../bin/fw9369-tests`
- **Compiler Invocations:** `cc -Wall -Wextra -Werror -pedantic -std=c11 -O2 -pthread -Iinclude`
- **Build Status:** **CLEAN (0 Warnings, 0 Errors)**
- **Execution Mode:** Strictly offline in-memory execution (Zero physical hardware access)
- **Total Test Results:** **199 / 199 Passed (100.0%)** (Up from 181 in Stage 2C.5C.3)

---

## 1. Test Suite Summary Table

| Suite # | Test Suite Description | Total Assertions | Status |
| :---: | :--- | :---: | :---: |
| 1 | CRC Implementations & Vectors | 5 | **PASS** |
| 2 | Protocol Framing & Allowlist | 15 | **PASS** |
| 3 | Mock Transport Invariants & Safety | 7 | **PASS** |
| 4 | FDT Calibration & Minimum Tracking | 7 | **PASS** |
| 5 | Image Baseline & Single-Frame Copy | 5 | **PASS** |
| 6 | Full Pipeline & Protected Register Cleanup | 8 | **PASS** |
| 7 | Deterministic Failure Injection Scenarios | 9 | **PASS** |
| 8 | Hardware Transport Offline Validation & Isolation | 13 | **PASS** |
| 9 | Physical Device Identity & Sysfs Hierarchy | 14 | **PASS** |
| 10 | Adversarial Protocol Hardening & Edge Cases | 44 | **PASS** |
| 11 | Sensor State Uncertainty & Recovery-State Hardening | 22 | **PASS** |
| 12 | Independent Lock-State Transition & Recovery Audit | 32 | **PASS** |
| 13 | **Stage 2C.5C.4 Fail-Closed UNKNOWN-State & Concurrency Hardening (NEW)** | **18** | **PASS** |
| **Total** | **Comprehensive Automated Test Suite** | **199** | **100.0% PASS** |

---

## 2. Detailed Verification of Section 5 Requirements

Test Suite 13 directly validates all 15 operational requirements mandated by the Stage 2C.5C.4 prompt:

| # | Prompt Requirement | Test Result | Mechanism / Assertion Reference |
| :---: | :--- | :---: | :--- |
| **1** | New context starts in UNKNOWN | **PASS** | Context allocated with `calloc()` verifies `lock_state == UNKNOWN = 0`. |
| **2** | Unlock from UNKNOWN is rejected before ioctl | **PASS** | `t->transfer(unlock_pkt)` returns `-EPERM`; `fake_ioctl_called == false`. |
| **3** | Lock from UNKNOWN is rejected before ioctl | **PASS** | `t->transfer(lock_pkt)` returns `-EPERM`; `fake_ioctl_called == false`. |
| **4** | Failed unlock transitions to UNKNOWN | **PASS** | Forced ioctl `-EIO` error sets `lock_state = UNKNOWN`. |
| **5** | Lock from UNKNOWN after failed unlock is rejected | **PASS** | Subsequent lock returns `-EPERM`; `fake_ioctl_called == false`. |
| **6** | Failed lock transitions to UNKNOWN | **PASS** | Forced ioctl `-EIO` error sets `lock_state = UNKNOWN`. |
| **7** | Lock from UNKNOWN after failed lock is rejected | **PASS** | Subsequent lock returns `-EPERM`; `fake_ioctl_called == false`. |
| **8** | Context recreation starts in UNKNOWN | **PASS** | Destroyed and recreated context confirms `lock_state == UNKNOWN`. |
| **9** | Protected writes from UNKNOWN are rejected | **PASS** | Write to `0x00C0` returns `-EPERM`; `fake_ioctl_called == false`. |
| **10** | Successful unlock from ASSUMED_LOCKED reaches ASSUMED_UNLOCKED | **PASS** | From `ASSUMED_LOCKED`, unlock returns `0`; state becomes `ASSUMED_UNLOCKED`. |
| **11** | Successful lock from ASSUMED_UNLOCKED reaches ASSUMED_LOCKED | **PASS** | From `ASSUMED_UNLOCKED`, lock returns `0`; state becomes `ASSUMED_LOCKED`. |
| **12a**| Protected writes during UNLOCK_COMMAND_SENT rejected | **PASS** | Active ioctl hook attempts write to `0x00C0`; rejected with `-EPERM`. |
| **12b**| Protected writes during LOCK_COMMAND_SENT rejected | **PASS** | Active ioctl hook attempts write to `0x00C0`; rejected with `-EPERM`. |
| **13** | Concurrent operations serialized across pthreads | **PASS** | 4 threads execute 25 transfers each (100 total); all join with code 0. |
| **14** | Transaction logging consistent under concurrency | **PASS** | Atomic verification: `post_count == pre_count + 100` exact count. |
| **15** | Context destruction follows documented lifetime rules | **PASS** | `destroy()` drains active operations and frees resources safely. |

---

## 3. Epistemic Boundary Notice

> [!IMPORTANT]
> All 199 tests execute strictly in userspace memory using mock and simulated system call hooks. They verify software invariants, thread serialization, memory safety, and protocol allowlists. They **DO NOT** claim or prove physical FT9362 silicon behavior.
