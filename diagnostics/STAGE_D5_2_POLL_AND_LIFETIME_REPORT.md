# STAGE D5.2: POLL SYNCHRONIZATION AND DEVICE LIFETIME REPORT

## Document Information
- **Project:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`) Native Linux Fingerprint Driver
- **Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`, CS0)
- **Host Controller:** Intel LPSS SPI2 (`pxa2xx-spi.2`) on Linux `7.2.8-9-omarchy` (x86_64 SMP)
- **Author:** Lead Kernel Architect & Concurrency Specialist (Antigravity Agent)
- **Status:** COMPLETED POLL AND LIFETIME AUDIT
- **Safety Boundary:** STRICTLY SOURCE-ONLY VERIFICATION (Zero Hardware Communication)

---

## 1. Executive Summary

This report documents the remediation of `fte3600_cdev_poll()` and delivers a formal proof of device lifetime safety during asynchronous driver unbinding while userspace file descriptors remain open.

---

## 2. Poll Handler Architecture and Synchronization

### 2.1 The Remediation of `fte3600_cdev_poll()`

In Stage D5, `fte3600_cdev_poll()` contained two defects:
1. It inspected `data->state` bare without holding `data->lock` or using atomic access, creating a data race with writers.
2. It conditionally advertised `EPOLLIN | EPOLLRDNORM` for `TOUCH_DETECTED` and `FRAME_READY`, states that are unreachable in the current identification-only driver.

In Stage D5.2, `fte3600_cdev_poll()` has been refactored:

```c
static __poll_t fte3600_cdev_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct fte3600_data *data = filp->private_data;

	if (atomic_read(&data->offline))
		return EPOLLERR | EPOLLHUP;

	poll_wait(filp, &data->event_wq, wait);

	if (atomic_read(&data->offline))
		return EPOLLERR | EPOLLHUP;

	/*
	 * In the current hardware identification milestone, touch detection
	 * and image frame streaming are deferred to future authorized stages.
	 * Input readiness (EPOLLIN | EPOLLRDNORM) is never asserted while those
	 * operational states remain unreachable.
	 * Pollers wait on event_wq and are cleanly awakened with EPOLLERR | EPOLLHUP
	 * upon device unbind or removal.
	 */
	return 0;
}
```

### 2.2 Synchronization Strategy
- **Driver State (`data->state`):** Exclusively read and written under `mutex_lock(&data->lock)` in process context. It is **not** read in `poll()`, eliminating all unsynchronized reads and data races.
- **Device Lifecycle (`data->offline`):** An `atomic_t` variable. It is checked locklessly in `fte3600_cdev_poll()` and `fte3600_cdev_open()`, and re-checked under `data->lock` in hardware ioctl paths.
- **Poll Order:**
  1. Fast offline check: returns immediately if device is already unbound.
  2. `poll_wait(filp, &data->event_wq, wait)`: registers the waiter on `data->event_wq` BEFORE checking condition.
  3. Second offline check: prevents race where removal occurs during queue registration.
  4. Returns `0`: thread sleeps in VFS poll until event occurs or signal arrives.

---

## 3. Formal Device Lifetime and Teardown Proof

### 3.1 Object Lifetime Model: `struct device dev`

`struct fte3600_data` embeds a full `struct device dev`:
```c
struct fte3600_data {
	struct spi_device *spi;
	struct gpio_desc *reset_gpiod;
	struct cdev cdev;
	struct device dev;
	struct mutex lock;
	wait_queue_head_t event_wq;
	enum fte3600_state state;
	atomic_t is_open;
	atomic_t offline;
	...
};
```

#### Reference Counting Lifecycle:
1. **Probe Binding:**
   `device_initialize(&data->dev)` sets initial refcount = 1.
2. **Userspace Open (`fte3600_cdev_open`):**
   `get_device(&data->dev)` increments refcount = 2.
3. **Driver Removal (`fte3600_remove`):**
   - Deletes `cdev` from kernel character device registry via `cdev_device_del(&data->cdev, &data->dev)`.
   - Acquires `mutex_lock(&data->lock)`.
   - Sets `data->offline = 1`.
   - Sets `data->reset_gpiod = NULL` and `data->spi = NULL`.
   - Releases `data->lock`.
   - Wakes all sleeping pollers via `wake_up_all(&data->event_wq)`.
   - Drops the probe reference via `put_device(&data->dev)`.
   - **Refcount drops from 2 to 1.** The memory of `struct fte3600_data` is NOT freed.
4. **Operations on Open Descriptor Post-Removal:**
   - `poll()`: detects `atomic_read(&data->offline) == 1`, returns `EPOLLERR | EPOLLHUP`.
   - `ioctl(RESET / INIT / CAPTURE)`: detects `atomic_read(&data->offline) == 1`, returns `-ENODEV`.
   - `ioctl(GET_STATE)`: acquires `data->lock`, returns snapshot with `FTE3600_FLAG_OFFLINE` and state `UNINITIALIZED` (0).
5. **Userspace Close (`fte3600_cdev_release`):**
   - `put_device(&data->dev)` drops the open reference.
   - **Refcount drops from 1 to 0.**
   - Kernel device model triggers `fte3600_device_release()`:
     ```c
     static void fte3600_device_release(struct device *dev)
     {
         struct fte3600_data *data = container_of(dev, struct fte3600_data, dev);
         mutex_destroy(&data->lock);
         kfree(data);
     }
     ```
   - Memory is freed safely.

---

## 4. Protection Against Devres Use-After-Free

When `fte3600_remove()` returns, the SPI core automatically releases all devres allocations associated with `&spi->dev` (including the GPIO descriptor acquired via `devm_gpiod_get`).

Can a concurrent or subsequent ioctl access the freed descriptor?
**Proof of Non-Access:**
1. Under `mutex_lock(&data->lock)`, `fte3600_remove()` assigns `data->reset_gpiod = NULL;` and `data->spi = NULL;` BEFORE `remove()` returns.
2. Every hardware ioctl (`FTE3600_IOC_RESET`, `FTE3600_IOC_INIT`) executes under `mutex_lock(&data->lock)`:
   ```c
   if (atomic_read(&data->offline) || !data->reset_gpiod) {
       mutex_unlock(&data->lock);
       return -ENODEV;
   }
   ```
3. If an ioctl was waiting on `data->lock` while `remove()` ran, it wakes up, inspects `data->offline` and `!data->reset_gpiod`, and aborts with `-ENODEV` without touching any devres pointers.
4. Access to freed hardware descriptors is mathematically impossible under this locking hierarchy.

---

## 5. Audit Verdict

**PASS — POLL SYNCHRONIZATION AND DEVICE LIFETIME VERIFIED**
