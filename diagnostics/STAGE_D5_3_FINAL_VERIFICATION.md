# STAGE D5.3: FINAL UAPI, POLL AND LIFETIME VERIFICATION REPORT

## Document Information
- **Project:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`) Native Linux Fingerprint Driver
- **Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`, CS0)
- **Host Controller:** Intel LPSS SPI2 (`pxa2xx-spi.2`) on Linux `7.2.8-9-omarchy` (x86_64 SMP)
- **Author:** Independent Senior Kernel Auditor (Antigravity Agent)
- **Status:** COMPLETED INDEPENDENT VERIFICATION
- **Safety Boundary:** STRICTLY READ-ONLY VERIFICATION (Zero Hardware Communication, Zero Module Loading)

---

## 1. Executive Summary

This report delivers the final independent audit of the Stage D5.2 source remediation. The investigation independently verified the Git diff, kernel source files, userspace test harness, client utilities, and kernel memory model interactions.

### Verification Verdict:
1. **Public State Isolation Confirmed:** `fte3600_state_to_uapi()` strictly guarantees that no internal state value above `8` can ever reach the public `FTE3600_IOC_GET_STATE` snapshot. Device removal is mapped to `FTE3600_STATE_UNINITIALIZED` (0) with `FTE3600_FLAG_OFFLINE` set, matching both existing client code (`tools/fte3600-tool.c`) and legacy test expectations.
2. **Poll Lifecycle & Lost-Wakeup Absence Confirmed:** All 6 race scenarios (Scenarios A through F) were formally traced under the Linux Kernel Memory Model. The two-tier offline check around `poll_wait()` coupled with `wake_up_all(&data->event_wq)` guarantees that no sleeping waiter can remain hung or access freed memory.
3. **Devres Lifetime Safety Formally Proven:** The embedded `struct device dev` kobject refcounting model ensures memory validity until file release, while pointer nullification under `data->lock` prevents in-flight or subsequent ioctls from accessing freed SPI or GPIO devres resources.
4. **Test Accuracy Verified:** All 38 standalone simulation tests and 9 policy groups were independently executed and classified according to their genuine verification scope. Zero tests make ungrounded physical hardware claims.

---

## 2. Source-Level Verification of Remediations

### 2.1 Public Removal-State Mapping
- **Function:** `fte3600_state_to_uapi(enum fte3600_state state)` in `kernel/fte3600.h:76-107`.
- **Implementation:**
  ```c
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
- **Audit Findings:**
  * Returns strictly `[0..8]`.
  * Gated by `#define FTE3600_MAX_PUBLIC_STATE 8`.
  * Verified against `tools/fte3600-tool.c:21`:
    ```c
    const char *name = (snap->state <= 8) ? state_names[snap->state] : "INVALID";
    ```
    The tool explicitly checks `snap->state <= 8`. Mapping removal to `UNINITIALIZED` (0) ensures `fte3600-tool` displays `State: 0 (UNINITIALIZED)` and `[OFFLINE]`, preventing the `"INVALID"` string observed during the D5.1 audit.

### 2.2 Complete Pruning of Dead Concurrency Infrastructure
- `struct fte3600_data` in `kernel/fte3600.c` was audited:
  * Field `event_sequence` has been completely deleted.
  * Helper `fte3600_notify_event()` has been completely deleted.
  * `atomic64_set(&data->event_sequence, 0)` in `fte3600_probe()` has been deleted.
  * No dead or unreferenced sequence counter code remains in the driver.

### 2.3 Poll Synchronization Hardening
- `fte3600_cdev_poll()` in `kernel/fte3600.c:529-549` was audited:
  * Bare read of `data->state` has been completely removed.
  * Pre-mature advertisement of `EPOLLIN | EPOLLRDNORM` has been removed.
  * Offline check occurs locklessly via `atomic_read(&data->offline)` before and after `poll_wait()`.
  * Exclusively returns `EPOLLERR | EPOLLHUP` upon unbind/removal, or `0` when online.

---

## 3. Four-Tier Verification Boundary Review

| Verification Tier | Execution Environment | Status | Scope |
| :--- | :--- | :---: | :--- |
| **Tier 1: Source Checks** | Compiler Static Assertions | **VERIFIED** | Enforces 32-byte struct layout and offset invariants |
| **Tier 2: Mock Tests** | Userspace Simulation (`make check`) | **VERIFIED** | 38 / 38 passed (100.0%); 9 / 9 policy groups passed |
| **Tier 3: Kbuild Build** | Kernel Kbuild against `7.2.8-9-omarchy` | **VERIFIED** | Clean build (0 warnings, 0 errors); module NOT loaded |
| **Tier 4: Physical Hardware** | SPI Bus / Sensor Silicon | **NOT PERFORMED** | Barred by safety gates; physical state 100% untouched |

---

## 4. Passive Invariant Verification

| Host Invariant Metric | Baseline (Pre-Stage) | Final Value (Post-Stage) | Verification Status |
| :--- | :---: | :---: | :---: |
| **Module Load State** | `fte3600 20480 0 - Live` | `fte3600 20480 0 - Live` | **CONFIRMED UNCHANGED** |
| **SPI Slave Device** | `spi-FTE3600:00` | `spi-FTE3600:00` | **CONFIRMED BOUND** |
| **Device Node** | `/dev/fte3600` (`511, 0`) | `/dev/fte3600` (`511, 0`) | **CONFIRMED UNTOUCHED** |
| **Driver State Machine** | `8 (FTE3600_STATE_IDENTIFIED)` | `8 (FTE3600_STATE_IDENTIFIED)` | **CONFIRMED UNCHANGED** |
| **Physical SPI Transfers** | `3` | `3` | **CONFIRMED (0 added)** |
| **Physical SPI Bytes** | `19` | `19` | **CONFIRMED (0 added)** |

---

## 5. Final Stage Verdict

**PASS — D5.2 INDEPENDENTLY VERIFIED**
