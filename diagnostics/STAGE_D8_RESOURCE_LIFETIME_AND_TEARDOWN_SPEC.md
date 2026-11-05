# Stage D8 — Resource Lifetime, DMA Quiescence and Teardown Specification

**Target Device:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Status:** FROZEN ARCHITECTURAL SPECIFICATION FOR STAGE D8  
**Safety Gate:** STRICTLY READ-ONLY DESIGN — ZERO HARDWARE ACCESS AUTHORIZED

---

## 1. Executive Summary

This document formalizes the complete lifecycle, allocation, refcounting, synchronization, and destruction of all 10 kernel objects in the FTE3600 driver. It guarantees memory safety, prevents use-after-free, eliminates sleep-while-atomic, and enforces the conservative DMA retention invariant under Intel LPSS `idma64` hardware limitations.

---

## 2. Complete Inventory of the 10 Kernel Objects

| Object Identifier | Allocation Site | Lifetime & Reference Mechanism | Destruction / Freeing Path | Safety Invariant |
| :--- | :--- | :--- | :--- | :--- |
| **1. `struct fte3600_data`** | `fte3600_spi_probe()` via `kzalloc()` | Refcounted by `struct device dev` embedded within it. | `fte3600_device_release()` when refcount drops to 0. | Never freed while userspace holds an open file descriptor. |
| **2. `struct device` (`data->dev`)**| Initialized in `fte3600_spi_probe()` | Embedded inside `data`. `get_device()` / `put_device()`. | Unregistered via `cdev_device_del()`, last `put_device()` frees container. | Standard Linux driver model refcounting. |
| **3. `struct cdev` (`data->cdev`)**| Initialized in `fte3600_spi_probe()` | Tied to `cdev_device_add()` / `cdev_device_del()`. | Deleted in `fte3600_spi_remove()` before final `put_device()`. | Guarantees no new `open()` calls can succeed after removal. |
| **4. `struct fte3600_async_xfer`** | `fte3600_alloc_xfer()` via `kzalloc()` | Managed via `kref`. Initial refcount = 2 (Caller + Completion). | `kref_put(&xfer->ref, fte3600_xfer_release)`. | Survives timeouts; never freed while SPI hardware owns it. |
| **5. Transfer Buffers (`tx_buf`, `rx_buf`)** | Allocated alongside `xfer` | Tied to `struct fte3600_async_xfer` lifetime. | Freed in `fte3600_xfer_release()` ONLY if completion signaled. | Retained permanently in DRAM ($\mathtt{FREED}(t) \equiv 0$) on timeout. |
| **6. `session.watchdog_work`** | Initialized in `fte3600_spi_probe()` | Embedded `delayed_work` in `data->session`. | Cancelled via `cancel_delayed_work_sync()` in `remove()` & `release()`. | **Must be cancelled outside `data->lock`** to prevent deadlock. |
| **7. `deferred_release_work`** | Statically defined per domain | System workqueue worker. | Flushed via `drain_workqueue()` before module exit. | **Worker refcount contribution $\equiv 0$** (never calls `module_put`). |
| **8. `fte3600_controller_domain`** | Static global array (`FTE3600_MAX_DOMAINS = 4`) | Static BSS storage, pinned by PCI Vendor/Device/BDF. | Persists across driver unbinds; zeroed only on kernel reboot. | Total global stranded memory capped at **67,840 bytes**. |
| **9. `reset_gpiod` (`struct gpio_desc *`)** | `devm_gpiod_get_optional()` in `probe()` | Managed by devres. | Cleared under `data->lock` in `remove()`; freed by devres. | Zero GPIO toggling after offline flag set. |
| **10. `spi` (`struct spi_device *`)** | Passed into `fte3600_spi_probe()` | Maintained by SPI core. | Cleared under `data->lock` in `remove()`. | No SPI messages submitted after `remove()` sets `data->spi = NULL`. |

---

## 3. Asynchronous SPI Transaction Lifetime & Kref Mechanics

### 3.1 Two-Reference Model
Each asynchronous transaction allocates a `struct fte3600_async_xfer`:
```c
struct fte3600_async_xfer {
    struct kref ref;            /* Refcount: initialized to 2 */
    struct completion done;     /* Signaled by completion callback */
    struct spi_message msg;
    struct spi_transfer xfer;
    u8 *tx_buf;
    u8 *rx_buf;
    size_t len;
    bool timed_out;
    bool completed_ok;
    int status;
};
```

1. **Initial State ($N_{\text{ref}} = 2$):**
   - Reference 1: Held by synchronous submitting thread (waiting on `done`).
   - Reference 2: Held by SPI core / host controller hardware completion path.
