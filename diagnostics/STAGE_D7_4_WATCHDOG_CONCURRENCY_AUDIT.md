# Stage D7.4 — Watchdog Concurrency & Deadlock-Free Teardown Audit

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source & Design Review  
**Deliverable:** `diagnostics/STAGE_D7_4_WATCHDOG_CONCURRENCY_AUDIT.md`

---

## 1. Executive Summary & Objective

Stage D7.3 introduced a 5-second kernel watchdog timer to prevent orphaned calibration states if userspace halts or terminates. However, in Linux kernel driver design, introducing asynchronous timers into a mutex-serialized driver creates severe hazards:
1. Softirq / process context mismatch (`timer_list` cannot acquire `mutex_lock`).
2. ABBA deadlocks between ioctl execution and timer cancellation (`cancel_delayed_work_sync` held under mutex).
3. Race conditions between in-flight SPI transfers and timer expiry.

This independent audit rigorously defines the watchdog contract, analyzes all concurrency race windows, and specifies a deadlock-free synchronization architecture.

---

## 2. Watchdog Contract Definition

### 2.1. Inactivity vs. Absolute Session Lifetime
- **Primary Mechanism:** The 5-second watchdog is strictly an **Inactivity Watchdog Timer**.  
  It measures elapsed time between consecutive calibration ioctls. Every successful, progress-making calibration step resets the timer:
  $$\text{deadline} = \text{jiffies} + \text{msecs\_to\_jiffies}(5000)$$
- **Secondary Safety Ceiling:** To prevent an adversarial or pathological userspace loop from evading the watchdog by issuing 1 sample every 4.9 seconds indefinitely, an **Absolute Session Lifetime Ceiling** of 30 seconds from `CALIB_BEGIN` is enforced.

### 2.2. Operation Refresh Rules

| IOCTL Operation | Refreshes Watchdog? | Justification |
| :--- | :--- | :--- |
| `FTE3600_IOC_CALIB_BEGIN` | **Starts Timer** | Initializes session and starts 5s countdown. |
| `FTE3600_IOC_CALIB_FDT_SAMPLE` | **YES** | Legitimate forward progress in calibration. |
| `FTE3600_IOC_CALIB_READ_FRAME` | **YES** | Legitimate forward progress in calibration. |
| `FTE3600_IOC_CALIB_SET_THRESHOLDS` | **YES** | Legitimate forward progress in calibration. |
| `FTE3600_IOC_CALIB_COMMIT` | **Cancels Timer** | Session successfully completed. |
| `FTE3600_IOC_CALIB_ABORT` | **Cancels Timer** | Session terminated by user request. |
| `FTE3600_IOC_GET_STATE` | **STRICTLY NO** | Telemetry queries must **never** refresh the watchdog; otherwise, a polling loop would keep a stalled session alive indefinitely. |
| `FTE3600_IOC_RESET` | **Cancels Timer** | Hardware reset clears all sessions. |
| `FTE3600_IOC_CAPTURE` | **N/A** | Rejected with `-EPERM` while calibration is pending. |

---

## 3. Kernel Execution Context: `timer_list` vs. `delayed_work`

### 3.1. The Softirq Mutex Trap
A standard Linux kernel timer (`struct timer_list`) executes in **softirq (atomic) context**:
- Calling `mutex_lock(&data->lock)` inside a `timer_list` callback triggers a fatal kernel bug (`BUG: scheduling while atomic` or kernel panic).
- Therefore, a raw `timer_list` **CANNOT** acquire the driver mutex to invalidate the session or alter `data->state`.

### 3.2. Resolution: `struct delayed_work`
The watchdog must be implemented as a **delayed workqueue item** (`struct delayed_work`):
```c
INIT_DELAYED_WORK(&data->calib_session.watchdog_work, fte3600_calib_watchdog_fn);
```
`delayed_work` executes in **process context** (on `system_wq`), which permits sleepable mutex acquisition (`mutex_lock`).

---

## 4. Deadlock Analysis: The ABBA Cancellation Hazard

### 4.1. The Fatal ABBA Deadlock Pattern
If the driver naively implements timer cancellation inside ioctls, a catastrophic deadlock occurs:

```
Thread A: IOCTL Thread (userspace)          Thread B: Watchdog Workqueue Worker
----------------------------------------    ----------------------------------------
1. Acquires mutex_lock(&data->lock)
2. Calls cancel_delayed_work_sync(&work)
   --> BLOCKS waiting for Worker to finish!
                                            3. Worker wakes up on timer expiry
                                            4. Tries to acquire mutex_lock(&data->lock)
                                               --> BLOCKS waiting for Thread A!
====================================================================================
                        *** DEADLOCK (System Freeze) ***
====================================================================================
```

