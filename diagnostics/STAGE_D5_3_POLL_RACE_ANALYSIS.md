# STAGE D5.3: POLL LIFECYCLE AND CONCURRENCY RACE ANALYSIS

## Document Information
- **Project:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`) Native Linux Fingerprint Driver
- **Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`, CS0)
- **Host Controller:** Intel LPSS SPI2 (`pxa2xx-spi.2`) on Linux `7.2.8-9-omarchy` (x86_64 SMP)
- **Author:** Senior Concurrency & Kernel Synchronization Specialist (Antigravity Agent)
- **Status:** COMPLETED RACE ANALYSIS
- **Safety Boundary:** STRICTLY READ-ONLY AUDIT (Zero Hardware Operations)

---

## 1. Executive Summary

This document presents a rigorous analysis of the synchronization, waitqueue lifecycle, and concurrency race conditions in `fte3600_cdev_poll()` and `fte3600_remove()`.

The investigation evaluated six critical execution races between concurrent polling threads and asynchronous device unbinding/removal. The analysis confirms that under the Linux Kernel Memory Model, no race window permits a waiter to remain blocked indefinitely, and no execution path allows access to freed memory.

---

## 2. Core Implementation Source Walkthrough

### 2.1 The Poll Handler (`kernel/fte3600.c`)
```c
static __poll_t fte3600_cdev_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct fte3600_data *data = filp->private_data;

	/* [Check 1: Fast offline check] */
	if (atomic_read(&data->offline))
		return EPOLLERR | EPOLLHUP;

	/* [Queue Registration] */
	poll_wait(filp, &data->event_wq, wait);

	/* [Check 2: Post-registration offline check] */
	if (atomic_read(&data->offline))
		return EPOLLERR | EPOLLHUP;

	return 0;
}
```

### 2.2 The Removal Handler (`kernel/fte3600.c`)
```c
static void fte3600_remove(struct spi_device *spi)
{
	struct fte3600_data *data = spi_get_drvdata(spi);

	if (!data)
		return;

	/* Step 1: Remove character device node from sysfs/devtmpfs */
	cdev_device_del(&data->cdev, &data->dev);

	/* Step 2: Acquire mutex to serialize with active operations */
	mutex_lock(&data->lock);

	/* Step 3: Flag offline and nullify devres pointers under lock */
	atomic_set(&data->offline, 1);
	data->state = FTE3600_STATE_DEVICE_REMOVAL;
	data->reset_gpiod = NULL;
	data->spi = NULL;

	mutex_unlock(&data->lock);

	/* Step 5: Wake all sleeping pollers */
	wake_up_all(&data->event_wq);

	spi_set_drvdata(spi, NULL);

	/* Step 6: Drop probe binding reference */
	put_device(&data->dev);
	dev_info(&spi->dev, "FTE3600 driver unbound\n");
}
```

---

## 3. Formal Analysis of the Six Race Scenarios

### Scenario A: Poll starts before removal and observes the device online
1. **Execution Sequence:**
   - Thread invokes `poll()`. VFS enters `fte3600_cdev_poll()`.
   - Check 1 observes `data->offline == 0`.
   - `poll_wait(filp, &data->event_wq, wait)` adds waitqueue entry to `&data->event_wq`.
   - Check 2 observes `data->offline == 0`.
   - Function returns `0`. VFS puts task to sleep (`TASK_INTERRUPTIBLE`).
   - At a later time, `fte3600_remove()` executes:
     * Sets `atomic_set(&data->offline, 1)`.
     * Invokes `wake_up_all(&data->event_wq)`.
2. **Resolution:**
   `wake_up_all()` acquires `data->event_wq.lock`, traverses the waitqueue, and sets the sleeping task to `TASK_RUNNING`.
   The task awakens in VFS `do_pollfd()` / `ep_poll()`, re-evaluates `fte3600_cdev_poll()`, hits Check 1, observes `data->offline == 1`, and returns `EPOLLERR | EPOLLHUP`.
3. **Safety Proof:** Waiter exits immediately upon removal; `data` remains valid via open file descriptor refcount.

---

### Scenario B: Removal happens immediately before `poll_wait()` registration
1. **Execution Sequence:**
   - Thread enters `fte3600_cdev_poll()`.
   - Check 1 evaluates `atomic_read(&data->offline) == 0`.
   - **Race Interleaving:** Before line 536 executes, CPU 1 runs `fte3600_remove()`, sets `data->offline = 1`, and calls `wake_up_all(&data->event_wq)`.
   - CPU 0 now executes line 536: `poll_wait(filp, &data->event_wq, wait)`.
   - CPU 0 advances to Check 2 (line 538): `if (atomic_read(&data->offline))`.
2. **Resolution:**
   Check 2 reads `data->offline == 1`. It immediately returns `EPOLLERR | EPOLLHUP`.
   The VFS observes a non-zero event mask and **never puts the task to sleep**.
