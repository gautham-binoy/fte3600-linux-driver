# Stage D7.9-R5 — Module Reference Accounting and Worker Return Safety Proof

**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Host Controller:** Intel LPSS SPI (`pxa2xx-spi.2` / `idma64`)  
**Audit Status:** STRICTLY READ-ONLY ARCHITECTURAL SPECIFICATION & PROOF

---

## 1. Executive Summary and Problem Formulation

A classic, subtle concurrency bug in Linux loadable kernel modules is the **Worker-Return Instruction Pointer Race**:
If an asynchronous kernel workqueue worker function calls `module_put(THIS_MODULE)` as its final operation, the module refcount drops to zero while the worker CPU is still executing the epilogue or `ret` instruction of that worker function. If a concurrent `rmmod` (`delete_module`) observes the zero refcount, it immediately frees the module's memory via `vfree()` / `module_mem_free()`. The worker CPU then executes the return instruction into unmapped memory, triggering an instantaneous fatal kernel page fault (Oops/Panic).

This proof demonstrates how the Stage D7.9-R5 architecture completely eliminates this race through dedicated workqueue destruction synchronization and comprehensive reference accounting across all five module reference holders.

---

## 2. Anatomy of the Worker-Return Instruction Pointer Race

```
CPU 0 (Deferred Release Worker)             CPU 1 (sys_delete_module / rmmod)
----------------------------------------     ---------------------------------
/* worker function in fte3600.ko */
void fte3600_release_worker(work)
{
    kfree(xfer->rx_buf);
    kfree(xfer);
    
    module_put(THIS_MODULE); -------------> atomic_dec(&mod->refcnt) == 0!
    /* Instruction pointer still inside */   try_stop_module(mod) succeeds!
    /* fte3600.ko text section!        */   mod->state = MODULE_STATE_GOING;
    /* EPILOGUE: pop %rbp              */   free_module(mod);
    /* INSTRUCTION: ret                */     --> vm_unmap_ram / vfree(mod->core_layout.base)
    /* TARGET FAILS: 0xffffffffa0123456 */         (fte3600.ko .text is UNMAPPED!)
         |
         v
    BUG: unable to handle page fault
    Kernel Panic / Oops!
```

---

## 3. The Stage D7.9-R5 Elimination Protocol

To make asynchronous worker cleanup completely race-free, the architecture enforces two invariant mechanisms:

### 3.1 Mechanism A: Dedicated Workqueue Draining via `destroy_workqueue()`

The driver does **not** rely on the system workqueue (`system_wq`) for deferred release. Instead, it allocates a private workqueue:
```c
static struct workqueue_struct *fte3600_release_wq;

int __init fte3600_init(void)
{
    fte3600_release_wq = alloc_workqueue("fte3600_rel", WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
    if (!fte3600_release_wq)
        return -ENOMEM;
    ...
}
```

In `fte3600_exit()`, teardown proceeds in strict order:
```c
void __exit fte3600_exit(void)
{
    // Step 1: Unregister SPI and character drivers
    driver_unregister(&fte3600_spi_driver.driver);
    cdev_del(&fte3600_cdev);
    unregister_chrdev_region(fte3600_dev, 1);

    // Step 2: Synchronously flush and destroy deferred workqueue
    if (fte3600_release_wq) {
        destroy_workqueue(fte3600_release_wq);
        fte3600_release_wq = NULL;
    }

    // Step 3: Domain table teardown under spinlock
    ...
}
```

### 3.2 Formal Analysis of `destroy_workqueue()` in Linux `7.2.8`

In `kernel/workqueue.c`:
1. `destroy_workqueue(wq)` calls `drain_workqueue(wq)`.
2. `drain_workqueue(wq)` flushes all existing works and waits on `wait_event()` until the worklist is empty AND all active workers executing any work item associated with `wq` have finished their callbacks.
3. The worker function finishes executing, executes `ret`, and returns control back into `process_one_work()` located in core kernel memory (`vmlinux`).
4. Core `process_one_work()` marks the work item inactive and notifies `drain_workqueue()`.
5. Only then does `destroy_workqueue()` return to `fte3600_exit()`.
6. Only when `fte3600_exit()` completes does the kernel's module loader invoke `free_module(mod)`.

**Invariant 1:** No code inside `fte3600.ko` is being executed by any workqueue worker when module `.text` is unmapped.

### 3.3 Mechanism B: Elimination of Worker-Internal `module_put(THIS_MODULE)`

Because `destroy_workqueue()` provides deterministic barrier synchronization prior to module unloading:
- The deferred release worker **does NOT call `module_put(THIS_MODULE)`**.
- Instead, the module reference associated with the transaction was already handled during the admission/timeout phase.
- Specifically:
  - If a transaction completes normally, `module_put(THIS_MODULE)` is called by the ioctl process context before releasing `data->lock`.
  - If a transaction times out, the module reference is **retained permanently** as the controller domain pin.
  - The worker only frees DRAM allocations (`kfree(rx_buf)`, `kfree(xfer)`). It does not manipulate module refcounts.

---

## 4. Complete Accounting Across All 5 Reference Holders

The driver defines five distinct holders of module references, each governed by exact lifecycle rules:

