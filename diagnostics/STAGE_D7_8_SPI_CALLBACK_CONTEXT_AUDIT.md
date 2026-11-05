# STAGE D7.8: SPI Message Completion Callback Context Audit

**Target:** FocalTech FT9362 / FW9369 Driver (`fte3600`)  
**Kernel:** Linux 7.2.8-9-omarchy (x86_64 SMP PREEMPT_DYNAMIC)  
**Host Controller:** Intel LPSS SPI (`pxa2xx-spi` / `idma64`)  
**Audit Classification:** READ-ONLY Verification and Proof Audit  

---

## 1. Executive Summary

This audit establishes the exact execution context contract for `spi_message.complete` callbacks within the Linux SPI subsystem (`drivers/spi/spi.c`) and host controller driver (`drivers/spi/spi-pxa2xx.c`). 

Stage D7.7 proposed transitioning from synchronous `spi_sync()` to asynchronous `spi_async()` with a bounded software completion timeout (250 ms) and deferred buffer reclamation inside the SPI completion callback. Before implementing this mechanism, we must mathematically and architecturally verify what operations are permissible inside `msg->complete(msg->context)` to guarantee zero kernel panics, zero deadlock scenarios, and zero use-after-free conditions.

### Primary Audit Findings:
1. **Atomic Context Invariant:** The SPI message completion callback can be executed in **hardirq context** (PIO FIFO interrupt `ssp_int()`) or **softirq/tasklet context** (DMA completion via `idma64`), or from kernel worker thread context (`spi_pump_messages()`). Therefore, the driver must treat the callback as strictly **atomic and non-sleepable**.
2. **Sleeping Prohibitions:** `might_sleep()` primitives, including standard mutexes (`mutex_lock(&data->lock)`), semaphores, `msleep()`, and memory allocations using `GFP_KERNEL`, are **strictly forbidden** inside the callback.
3. **Safe Primitives:** Non-sleeping spinlocks (`spin_lock_irqsave`), atomic counters (`kref_put`, `refcount_dec_and_test`), `complete()`, and lockless work scheduling (`schedule_work()`, `queue_work()`) are **100% safe**.
4. **Memory Deallocation:** `kfree()` is explicitly documented and implemented in the Linux kernel as safe to invoke from atomic/interrupt context.

---

## 2. Kernel Call Graph and Execution Context Tracing

### 2.1 SPI Core Finalization Path

In `drivers/spi/spi.c`, message completion is handled by `spi_finalize_current_message()`:

```c
void spi_finalize_current_message(struct spi_controller *ctlr)
{
    struct spi_message *mesg;
    ...
    mesg = ctlr->cur_msg;
    ...
    spi_unmap_msg(ctlr, mesg);

    if (mesg->complete)
        mesg->complete(mesg->context);
    ...
}
EXPORT_SYMBOL_GPL(spi_finalize_current_message);
```

### 2.2 Host Controller Invocation Context (`pxa2xx-spi`)

The Dynabook Satellite Pro C40-J routes the FT9362 SPI bus through an Intel LPSS SPI controller driven by `spi-pxa2xx.c`. Depending on transfer size and system configuration, the controller operates in one of three modes:

```mermaid
flowchart TD
    subgraph Host_Controller_Modes [PXA2xx SPI Controller Execution Paths]
        PIO[Programmed I/O Mode\nssp_int IRQ Handler] -->|Hardirq / ISR| FIN1[spi_finalize_current_message]
        DMA[DMA Mode\nidma64 Tasklet / Callback] -->|Softirq / Tasklet| FIN2[spi_finalize_current_message]
        KWORKER[Queue Worker\nspi_pump_messages] -->|kworker Process Context| FIN3[spi_finalize_current_message]
    end

    FIN1 --> CB[msg->complete Callback]
    FIN2 --> CB
    FIN3 --> CB

    CB --> RULE{Execution Rule:\nStrictly Atomic}
```