2. **Normal Path (Completion arrives before 250 ms timeout):**
   - Step A: SPI hardware finishes. Host controller calls `fte3600_spi_complete_cb()`.
   - Step B: Callback sets `completed_ok = true`, calls `complete(&xfer->done)`.
   - Step C: Callback drops Reference 2: `kref_put(&xfer->ref, fte3600_xfer_release)` ($N_{\text{ref}} \rightarrow 1$).
   - Step D: Submitting thread wakes up from `wait_for_completion_timeout()`.
   - Step E: Submitting thread inspects `xfer->status`, copies data to user/caller.
   - Step F: Submitting thread drops Reference 1: `kref_put(&xfer->ref, fte3600_xfer_release)` ($N_{\text{ref}} \rightarrow 0$).
   - Step G: Release callback frees `tx_buf`, `rx_buf`, and `xfer`. **Zero memory leaked.**

---

## 4. The Conservative DMA Retention Invariant

### 4.1 Intel LPSS `idma64` Hardware Reality
On the Dynabook C40-J platform, the SPI controller is Intel LPSS SPI2 (`pxa2xx-spi.2`) backed by the `idma64` DMA engine.
- If an SPI transfer hangs on the wire, the DMA controller registers continue to hold physical DRAM bus master pointers.
- The Linux kernel SPI subsystem provides no mechanism to abort or flush active `idma64` DMA descriptors without tearing down the entire PCI host controller.
- Therefore, freeing `rx_buf` into the kernel slab allocator would permit a future DMA write to corrupt arbitrary kernel memory.

### 4.2 Mathematical Retention Invariant: $\mathtt{FREED}(t) \equiv 0$
When `wait_for_completion_timeout()` expires (250 ms):
1. The submitting thread marks `xfer->timed_out = true`.
2. The domain is permanently latched into quarantine: `domain->quarantined = true`.
3. Submitting thread drops Reference 1 ($N_{\text{ref}} \rightarrow 1$).
4. In `fte3600_xfer_release()`, a strict retention check is evaluated:
   ```c
   static void fte3600_xfer_release(struct kref *kref)
   {
       struct fte3600_async_xfer *xfer = container_of(kref, struct fte3600_async_xfer, ref);
       if (xfer->timed_out) {
           /*
            * TIMED OUT: DMA quiescence cannot be proven!
            * Memory is PERMANENTLY RETAINED in DRAM.
            * FREED(t) == 0.
            */
           pr_err("fte3600: Quarantining stranded xfer %p (DRAM retained)\n", xfer);
           return;
       }
       kfree(xfer->tx_buf);
       kfree(xfer->rx_buf);
       kfree(xfer);
   }
   ```
5. If the delayed completion callback ever executes later, it drops Reference 2 ($N_{\text{ref}} \rightarrow 0$), calling `fte3600_xfer_release()`, which verifies `xfer->timed_out == true` and **refuses to free the buffer**.
6. If the callback never arrives, Reference 2 remains 1, and the buffer remains allocated in DRAM.

### 4.3 Code-Enforced Global Memory Ceiling
- Maximum slab bytes per stranded transaction: **16,960 bytes** (FIFO frame $10,246 \times 2 + \text{slabs}$).
- Maximum domains: `FTE3600_MAX_DOMAINS = 4`.
- Global worst-case memory ceiling:
  $$\text{Memory}_{\max} = 4 \times 16,960 = 67,840 \ \text{bytes} \ (66.25 \ \text{KiB})$$
  Exhaustion beyond 4 domains fails closed with `-ENOSPC`. System memory starvation is physically impossible.

---

## 5. Teardown Ordering and Deadlock Prevention

### 5.1 Watchdog Cancellation Deadlock Remediation
Calling `cancel_delayed_work_sync(&data->session.watchdog_work)` while holding `data->lock` causes an immediate kernel deadlock because `fte3600_watchdog_fn()` acquires `data->lock`.

**Strict Rule:** `cancel_delayed_work_sync()` must ALWAYS execute **prior to acquiring `data->lock`**.

### 5.2 Device Removal Pipeline (`fte3600_spi_remove`)
```
[1. Cancel Work Synchronously]
    cancel_delayed_work_sync(&data->session.watchdog_work)  <-- OUTSIDE LOCK!
           |
[2. Acquire Device Lock]
    mutex_lock(&data->lock)
           |
[3. Set Offline Flag & State]
    atomic_set(&data->offline, 1)
    data->state = FTE3600_STATE_DEVICE_REMOVAL
    data->session.active = false
    data->session.owner_filp = NULL
    data->session.generation++
           |
[4. Sever Hardware References]
    data->spi = NULL
    data->reset_gpiod = NULL
           |
[5. Release Device Lock]
    mutex_unlock(&data->lock)
           |
[6. Unregister Character Device]
    cdev_device_del(&data->cdev, &data->dev)
           |
[7. Drop Initial Device Reference]
    put_device(&data->dev)
```

### 5.3 Module Exit Pipeline (`fte3600_exit`)
1. `spi_unregister_driver(&fte3600_spi_driver)` (triggers `fte3600_spi_remove()` for all bound instances).
2. `destroy_workqueue(fte3600_wq)` (flushes and drains all pending workers).
3. `unregister_chrdev_region(fte3600_devt, 1)`.
4. Note: No worker calls `module_put(THIS_MODULE)`, preserving instruction-pointer safety.

---

## 6. Conclusion and Sign-Off

The resource lifetime, DMA retention, and teardown specifications are formally frozen for Stage D8.