```mermaid
graph LR
    subgraph Module [fte3600.ko (THIS_MODULE)]
        RefCnt[module_refcount]
    end

    H1["1. Char Dev Open (cdev pin)"] -->|open: try_module_get / close: module_put| RefCnt
    H2["2. Active ioctl Xfer"] -->|admit: try_module_get / done: module_put| RefCnt
    H3["3. Quarantined Controller Pin"] -->|CONVERTED live ref / NEVER released| RefCnt
    H4["4. Peripheral Driver (bus bind)"] -->|probe: auto / remove: auto| RefCnt
    H5["5. Deferred Worker Lifetime"] -->|Protected by destroy_workqueue barrier| RefCnt

    style RefCnt fill:#ffcdd2,stroke:#d32f2f,stroke-width:3px
    style H1 fill:#e8f5e9,stroke:#388e3c,stroke-width:2px
    style H2 fill:#e1f5fe,stroke:#0288d1,stroke-width:2px
    style H3 fill:#ffebee,stroke:#c62828,stroke-width:2px
    style H4 fill:#ede7f6,stroke:#512da8,stroke-width:2px
    style H5 fill:#fff3e0,stroke:#f57c00,stroke-width:2px
```

| Ref Holder | Entity | Acquisition Mechanism | Release Mechanism | Ref Count Contribution | Guarantee under Module Unload |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Holder 1** | Open File Descriptor (`/dev/fte3600`) | Core VFS `chrdev_open()` via `cdev->owner = THIS_MODULE` | Core VFS `__fput()` via `module_put()` | $+1$ per open fd | `rmmod` returns `-EBUSY` while any userspace process has file open. |
| **Holder 2** | In-flight ioctl transaction | `try_module_get(THIS_MODULE)` in process context under `data->lock` | `module_put(THIS_MODULE)` after completion under `data->lock` | $+1$ per active SPI transaction | Prevents unbind/unload while hardware transaction is running. |
| **Holder 3** | Quarantined Controller Domain Pin | Converted from Holder 2 reference upon 250 ms timeout | **NEVER RELEASED** (Permanent until platform reboot) | $+1$ per quarantined controller | **Permanently blocks `rmmod`** (`-EBUSY`). Cannot be unloaded after hardware lockup. |
| **Holder 4** | SPI Device Driver (`spi_driver`) | Core SPI bus `driver_register(&fte3600_spi_driver.driver)` | Core SPI bus `driver_unregister()` | Managed by Linux driver core (`drv->owner`) | Module cannot be unloaded while driver is bound to device. |
| **Holder 5** | Deferred Release Work Item | Queued onto `fte3600_release_wq` | Synchronously drained by `destroy_workqueue()` in `fte3600_exit()` | $0$ (Worker holds no module ref; drain barrier guarantees return) | Worker instruction pointer returns to `vmlinux` before module unmap. |

---

## 5. Formal Verification of Module Unload Scenarios

### Scenario 1: Clean State (No Transactions, No Open FDs)
- Open FDs: $0$
- Active Xfers: $0$
- Quarantined Domains: $0$
- Total Module Refcount: $0$
- Execution: `rmmod fte3600` proceeds. `fte3600_exit()` runs, `destroy_workqueue()` flushes empty queue, drivers unregistered, `.text` freed cleanly. **PASS.**

### Scenario 2: Active Transaction Running (In-flight SPI)
- Open FDs: $\ge 1$
- Active Xfer: $1$
- Quarantined Domains: $0$
- Total Module Refcount: $\ge 2$
- Execution: `rmmod fte3600` invokes `delete_module()`, which checks `module_refcount(mod) > 0` and immediately returns `-EBUSY`. Driver remains untouched. **PASS.**

### Scenario 3: Hardware Hung / Quarantined Controller
- Open FDs: Closed by userspace ($0$)
- Active Xfers: $0$
- Quarantined Domains: $1$ (Domain Pin converted)
- Total Module Refcount: $1$
- Execution: `rmmod fte3600` checks `module_refcount(mod) == 1` and returns `-EBUSY`.
- Even if forced with `rmmod -f` (`CONFIG_MODULE_FORCE_UNLOAD`):
  - Because `destroy_workqueue()` drains pending work and timed-out buffers are permanently pinned in DRAM, memory integrity is maintained.
  - Standard production kernels have `CONFIG_MODULE_FORCE_UNLOAD=n`, making unload physically impossible. **PASS.**

### Scenario 4: Late Completion After Driver Unbind
- If the peripheral driver is unbound via sysfs (`echo spi-FTE3600:00 > unbind`):
  - `fte3600_remove()` runs.
  - Sets `domain->state = DOMAIN_REMOVING`.
  - When late interrupt fires, completion callback checks generation and domain state:
  - Generates zero work, acquires no module ref, performs zero invalid memory dereferences. **PASS.**

---

## 6. Conclusion and Stage D8 Readiness

The Worker-Return Instruction Pointer Race is fundamentally solved:
1. Work items are drained completely by `destroy_workqueue()` prior to module memory unmapping.
2. Workers never call `module_put(THIS_MODULE)`.
3. Quarantined controllers hold permanent module references, guaranteeing that a poisoned driver can never be unloaded by standard system utilities.
4. All reference holders are accounted for with zero leaks and zero race windows.
