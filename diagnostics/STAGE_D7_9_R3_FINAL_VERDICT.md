# STAGE D7.9-R3: Final Verdict — Adversarial Verification of Quarantine, DMA Quiescence and Reference Safety

**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Peripheral Device:** FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Host Controller:** Intel LPSS SPI (`pxa2xx-spi.2` / `idma64`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Audit Scope:** Independent Adversarial Verification of Stage D7.9-R2  
**Final Verdict:** **CONDITIONAL PASS (Architecture Viable Subject to 4 Mandated Adversarial Corrections)**  

---

## 1. Adversarial Audit Findings Summary

Stage D7.9-R3 subjected the D7.9-R2 asynchronous SPI lifetime and global quarantine architecture to rigorous, adversarial source-level verification against exact Linux 7.2.8 kernel code.

While the fundamental hybrid calibration concept and dual-ownership reference model (`kref = 2`) are sound, this adversarial audit discovered four critical vulnerabilities in the earlier D7.9-R2 conclusions:

| Investigation Area | D7.9-R2 Assertion | Adversarial R3 Finding | Safety Resolution |
| :--- | :--- | :--- | :--- |
| **Hardware DMA Quiescence** | Claimed mathematically proven by `idma64_terminate_all()` | **UNPROVEN AT SILICON LEVEL:** `idma64_chan_deactivate()` loops 100 µs without error checking; `channel_clear_bit(CH_EN)` is a posted MMIO write without readback; `tasklet_kill()` is software-only. | **Conservative Buffer Retention:** Buffers and transaction objects must remain retained indefinitely ($\mathtt{FREED}(t) = 0$). |
| **Quarantine Synchronization** | Claimed synchronized under `data->lock` | **PER-DEVICE LOCK ANTIPATTERN:** `data->lock` is dynamically allocated per instance in `probe()`. It cannot synchronize across probe, rebind, or other device instances. | **Global Spinlock Mandate:** Implement `static DEFINE_SPINLOCK(fte3600_quarantine_lock)` protecting both lookup and publication. |
| **Final Release Context** | Claimed direct atomic release via `spi_dev_put()` was safe | **ATOMIC CONTEXT VIOLATION:** `spi_dev_put()` calls `put_device()` (`/* might_sleep(); */`) $\rightarrow$ `device_release()` $\rightarrow$ `devres_release_all()`, which runs sleepable devres hooks. | **Deferred Workqueue Release:** Defer final release to `system_wq` when dropped in atomic softirq / hardirq context. |
| **Quarantine Persistence** | Claimed `.bss` quarantine persists across unloads | **NON-STRANDED UNLOAD VULNERABILITY:** A module-global variable in `.bss` is wiped by `rmmod; insmod`. Stranded transactions pin the module, but non-stranded aborts do not. | **Universal Module Pinning:** Call `__module_get(THIS_MODULE)` on *any* quarantine trigger to block `rmmod`. |

---

## 2. Verdict Determination

In accordance with Stage D7.9-R3 verdict criteria:
- **PASS** is disqualified because hardware DMA quiescence cannot be established by driver source alone, and direct `spi_dev_put()` in atomic context violates kernel sleeping rules.
- **BLOCKED** is avoided because memory safety does not depend on silicon quiescence; memory safety is guaranteed by the **Conservative Buffer Retention Rule** (retaining buffers permanently) and the **Deferred Workqueue Release Pattern**.

> ### **CONDITIONAL PASS**
> **The asynchronous hybrid architecture is viable, but memory safety and race-freedom are strictly dependent on the 4 Mandated Adversarial Corrections.**  
> **Stage D8 (Source Implementation) is APPROVED exclusively under the incorporation of these 4 corrections into the codebase.**

---

## 3. The 4 Mandated Adversarial Corrections for Stage D8

### Correction 1: Conservative Buffer Retention Rule
Whenever an asynchronous SPI transaction times out:
1. The submitting thread drops its reference (`Ref_submitter`).
2. The transaction object and its allocated DMA buffers (`rx_buf`, `tx_buf`) must **never be freed by the driver**.
3. `Ref_callback` remains held indefinitely. Even if the hardware DMA engine continues bus bursts into memory, it writes into a valid, permanently pinned kernel slab buffer. Zero Use-After-Free is possible.

### Correction 2: Module-Global Spinlock Synchronization
Quarantine lookup and publication must not use `data->lock`. The driver must define:
```c
static DEFINE_SPINLOCK(fte3600_quarantine_lock);
```
Both `fte3600_quarantine_check_and_admit()` and `fte3600_quarantine_publish()` must execute under `spin_lock_irqsave(&fte3600_quarantine_lock, flags)`. `probe()` must verify the quarantine table under this lock before allocating or registering `/dev/fte3600`.

### Correction 3: Deferred Workqueue Release for `spi_dev_put()`
The transaction release function must inspect execution context:
```c
static void fte3600_xfer_release(struct kref *kref)
{
    struct fte3600_async_xfer *xfer = container_of(kref, struct fte3600_async_xfer, ref);

    if (in_interrupt() || irqs_disabled()) {
        INIT_WORK(&xfer->release_work, fte3600_xfer_release_work);
        schedule_work(&xfer->release_work);
    } else {
        fte3600_xfer_release_work(&xfer->release_work);
    }
}
```
`spi_dev_put(xfer->spi)` must execute exclusively inside `fte3600_xfer_release_work()` in process context, completely eliminating atomic scheduling violations.

### Correction 4: Universal Module Pinning on All Quarantine Triggers
Whenever a hardware bus domain is quarantined (whether due to a transaction timeout, a watchdog abort, or unrecoverable sensor framing corruption), the driver must execute:
```c
__module_get(THIS_MODULE);
```
This guarantees that standard `rmmod` will return `-EBUSY`, preventing administrative unloading from wiping the in-memory quarantine table.

---

## 4. Final Safety Status

- Kernel Module `fte3600` is **NOT LOADED**.
- Device node `/dev/fte3600` is **ABSENT**.
- Device binding `spi-FTE3600:00` is **UNBOUND**.
- Hardware SPI statistics: strictly **transfers = 0, bytes = 0**.
- Working copy driver source: **100% UNTOUCHED**.
