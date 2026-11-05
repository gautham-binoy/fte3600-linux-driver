# STAGE D5.1: DEVICE LIFETIME AND UAPI COMPATIBILITY AUDIT

## Document Information
- **Project:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`) Native Linux Fingerprint Driver
- **Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`, CS0)
- **Host Controller:** Intel LPSS SPI2 (`pxa2xx-spi.2`) on Linux `7.2.8-9-omarchy` (x86_64 SMP)
- **Author:** Adversarial Kernel Auditor & ABI Specialist (Antigravity Agent)
- **Status:** COMPLETED LIFETIME AND UAPI AUDIT
- **Safety Boundary:** STRICTLY READ-ONLY AUDIT (Zero Hardware Access)

---

## 1. Executive Summary

This report delivers an in-depth adversarial audit of the object lifetime architecture, device removal teardown protocol, and UAPI compatibility guarantees of the FTE3600 Linux driver following Stage D5.

### Key Conclusions:
1. **Object Lifetime is Sound Against Memory Corruption:** The embedded `struct device dev` model with `get_device()` in `fte3600_cdev_open()` and `put_device()` in `fte3600_cdev_release()` and `fte3600_remove()` successfully prevents use-after-free, double-free, and dangling devres memory access across asynchronous driver unbinding.
2. **UAPI State Leakage Defect Confirmed:** Setting `data->state = FTE3600_STATE_DEVICE_REMOVAL` (13) in `fte3600_remove()` leaks internal state `13` directly into userspace via `FTE3600_IOC_GET_STATE`, violating UAPI v1 commitments.
3. **Mock Test Harness Desynchronization:** The userspace test suite passed `test_scenario_5_get_state_after_removal` only because the test harness was not updated to reflect `kernel/fte3600.c:689`. In real execution, the driver returns state `13`, whereas the mock asserted state `0` (`UNINITIALIZED`).

---

## 2. Asynchronous Device-Removal Lifecycle Audit

### 2.1 The Four-Stage Lifecycle Scenario

#### Step 1: Process Opens `/dev/fte3600`
```c
static int fte3600_cdev_open(struct inode *inode, struct file *filp)
{
	struct fte3600_data *data = container_of(inode->i_cdev, struct fte3600_data, cdev);

	if (atomic_read(&data->offline))
		return -ENODEV;

	if (atomic_cmpxchg(&data->is_open, 0, 1) != 0)
		return -EBUSY;

	if (atomic_read(&data->offline)) {
		atomic_set(&data->is_open, 0);
		return -ENODEV;
	}

	get_device(&data->dev);
	filp->private_data = data;
	return 0;
}
```
- Initial state: `data->dev.kobj` refcount is 1 (probe binding reference).
- Upon successful `open()`: `get_device(&data->dev)` increments kobject refcount to 2.
- `filp->private_data` holds a valid pointer to `struct fte3600_data`.

#### Step 2: Driver is Removed (`fte3600_remove()`)
```c
static void fte3600_remove(struct spi_device *spi)
{
	struct fte3600_data *data = spi_get_drvdata(spi);

	if (!data)
		return;

	cdev_device_del(&data->cdev, &data->dev);

	mutex_lock(&data->lock);

	atomic_set(&data->offline, 1);
	data->state = FTE3600_STATE_DEVICE_REMOVAL;
	data->reset_gpiod = NULL;
	data->spi = NULL;

	mutex_unlock(&data->lock);

	wake_up_all(&data->event_wq);

	spi_set_drvdata(spi, NULL);

	put_device(&data->dev);
	dev_info(&spi->dev, "FTE3600 driver unbound\n");
}
```
- `cdev_device_del()` deletes the cdev character device from the chrdev table and removes `/dev/fte3600` from sysfs/devtmpfs. No new opens can occur.
- Under `mutex_lock(&data->lock)`:
  * `atomic_set(&data->offline, 1)` flags the driver as permanently offline.
  * `data->state = FTE3600_STATE_DEVICE_REMOVAL;` (value 13).
  * Hardware pointers `reset_gpiod` and `spi` are set to `NULL`.