1. **Programmed I/O Mode (PIO):**
   - Triggered for small command/register transfers (< 32 bytes).
   - Interrupt handler `ssp_int()` handles FIFO TX/RX.
   - When FIFO empties and last byte is read, `pxa2xx_spi_handle_dirty_fifos()` or `pxa2xx_spi_transfer_one()` calls `spi_finalize_current_message()`.
   - **Context:** **Hard Interrupt Context (`in_hardirq() == true`)**.
2. **DMA Mode (`idma64`):**
   - Triggered for bulk frame reads (e.g. 5,120 bytes or 10,240 bytes).
   - DMA controller raises MSI/IRQ; DMA driver processes completion via tasklet or callback.
   - `pxa2xx_spi_dma_transfer_complete()` calls `spi_finalize_current_message()`.
   - **Context:** **Softirq / Bottom Half Context (`in_serving_softirq() == true` or `in_interrupt() == true`)**.
3. **Queued Worker Path:**
   - If controller handles completion in queue pump thread.
   - **Context:** Kernel worker (`kworker`) thread with interrupts enabled.

**Conclusion:** Because the callback can execute in hardirq or softirq context, **the callback must adhere to the strictest atomic execution contract**.

---

## 3. Atomic Context Restrictions Analysis

| Primitive / Operation | Permissible in Callback? | Kernel Mechanism & Consequence if Violated |
| :--- | :---: | :--- |
| `mutex_lock(&data->lock)` | **STRICTLY FORBIDDEN** | `might_sleep()` bug; system crash / kernel panic (`scheduling while atomic`). |
| `spin_lock_irqsave(&xfer->lock, flags)` | **PERMITTED** | Safe. Disables local IRQs and protects shared state across CPU cores. |
| `complete(&xfer->done)` | **PERMITTED** | `complete()` is explicitly designed to be callable from interrupt context. |
| `kmalloc(..., GFP_KERNEL)` | **STRICTLY FORBIDDEN** | Can sleep during page reclaim. Panics in atomic context. |
| `kmalloc(..., GFP_ATOMIC)` | Avoid | Permitted in atomic, but dynamic allocation in callback is poor design. |
| `kfree(ptr)` | **PERMITTED** | SLUB/SLAB `kfree()` is explicitly IRQ-safe on Linux. |
| `schedule_work(&data->work)` | **PERMITTED** | Lockless insertion into workqueue. Non-blocking and safe from IRQ context. |
| `put_device(&spi->dev)` | Conditional / Caution | Permitted only if device release callback does not sleep. Safer deferred. |
| `kref_put(&xfer->kref, release)` | **PERMITTED** | Atomic refcount decrement; safe if `release` only executes `kfree()`. |

---

## 4. Safety Contract for `fte3600_async_complete()`

Based on the verified kernel constraints, the callback function `fte3600_async_complete()` must follow these exact structural rules:

```c
static void fte3600_async_complete(void *context)
{
    struct fte3600_async_xfer *xfer = context;
    unsigned long flags;
    bool was_timed_out;

    /* 1. Acquire spinlock with IRQ protection - strictly non-sleeping */
    spin_lock_irqsave(&xfer->lock, flags);
    
    was_timed_out = (xfer->state == FTE3600_XFER_TIMED_OUT);
    if (!was_timed_out) {
        xfer->state = FTE3600_XFER_COMPLETED;
    }
    
    spin_unlock_irqrestore(&xfer->lock, flags);

    /* 2. Signal the waiting ioctl thread if still waiting */
    complete(&xfer->done);

    /* 3. If timeout already occurred, defer any heavy cleanup or schedule work */
    if (was_timed_out) {
        /* Optional: queue recovery work */
    }

    /* 4. Drop callback reference count. If waiter already dropped its ref,
     * this frees the buffer and xfer structure safely. */
    kref_put(&xfer->kref, fte3600_xfer_release);
}
```

---

## 5. Summary Verdict

The execution context of the SPI callback is definitively **atomic** (can run in hardirq or softirq). The proposed asynchronous design is mathematically safe provided the callback uses **only spinlocks, `complete()`, `kref_put()`, and `kfree()`**, and never attempts to acquire the peripheral mutex or sleep.