3. **Safety Proof:** Check 2 completely eliminates the lost-wakeup window between Check 1 and `poll_wait()`.

---

### Scenario C: Removal happens immediately after `poll_wait()` registration
1. **Execution Sequence:**
   - Thread enters `fte3600_cdev_poll()`.
   - Passes Check 1 (`offline == 0`).
   - Executes `poll_wait()`. Wait entry is now linked onto `data->event_wq`.
   - **Race Interleaving:** CPU 1 executes `fte3600_remove()`, sets `data->offline = 1`, and calls `wake_up_all(&data->event_wq)`.
   - CPU 0 advances to Check 2.
2. **Resolution:**
   Because `poll_wait()` already linked the entry, `wake_up_all()` flags the task as runnable.
   Simultaneously, Check 2 observes `data->offline == 1` and returns `EPOLLERR | EPOLLHUP`.
3. **Safety Proof:** Double protection: the condition check aborts sleep directly, and the waitqueue wakeup ensures that even if VFS attempted sleep, the task is immediately runnable.

---

### Scenario D: Removal happens between the second offline check and return
1. **Execution Sequence:**
   - Thread enters `fte3600_cdev_poll()`, registers on `event_wq`, and passes Check 2 (`offline == 0`).
   - **Race Interleaving:** Immediately prior to the `return 0;` statement, CPU 1 executes `fte3600_remove()`:
     * Sets `data->offline = 1`.
     * Calls `wake_up_all(&data->event_wq)`.
2. **Resolution:**
   Because `poll_wait()` already registered the wait entry with the VFS poll table, `wake_up_all()` invokes the entry's callback (`pollwake` / `ep_poll_callback`).
   In standard Linux VFS epoll: the event is placed on `ep->rdllist`, and `ep->wq` is awakened.
   In standard Linux `poll()`: `task->state` is set to `TASK_RUNNING`.
   When the thread returns `0` to VFS, VFS inspects task state and immediately re-evaluates the poll handler rather than calling `schedule()`.
   Upon re-evaluation, Check 1 returns `EPOLLERR | EPOLLHUP`.
3. **Safety Proof:** Waiter cannot sleep because `wake_up_all()` signaled the VFS poll entry before the scheduler was invoked.

---

### Scenario E: An already-open file descriptor calls poll after removal
1. **Execution Sequence:**
   - Driver removal has completed (`data->offline == 1`, pointers nullified).
   - A userspace process holding an open descriptor calls `poll()`.
2. **Resolution:**
   Enters `fte3600_cdev_poll()`.
   Check 1 evaluates `atomic_read(&data->offline) == 1`.
   Returns `EPOLLERR | EPOLLHUP` immediately.
   `poll_wait()` is never called; no locks are taken; hardware is untouched.
3. **Safety Proof:** Zero allocation or hardware dependencies.

---

### Scenario F: Multiple processes / threads poll the same device during removal
1. **Execution Sequence:**
   - Multiple threads (or processes sharing an fd after `fork()`) call `poll()` concurrently while `fte3600_remove()` executes.
2. **Resolution:**
   `data->event_wq` is a Linux `wait_queue_head_t` protected by internal spinlock `event_wq.lock`.
   All concurrent calls to `poll_wait()` serialize on `event_wq.lock`.
   When `fte3600_remove()` calls `wake_up_all(&data->event_wq)`:
   * `wake_up_all()` acquires `event_wq.lock`.
   * Traverses the entire wait list and awakens **every waiter**, not merely one.
   * All sleeping waiters awaken, re-enter `fte3600_cdev_poll()`, and receive `EPOLLERR | EPOLLHUP`.
3. **Safety Proof:** All waiters unblock cleanly. Single-client lease at open time limits fd distribution, but even with multithreaded sharing, `wake_up_all()` ensures zero hung waiters.

---

## 4. Waitqueue Lifetime Guarantee

1. **Initialization Order in `fte3600_probe()`:**
   - Line 583: `init_waitqueue_head(&data->event_wq);`
   - Line 635: `cdev_device_add(&data->cdev, &data->dev);`
   - **Guarantee:** Character device registration occurs strictly **after** `event_wq` is initialized. No userspace process can open or poll `/dev/fte3600` before `event_wq` is fully initialized and operational.
2. **Teardown Lifetime:**
   - `struct fte3600_data` memory is retained as long as `data->dev.kobj` refcount > 0.
   - Every open file descriptor holds an active reference taken by `get_device(&data->dev)` in `fte3600_cdev_open()`.
   - `fte3600_device_release()` (which destroys the mutex and frees `data`) is triggered only after all descriptors are closed (`fte3600_cdev_release()`).
   - **Guarantee:** `event_wq` remains valid in memory for the entire duration that any userspace descriptor remains open.

---

## 5. Audit Verdict

**PASS — ALL POLL CONCURRENCY RACE CONDITIONS RESOLVED AND VERIFIED**
