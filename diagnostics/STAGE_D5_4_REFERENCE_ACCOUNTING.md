# STAGE D5.4: DEVICE REFERENCE ACCOUNTING AND OPERATION LIFETIME AUDIT

## Document Information
- **Project:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`) Native Linux Fingerprint Driver
- **Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`, CS0)
- **Host Controller:** Intel LPSS SPI2 (`pxa2xx-spi.2`) on Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)
- **Auditor:** Independent Senior Kernel Auditor (Antigravity Agent)
- **Status:** COMPLETED INDEPENDENT AUDIT
- **Safety Boundary:** STRICTLY READ-ONLY SOURCE AUDIT (Zero Hardware Communication, Zero Live Module Manipulation)

---

## 1. Executive Summary

This audit performs a comprehensive accounting of every reference count transition on `struct device dev` and `struct cdev cdev` across the entire driver lifecycle. It maps every error branch, unbind condition, and file operation to guarantee that no reference leaks or premature deallocations can occur.

---

## 2. Reference Accounting Lifecycle Table

The table below tracks the reference count of the embedded `struct device` (`data->dev.kobj.kref`) and the embedded `struct cdev` (`data->cdev.kobj.kref`) across standard operations:

| Lifecycle Stage | `data->dev` Refcount | `data->cdev` Refcount | Active Reference Owners | Memory State of `data` |
| :--- | :---: | :---: | :--- | :--- |
| **1. Immediately after `device_initialize()`** | **1** | 0 | Probe reference (`data->dev`) | Valid (Allocated) |
| **2. After `cdev_init()`** | 1 | **1** | Probe (`dev`), Initial Cdev (`cdev`) | Valid |
| **3. After `cdev_device_add()`** | **2** | **1** | Probe (`dev`), Cdev Parent Pin (`cdev->dev`) | Valid |
| **4. In VFS `chrdev_open()` (before driver open)** | 2 | **2** | Probe (`dev`), Cdev Pin, VFS `cdev_get()` | Valid |
| **5. After successful `fte3600_cdev_open()`** | **3** | **2** | Probe (`dev`), Cdev Pin, Open File (`filp->dev`), VFS `cdev` | Valid |
| **6. Second simultaneous open attempt** | **3** | **2** | Same (Second open fails with `-EBUSY` via `is_open` lease; VFS drops its temp `cdev` ref) | Valid |
| **7. Immediately after `fte3600_remove()` (1 FD open)** | **2** | **1** | Cdev Pin (`cdev->dev`), Open File (`filp->dev`); Probe ref dropped | Valid (Offline) |
| **8. After FD closes (`fte3600_cdev_release()`)** | **1** | **1** | Cdev Pin (`cdev->dev`); File ref dropped by `put_device()` | Valid |
| **9. In VFS `__fput()` (`cdev_put()`)** | **0** | **0** | `cdev_put` drops `cdev.kref` to 0 $\rightarrow$ triggers `cdev_default_release()` $\rightarrow$ drops parent pin on `dev` $\rightarrow$ triggers `fte3600_device_release()` | **FREED (`kfree(data)`)** |

---

## 3. Reference Ownership Inventory

Every `get_device()` and `put_device()` call in `kernel/fte3600.c` was audited:

| Source Location | Function Call | Context / Trigger | Reference Owned / Transferred |
| :--- | :--- | :--- | :--- |
| `kernel/fte3600.c:591` | `device_initialize(&data->dev)` | Driver Probe | **Creates Probe Reference (Refcount = 1)** |
| `kernel/fte3600.c:633` | `cdev_device_add(&data->cdev, &data->dev)` | Driver Probe | **Creates Cdev Parent Reference (via `cdev_add()`, Refcount = 2)** |
| `kernel/fte3600.c:598` | `put_device(&data->dev)` | Error unwind (`dev_set_name` failed) | Drops Probe Reference |
| `kernel/fte3600.c:606` | `put_device(&data->dev)` | Error unwind (ACPI GPIO add failed) | Drops Probe Reference |
| `kernel/fte3600.c:615` | `put_device(&data->dev)` | Error unwind (Reset GPIO lookup failed) | Drops Probe Reference |
| `kernel/fte3600.c:626` | `put_device(&data->dev)` | Error unwind (SPI setup failed) | Drops Probe Reference |
| `kernel/fte3600.c:636` | `put_device(&data->dev)` | Error unwind (`cdev_device_add` failed) | Drops Probe Reference (Note: `cdev_device_add` cleans up cdev) |
| `kernel/fte3600.c:387` | `get_device(&data->dev)` | `fte3600_cdev_open()` | **Creates Open File Descriptor Reference** |
| `kernel/fte3600.c:398` | `put_device(&data->dev)` | `fte3600_cdev_release()` | Drops Open File Descriptor Reference |
| `kernel/fte3600.c:695` | `put_device(&data->dev)` | `fte3600_remove()` | **Drops Probe Reference** |
| `kernel/fte3600.c:104` | `fte3600_device_release()` | Final `kobject_put` | Invoked when Refcount reaches 0: destroys mutex and calls `kfree(data)` |

