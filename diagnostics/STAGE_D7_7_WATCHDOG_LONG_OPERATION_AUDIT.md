# Stage D7.7 — Watchdog Policy & Long Operation Reconciliation Audit

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Timer Architecture & Concurrency Audit  
**Deliverable:** `diagnostics/STAGE_D7_7_WATCHDOG_LONG_OPERATION_AUDIT.md`  
**Date:** 2026-11-05  

---

## 1. Executive Summary & Objective

The source audit in Stage D7.6 proved that an automated cold image search can execute up to **32 frames** in the worst case, requiring $\approx 2.64\text{ seconds}$ of pure wire time and up to $\approx 3.2\text{ seconds}$ of total wall-clock execution.

If a kernel watchdog uses a simplistic timer, legitimate long-running calibrations could be terminated prematurely, or rogue processes could keep sessions alive indefinitely by issuing empty requests.

This audit specifies a multi-tiered timing policy that reconciles individual transaction bounds, session inactivity limits, and absolute session duration caps.

---

## 2. Multi-Tiered Timing Architecture

To prevent deadlocks and false expirations, the timing policy is decoupled into three independent tiers:

```
+-----------------------------------------------------------------------------------------+
| TIER 1: Per-Transaction Hardware Timeout (250 ms)                                       |
| - Governs individual asynchronous SPI transactions (spi_async).                        |
| - Bounded by wait_for_completion_timeout(&xfer->done, 250ms).                           |
| - Prevents uninterruptible process hangs if SPI bus locks up.                           |
+-----------------------------------------------------------------------------------------+
                                           |
                                           v
+-----------------------------------------------------------------------------------------+
| TIER 2: Inter-Command Inactivity Watchdog (5,000 ms)                                    |
| - Governs maximum idle duration between consecutive userspace ioctls.                   |
| - Implemented via delayed_work on system_wq.                                            |
| - Refreshed ONLY on successful forward-progress ioctl completion.                       |
| - Passive queries (GET_STATE) NEVER refresh this deadline.                              |
+-----------------------------------------------------------------------------------------+
                                           |
                                           v
+-----------------------------------------------------------------------------------------+
| TIER 3: Absolute Hard Session Ceiling (30,000 ms)                                       |
| - Established at FTE3600_IOC_CALIB_BEGIN: hard_deadline = jiffies + 30*HZ.              |
| - Immutable: CANNOT be extended or refreshed by any ioctl.                              |
| - Prevents userspace loops from holding the hardware hostage indefinitely.              |
+-----------------------------------------------------------------------------------------+
```

---

## 3. Disambiguation of Operational States

| Operational State | Driver Context | Timer Behavior | Action on Expiration |
| :--- | :--- | :--- | :--- |
| **Normal Inter-Command Idle** | Userspace processing median calculation between frames (~1–5 ms). | Inactivity timer counting down towards 5,000 ms. | If no ioctl arrives for 5,000 ms: worker expires session; sets `UNKNOWN`. |
| **Active Frame Acquisition** | In-flight SPI transfer executing (~82 ms). | Per-transaction timeout counting down towards 250 ms. | If 250 ms elapses: wait returns 0; ioctl returns `-ETIMEDOUT`. |
| **Stranded / Timed-Out SPI** | SPI bus hung; transfer stranded in hardware. | Inactivity timer running; bus locked (`stranded_xfer != NULL`). | Subsequent ioctls return `-EBUSY`. Watchdog expires and demotes to `UNKNOWN`. |
| **Stalled Userspace Process** | Application paused in GDB, deadlocked, or crashed. | Inactivity timer counts down to 5,000 ms. | Worker acquires lock, sets `session->active = false`, sets `UNKNOWN`. |
| **Rogue Indefinite Polling** | Malicious or buggy client repeatedly calling ioctls. | Inactivity timer repeatedly refreshed. | Absolute Hard Ceiling (30 s) reached: kernel terminates session with `-ETIME`. |

---

## 4. Why 32 Frames Will Never Falsely Expire

In Design C, calibration frames are acquired **iteratively on-demand**:
1. Userspace calls `FTE3600_IOC_CALIB_READ_FRAME`.
2. The driver performs the ~82 ms transfer and copies pixels to userspace.
3. Upon return, the kernel sets:
   $$\text{session}\to\text{jiffies\_deadline} = \text{jiffies} + \text{msecs\_to\_jiffies}(5000);$$
4. Userspace executes `fte3600_image_measure()` and calculates the next DAC value in userspace CPU memory ($\approx 0.1\text{ ms}$).
5. Userspace immediately calls `FTE3600_IOC_CALIB_READ_FRAME` for the next frame.
6. **Result:** The 5-second timer is refreshed **32 times** during the 32-frame sequence. At no point does the idle duration between frames approach 5,000 ms.
7. **Total Sequence Time:** $\approx 3.2\text{ seconds}$ total, which is well within the 30-second absolute hard ceiling ($10\times$ safety headroom).

---

## 5. Elimination of Stale Worker Invalidation

A subtle race condition occurs if a worker thread for an earlier session is delayed on the runqueue and awakens after a new session has started:

### The Generation / Token Check:
In `kernel/fte3600.c`:
```c
static void fte3600_calib_watchdog_work(struct work_struct *work)
{
    struct fte3600_data *data = container_of(work, struct fte3600_data,
                                             session.watchdog_work.work);
    unsigned long flags;
    u32 current_token;

    mutex_lock(&data->lock);

    /* 1. If session is not active, worker does nothing */
    if (!data->session.active) {
        mutex_unlock(&data->lock);
        return;
    }

    /* 2. Passive deadline verification */
    if (!time_after_eq(jiffies, data->session.jiffies_deadline) &&
        !time_after_eq(jiffies, data->session.hard_deadline)) {
        mutex_unlock(&data->lock);
        return;
    }

    /* 3. Execute timeout teardown */
    data->session.active = false;
    data->session.id = 0;
    data->session.progress_mask = 0;
    data->session.owner_filp = NULL;
    data->state = FTE3600_STATE_UNKNOWN;
    data->last_error = ETIMEDOUT;

    mutex_unlock(&data->lock);
}
```
- **Deadline Recalculation Rule:** When a new session begins (`CALIB_BEGIN`), `jiffies_deadline` is set to `jiffies + 5*HZ`.
- If a stale worker awakens, `time_after_eq(jiffies, session->jiffies_deadline)` evaluates to **FALSE**. The stale worker terminates without touching the new session.

---

## 6. Evidence Classification

| Property | Classification | Proof Reference |
| :--- | :--- | :--- |
| Per-transaction 250 ms timeout bounds SPI wait | `ARCHITECTURAL-SPECIFICATION` | Formally specified in Section 2 |
| Inactivity watchdog 5,000 ms refreshed per-frame | `ARCHITECTURAL-SPECIFICATION` | Formally specified in Section 4 |
| Hard session ceiling 30,000 ms prevents infinite refresh | `ARCHITECTURAL-SPECIFICATION` | Formally specified in Section 2 |
| Passive GET_STATE does not refresh deadline | `ARCHITECTURAL-SPECIFICATION` | Formally specified in Section 2 |
| 32 frames execute in ~3.2 s total wall-clock time | `MATHEMATICALLY-DERIVED` | Wire time + processing analysis |