- `wake_up_all(&data->event_wq)` notifies any threads waiting on `event_wq`.
- `put_device(&data->dev)` drops the probe binding reference, decrementing the refcount from 2 to 1.
- **Critical Invariant:** Because refcount remains 1, `fte3600_device_release()` is NOT called. `struct fte3600_data` remains allocated and valid in kernel memory.

#### Step 3: Process Holds Open File Descriptor
The userspace process retains `filp`, which points to valid memory. The kernel's file table maintains the reference.

#### Step 4: Process Executes File Operations After Removal

##### Sub-Scenario 4A: Process calls `poll()`
```c
static __poll_t fte3600_cdev_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct fte3600_data *data = filp->private_data;
	__poll_t mask = 0;

	if (atomic_read(&data->offline))
		return EPOLLERR | EPOLLHUP;
...
```
- `atomic_read(&data->offline)` is evaluated at line 550.
- Because `data->offline == 1`, it returns `EPOLLERR | EPOLLHUP` immediately.
- Zero locks acquired, zero hardware accessed, zero sleeps.

##### Sub-Scenario 4B: Process calls `ioctl()`
```c
static long fte3600_ioctl_common(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct fte3600_data *data = filp->private_data;
	int ret = 0;

	if (_IOC_TYPE(cmd) != FTE3600_IOC_MAGIC)
		return -ENOTTY;

	/* Fast-path check: if already offline, reject hardware commands immediately */
	if (atomic_read(&data->offline) && cmd != FTE3600_IOC_GET_STATE)
		return -ENODEV;
...
```
- If `cmd` is `RESET`, `INIT`, or `CAPTURE`:
  `atomic_read(&data->offline)` is 1 and `cmd != GET_STATE`.
  Returns `-ENODEV` immediately via the fast-path check without acquiring `data->lock` and without touching hardware pointers.
- If `cmd` is `FTE3600_IOC_GET_STATE`:
  Acquires `data->lock`, sets `snap.flags |= FTE3600_FLAG_OFFLINE`, sets `snap.state = (u32)data->state`, releases lock, and returns 0 via `copy_to_user()`.
  **UAPI Defect:** `snap.state` receives value `13` (`DEVICE_REMOVAL`).

##### Sub-Scenario 4C: Process Closes Descriptor
```c
static int fte3600_cdev_release(struct inode *inode, struct file *filp)
{
	struct fte3600_data *data = filp->private_data;

	atomic_set(&data->is_open, 0);
	put_device(&data->dev);
	return 0;
}
```
- `put_device(&data->dev)` drops the final open reference, decrementing refcount from 1 to 0.
- The kobject core calls `fte3600_device_release()`:
  ```c
  static void fte3600_device_release(struct device *dev)
  {
      struct fte3600_data *data = container_of(dev, struct fte3600_data, dev);

      mutex_destroy(&data->lock);
      kfree(data);
  }
  ```
- Memory is freed safely and cleanly.

---

### 2.2 Hazard Audit Matrix

| Hazard Threat | Audit Finding | Mechanism of Protection |
| :--- | :---: | :--- |
| **Use-after-free** | **ABSENT** | `get_device(&data->dev)` holds memory until `cdev_release()` |
| **Double release** | **ABSENT** | Strict 1-for-1 balance between `device_initialize` / `get_device` and `put_device` |
| **Access to freed device data** | **ABSENT** | `cdev_del` unlinks node; `data` preserved by open fd reference count |
| **Deadlock** | **ABSENT** | `remove()` holds `data->lock` only while updating pointers; does not sleep |
| **Indefinite wait** | **ABSENT** | `wake_up_all(&data->event_wq)` forces all sleeping pollers to awaken |
| **Access to removed SPI device** | **ABSENT** | `data->spi` and `data->reset_gpiod` set to `NULL` under `data->lock` |

---

## 3. UAPI Compatibility Audit