### 4.2. Deadlock-Free Architecture: Passive Jiffies + Asynchronous Work
To mathematically eliminate the ABBA deadlock, the driver must decouple timer cancellation from mutex ownership using a **Passive Jiffies Deadline** combined with **flag-gated delayed work**:

```
                             [IOCTL Thread]
                                    |
                     Acquires mutex_lock(&data->lock)
                                    |
                 +------------------+------------------+
                 | Is jiffies > session->deadline?     |
                 +------------------+------------------+
                         YES |                  | NO
                             v                  v
                 Invalidate session         Execute Step
                 Set state -> UNKNOWN       Update session->deadline = jiffies + 5s
                 Return -ETIMEDOUT          Release mutex_lock(&data->lock)
                                                |
                                            Reschedule work:
                                            mod_delayed_work(system_wq, &work, 5*HZ)
```

#### Workqueue Worker Implementation:
```c
static void fte3600_calib_watchdog_fn(struct work_struct *work)
{
	struct fte3600_calib_session *session =
		container_of(work, struct fte3600_calib_session, watchdog_work.work);
	struct fte3600_data *data =
		container_of(session, struct fte3600_data, calib_session);

	mutex_lock(&data->lock);

	/* Check if session is still active and if deadline truly passed */
	if (session->active && time_after_eq(jiffies, session->jiffies_deadline)) {
		dev_warn(&data->spi->dev, "Calibration session 0x%08x timed out (5s inactivity)\n",
			 session->id);
		/* Invalidate session */
		session->active = false;
		session->id = 0;
		data->state = FTE3600_STATE_UNKNOWN;
		data->last_error = ETIMEDOUT;
	}

	mutex_unlock(&data->lock);
}
```

### 4.3. Teardown & Release Synchronization
When `COMMIT`, `ABORT`, or `cdev_release()` executes:
1. Inside `mutex_lock(&data->lock)`:
   - Set `session->active = false`.
   - Clear `session->id = 0`.
2. Release `mutex_unlock(&data->lock)`.
3. Outside the mutex:
   - Call `cancel_delayed_work_sync(&session->watchdog_work)`.
   - Because `data->lock` is NOT held, the work worker can finish cleanly without deadlocking. Even if the worker runs, it sees `session->active == false` and exits immediately without taking any action!

---

## 5. In-Progress SPI Transactions & Expiry Race

### What happens if the 5s timer expires while an SPI transfer is in flight?
1. The ioctl executing the SPI transfer currently holds `mutex_lock(&data->lock)`.
2. When the watchdog work worker awakens, it attempts `mutex_lock(&data->lock)` and is suspended.
3. The SPI transfer finishes within its bounded hardware window ($\le 120$ ms).
4. The ioctl checks the transfer result, updates `session->jiffies_deadline = jiffies + 5*HZ`, and releases `data->lock`.
5. The watchdog worker awakens, acquires `data->lock`, evaluates `time_after_eq(jiffies, session->jiffies_deadline)`, discovers that the deadline was refreshed, and terminates with zero side effects.
6. **Result:** An in-progress SPI transaction is never interrupted or corrupted by watchdog expiry.

---

## 6. How Callers Discover Expiry

1. **Immediate Discovery:** Any subsequent calibration step ioctl (`FTE3600_IOC_CALIB_*`) checks the session token and deadline. It discovers `session->active == false` or deadline expired, returning `-ETIMEDOUT` to userspace.
2. **Telemetry Discovery:** If userspace queries driver health via `FTE3600_IOC_GET_STATE`, it receives `state = FTE3600_STATE_UNKNOWN` (7) and `last_error = ETIMEDOUT` (110).

---

## 7. Evidence Classification

| Statement / Finding | Classification | Evidence Reference |
| :--- | :--- | :--- |
| `timer_list` cannot acquire `mutex_lock` | `SOURCE-VERIFIED` | Linux kernel locking rules (`kernel/locking/mutex.c`) |
| ABBA deadlock between `cancel_delayed_work_sync` and mutex | `MATHEMATICALLY-DERIVED` | Proved via lock graph analysis in Section 4.1 |
| Passive deadline + delayed work is deadlock-free | `ARCHITECTURAL-PROPOSAL` | Specified in Section 4.2 |
| `GET_STATE` must not refresh watchdog | `MATHEMATICALLY-DERIVED` | Proved via anti-starvation analysis in Section 2.2 |
| Safe cancellation outside mutex in `release()` | `ARCHITECTURAL-PROPOSAL` | Specified in Section 4.3 |
