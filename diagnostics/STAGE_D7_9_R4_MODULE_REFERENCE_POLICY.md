# STAGE D7.9-R4: Deliverable 3 — Module Reference Policy and Unload Control Specification

**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Peripheral Device:** FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Host Controller:** Intel LPSS SPI (`pxa2xx-spi.2` / `idma64`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Scope:** Kernel Module Refcount Accounting, Lifetime Pinning, and Unload Invariants  

---

## 1. Executive Summary

In Linux, a loadable kernel module's text and static `.data` / `.bss` variables exist only as long as the module remains loaded in the kernel. If a module is unloaded via `rmmod`, all static structures (including the quarantine table) are unmapped and wiped.

This specification details the formal **Module Reference Policy**:
1. **Transaction Lifetime Pinning:** Each asynchronous SPI transaction acquires a module reference via `try_module_get(THIS_MODULE)` *prior* to `spi_async()`. If acquisition fails, the transaction is rejected immediately.
2. **Domain Quarantine Pinning:** Whenever a hardware bus domain transitions to `DOMAIN_QUARANTINED` (whether caused by an asynchronous timeout, watchdog failure, or consecutive framing faults), a permanent module reference is acquired via `__module_get(THIS_MODULE)`.
3. **Idempotent Acquisition:** A boolean flag `fte3600_domains[i].module_pinned` prevents duplicate module reference acquisitions across repeated quarantine triggers on the same domain.
4. **Operational Unload Behavior:** With one or more domains quarantined, standard `rmmod` will **permanently return `-EBUSY`**. The module code and quarantine table cannot be unloaded without power-cycling the host machine.

---

## 2. Detailed Policy Questions & Architectural Proofs

### Question 1: When is the first module reference acquired?
- For an asynchronous SPI transaction, the first reference is acquired in `fte3600_async_submit()` **before** calling `spi_async()`.
- The acquisition uses `try_module_get(THIS_MODULE)`:
  ```c
  if (!try_module_get(THIS_MODULE))
      return -ENODEV; /* Module is going down; reject submission */
  ```

### Question 2: Does acquisition happen before or after quarantine publication?
- **For a Stranded Transaction:** The module reference was already acquired at submission (Step 1). When the 250 ms timeout expires, the submitting thread retains this reference permanently by not executing `module_put()`.
- **For Non-Stranded Quarantine Triggers (e.g. framing fault, abort):** The module reference is acquired **atomically inside `fte3600_domain_quarantine()`** under `fte3600_domain_lock` using `__module_get(THIS_MODULE)`.

### Question 3: What happens if module pin acquisition fails?
- At submission time: If `try_module_get(THIS_MODULE)` returns `false`, it proves the kernel is currently executing `sys_delete_module()` (`mod->state == MODULE_STATE_GOING`). The driver immediately aborts admission with `-ENODEV`, performs failed admission rollback (`DOMAIN_IDLE`), and drops private references.
- At quarantine publication time: `__module_get(THIS_MODULE)` is an unconditional atomic increment (`atomic_inc(&mod->refcnt)`). It cannot fail because the code executing the function is currently running inside the module text.

### Question 4: Can multiple quarantine events acquire duplicate references?
- **NO.** The domain entry contains `bool module_pinned`:
  ```c
  if (!fte3600_domains[i].module_pinned) {
      __module_get(THIS_MODULE);
      fte3600_domains[i].module_pinned = true;
  }
  ```
  Protected by `fte3600_domain_lock`, this check-and-set is atomic. Even if multiple errors or timeouts are reported on the same controller, exactly ONE permanent reference is taken per physical controller domain.

### Question 5: Who owns each permanent reference?
- **Reference Type 1 (Stranded Transaction):** Owned by the allocated `struct fte3600_async_xfer`. It is anchored to the transaction's `Ref_callback` and is never released because the callback was suppressed or lost.
- **Reference Type 2 (Quarantine Domain Pin):** Owned by the static entry in `fte3600_domains[i]`. It is anchored to the permanent quarantine record.

### Question 6: Can a permanent reference ever be released?
- **NO.** In this fail-closed safety architecture, a quarantined domain represents compromised or unverified hardware state.
- The reference is **never released during the OS session**.
- It is cleared only when the machine is power-cycled / cold-rebooted, which also power-cycles the SPI controller and fingerprint sensor hardware.

### Question 7: How does module unload behave with one or more quarantined domains?
- In `kernel/module/main.c:804-860`:
  ```c
  SYSCALL_DEFINE2(delete_module, const char __user *, name_user, unsigned int, flags)
  {
      ...
      ret = try_stop_module(mod, flags, &forced);
      if (ret != 0)
          goto out; /* Returns -EBUSY / -EWOULDBLOCK */
      ...
  }
  ```
  Because `mod->refcnt > MODULE_REF_BASE`, `try_stop_module()` detects that the module is in use.
- Any attempt to run `rmmod fte3600` or `modprobe -r fte3600` will fail with:
  `rmmod: ERROR: Module fte3600 is in use`
- The driver text and quarantine records remain permanently resident in kernel memory.

### Question 8: Can any execution path publish quarantine without a corresponding module pin?
- **NO.** By code design, `fte3600_domain_quarantine()` is the **single choke point** for transitioning any domain to `DOMAIN_QUARANTINED`.
- Every path that sets `state = FTE3600_DOMAIN_QUARANTINED` executes `__module_get(THIS_MODULE)` under `fte3600_domain_lock`.

---

## 3. Operational Consequences Summary

| Operational Action | Result with Active Quarantine | Safety Impact |
| :--- | :--- | :--- |
| **Normal sysfs unbind (`echo ... > unbind`)** | **Succeeds.** Driver core detaches device; module remains loaded. | Safe. Quarantine table persists. |
| **Normal sysfs rebind (`echo ... > bind`)** | **Fails (`-EBUSY`).** Probe checks quarantine table and rejects. | Safe. Prevents hardware reuse. |
| **Standard `rmmod fte3600`** | **Fails (`-EBUSY`).** Kernel blocks unload due to pinned refcount. | Safe. Prevents table wipe. |
| **Forced `rmmod -f fte3600`** | **Fails or Taints Kernel.** Allowed only if `CONFIG_MODULE_FORCE_UNLOAD=y`. | Documented administrative violation. |
| **System Cold Reboot** | **Succeeds.** Clears RAM, power-cycles hardware to reset state. | Clean recovery. |

**Conclusion:** The module reference policy guarantees that no execution path can quarantine a domain without permanently pinning the driver text against standard module unload.
