# Stage 2E.8.5 In-Flight Character Device Open vs SPI Driver Removal Audit

**Target System:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Target SPI Device:** `spi-FTE3600:00` on Intel LPSS SPI2 (`pxa2xx-spi.2`, CS0)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (x86_64)  
**Task Type:** Static-Only Concurrency Audit: In-Flight `open()` vs `fte3600_remove()`  
**Status:** **STAGE B HARDWARE EXECUTION ON HOLD — STRICTLY STATIC AUDIT**

---

## 1. Executive Summary & Safety Restrictions

This report provides an independent, static-only source audit investigating the specific race condition where a userspace process executes `open("/dev/fte3600")` concurrently while the kernel driver executes `fte3600_remove()`, calls `cdev_device_del()`, and transitions the device to offline.

### Strict Safety Boundaries
- No physical hardware testing performed.
- No kernel module loading (`insmod`), unloading (`rmmod`), or driver unbinding (`unbind`).
- No device node access (`/dev/fte3600`) or ioctl execution.
- No GPIO toggling, SPI transactions, MMIO operations, or sensor register accesses.
- No modifications to kernel source code, userspace utilities, or test scripts.
- No git commits.

---

## 2. Core Kernel Subsystem Implementations & Reference Mechanics

### 2.1 Kernel Header Availability and Methodology Limitation
- **Installed Kernel Environment:** Linux `7.2.8-9-omarchy` (x86_64).
- **Inspected Headers:** `/lib/modules/7.2.8-9-omarchy/build/include/linux/cdev.h` and `/lib/modules/7.2.8-9-omarchy/build/include/linux/device.h`.
- **Limitation Statement:** The full C implementation files of the core kernel (`fs/char_dev.c` and `drivers/base/core.c`) are not installed in the modules build directory; analysis of internal VFS and cdev helper mechanics is derived from standard Linux kernel architecture, the installed kernel header declarations, and historical commit semantics (`cdev_device_add` architecture).

### 2.2 Character Device Registration & Kobject Parenting Architecture
In [`kernel/fte3600.c:621-628`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L621-L628), device registration uses `cdev_device_add()`:
```c
cdev_init(&data->cdev, &fte3600_fops);
data->cdev.owner = THIS_MODULE;
ret = cdev_device_add(&data->cdev, &data->dev);
```

In standard Linux kernel architecture (introduced in kernel 4.12 by Greg Kroah-Hartman):
1. **`cdev_device_add(struct cdev *cdev, struct device *dev)`:**
   - Automatically invokes `cdev_set_parent(cdev, &dev->kobj)`.
   - Links the character device's kobject parent directly to the embedded `struct device` kobject (`data->dev.kobj`).
   - Calls `cdev_add(cdev, dev->devt, 1)`, which registers the character device in `cdev_map` and increments the parent's kobject reference count: `kobject_get(p->kobj.parent)`.
2. **`chrdev_open(struct inode *inode, struct file *filp)` (`fs/char_dev.c`):**
   - Finds the cdev via `kobj_lookup(cdev_map, inode->i_rdev)`.
   - Invokes `cdev_get(p)`: increments `kobject_get(&p->kobj)` and increments module refcount via `try_module_get(p->owner)`.
   - Calls the driver's open callback: `filp->f_op->open(inode, filp)` (`fte3600_cdev_open`).
   - If `.open` fails: calls `cdev_put(p)` to decrement `p->kobj` and `p->owner`.
   - If `.open` succeeds: reference is retained until `filp` is closed by userspace (`fput()`).
3. **`cdev_device_del(struct cdev *cdev, struct device *dev)` (`fs/char_dev.c`):**
   - Calls `cdev_del(cdev)`: unmaps the cdev from `cdev_map` (preventing any subsequent lookups) and calls `kobject_put(&cdev->kobj)`.
   - Calls `device_del(dev)`: unlinks the device from sysfs and devtmpfs.

---

## 3. Exact Sequence and Ordering

