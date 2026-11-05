# STAGE D7.9-R4: Deliverable 6 — DMA Buffer Retention Policy and Hardware Quiescence Evidence Criteria

**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Peripheral Device:** FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Host Controller:** Intel LPSS SPI (`pxa2xx-spi.2` / `idma64`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Scope:** Revalidation of Conservative Retention Invariant and Required Hardware Evidence  

---

## 1. Executive Summary

This specification revalidates the fundamental safety invariant established in Stage D7.9-R3:
Because Linux 7.2.8 kernel driver sources cannot mathematically prove that physical silicon DMA memory cycles have ceased upon `dmaengine_terminate_sync()` return, **the driver must never deallocate a timed-out transaction or its DMA buffers during the active OS session**.

$$\boxed{\mathtt{DMA\_ACTIVE}(t) \text{ unknown} \implies \mathtt{REFS}(t) \ge 1 \implies \mathtt{FREED}(t) \equiv 0}$$

This document re-affirms this non-negotiable policy and defines the exact, rigorous empirical criteria that would be required from future hardware specifications before any buffer reclamation could ever be considered.

---

## 2. Revalidation of Conservative Retention

In Linux kernel driver development, if a peripheral or bus hangs:
1. `spi_async()` timeout expires (250 ms).
2. The peripheral driver has no architectural mechanism to reset the host PCI LPSS controller without affecting other subsystem components.
3. The DMA controller (`idma64.c`) may attempt to deactivate the channel, but if FIFO draining times out after 100 µs, hardware registers remain in an intermediate state.
4. If the kernel were to call `kfree(xfer->rx_buf)`, the SLUB allocator would return those memory addresses to the free pool, where they would be reassigned to other kernel subsystems or userspace processes.
5. If the hung DMA hardware subsequent to the timeout recovers, flushes its FIFO, or receives a stray clock edge, it will execute bus master writes into DRAM addresses that now belong to other objects, causing **untraceable silent memory corruption or kernel panics**.

### Absolute Policy Invariants for Stage D8
- **Zero Buffer Reclamation:** On any transaction timeout (`wait_for_completion_timeout() == 0`), the driver drops strictly the submitter reference (`Ref_submitter`).
- **Callback Reference Preserved:** `Ref_callback` is **never dropped** by the driver timeout handler.
- **Buffers Pinned Forever:** `xfer->rx_buf` and `xfer->tx_buf` remain allocated in the `kmalloc-16k` and `kmalloc-64` slab caches.
- **Fail-Closed Admission:** The controller domain is marked `DOMAIN_QUARANTINED`. All future admissions return `-EBUSY`.

---

## 3. Required Evidence for Future Reclamation (Non-D8 Future Scope)

To transition from conservative retention to safe buffer reclamation in any future driver revision, the following **three empirical evidence pillars** must be rigorously established:

```
+-------------------------------------------------------------------------------------------------+
|                                 REQUIRED HARDWARE EVIDENCE GATES                                |
+-------------------------------+-----------------------------------------------------------------+
| Gate 1: Silicon Architecture  | Official Intel Tiger Lake / LPSS hardware datasheet documenting  |
|         Specification         | that clearing IDMA64 CH_EN synchronously severs AHB/PCIe master |
|                               | request lines and aborts pending write buffers within N cycles. |
+-------------------------------+-----------------------------------------------------------------+
| Gate 2: Hardware Status       | Implementation of an MMIO readback polling loop verifying that   |
|         Verification          | channel active status (e.g. IDMA64_CH_EN[chan] & ACTIVE_BIT == 0)|
|                               | has cleared in silicon before the termination function returns.  |
+-------------------------------+-----------------------------------------------------------------+
| Gate 3: Logic Analyzer / Bus  | Physical PCIe/AHB protocol analyzer trace proving that zero     |
|         Burst Verification    | memory write cycles occur on the host bus after terminate_sync().|
+-------------------------------+-----------------------------------------------------------------+
```

### Prohibited Inferences
Under this policy, the following are strictly rejected as proof of quiescence:
1. **Tasklet Synchronization:** `tasklet_kill(&vc->task)` proves only that a software tasklet has stopped; it proves nothing about hardware silicon.
2. **Descriptor Freeing:** `idma64_vdesc_free()` proves only that software kernel structs were freed; it proves nothing about hardware bus master activity.
3. **`dma_running` Flag:** `atomic_set(&drv_data->dma_running, 0)` is a driver software boolean; it does not disable hardware silicon.

**Conclusion:** In the absence of silicon datasheets and bus-level protocol traces, the Conservative Retention Policy is the sole mathematically sound mechanism guaranteeing 100% memory safety.
