# STAGE D7.9-R3: Audit 1 — IDMA64 Hardware Quiescence and DMAengine Termination Proof

**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Peripheral Device:** FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Host Controller:** Intel LPSS SPI (`pxa2xx-spi.2` / `idma64`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Audit Scope:** Adversarial Source Verification of DMA Termination, Channel Deactivation, Register Accesses, and Hardware Quiescence  

---

## 1. Executive Summary

Stage D7.9-R2 asserted that `dmaengine_terminate_sync()` and `idma64_terminate_all()` mathematically guarantee that physical DMA bus transactions have ceased upon function return. This adversarial audit reconstructed the complete call graph from Linux 7.2.8 kernel sources (`drivers/dma/idma64.c`, `drivers/dma/idma64.h`, `drivers/dma/virt-dma.h`, `drivers/spi/spi-pxa2xx-dma.c`, `include/linux/dmaengine.h`).

The adversarial investigation reveals three critical gaps in silicon-level quiescence guarantees:
1. `idma64_chan_deactivate()` attempts to drain channel FIFOs, but loop bounds at 100 µs (`--count == 0`) and exits **without checking for error or verifying that `FIFO_EMPTY` was asserted**.
2. `idma64_chan_stop()` writes 0 to `IDMA64_CH_EN` (`0x3a0`), but this write is **posted without an MMIO readback (`readl`) or ordering barrier**.
3. `vchan_synchronize()` invokes `tasklet_kill(&vc->task)`, which synchronizes only software tasklet execution and provides **zero hardware bus quiescence verification**.

**Crucial Finding:** The Linux kernel driver source alone **does not and cannot prove silicon-level hardware quiescence** when a DMA transfer is terminated mid-flight on a stalled or hung peripheral bus. Consequently, memory safety cannot rely on hardware quiescence; it must strictly rely on the **Conservative Buffer Retention Rule**: retaining the DMA buffer and transaction object in memory indefinitely and permanently quarantining the hardware transaction domain.

---

## 2. Reconstructed Call Graph for DMAengine Termination

The complete termination sequence spans the SPI host controller, DMAengine core, IDMA64 hardware driver, and Virtual DMA framework:

```
pxa2xx_spi_dma_stop() [drivers/spi/spi-pxa2xx-dma.c:178]
  |
  +--> atomic_set(&drv_data->dma_running, 0)
  |
  +--> dmaengine_terminate_sync(controller->dma_rx) [include/linux/dmaengine.h:1213]
  |      |
  |      +--> dmaengine_terminate_async(chan) [include/linux/dmaengine.h:1180]
  |      |      |
  |      |      +--> chan->device->device_terminate_all(chan)
  |      |             = idma64_terminate_all(chan) [drivers/dma/idma64.c:478]
  |      |                 |
  |      |                 +--> spin_lock_irqsave(&idma64c->vchan.lock, flags)
  |      |                 |
  |      |                 +--> idma64_chan_deactivate(idma64c, true) [idma64.c:422]
  |      |                 |      |
  |      |                 |      +--> cfglo = channel_readl(idma64c, CFG_LO)
  |      |                 |      +--> channel_writel(idma64c, CFG_LO, cfglo | CH_DRAIN | CH_SUSP)
  |      |                 |      +--> do { udelay(1); cfglo = channel_readl(...); }
  |      |                 |           while (!(cfglo & FIFO_EMPTY) && --count); [UNCHECKED TIMEOUT]
  |      |                 |
  |      |                 +--> idma64_stop_transfer(idma64c) [idma64.c:105]
  |      |                 |      |
  |      |                 |      +--> idma64_chan_stop(idma64, idma64c) [idma64.c:84]
  |      |                 |             |
  |      |                 |             +--> channel_clear_bit(idma64, CH_EN, idma64c->mask) [idma64.h:148]
  |      |                 |                    = dma_writel(idma64, CH_EN, (mask << 8) | 0) [POSTED MMIO]
  |      |                 |
  |      |                 +--> if (idma64c->desc) {
  |      |                 |        idma64_vdesc_free(&idma64c->desc->vdesc) [SUPPRESSES CALLBACK]
  |      |                 |        idma64c->desc = NULL;
  |      |                 |    }
  |      |                 +--> vchan_get_all_descriptors(&idma64c->vchan, &head)
  |      |                 +--> spin_unlock_irqrestore(&idma64c->vchan.lock, flags)
  |      |                 +--> vchan_dma_desc_free_list(&idma64c->vchan, &head)
  |      |
  |      +--> dmaengine_synchronize(chan) [include/linux/dmaengine.h:1200]
  |             |
  |             +--> chan->device->device_synchronize(chan)
  |                    = idma64_synchronize(chan) [drivers/dma/idma64.c:500]
  |                        |
  |                        +--> vchan_synchronize(&idma64c->vchan) [drivers/dma/virt-dma.h:221]
  |                               |
  |                               +--> tasklet_kill(&vc->task) [SOFTWARE ONLY]
  |                               +--> spin_lock_irqsave(&vc->lock, flags)
  |                               +--> list_splice_tail_init(&vc->desc_terminated, &head)
  |                               +--> spin_unlock_irqrestore(&vc->lock, flags)
  |                               +--> vchan_dma_desc_free_list(vc, &head)
```

---

## 3. Detailed Hardware Operation Analysis

### Question 1: Which Hardware Registers Are Written?
- `CFG_LO` (per-channel offset `0x10 + 0x58 * chan_id`): Written by `idma64_chan_deactivate()` with `IDMA64C_CFGL_CH_DRAIN | IDMA64C_CFGL_CH_SUSP` (`0x00000001 | 0x00000002`).
- `IDMA64_CH_EN` (global offset `0x3a0`): Written by `idma64_chan_stop()` via `channel_clear_bit()` with `((idma64c->mask) << 8) | 0`. The upper 8 bits act as write-enable flags for individual channels, and the lower 8 bits set the channel enable state to 0.
- `SSCR0` / `SSCR1` (PXA2xx SSP controller): When teardown cascades to `pxa_ssp_disable()`, SSP enable (`SSCR0_SSE`) is cleared.

### Question 2: Whether Channel Enable Is Cleared?
- Yes. The MMIO write to `IDMA64_CH_EN` (`0x3a0`) writes 0 to the bit selected by `idma64c->mask`.

### Question 3: Whether the Implementation Polls for Inactive or Idle Status?
- `idma64_chan_deactivate()` polls `CFG_LO & IDMA64C_CFGL_FIFO_EMPTY`, but **only up to 100 iterations of 1 µs** (`udelay(1)`).
- `idma64_chan_stop()` does **NOT poll `CH_EN`** or any status register to verify that hardware has transitioned to the disabled state.
- There is **no readback** of `CH_EN` or channel active status registers.

### Question 4: Whether Register Readback or Ordering Barriers Are Required?
- On x86 PCIe/AHB interconnects (Intel LPSS), MMIO writes are **posted writes**. A posted write is buffered in the host bridge or memory controller and does not immediately complete at the target peripheral.
- `writel()` includes an architecture write barrier (`wmb()`), which orders prior memory writes before the MMIO write. However, it does **not flush the posted write** from the bus bridge.
- Standard Linux kernel practice to ensure an MMIO write has reached the hardware requires a subsequent readback (`readl()`) from the same peripheral.
- In `idma64.c:84-87`, `idma64_chan_stop()` performs `dma_writel()` without a subsequent readback. Therefore, bus cycles could theoretically remain in the PCIe/LPSS pipeline when the function returns.

### Question 5: Whether FIFO Draining Is Actually Guaranteed?
- **NO.** In `idma64_chan_deactivate()`:
  ```c
  do {
      udelay(1);
      cfglo = channel_readl(idma64c, CFG_LO);
  } while (!(cfglo & IDMA64C_CFGL_FIFO_EMPTY) && --count);
  ```
  If the peripheral or SPI interface is hung, the hardware FIFO may fail to drain within 100 µs. When `count` reaches 0, the loop exits silently. The driver does not report an error, does not abort, and proceeds directly to clearing `CH_EN`.

### Question 6: Whether DMA Memory Accesses Can Still Occur After Return?
- If the channel FIFO failed to drain, or if the posted write to `CH_EN` has not propagated across the LPSS fabric, or if an in-flight AHB burst is still flushing to DRAM, **memory bus cycles may still occur after `idma64_terminate_all()` returns**.
- The driver source **does not provide mathematical proof of immediate memory quiescence**.

### Question 7: Whether Tasklet Synchronization Proves Hardware Quiescence?
- **NO.** `vchan_synchronize()` executes `tasklet_kill(&vc->task)`.
- `tasklet_kill()` is strictly a software scheduler primitive in `kernel/softirq.c`. It ensures that any running softirq/tasklet handler on another CPU finishes and removes the tasklet from the scheduling queue.
- It touches zero hardware registers, performs zero bus synchronization, and has zero awareness of silicon DMA bus state.

### Question 8: Can the SPI Controller Independently Access DMA Buffers?
- The SPI controller (`pxa2xx-spi`) interacts with memory strictly through the DMA engine when DMA is configured. The SPI controller itself only accesses its internal RX/TX FIFOs.
- However, if `pxa2xx_spi_dma_stop()` clears `atomic_set(&drv_data->dma_running, 0)`, but the SPI hardware core remains clocked, any stale data in the SPI FIFO remains unread. The SPI core itself cannot write directly to host DRAM without the DMA controller.

---

## 4. Fundamental Semantic Distinctions

To eliminate conceptual conflations from earlier reports, the following operations are strictly distinguished:

| Operation | Implementation Function | Execution Scope | Hardware Guarantee Provided |
| :--- | :--- | :--- | :--- |
| **Descriptor Destruction** | `vchan_dma_desc_free_list()` | Software virtual descriptor pool | None (frees kernel memory structures) |
| **Callback Suppression** | `idma64_vdesc_free()` without `vchan_cookie_complete()` | Software descriptor deallocation | Guarantees completion callback will never run |
| **Tasklet Synchronization** | `tasklet_kill(&vc->task)` | Linux softirq tasklet scheduler | Guarantees tasklet code has stopped running |
| **DMA Channel Deactivation** | `channel_clear_bit(CH_EN)` | MMIO posted write to LPSS controller | Instructs hardware to disable channel |
| **Hardware DMA Quiescence** | Verification of idle bus + empty FIFO | Physical silicon memory bus interface | **NOT PROVEN** by Linux 7.2.8 driver source |
| **SPI Message Completion** | `spi_finalize_current_message()` | SPI core message workqueue | Notified only upon successful callback |

---

## 5. Architectural Mandate: The Conservative Retention Invariant

Because hardware quiescence cannot be proven at the silicon level upon termination return:

$$\boxed{\text{If } \mathtt{DMA\_ACTIVE}(t) = 1 \text{ cannot be disproven, } \mathtt{FREED}(t) \text{ must remain } 0 \text{ indefinitely.}}$$

1. **Transaction Object Retention:** The driver must never free `struct fte3600_async_xfer` or its allocated `rx_buf` / `tx_buf` following a timeout or termination error.
2. **Permanent Bus Quarantine:** The physical SPI controller domain must be permanently marked as quarantined, refusing all future submissions with `-EBUSY`.
3. **Module Pinning:** The module reference must remain permanently held by the stranded transaction to prevent `rmmod` from unloading code while hardware could theoretically access memory.

**Conclusion:** The memory safety of the FT9362 driver rests on **conservative lifetime retention**, not on unproven silicon quiescence.
