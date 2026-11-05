# Stage D7.7 — Comparative Audit of SPI Timeout & Recovery Strategies

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Architectural Strategy Evaluation  
**Deliverable:** `diagnostics/STAGE_D7_7_ASYNC_TIMEOUT_AND_RECOVERY_AUDIT.md`  
**Date:** 2026-11-05  

---

## 1. Executive Summary & Objective

In Linux device driver development, waiting for hardware transactions can be handled synchronously or asynchronously. Synchronous calls risk uninterruptible thread hangs, while asynchronous timeouts risk use-after-free corruption when hardware completions arrive late.

This audit systematically compares four distinct architectural strategies across nine safety and operational criteria to determine the optimal approach for the FT9362/FW9369 driver.

---

## 2. Systematic Strategy Comparison Matrix

| Evaluation Criterion | Strategy A: Synchronous `spi_sync()` | Strategy B: `spi_async()` + Stranded Retention | Strategy C: Dedicated Worker + Client Timeout | Strategy D: Controller Abort (`pxa2xx` hooks) |
| :--- | :--- | :--- | :--- | :--- |
| **1. Buffer Lifetime Safety** | **PERFECT:** Buffer freed strictly after `spi_sync` returns. | **PERFECT:** Buffer retained via `kref` until callback executes. | **MODERATE:** Worker keeps buffer, but client may exit. | **UNSUPPORTED:** Master abort does not exist in Linux core. |
| **2. Message Serialization** | **EXCELLENT:** Serialized under `data->lock`. | **EXCELLENT:** Serialized under `data->lock` + `in_flight` flag. | **EXCELLENT:** Workqueue serializes FIFO tasks. | **N/A** (No API) |
| **3. Timeout Behavior** | **UNBOUNDED:** Hangs forever in state `D` if hardware locks. | **BOUNDED:** Waiter unblocks after 250 ms, returning `-ETIMEDOUT`. | **BOUNDED:** Client unblocks, but worker thread remains stuck. | **N/A** (No API) |
| **4. Late Completion Behavior** | **N/A:** No timeout, so completion is never late. | **SAFE:** Callback detects `XFER_TIMED_OUT` and cleans up buffer. | **POOR:** Worker blocked; cannot process future queue items. | **N/A** (No API) |
| **5. Device Removal** | **RISKY:** If blocked in `spi_sync`, unbind hangs indefinitely. | **SAFE:** `spi_unregister_controller` aborts queue; callback fires. | **RISKY:** Worker thread may not drain cleanly. | **N/A** (No API) |
| **6. Memory Leak Risk** | **ZERO:** Buffer always freed. | **BOUNDED (10 KB):** Leaks single buffer only if hardware dies. | **ZERO to LOW:** Buffer owned by worker. | **N/A** |
| **7. Use-After-Free Risk** | **ZERO:** No asynchronous detachment. | **ZERO:** Refcounting + device kobject reference prevents UAF. | **LOW-MEDIUM:** If client cancels work item unsafely. | **HIGH:** Calling non-existent abort corrupts DMA. |
| **8. Stuck Controller Recovery** | **NONE:** Thread unkillable; requires host reboot. | **PARTIAL (Software unblocks):** Userspace freed; bus locked. | **PARTIAL:** Userspace freed; worker remains wedged. | **NONE:** Controller has no external master reset API. |
| **9. Implementation Complexity** | **MINIMAL:** ~5 lines of code. | **MODERATE-HIGH:** ~80 lines (spinlock state machine). | **HIGH:** Dedicated kthread/workqueue infrastructure. | **INVALID:** Cannot be implemented. |

---

## 3. Deep-Dive Analysis of Strategies

### 3.1. Strategy A: Existing Synchronous `spi_sync()`
- **Strengths:** Maximum simplicity; impossible to introduce buffer use-after-free or memory leaks.
- **Fatal Weakness:** Inability to bound execution. Under standard hardware operation, transfers take ~82 ms. However, if an electrical anomaly or silicon lockup occurs, any process calling `ioctl` blocks in `TASK_UNINTERRUPTIBLE`. The process cannot be killed (`kill -9`), preventing system shutdown or driver unbind without reboot.

### 3.2. Strategy B: `spi_async()` with Stranded Buffer Retention (Selected Strategy)
- **Strengths:**
  1. The calling ioctl thread never hangs; it is guaranteed to return to userspace within 250 ms.
  2. Memory safety is mathematically guaranteed: the buffer and descriptor are **never freed on timeout**. They are retained until the SPI core invokes the completion callback.
  3. When late completion occurs (e.g. at 350 ms), the callback safely reclaims the memory without touching userspace or the session.
  4. Device unbind triggers standard SPI core queue flushing (`-ESHUTDOWN`), allowing clean teardown.
- **Limitation:** If silicon freezes permanently and never invokes the callback, a single 10,246-byte allocation is leaked for the remainder of the boot session.

### 3.3. Strategy C: Dedicated Transaction Worker
- **Analysis:** Moving `spi_sync()` to a dedicated kernel worker thread does not solve the underlying hardware problem. While userspace can time out waiting for the worker, the worker thread itself becomes permanently wedged on `spi_sync()`. The worker can never execute another SPI message, effectively bricking the driver without providing any memory safety advantage over Strategy B.

### 3.4. Strategy D: Controller-Supported Abort
- **Audit Findings:** Exhaustive kernel source inspection confirmed that neither `drivers/spi/spi.c` nor `drivers/spi/spi-pxa2xx.c` provides a master-mode abort or reset API. Attempting to force an abort by directly resetting controller registers from a client driver violates Linux driver isolation and corrupts DMA engine state.

---

## 4. Architectural Selection & Physical Limit Statement

### Formal Selection:
**Strategy B (`spi_async()` with Stranded Buffer Retention)** is selected as the only architecture that combines **bounded userspace wait times** with **provable use-after-free immunity**.

### Honest Physical Limitation Statement:
> [!WARNING]
> While Strategy B prevents userspace threads from hanging in uninterruptible sleep, **no software strategy can force a permanently wedged SPI controller to resume operations without a hardware power-cycle or GPIO reset.**
> If the host GSPI controller or DMA engine locks up permanently:
> 1. The stranded buffer will remain in memory.
> 2. The driver will lock the SPI bus and return `-EBUSY` or `-EIO` to subsequent requests.
> 3. Full hardware recovery requires an active-low GPIO reset pulse (`FTE3600_IOC_RESET`) or a host system reboot.