---

## 4. Error Paths and Teardown Robustness

### 4.1 Probe Failure Branches
- **Failure prior to `cdev_device_add()` (lines 598, 606, 615, 626):**
  Only `device_initialize()` has executed (`dev.kref == 1`).
  Calling `put_device(&data->dev)` decrements `dev.kref` from 1 to 0.
  `fte3600_device_release()` runs immediately, executing `mutex_destroy(&data->lock)` and `kfree(data)`.
  *Zero leaks, zero dangling resources.*
- **Failure inside `cdev_device_add()` (line 636):**
  In upstream `fs/char_dev.c`:
  If `device_add()` fails after `cdev_add()` succeeded, `cdev_device_add()` internally calls `cdev_del(cdev)`, dropping the cdev parent reference.
  Probe's subsequent `put_device(&data->dev)` drops the initial probe reference, freeing `data` cleanly.

### 4.2 Removal Before Any Open
- `cdev_device_del()` calls `device_del()` and `cdev_del()`.
- `cdev_del()` calls `kobject_put(&data->cdev.kobj)`. Since no file descriptors were ever opened, `cdev.kref` drops from 1 to 0.
- `cdev_default_release()` runs immediately and calls `kobject_put(&data->dev.kobj)`, decrementing `dev.kref` from 2 to 1.
- `fte3600_remove()` calls `put_device(&data->dev)` at line 695, decrementing `dev.kref` from 1 to 0.
- `fte3600_device_release()` runs and frees `data`.
  *Clean termination without open clients.*

### 4.3 Removal With Active Open FDs
- Driver enforces single-client lease (`data->is_open`), so at most 1 FD can be open.
- When `fte3600_remove()` runs:
  * `cdev_del()` unmaps the device numbers from `cdev_map`.
  * `atomic_set(&data->offline, 1)` and pointers `data->spi = NULL`, `data->reset_gpiod = NULL` are executed under `data->lock`.
  * `wake_up_all(&data->event_wq)` wakes pollers.
  * `put_device(&data->dev)` drops probe reference (`dev.kref` drops from 3 to 2).
  * `data` remains fully valid in memory!
- When userspace calls `close(fd)`:
  * `fte3600_cdev_release()` calls `put_device(&data->dev)` (`dev.kref` drops from 2 to 1).
  * VFS `__fput()` calls `cdev_put(&data->cdev)` (`cdev.kref` drops from 1 to 0).
  * `cdev_default_release()` drops the parent reference (`dev.kref` drops from 1 to 0).
  * `fte3600_device_release()` frees `data`.
  *Zero leaks, zero use-after-free.*

---

## 5. File Operations Locking and Lifetime Matrix

| File Operation | Entry Locking | Offline Check | Devres Resource Access | Behavior After Removal | Waitqueue Lifetime |
| :--- | :---: | :---: | :---: | :--- | :---: |
| **`open`** | Atomic `is_open` cmpxchg | Twice (`pre` and `post` lease) | None | Rejects with `-ENODEV` | Valid |
| **`release`** | None (atomic `is_open` clear) | None | None | Drops file reference via `put_device()` | Valid |
| **`ioctl GET_STATE`** | `mutex_lock_interruptible` | Under lock (`snap.flags \|= OFFLINE`) | None | Succeeds, returns state 0 (`UNINITIALIZED`) + `OFFLINE` flag | Valid |
| **`ioctl RESET`** | Fast-path + `mutex_lock_interruptible` | Under lock (`if (offline \|\| !reset_gpiod)`) | `data->reset_gpiod` | Rejects with `-ENODEV` under lock; never accesses NULL pointer | Valid |
| **`ioctl INIT`** | Fast-path + `mutex_lock_interruptible` | Under lock (`if (offline \|\| !spi)`) | `data->spi` | Rejects with `-ENODEV` under lock; never accesses NULL pointer | Valid |
| **`ioctl CAPTURE`** | None | Fast-path | None | Rejects immediately with `-EOPNOTSUPP` | Valid |
| **`poll`** | Lockless atomics + waitqueue spinlock | Twice (`pre` and `post` `poll_wait`) | None | Returns `EPOLLERR \| EPOLLHUP` immediately | Valid |
| **`llseek`** | None (`noop_llseek`) | None | None | Returns 0 | Valid |

---

## 6. Audit Verdict

All device reference counts, error unwinds, and concurrent file operations are completely accounted for with zero memory leaks and zero use-after-free conditions.

**Audit Classification: PASS — REFERENCE ACCOUNTING VERIFIED**
