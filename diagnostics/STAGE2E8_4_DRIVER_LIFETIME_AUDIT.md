# Stage 2E.8.4 Driver Removal, Open-File and GPIO Lifetime Audit

**Target System:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Target SPI Device:** `spi-FTE3600:00` on Intel LPSS SPI2 (`pxa2xx-spi.2`, CS0)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (x86_64)  
**Task Type:** Static-Only Audit of Removal, Open-File Concurrency, and Resource Lifetime  
**Status:** **STAGE B HARDWARE EXECUTION ON HOLD — STRICTLY STATIC AUDIT**

---

## 1. Executive Summary & Safety Restrictions

This report provides an independent, static-only source audit of the driver lifecycle, removal ordering, character-device concurrency, and GPIO resource lifetimes in `kernel/fte3600.c`.

### Strict Safety Restrictions
- No physical hardware testing performed.
- No kernel module insertion (`insmod`), removal (`rmmod`), or driver unbinding (`unbind`).
- No device node opening (`/dev/fte3600`) or ioctl execution.
- No SPI transactions, GPIO line toggling, MMIO accesses, or sensor register communications.
- No modifications to kernel source code, userspace tools, or test scripts.
- No git commits.

---

## 2. Complete Driver Lifecycle Diagram

The complete driver lifecycle encompasses the separate and intersecting lifetimes of three distinct entities:
1. **The Physical SPI Device / Bus Binding** (`struct spi_device *spi`)
2. **The Character Device Node & Userspace File Descriptors** (`/dev/fte3600`, `struct file *filp`)
3. **The Driver Private Memory Context** (`struct fte3600_data *data`)

```
===================================================================================================
                                FTE3600 COMPLETE LIFECYCLE FLOW
===================================================================================================

[SPI Core: ACPI Match "FTE3600"]
      │
      ▼
1. fte3600_probe(&spi->dev) (kernel/fte3600.c:557)
      ├─► 2. Allocate data: kzalloc(sizeof(*data)) (line 565)
      ├─► 3. Initialize locks & state: mutex_init(&data->lock), offline=0, is_open=0 (lines 572-575)
      ├─► 4. Initialize embedded device: device_initialize(&data->dev) [kref = 1] (line 582)
      ├─► 5. Register ACPI GPIOs & Acquire reset descriptor:
      │       devm_acpi_dev_add_driver_gpios() (line 594)
      │       data->reset_gpiod = devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_LOW) (line 602)
      ├─► 6. Configure SPI bus: spi_setup(spi) (line 614)
      ├─► 7. Character Device Registration:
      │       cdev_init(&data->cdev, &fte3600_fops), cdev.owner = THIS_MODULE (lines 622-623)
      │       cdev_device_add(&data->cdev, &data->dev) (line 624) -> /dev/fte3600 published
      └─► 8. Probe completes with state UNINITIALIZED (line 633) [data->dev kref = 1]
                                       │
            ┌──────────────────────────┴──────────────────────────┐
            ▼                                                     ▼
[Userspace Application: /dev/fte3600]                   [Kernel / Admin: Driver Unbind]
            │                                                     │
9. open("/dev/fte3600", O_RDWR) (fte3600-tool.c:41)               │
   └─► fte3600_cdev_open() (kernel/fte3600.c:368)                 │
       ├─► Check offline == 0 (line 373)                         │
       ├─► atomic_cmpxchg(&is_open, 0, 1) -> lease (line 377)    │
       ├─► Re-check offline == 0 (line 381)                      │
       ├─► get_device(&data->dev) [kref: 1 -> 2] (line 386)      │
       └─► filp->private_data = data; return 0 (lines 387-388)   │
            │                                                     │
10. ioctl(fd, FTE3600_IOC_RESET) (fte3600-tool.c:57)              │
    └─► fte3600_ioctl_common() (kernel/fte3600.c:402)             │
        ├─► Fast-path check !offline (line 411)                   │
        ├─► mutex_lock_interruptible(&data->lock) (line 437)      │
        ├─► Pre-pulse check !offline && reset_gpiod (line 446)    │
        ├─► fte3600_hw_reset_pulse(data) (lines 131-158)          │
        │   ├─► gpiod_set_value_cansleep(reset_gpiod, 0)          │
        │   ├─► msleep(10) [TASK_UNINTERRUPTIBLE]                 │
        │   ├─► gpiod_set_value_cansleep(reset_gpiod, 1) [LOW]    │
        │   ├─► msleep(20) [TASK_UNINTERRUPTIBLE]                 │
        │   ├─► gpiod_set_value_cansleep(reset_gpiod, 0) [HIGH]   │
        │   └─► msleep(10) [TASK_UNINTERRUPTIBLE]                 │
        ├─► mutex_unlock(&data->lock) (line 455)                  │
        └─► return 0                                              │
            │                                                     │
            │                                      11. fte3600_remove(spi) (kernel/fte3600.c:637)
            │                                          ├─► Step 1: cdev_device_del(&data->cdev) (line 649)
            │                                          │   (Unlinks /dev/fte3600; blocks new opens)
            │                                          ├─► Step 2: mutex_lock(&data->lock) (line 655)
            │                                          │   (BLOCKS if ioctl in Step 10 is active!)
            │                                          ├─► Step 3: Shutdown invalidation: (lines 661-663)
            │                                          │   atomic_set(&data->offline, 1)
            │                                          │   data->reset_gpiod = NULL
            │                                          │   data->spi = NULL
            │                                          ├─► Step 4: mutex_unlock(&data->lock) (line 671)
            │                                          ├─► Step 5: wake_up_all(&data->event_wq) (line 676)
            │                                          ├─► Step 6: put_device(&data->dev) (line 685)
            │                                          │   [kref: 2 -> 1, context preserved!]
            │                                          └─► Step 7: return from fte3600_remove() (line 687)
            │                                                     │
            │                                      12. Devres Cleanup: devres_release_all(&spi->dev)
            │                                          └─► devm releases reset_gpiod memory safely!
            │
13. close(fd) (fte3600-tool.c:101)
    └─► fte3600_cdev_release() (kernel/fte3600.c:391)
        ├─► atomic_set(&data->is_open, 0) (line 396)
        └─► put_device(&data->dev) [kref: 1 -> 0!] (line 397)
                 │
                 ▼
14. fte3600_device_release(&data->dev) (kernel/fte3600.c:104)
    ├─► mutex_destroy(&data->lock) (line 108)
    └─► kfree(data) (line 109) [Context freed with zero leaks]
===================================================================================================
```