```
Step 1:  VFS begins opening character device (/dev/fte3600)
         └─► VFS invokes chrdev_open(inode, filp)

Step 2:  Character-device lookup obtains cdev reference
         └─► kobj_lookup(cdev_map, inode->i_rdev) returns &data->cdev.kobj
         └─► cdev_get(&data->cdev) increments data->cdev.kobj refcount
         └─► Parent link: data->dev.kobj refcount is protected by cdev child reference

Step 3:  Driver's .open callback begins
         └─► chrdev_open invokes fte3600_cdev_open(inode, filp) (kernel/fte3600.c:368)

Step 4:  fte3600_cdev_open checks offline
         └─► Line 373: if (atomic_read(&data->offline)) return -ENODEV;

Step 5:  fte3600_cdev_open acquires single-client lease
         └─► Line 377: if (atomic_cmpxchg(&data->is_open, 0, 1) != 0) return -EBUSY;

Step 6:  fte3600_cdev_open re-checks offline and calls get_device()
         └─► Line 381: if (atomic_read(&data->offline)) { atomic_set(&data->is_open, 0); return -ENODEV; }
         └─► Line 386: get_device(&data->dev); (increments data->dev refcount)
         └─► Line 387: filp->private_data = data;

Step 7:  fte3600_remove calls cdev_device_del()
         └─► Line 649: cdev_device_del(&data->cdev, &data->dev)
         └─► cdev_del() unmaps from cdev_map; calls kobject_put(&data->cdev.kobj)
         └─► device_del() unlinks /dev/fte3600 and sysfs node

Step 8:  fte3600_remove acquires data->lock
         └─► Line 655: mutex_lock(&data->lock); (serializes against in-flight hardware ops)

Step 9:  fte3600_remove sets offline = 1
         └─► Line 661: atomic_set(&data->offline, 1);

Step 10: fte3600_remove clears reset_gpiod and spi pointers
         └─► Line 662: data->reset_gpiod = NULL;
         └─► Line 663: data->spi = NULL;
         └─► Line 671: mutex_unlock(&data->lock);

Step 11: fte3600_remove drops probe's device reference
         └─► Line 685: put_device(&data->dev); (drops probe's initial reference)

Step 12: Open callback returns
         └─► Returns 0 (success) or -ENODEV / -EBUSY (failure)

Step 13: VFS manages cdev reference
         └─► On failure: chrdev_open calls cdev_put(&data->cdev)
         └─► On success: cdev reference transferred to open struct file until fput()
```

---

## 4. Exhaustive Interleaving Analysis (Scenarios A through H)

### Interleaving A: Open completes before remove begins
- **Interleaving:** Steps 1–6, 12, 13 finish before Step 7 begins.
- **Can `data` be freed while `.open` uses it?** **No.** `data->dev` refcount is 2 (probe ref + open ref).
- **Can `.open` return success after device is offline?** **No.** Device was fully online when open ran.
- **Can a later ioctl access a released GPIO descriptor?** **No.** Remove sets `offline = 1` and `reset_gpiod = NULL` under mutex. Later ioctls abort at line 411 / line 446 with `-ENODEV`.
- **Can GPIO devres resource be released while reset helper uses it?** **No.** Mutex in `remove` blocks until active reset pulse finishes.
- **Preventing Mechanism:** Standard sequential ordering, `get_device()`, `data->lock`.

### Interleaving B: Remove completes `cdev_device_del()` before open lookup begins
- **Interleaving:** Step 7 completes before Step 1/2 starts.
- **Can `data` be freed while `.open` uses it?** **No.** `.open` is never reached.
- **Can `.open` return success after device is offline?** **No.** `kobj_lookup(cdev_map)` fails because `cdev_del()` unmapped the device number. `chrdev_open` returns `-ENXIO` or `-ENODEV`.
- **Can a later ioctl access a released GPIO descriptor?** **No.**
- **Can GPIO devres resource be released while reset helper uses it?** **No.**
- **Preventing Mechanism:** `cdev_unmap()` inside `cdev_del()` executed in Step 7.

