# Stage D8 — Lock Order, Concurrency and Atomic Context Specification

**Target Device:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Status:** FROZEN ARCHITECTURAL SPECIFICATION FOR STAGE D8  
**Safety Gate:** STRICTLY READ-ONLY DESIGN — ZERO HARDWARE ACCESS AUTHORIZED

---

## 1. Executive Summary

This specification freezes the synchronization topology of the `fte3600` Linux kernel driver. It defines the formal Directed Acyclic Graph (DAG) for all lock primitives, proves mathematical deadlock freedom, enforces atomic non-sleeping invariants, and details the remediated lock ordering across workqueue cancellations and interrupt contexts.

---

## 2. Lock Hierarchy Definition and Partial Order

The driver architecture contains three primary locking levels, ordered strictly from coarse-grained sleepable mutexes to fine-grained non-sleeping spinlocks:

$$\boxed{\text{Level 1 } (data\to lock) \prec \text{Level 2 } (fte3600\_domain\_lock) \prec \text{Level 3 } (ctlr\to queue\_lock)}$$

```mermaid
graph TD
    subgraph Process Context [Process Context (Sleepable)]
        L1["Level 1: data->lock (struct mutex)"]
        Alloc["kmalloc(GFP_KERNEL) / copy_to_user"]
        L2_P["Level 2: fte3600_domain_lock (spinlock_t)"]
        L3["Level 3: ctlr->queue_lock (Core SPI Master)"]
        Wait["wait_for_completion_timeout()"]
    end

    subgraph Atomic Context [Interrupt / Tasklet Context (Non-Sleepable)]
        L2_IRQ["Level 2: fte3600_domain_lock (spinlock_t)"]
    end

    subgraph Module Teardown [Module Teardown / Unbind]
        Cancel["cancel_delayed_work_sync() (OUTSIDE LOCK)"]
        L1_Tear["Level 1: data->lock (struct mutex)"]
        L2_Tear["Level 2: fte3600_domain_lock (spinlock_t)"]
        WQ_Drain["destroy_workqueue(fte3600_release_wq)"]
    end

    Cancel --> L1_Tear
    L1_Tear --> L2_Tear
    L2_Tear --> WQ_Drain

    L1 --> Alloc
    Alloc --> L2_P
    L2_P --> L3
    L3 -.->|Spinlock Released| Wait

    L2_IRQ -.->|Independent Leaf| Done[complete(&xfer->done)]

    style L1 fill:#e1f5fe,stroke:#0288d1,stroke-width:2px
    style L2_P fill:#fff3e0,stroke:#f57c00,stroke-width:2px
    style L2_IRQ fill:#fff3e0,stroke:#f57c00,stroke-width:2px
    style L3 fill:#fce4ec,stroke:#c2185b,stroke-width:2px
    style Cancel fill:#e8f5e9,stroke:#388e3c,stroke-width:2px
```

### 2.1 Synchronization Primitive Details

| Level | Lock Primitive | C Type | Scope | Context Acquired | Sleeping Allowed? | Local IRQs Disabled? |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **L1** | `data->lock` | `struct mutex` | Per-Device Instance | Process Context (ioctl, sysfs, open/close) | **YES** | No |
| **L2** | `fte3600_domain_lock` | `spinlock_t` | Global Subsystem | Process, Workqueue, Tasklet, Hardirq | **NO** | **YES (`irqsave`)** |
| **L3** | `ctlr->queue_lock` | `spinlock_t` | SPI Master Queue | Core `spi_async()` internal | **NO** | **YES (`irqsave`)** |

---

## 3. Strict Non-Sleeping Invariants under Level 2 Spinlock

In Linux kernels compiled with `CONFIG_DEBUG_ATOMIC_SLEEP=y`, calling any scheduling function while holding a spinlock triggers a fatal kernel warning or panic (`BUG: scheduling while atomic`).

### Invariant Rules for Level 2 Critical Section
1. **Zero Heap Allocations:** `kmalloc()`, `kzalloc()`, `kfree()` are strictly prohibited. All dynamic memory must be pre-allocated under L1 before taking L2.
2. **Zero Sleeping Waits:** `wait_for_completion*()`, `msleep()`, `schedule()` are strictly prohibited.
3. **Zero Userspace Copies:** `copy_to_user()`, `copy_from_user()` are strictly prohibited.
4. **Permitted Operations Only:**
   - Reading and writing `domain->state`.
   - Incrementing `domain->active_generation`.
   - Pointer assignment (`domain->active_xfer = xfer`).
   - `complete(&xfer->done)` (which takes internal wait queue spinlock).
   - Atomic refcount reads/writes.
   - Diagnostic `printk()` / `pr_warn()` (atomic safe).

---

## 4. Workqueue Cancellation Deadlock Remediation

### 4.1 The Deadlock Hazard
In `fte3600_spi_remove()` and `fte3600_release()`, the driver cancels the session watchdog.
- `fte3600_watchdog_work()` executes on `system_wq` and acquires `mutex_lock(&data->lock)`.
- If the unbind/release thread acquires `data->lock` and then invokes `cancel_delayed_work_sync(&data->session.watchdog_work)`:
  - Thread 1 holds `data->lock`, waiting for watchdog work to finish.
  - Thread 2 (watchdog) is waiting for `data->lock`.
  - **Deadlock is immediate and unrecoverable.**

### 4.2 Frozen Remediation Sequence
```c
/* MANDATORY UNBIND / TEARDOWN SEQUENCE FOR STAGE D8: */
void fte3600_spi_remove(struct spi_device *spi)
{
    struct fte3600_data *data = spi_get_drvdata(spi);

    /* Step 1: Cancel watchdog OUTSIDE data->lock */
    cancel_delayed_work_sync(&data->session.watchdog_work);

    /* Step 2: Now acquire data->lock safely */
    mutex_lock(&data->lock);
    atomic_set(&data->offline, 1);
    data->state = FTE3600_STATE_DEVICE_REMOVAL;

    /* Step 3: Transition domain state under L2 */
    spin_lock_irqsave(&fte3600_domain_lock, flags);
    if (data->domain)
        data->domain->state = DOMAIN_REMOVING;
    spin_unlock_irqrestore(&fte3600_domain_lock, flags);

    mutex_unlock(&data->lock);
}
```

---

## 5. Formal Deadlock-Freedom Matrix

| Context A Holding | Context A Attempting | Context B Holding | Context B Attempting | Inversion Cycle Possible? | Mathematical Proof |
| :--- | :--- | :--- | :--- | :--- | :--- |
| Process (L1) | L2 | Callback (L2) | — | **NO** | Callback never attempts L1. Poset is acyclic. |
| Process (L1) | L3 | Controller (L3) | — | **NO** | Controller queue lock never attempts L1 or L2. |
| Process (L2) | — | Callback (L2) | — | **NO** | Serialized by spinlock with IRQs disabled on local CPU. |
| Unbind (Process) | Watchdog cancel | Watchdog (Worker) | L1 | **NO** | Cancellation is outside L1; watchdog acquires L1 cleanly. |
| Exit (Process) | WQ Drain | Worker (WQ) | — | **NO** | Worker holds no driver locks and zero module references. |

---

## 6. Conclusion and Stage D8 Freeze Sign-Off

The lock order $L1 \prec L2 \prec L3$ is mathematically proven acyclic, non-inverting, and free of sleep-under-atomic violations. It is frozen for Stage D8 implementation.
