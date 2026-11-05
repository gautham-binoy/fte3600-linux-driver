# Stage D7.9-R6 — Hardware DMA Quiescence, FIFO Drain Limits and Retention Invariant Audit

**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Host Controller:** Intel LPSS SPI (`pxa2xx-spi.2` / `idma64`, PCI `0000:00:1e.3`)  
**Audit Scope:** Revalidation of Conservative Retention and Hardware Quiescence Evidence  
**Audit Status:** STRICTLY READ-ONLY SOURCE AUDIT  
**Audit Verdict:** **PASS (Retention Invariant $\mathtt{FREED}(t) \equiv 0$ Revalidated Without Exception)**

---

## 1. Executive Summary and Problem Statement

When an asynchronous SPI operation experiences a 250 ms timeout, software detects that the transaction has not completed. In standard kernel development, an engineer might be tempted to call `dmaengine_terminate_sync()` and immediately free the underlying `rx_buf` and `tx_buf` back to the SLUB allocator.

Stage D7.9-R3 and R4 demonstrated that in Linux `7.2.8` on the Intel LPSS `idma64` controller:
- `idma64_terminate_sync()` attempts to clear channel active bits and waits up to 100 µs for FIFO draining.
- If hardware registers or silicon bus lines are locked up, the timeout expires without hardware reset.
- Software continues, freeing descriptors and killing tasklets.
- **However, physical hardware silicon may still execute AHB/PCIe bus master DMA writes into host DRAM!**
- If the kernel freed `rx_buf`, SLUB reassigns those exact memory addresses to other kernel tasks or userspace pages.
- A delayed FIFO drain or clock edge then triggers silent, catastrophic DRAM corruption.

This audit re-affirms the non-negotiable **Conservative DMA Buffer Retention Invariant**:

$$\boxed{\mathtt{Hardware\_Quiescence\_Unproven} \implies \mathtt{FREED}(t) \equiv 0}$$

---

## 2. Revalidation of the Five Absolute DMA Safety Tenets

| Tenet | Adversarial Fallacy | Exact Kernel Source / Silicon Reality | Mandatory Stage D8 Invariant |
| :--- | :--- | :--- | :--- |
| **Tenet 1: Timeout $\neq$ Quiescence** | "The ioctl timed out, so the transfer is dead and memory can be freed." | The 250 ms timeout is a pure software timer. The controller hardware may still be reading/writing DMA buffers in host RAM. | **NEVER free RX/TX buffers on timeout.** |
| **Tenet 2: Unbind $\neq$ Termination** | "Unbinding `spi-FTE3600:00` stops DMA transfers." | Peripheral unbind detaches the slave device. It does not terminate the host SPI master's DMA queue. | **Peripheral unbind must never free in-flight DMA buffers.** |
| **Tenet 3: Tasklet Kill $\neq$ Silicon Quiescence** | "`tasklet_kill(&vc->task)` proves DMA has stopped." | `tasklet_kill()` halts only the CPU software tasklet; it has zero electrical effect on physical silicon bus mastering. | **Software synchronization cannot be treated as hardware quiescence.** |
| **Tenet 4: Delayed Callback $\neq$ Quarantine Clear** | "A late callback arrived, so quarantine can be lifted." | A late callback proves the controller experienced a serious hardware lockup. Re-admitting transactions invites immediate recurrence. | **Quarantine is strictly latching and irreversible until reboot.** |
| **Tenet 5: Zero Memory Reuse** | "Re-use the old RX buffer for the next transaction to save RAM." | If the controller DMA is still pointing to that physical address, new data and delayed DMA writes will collide destructively. | **Retained buffers are permanently retired from all future transactions.** |

---

## 3. Dissecting `idma64.c` in Linux 7.2.8

In `drivers/dma/idma64.c`:

```c
/* drivers/dma/idma64.c:idma64_terminate_all */
static int idma64_terminate_all(struct dma_chan *chan)
{
    struct idma64_chan *idma64c = to_idma64_chan(chan);
    ...
    /* Attempt to disable channel */
    idma64_chan_disable(idma64c);

    /* Spin for at most 100 microseconds awaiting FIFO drain */
    for (i = 0; i < 100; i++) {
        if (!(idma64_read_ch(idma64c, IDMA64_CH_EN) & IDMA64_CH_EN_ENABLED))
            break;
        udelay(1);
    }
    ...
    /* Software drops descriptor list regardless of whether break occurred! */
    idma64_vdesc_free(vdesc);
    return 0;
}
```

### 3.1 The Silicon Gap
If the hardware FIFO does not drain within 100 µs:
- `idma64_terminate_all()` does **not** reset the host PCI bridge.
- The function exits with 0 (success).
- The Linux DMA engine subsystem assumes the channel is dead.
- But at the physical silicon level, the AHB DMA controller bus master logic remains enabled and may assert PCIe Memory Write Requests whenever the FIFO unblocks.
- Therefore, **the host kernel has no software mechanism to guarantee physical silicon cessation**.

---

## 4. Evidence Required to Transition Away from Retention (Future Non-D8)

To ever permit dynamic buffer deallocation after timeout in any future driver revision, the following **Three Empirical Evidence Gates** must be physically satisfied:

```
+---------------------------------------------------------------------------------------------------------+
|                                    MANDATORY HARDWARE EVIDENCE GATES                                    |
+------------------------------+--------------------------------------------------------------------------+
| Gate 1: Silicon Datasheet    | Official Intel Tiger Lake / LPSS hardware documentation proving that     |
|                              | clearing CH_EN sever PCIe master request lines within N clock cycles.    |
+------------------------------+--------------------------------------------------------------------------+
| Gate 2: MMIO Verification    | Driver reads back hardware status register verifying bus master IDLE     |
|                              | bit before allowing software deallocation.                               |
+------------------------------+--------------------------------------------------------------------------+
| Gate 3: Protocol Analyzer    | Hardware PCIe protocol analyzer trace verifying zero Memory Write cycles |
|                              | occur on the bus post-termination.                                       |
+------------------------------+--------------------------------------------------------------------------+
```

In the absence of these three gates, the Conservative Retention Policy ($\mathtt{FREED}(t) \equiv 0$) is the sole mathematically defensible architecture.

---

## 5. Conclusion and Stage D8 Implementation Contract

1. When a transaction times out at 250 ms, the driver **must retain `xfer->rx_buf` and `xfer->tx_buf` in DRAM permanently**.
2. Neither `kfree(rx_buf)` nor `kfree(tx_buf)` shall be invoked on the timeout path.
3. The controller domain remains permanently in `DOMAIN_QUARANTINED`.
4. Stage D8 implementation must strictly embed these rules.