---

## 3. Remove Ordering and Concurrency Analysis

### 3.1 Exact Remove Callback Ordering
In [`kernel/fte3600.c:637-687`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L637-L687), `fte3600_remove()` follows a strict 6-step ordering:

1. **Step 1: Character-Device Deregistration (`cdev_device_del`)** ([Line 649](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L649)):
   - Deletes `data->cdev` from the kernel chrdev kobj_map.
   - Deletes `data->dev` from sysfs (`/sys/class/fte3600/fte3600`).
   - Any new userspace `open("/dev/fte3600")` initiated after this point fails immediately in the VFS layer (`-ENODEV` or `-ENXIO`).
2. **Step 2: Mutual Exclusion Acquisition (`mutex_lock(&data->lock)`)** ([Line 655](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L655)):
   - Serializes driver removal against any hardware operation currently executing or waiting.
   - If an ioctl is running inside `fte3600_hw_reset_pulse()`, `fte3600_remove()` **blocks and waits**.
3. **Step 3: Hardware Invalidation Under Mutex** ([Lines 661–663](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L661-L663)):
   - `atomic_set(&data->offline, 1)`: Marks driver permanently offline.
   - `data->reset_gpiod = NULL`: Clears GPIO pointer.
   - `data->spi = NULL`: Clears SPI device pointer.
4. **Step 4: Mutual Exclusion Release (`mutex_unlock(&data->lock)`)** ([Line 671](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L671)):
   - Releases lock. Any waiting operation wakes up, checks `data->offline` and `!data->reset_gpiod` under the lock, and aborts with `-ENODEV`.
5. **Step 5: Event Queue Flush & Driver Data Unlink** ([Lines 676–678](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L676-L678)):
   - `wake_up_all(&data->event_wq)`: Wakes up pollers.
   - `spi_set_drvdata(spi, NULL)`.
6. **Step 6: Drop Probe Reference (`put_device(&data->dev)`)** ([Line 685](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L685)):
   - Drops the initial kobject reference acquired during probe (`device_initialize`).
   - If userspace still has `/dev/fte3600` open, the refcount remains $\ge 1$ and `data` is preserved.
7. **Return from Remove & Devres Cleanup** ([Line 687](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L687)):
   - `fte3600_remove()` returns to the Linux driver core.
   - The driver core calls `devres_release_all(&spi->dev)`, freeing the devres-managed GPIO descriptor.

### 3.2 Race Evaluation: New Opens vs Removal
- **Post-Step 1:** Any `open()` initiated after `cdev_device_del()` fails at VFS lookup.
- **Concurrent with Step 1:** If `open()` entered `fte3600_cdev_open()` ([lines 368–389](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L368-L389)) just before `cdev_device_del()`:
  - If `remove()` executed Step 3, `open()` observes `atomic_read(&data->offline) == 1` at line 373 or line 381. It rolls back the lease (`atomic_set(&data->is_open, 0)`) and returns `-ENODEV`.
  - If `open()` completed line 386 before Step 3, it holds a valid file descriptor, but any subsequent ioctl fails immediately via the offline check.

