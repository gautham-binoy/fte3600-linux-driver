# STAGE D7.9-R3: Audit 3 — Quarantine Identity, Topological Specificity and Persistence Audit

**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Peripheral Device:** FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Host Controller:** Intel LPSS SPI (`pxa2xx-spi.2` / `idma64`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Audit Scope:** Adversarial Evaluation of Quarantine Key Specificity, Topological False Positives/Negatives, and Persistence across Module Unload  

---

## 1. Executive Summary

Stage D7.9-R2 proposed identifying a quarantined transaction domain using the 3-tuple `(bus_num, chip_select, parent_name)`. This adversarial audit evaluates whether this keying mechanism is genuinely immune to topological re-aliasing, and whether quarantine persistence can survive driver unloading.

The adversarial investigation establishes:
1. **Topological Precision:** The 3-tuple `(bus_num, chip_select, parent_name)` successfully prevents false negatives across controller re-registration because the parent PCI/ACPI path (`0000:00:1e.3` / `\_SB.PC00.SPI1`) is physically hardwired in firmware and cannot be forged by dynamically reassigned integer bus IDs.
2. **False Positives (Fail-Closed):** If the host controller is hot-unplugged and a completely different peripheral is attached to the same physical bus, the domain remains quarantined. In safety engineering, this false positive is an intentional **fail-closed** property.
3. **The Module Unload Persistence Boundary:** A module-global variable resides in the `.bss` section of `fte3600.ko`. **It CANNOT survive `rmmod` and `insmod`.**
4. **Conditional vs. Unconditional Pinning:** If a transaction times out, `THIS_MODULE` refcount is permanently pinned by the unreleased `Ref_callback`, causing `rmmod` to fail with `-EBUSY`. However, if a bus is quarantined due to a non-stranded condition (e.g. calibration abort, watchdog reset, or framing corruption), `THIS_MODULE` is **NOT pinned**. In those cases, `rmmod` succeeds and subsequent `insmod` silently resets the quarantine table!
5. **Forced Unload Hazard:** If `CONFIG_MODULE_FORCE_UNLOAD=y` and `rmmod -f` is executed, the module text is ripped out regardless of reference counts, leaving any stranded DMA engine pointed at unmapped memory.

---

## 2. Topological Key Evaluation Matrix

| Dynamic System Scenario | Risk of False Negative (Unsafe Admission) | Risk of False Positive (Safe Rejection) | Resolution by Proposed 3-Tuple |
| :--- | :--- | :--- | :--- |
| **Controller Removal & Re-registration** | High if using bare `bus_num` (IDR assigns same integer to new controller) | None | **Resolved:** `parent_name` (`0000:00:1e.3`) verifies identity of the physical controller hardware. |
| **Bus-Number Dynamic Reallocation** | High if IDR shifts numbers | Low | **Resolved:** Both `bus_num` and `parent_name` must match. |
| **Multiple Peripheral Devices on Same Controller** | High if keyed by bus only | High (blocks unrelated devices) | **Resolved:** `chip_select` isolates `cs=0` from `cs=1`. |
| **Multiple Identical Controllers** | High if keyed by driver name | Low | **Resolved:** PCI BDF (`0000:00:1e.3` vs `0000:00:1f.5`) differentiates controllers. |
| **Device Hot-Unplug / Rebind** | Zero (quarantine persists in module RAM) | Low | **Resolved:** Module-global domain survives sysfs `unbind`/`bind`. |
| **System Cold Reboot** | Zero (power cycle clears hardware DMA) | None | Preserved: Resetting quarantine on reboot is mathematically safe because hardware state is re-initialized. |
| **Module Unload & Reload (`rmmod; insmod`)** | **CRITICAL:** If `rmmod` succeeds, `.bss` is wiped! | None | **CONDITIONAL:** Survives IF AND ONLY IF `THIS_MODULE` refcount is pinned. |

---

## 3. Module Reference Pinning Proof and Vulnerability Analysis

### The Standard Unload Blocking Mechanism
In `kernel/module/main.c`:
```c
bool try_module_get(struct module *module)
{
    if (module) {
        if (likely(module_is_live(module) &&
                   atomic_inc_not_zero(&module->refcnt) != 0))
            return true;
        else
            return false;
    }
    return true;
}

SYSCALL_DEFINE2(delete_module, const char __user *, name_user, unsigned int, flags)
{
    ...
    ret = try_stop_module(mod, flags, &forced);
    if (ret != 0)
        goto out;
    ...
}

static int try_release_module_ref(struct module *mod)
{
    ret = atomic_sub_return(MODULE_REF_BASE, &mod->refcnt);
    if (ret)
        ret = atomic_add_unless(&mod->refcnt, MODULE_REF_BASE, 0);
    return ret;
}
```

When an asynchronous transaction is submitted:
1. Submitter calls `try_module_get(THIS_MODULE)`. Module refcount increments by 1.
2. If `spi_async()` times out, the waiter abandons waiting, but **does not call `module_put(THIS_MODULE)`**.
3. `module_put()` is located strictly in `fte3600_xfer_release()`, which is called when `kref` reaches 0.
4. Because the SPI completion callback never arrived (or was suppressed by IDMA64 termination), `Ref_callback` is never dropped.
5. Consequently, `kref` remains $\ge 1$, `fte3600_xfer_release()` is never invoked, and `module_put(THIS_MODULE)` is **never executed**.
6. When an administrator executes `rmmod fte3600`, `delete_module()` detects `mod->refcnt > MODULE_REF_BASE`.
7. `try_stop_module()` returns `-EWOULDBLOCK` / `-EBUSY`.
8. The module cannot be unloaded. The `.bss` section containing the quarantine table cannot be deallocated.

### The Adversarial Discovery: Non-Stranded Quarantine Clearing
What if a bus domain is quarantined due to a condition that does **not** strand an asynchronous SPI transaction?
- Example 1: Calibration watchdog expires while waiting for userspace computation.
- Example 2: Consecutive framing / CRC corruptions trigger the fail-closed latch.
- Example 3: Peripheral initialization protocol detects an unrecoverable hardware state.

In these cases, all prior SPI transactions completed normally and dropped their module references!
- Submitter refcount: 0
- Callback refcount: 0
- `THIS_MODULE->refcnt == MODULE_REF_BASE`
- **Result:** If userspace runs `rmmod fte3600`, `delete_module()` **succeeds immediately**!
- The kernel unmaps `fte3600.ko`.
- When userspace runs `insmod fte3600.ko`, the module's `.bss` section is re-allocated as all zeros!
- **The quarantine is wiped, and transactions can be admitted again!**

---

## 4. Mandated Quarantine Invariants for Stage D8

To guarantee absolute persistence across all failure scenarios:

1. **Mandatory Module Pinning on Any Quarantine Event:**
   Whenever a bus is quarantined—regardless of whether a transaction was stranded or an operational error occurred—the quarantine publishing function must execute:
   ```c
   /* Permanently pin the module against rmmod */
   __module_get(THIS_MODULE);
   ```
   This ensures that `rmmod` will unconditionally return `-EBUSY` whenever any quarantine entry in the module is active.
2. **Topological Identification Binding:**
   Every quarantine entry must record `(bus_num, chip_select, parent_device_name)`.
3. **Explicit Warning on Forced Unload:**
   The driver documentation must specify that `rmmod -f` violates safety invariants and will corrupt kernel memory if hardware DMA was stranded.

**Verdict:** Quarantine identity is proven safe against ABA alias bugs via 3-tuple keying. Quarantine persistence across module unload is proven **conditional** on retaining an explicit module reference on all quarantine paths.