### Interleaving C: Open lookup obtains cdev reference, but `.open` has not started when remove begins
- **Interleaving:** Steps 1–2 complete (`cdev_get()` holds reference). Remove executes Steps 7–11. Then Step 3 begins.
- **Can `data` be freed while `.open` uses it?**
  - **No.** `cdev_get(&data->cdev)` holds `data->cdev.kobj` alive. Because `data->cdev.kobj.parent == &data->dev.kobj`, `data->dev` cannot drop to refcount 0. `struct fte3600_data` memory remains pinned.
- **Can `.open` return success after device is offline?**
  - **No.** Step 4 ([line 373](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L373)) reads `atomic_read(&data->offline)`. Since Step 9 set `offline = 1`, line 373 immediately returns `-ENODEV`.
- **Can a later ioctl access a released GPIO descriptor?** **No.** Open aborted.
- **Can GPIO devres resource be released while reset helper uses it?** **No.**
- **Preventing Mechanism:** Fast-path offline check at [`kernel/fte3600.c:373`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L373) and `cdev_get()` parent pin.

### Interleaving D: `.open` checks `offline` before remove sets it, then remove begins
- **Interleaving:** Step 4 checks line 373 (`offline == 0`). Before Step 5, remove runs Steps 7–10 (`offline = 1`).
- **Can `data` be freed while `.open` uses it?** **No.** `cdev_get()` holds `cdev` and parent device memory alive.
- **Can `.open` return success after device is offline?**
  - **No.** Step 5 acquires lease via `atomic_cmpxchg(&data->is_open, 0, 1)`.
  - Immediately following, Step 6 ([line 381](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L381)) re-checks `offline`:
    ```c
    if (atomic_read(&data->offline)) {
        atomic_set(&data->is_open, 0);
        return -ENODEV;
    }
    ```
  - Step 9 has already set `offline = 1`. Line 381 evaluates to true, rolls back the lease to 0, and returns `-ENODEV`!
- **Can a later ioctl access a released GPIO descriptor?** **No.** Open aborted with `-ENODEV`.
- **Can GPIO devres resource be released while reset helper uses it?** **No.**
- **Preventing Mechanism:** Secondary `atomic_read(&data->offline)` check with atomic lease rollback ([lines 380-384](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L380-L384)).

### Interleaving E: `.open` acquires lease, but remove begins before `get_device()`
- **Interleaving:** Step 5 completes (`is_open = 1`). Remove executes Steps 7–10 before Step 6.
- **Can `data` be freed while `.open` uses it?** **No.** Pinned by `cdev_get()`.
- **Can `.open` return success after device is offline?**
  - **No.** Line 381 re-checks `offline`, detects `offline == 1`, rolls back `is_open = 0`, and returns `-ENODEV`.
- **Can a later ioctl access a released GPIO descriptor?** **No.** Open failed.
- **Can GPIO devres resource be released while reset helper uses it?** **No.**
- **Preventing Mechanism:** Secondary offline check and `atomic_set(&data->is_open, 0)` rollback.

### Interleaving F: `.open` calls `get_device()` before remove drops the probe reference
- **Interleaving:** Steps 1–6 complete (`get_device` increments kref from 1 to 2). Then remove executes Steps 7–11 (`put_device` decrements kref from 2 to 1).
- **Can `data` be freed while `.open` uses it?** **No.** `data->dev` refcount is 2, decrements to 1. Memory remains valid.
- **Can `.open` return success after device is offline?**
  - If `.open` completed line 381 before remove set `offline = 1`, open returns 0 (success).
- **Can a later ioctl access a released GPIO descriptor?**
  - **No.** When application calls `ioctl(fd, FTE3600_IOC_RESET)`:
    - Line 411 checks `atomic_read(&data->offline)`. Since remove set `offline = 1`, ioctl aborts with `-ENODEV`.
    - Line 446 checks under `data->lock`: `if (atomic_read(&data->offline) || !data->reset_gpiod) return -ENODEV;`.
    - Both checks prevent any dereference of `reset_gpiod`.
- **Can GPIO devres resource be released while reset helper uses it?** **No.** Remove blocks on `data->lock` at line 655.
- **Preventing Mechanism:** Kobject reference count ($1 \rightarrow 2 \rightarrow 1$), fast-path offline check (line 411), mutex offline check (line 446).