### 3.3 Race Evaluation: Existing Open Files vs Removal
- Removing the device node unlinks the filesystem entry, but **does NOT close already-open file descriptors**.
- In POSIX, open file descriptors remain valid until explicitly closed.
- The driver safely accommodates this: open descriptors retain a kobject reference via `get_device(&data->dev)` ([line 386](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L386)), preventing `data` from being freed while files remain open.

---

## 4. Six-Scenario Active-IOCTL Race Matrix

| Scenario | State of Remove vs IOCTL | Can IOCTL access `reset_gpiod`? | Can GPIO be freed while in use? | Mutex Serialization Role | Does `data` remain alive? | Observable Userspace Behavior |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **A. Remove before Open** | Driver unbinding/unbound before application calls `open()` | **No** | **No** | Not reached | Preserved if refcount > 0 | `open()` fails with `-ENODEV` (or `-ENXIO`) |
| **B. Remove during Open** | `fte3600_remove()` runs while thread is inside `fte3600_cdev_open()` | **No** | **No** | `open()` uses atomic checks; ioctls serialize on mutex | Yes (`data->dev` refcount $\ge 1$) | `open()` returns `-ENODEV` (or succeeds; subsequent ioctl returns `-ENODEV`) |
| **C. Remove between Open and IOCTL** | File is open; `fte3600_remove()` runs to completion before `ioctl()` is issued | **No** | **No** | Mutex held during remove Step 2-4; ioctl fast-path check aborts at line 411 | Yes (`filp` holds refcount = 1) | `ioctl()` returns `-1` with `errno == ENODEV` |
| **D. Remove while IOCTL waits for Mutex** | IOCTL blocked at line 437; `fte3600_remove()` acquires `data->lock` at line 655 | **No** | **No** | `data->lock` serializes remove before ioctl; ioctl wakes, sees `offline=1` & `NULL` gpiod at line 446 | Yes (`filp` holds refcount = 1) | `ioctl()` returns `-1` with `errno == ENODEV` |
| **E. Remove while Reset Helper executes** | IOCTL inside `fte3600_hw_reset_pulse()` (lines 131-158) holding `data->lock` when remove starts | **Yes, safely** (pulse completes before remove acquires lock) | **NO.** `remove()` blocks on `data->lock`; devres cannot run until `remove()` returns | `remove()` blocked at line 655 until ioctl unlocks at line 455 | Yes (`filp` holds refcount = 1) | IOCTL completes successfully with $rc = 0$. Subsequent ioctls fail with `-ENODEV` |
| **F. Remove after IOCTL, before Close** | Reset completed ($rc=0$); `remove()` runs while userspace prepares to call `close()` | **No** (no new ioctls can access) | **No** (pulse already finished) | Clean sequencing; no mutex contention | Yes (`filp` holds refcount = 1 until `close`) | Reset was successful ($rc=0$); `close(fd)` completes cleanly ($rc=0$); `kfree(data)` runs |

---

## 5. Devres-Managed GPIO Resource Lifetime Analysis

### 5.1 Resource Ownership and Allocation
- **Parent Device:** The GPIO descriptor is owned by `&spi->dev` (the Intel LPSS SPI2 controller slave device `spi-FTE3600:00`), NOT the character device `data->dev`.
- **Allocation API:** Acquired via `devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_LOW)` ([line 602](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L602)).
- **Release Timing:** The Linux driver core invokes `devres_release_all(&spi->dev)` only **AFTER** `fte3600_remove()` has completely finished and returned.

### 5.2 Prevention of Use-After-Free
1. **Remove Blocks on Active Operations:**
   In `fte3600_remove()` ([line 655](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L655)), `mutex_lock(&data->lock)` is acquired. If `fte3600_hw_reset_pulse()` is executing, `fte3600_remove()` **cannot proceed or return**. Therefore, devres release cannot occur while the GPIO is in use.
2. **Pointer Invalidation Under Lock:**
   Before releasing `data->lock`, `fte3600_remove()` explicitly sets `data->reset_gpiod = NULL` ([line 662](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L662)).
3. **Pre-Hardware Check Under Lock:**
   Any thread that acquires `data->lock` in `case FTE3600_IOC_RESET:` evaluates:
   ```c
   if (atomic_read(&data->offline) || !data->reset_gpiod) {
       mutex_unlock(&data->lock);
       return -ENODEV;
   }
   ```
   Because pointer checking and pointer clearing both occur inside the critical section of `data->lock`, memory barriers are enforced. No thread can dereference `data->reset_gpiod` after remove unlocks.