### 3.1 IOCTL Number and Direction Audit
- `FTE3600_IOC_GET_STATE`: `_IOR('F', 0x01, struct fte3600_state_snapshot)` = `0x80204601` (32 bytes, Read). Unchanged.
- `FTE3600_IOC_RESET`: `_IO('F', 0x02)` = `0x00004602` (0 bytes, Void). Unchanged.
- `FTE3600_IOC_INIT`: `_IOWR('F', 0x03, struct fte3600_init_params)` = `0xC0204603` (32 bytes, Read/Write). Unchanged.
- `FTE3600_IOC_CAPTURE`: `_IOWR('F', 0x04, struct fte3600_frame_buffer)` = `0xC0204604` (32 bytes, Read/Write). Unchanged.

### 3.2 Structure Layout & Alignment Audit
- All three UAPI structures (`struct fte3600_state_snapshot`, `struct fte3600_init_params`, `struct fte3600_frame_buffer`) are exactly 32 bytes on both 32-bit and 64-bit architectures.
- `buffer_ptr` in `struct fte3600_frame_buffer` is typed as `__u64` at offset 8, ensuring identical layout without pointer-size discrepancies.
- `compat_ioctl` is wired cleanly to `fte3600_ioctl_common()`.

---

## 4. The UAPI State Leakage Defect and Test Desynchronization

### 4.1 State Machine Leakage
Stage D5 introduced internal states 9 through 13 into `enum fte3600_state` in `kernel/fte3600.h`.

The Stage D5 documentation claimed:
> *"These states govern internal kernel synchronization ... They are internal to driver operation and not exposed via public v1 UAPI snapshots."*

However, in `kernel/fte3600.c:440`:
```c
snap.state = (u32)data->state;
```
And in `kernel/fte3600.c:689`:
```c
data->state = FTE3600_STATE_DEVICE_REMOVAL;
```
When `fte3600_remove()` runs, `data->state` becomes `13`. If an existing userspace client holding the device node open queries `FTE3600_IOC_GET_STATE`, it receives `snap.state == 13`.
Existing clients programmed to validate public states `0..8` will encounter an unexpected integer, potentially causing assertion failures or unhandled switch branches.

### 4.2 Test Harness Desynchronization
In `kernel/fte3600-test.c`, Scenario 5 tests `GET_STATE` after removal:
```c
/* Scenario 5: GET_STATE after removal */
static bool test_scenario_5_get_state_after_removal(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);

    sim_driver_open(&ctx);
    sim_driver_remove(&ctx);

    struct fte3600_state_snapshot snap;
    int ret = sim_driver_ioctl_get_state(&ctx, &snap);

    /* GET_STATE succeeds safely against retained software state */
    bool pass = (ret == 0);
    pass = pass && (snap.version == FTE3600_UAPI_VERSION);
    pass = pass && (snap.state == (uint32_t)FTE3600_STATE_UNINITIALIZED);
    pass = pass && ((snap.flags & FTE3600_FLAG_OFFLINE) != 0);

    sim_driver_release(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}
```
**Why did this test pass during Stage D5?**
Inspecting `sim_driver_remove()` in `kernel/fte3600-test.c:350-365`:
```c
static void sim_driver_remove(sim_context_t *ctx)
{
    pthread_mutex_lock(&ctx->lock);
    atomic_store(&ctx->offline, 1);
    ctx->reset_gpiod = NULL;
    ctx->spi = NULL;
    pthread_mutex_unlock(&ctx->lock);
    sim_ctx_put(ctx);
}
```
`sim_driver_remove()` in the test harness was **never updated** to set `ctx->state = FTE3600_STATE_DEVICE_REMOVAL`!
Thus, `ctx->state` remained `0` (`FTE3600_STATE_UNINITIALIZED`), allowing line 591 to pass in simulation.
Had the simulation accurately matched `kernel/fte3600.c:689`, the test would have failed with `snap.state == 13 != 0`.

---

## 5. Audit Verdict

**FAIL — UAPI STATE LEAKAGE AND SIMULATION DESYNCHRONIZATION**