### Interleaving G: Remove drops probe reference while `.open` is still executing
- **Interleaving:** Thread 1 passed line 384 (where offline was 0), but has not yet called line 386 `get_device()`. Remove executes Steps 7–11 (`put_device(&data->dev)`).
- **Can `data` be freed while `.open` is still executing?**
  - **No.**
  - **Detailed Kernel Architecture Proof:**
    1. In probe, `cdev_device_add()` called `cdev_set_parent(&data->cdev, &data->dev.kobj)`, making `data->dev.kobj` the parent of `data->cdev.kobj`.
    2. In Step 2, `chrdev_open()` called `cdev_get(&data->cdev)`, incrementing `data->cdev.kobj` refcount.
    3. When `fte3600_remove()` calls `cdev_device_del()`, `cdev_del()` calls `kobject_put(&data->cdev.kobj)`. Because `chrdev_open()` holds a reference, `data->cdev.kobj` refcount remains $\ge 1$ and **does not drop to zero**.
    4. Because `data->cdev.kobj` has not dropped to zero, it has **not** released its parent reference to `data->dev.kobj`.
    5. When `fte3600_remove()` calls `put_device(&data->dev)` at Step 11 ([line 685](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L685)), it drops probe's reference, but `data->dev.kobj` refcount remains pinned $\ge 1$ by the active child `cdev.kobj`.
    6. Therefore, `fte3600_device_release` **CANNOT run**, and `data` **CANNOT be freed** while `fte3600_cdev_open()` is executing!
  - Thread 1 safely executes line 386 `get_device(&data->dev)` and returns.
- **Can a later ioctl access a released GPIO descriptor?** **No.** Blocked by lines 411 and 446.
- **Can GPIO devres resource be released while reset helper uses it?** **No.**
- **Preventing Mechanism:** `cdev_set_parent()` kobject hierarchy and `cdev_get()` in `chrdev_open()`.

### Interleaving H: Remove completes while an existing FD remains open
- **Interleaving:** Userspace holds open file descriptor; `fte3600_remove()` finishes completely and returns.
- **Can `data` be freed while FD remains open?** **No.** `filp` holds reference via `get_device(&data->dev)` ([line 386](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L386)). Refcount is $\ge 1$.
- **Can a later ioctl access a released GPIO descriptor?** **No.** Line 411 and line 446 return `-ENODEV`.
- **Can GPIO devres resource be released while reset helper uses it?** **No.** Reset pulse finished before remove returned.
- **When is `data` freed?** When userspace calls `close(fd)` $\rightarrow$ `fte3600_cdev_release` calls `put_device(&data->dev)` $\rightarrow$ refcount drops to 0 $\rightarrow$ `fte3600_device_release` executes `mutex_destroy` and `kfree(data)`.
- **Preventing Mechanism:** Kobject device reference counting and Hardware-Resource Shutdown Protocol.

---

## 5. Single-Client Lease and Rollback Verification

In [`kernel/fte3600.c:368-389`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L368-L389):

```c
static int fte3600_cdev_open(struct inode *inode, struct file *filp)
{
	struct fte3600_data *data = container_of(inode->i_cdev, struct fte3600_data, cdev);

	/* Reject immediately if device was already unbound */
	if (atomic_read(&data->offline))
		return -ENODEV;

	/* Enforce single-client exclusive lease */
	if (atomic_cmpxchg(&data->is_open, 0, 1) != 0)
		return -EBUSY;

	/* Re-check offline after acquiring lease in case remove ran concurrently */
	if (atomic_read(&data->offline)) {
		atomic_set(&data->is_open, 0);
		return -ENODEV;
	}

	get_device(&data->dev);
	filp->private_data = data;
	return 0;
}
```

### Verification Checklist:
1. **Branch 1 (Line 373: Pre-lease Offline Check):**
   - Condition: `atomic_read(&data->offline) != 0`.
   - Action: Returns `-ENODEV`.
   - Lease State: Untouched (remains 0).
   - Refcount State: Untouched (`get_device` not called).