### 5.3 Distinction: Driver Context vs Hardware Resource
- **`struct fte3600_data` (Driver Data Object):** Refcounted by embedded `struct device dev`. Remains allocated as long as any userspace file descriptor remains open.
- **`struct gpio_desc *reset_gpiod` (GPIO Resource):** Managed by devres on `&spi->dev`. Released as soon as the driver detaches from SPI.
- **Bridge:** The driver cleanly bridges these lifetimes: `data` remains valid after unbind, but hardware access through `data->reset_gpiod` is permanently severed under mutex before devres frees the descriptor.

---

## 6. Character-Device and Module Lifetime Analysis

### 6.1 Module Unload Protection (`THIS_MODULE`)
- **File Operations:** `fte3600_fops.owner = THIS_MODULE` ([line 545](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L545)).
- **Character Device:** `data->cdev.owner = THIS_MODULE` ([line 623](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L623)).
- **Mechanism:** When `/dev/fte3600` is opened, the Linux VFS character device layer automatically invokes `try_module_get(fops->owner)`, incrementing the module's reference count.
- **Protection Scope:**
  - Guarantees that `rmmod fte3600` or `delete_module()` will fail with `-EBUSY` while `/dev/fte3600` is open.
  - Prevents module code and static data (`.text`, `.data`) from being unloaded while code could be executing.

### 6.2 SPI Driver Unbinding vs Module Unloading
- **Crucial Distinction:** Module reference protection does **NOT** prevent SPI driver unbinding!
- If an administrator or script writes to `/sys/bus/spi/drivers/fte3600/unbind`, the kernel immediately invokes `fte3600_remove()`, regardless of how many userspace processes hold `/dev/fte3600` open.
- Because `fte3600_remove()` decouples the hardware resources (`reset_gpiod = NULL`) while preserving `struct fte3600_data` via `get_device`/`put_device`, the driver survives asynchronous unbinding without crashing or corrupting memory.

---

## 7. Concrete Race and Edge-Case Evaluation

An exhaustive line-by-line inspection was conducted to identify any potential race condition:

### Interleaving Check: Can `data` be freed before `fte3600_cdev_release` completes?
- `device_initialize(&data->dev)` ([line 582](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L582)) sets `refcount = 1`.
- `fte3600_cdev_open` ([line 386](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L386)) increments `refcount` to 2.
- `fte3600_remove` ([line 685](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L685)) drops probe reference: `refcount = 1`.
- `fte3600_cdev_release` ([line 397](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L397)) drops open reference: `refcount = 0`.
- Only when `refcount` reaches 0 does `fte3600_device_release` ([line 104](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L104)) destroy the mutex and `kfree(data)`.
- **Verdict: PROVEN SAFE.** No use-after-free or dangling reference is possible.

### Interleaving Check: Can `open()` race with `put_device()` in `remove()`?
- `fte3600_remove()` calls `cdev_device_del()` at line 649 before touching the lock or calling `put_device()` at line 685.
- Once `cdev_device_del()` completes, VFS character device lookup fails for any new open.
- Any open that entered `fte3600_cdev_open()` prior to `cdev_device_del()` holds a reference to `data->cdev`, ensuring `data->dev` cannot reach refcount 0 until both probe and all opens drop their references.
- **Verdict: PROVEN SAFE.**

### Interleaving Check: Can `mutex_destroy(&data->lock)` race with an active ioctl?
- `mutex_destroy()` is only called in `fte3600_device_release()`, which requires refcount == 0.
- While an ioctl is running, userspace holds an open file descriptor (`refcount >= 1`).
- Therefore, `mutex_destroy()` cannot execute while any ioctl is running or waiting.
- **Verdict: PROVEN SAFE.**

---

## 8. Final Audit Conclusion

### **Selected Finding: Conclusion A**

$$\text{\bf Conclusion A: The driver removal, open-file concurrency, and resource-lifetime handling are internally consistent and mathematically sound.}$$

**Summary of Verified Properties:**
1. Zero use-after-free races exist between driver removal, devres GPIO release, and userspace file descriptors.
2. `cdev_device_del()` cleanly terminates new userspace open attempts.
3. `data->lock` strictly serializes in-flight reset ioctls against driver removal.
4. Devres cannot free `reset_gpiod` while `fte3600_hw_reset_pulse()` is executing.
5. `struct fte3600_data` memory remains valid until all userspace file descriptors are closed.
6. The character device single-client exclusive lease (`atomic_cmpxchg`) prevents multiple concurrent opens.

*(Note: Conclusion A confirms source-level consistency and concurrency safety; it does not authorize Stage B hardware testing).*

---

## 9. Required Final Status Declarations

- **AUTOMATIC MODULE UNLOAD:** DISABLED
- **AUTOMATIC SPI UNBIND:** DISABLED
- **DRIVER LIFETIME AUDIT:** COMPLETED
- **STAGE B HARDWARE EXECUTION:** NOT PERFORMED
- **STAGE B AUTHORIZATION:** ON HOLD