2. **Branch 2 (Line 377: Exclusive Lease Acquisition):**
   - Condition: `atomic_cmpxchg(&data->is_open, 0, 1) != 0`.
   - Action: Returns `-EBUSY`.
   - Lease State: Untouched (remains 1 for current owner; not modified by failed cmpxchg).
   - Refcount State: Untouched (`get_device` not called).
3. **Branch 3 (Line 381: Post-lease Offline Check / Rollback):**
   - Condition: `atomic_read(&data->offline) != 0` after lease acquired.
   - Action: `atomic_set(&data->is_open, 0)` explicitly releases lease, then returns `-ENODEV`.
   - Lease State: Cleanly restored to 0. No stuck lease.
   - Refcount State: Untouched (`get_device` not called).
4. **Branch 4 (Line 386: Success):**
   - Condition: Device online and lease acquired.
   - Action: `get_device(&data->dev)` increments kref; returns 0.
   - Lease State: Owned (1).
   - Refcount State: Exactly one reference owned by this file descriptor.
5. **Release Symmetry (Lines 391–399):**
   - In `fte3600_cdev_release()`:
     - `atomic_set(&data->is_open, 0)` releases lease.
     - `put_device(&data->dev)` drops exactly the reference acquired in line 386.
   - **Result: Perfect mathematical symmetry across all branches.**

---

## 6. GPIO Lifetime Claim Re-Evaluation

### 6.1 Remove Blocking on Reset Helper
- In `fte3600_remove()` ([line 655](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L655)):
  `mutex_lock(&data->lock);`
- In `fte3600_ioctl_common()` ([line 437](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L437)):
  `mutex_lock_interruptible(&data->lock);` is held throughout `fte3600_hw_reset_pulse()`.
- Therefore, `fte3600_remove()` **cannot proceed past line 655 while a reset pulse is executing**.

### 6.2 Devres Release Timing
- In the Linux device model, devres resources (`reset_gpiod`) are freed in `devres_release_all(&spi->dev)`.
- This function is invoked by the driver core **strictly after** the driver's `.remove` callback returns.
- Because `fte3600_remove()` cannot return while waiting on `data->lock`, devres **cannot release the GPIO descriptor while the reset helper is executing**.

### 6.3 Post-Remove IOCTL Protection
- After `fte3600_remove()` executes lines 661–663 under `data->lock`:
  - `data->offline = 1`
  - `data->reset_gpiod = NULL`
- Any ioctl issued by an existing open file descriptor evaluates line 411 and line 446.
- Both checks abort with `-ENODEV` without touching `reset_gpiod`.

---

## 7. Final Audit Conclusion

### **Selected Finding: Conclusion A**

$$\text{\bf Conclusion A: The in-flight open vs SPI driver removal race is demonstrably safe across all interleavings.}$$

**Technical Proof Summary:**
1. Memory safety during in-flight `open()` is guaranteed by `cdev_set_parent()` kobject hierarchy and `cdev_get()` in `chrdev_open()`, preventing `data` from being freed prematurely.
2. The double-check of `data->offline` with atomic rollback in `fte3600_cdev_open()` guarantees that no open can succeed on an offline device.
3. `get_device()` and `put_device()` decouple `struct fte3600_data` memory from the physical SPI device lifecycle.
4. `data->lock` strictly serializes in-flight reset pulses against driver removal, preventing devres from freeing the GPIO while in use.
5. Pointer nullification under mutex ensures post-remove ioctls abort cleanly with `-ENODEV`.

*(Note: Conclusion A confirms source-level concurrency and memory safety; Stage B hardware execution remains strictly on hold).*

---

## 8. Required Final Status Declarations

- **AUTOMATIC MODULE UNLOAD:** DISABLED
- **AUTOMATIC SPI UNBIND:** DISABLED
- **IN-FLIGHT OPEN/REMOVE AUDIT:** COMPLETED
- **STAGE B HARDWARE EXECUTION:** NOT PERFORMED
- **STAGE B AUTHORIZATION:** ON HOLD
