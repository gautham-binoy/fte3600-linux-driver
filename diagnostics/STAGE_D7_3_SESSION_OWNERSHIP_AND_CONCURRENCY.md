# Stage D7.3 — Session Ownership, Concurrency & Crash Resilience Design

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source Architecture Review (Zero Hardware Operations)  
**Deliverable:** `diagnostics/STAGE_D7_3_SESSION_OWNERSHIP_AND_CONCURRENCY.md`  

---

## 1. Executive Summary

This report establishes the complete architectural design for managing transactional calibration sessions in the recommended Hybrid Architecture (Design C).

Crucially, it replaces long-held kernel mutexes with a robust, token-authenticated, stateful kernel session model. It guarantees that:
1. Only the authorized file descriptor can execute calibration transactions.
2. Concurrent ioctl callers are either serviced safely (e.g. `GET_STATE`) or cleanly rejected (`-EBUSY`).
3. An interrupted, killed, or crashed userspace process triggers fail-closed kernel cleanup via `cdev_release()`.
4. Asynchronous device removal instantly revokes the session and aborts pending transfers.

---

## 2. Session Context Structure & State Variables

The device context (`struct fte3600_data`) is extended with a dedicated session descriptor:

```c
struct fte3600_calib_session {
    bool active;                   /* True while calibration session is open */
    u32 session_id;                /* Non-zero pseudorandom authentication token */
    struct file *owner_filp;       /* File pointer of opening process */
    pid_t owner_tgid;              /* Thread group ID (PID) of session initiator */
    unsigned long expire_jiffies;  /* Inactivity timeout deadline */
    u32 completed_phases;          /* Bitmask tracking validated milestones */
    u8 calibrated_fdt_dac;         /* Chosen analog bias DAC byte */
    u8 calibrated_image_dac;       /* Chosen image bias DAC byte */
    u16 channel_minima[4];         /* Validated channel stability minima */
    u16 thresholds[4];             /* Programmed touch thresholds */
};
```

### Phase Tracking Bitmask:
```c
#define FTE3600_CALIB_PHASE_FDT_SAMPLED     (1U << 0)
#define FTE3600_CALIB_PHASE_FDT_VALIDATED   (1U << 1)
#define FTE3600_CALIB_PHASE_IMAGE_SEARCHED  (1U << 2)
#define FTE3600_CALIB_PHASE_THRESHOLDS_SET  (1U << 3)
```

---

## 3. Session Lifecycle State Machine

```
  ┌─────────────────────────────────────────────────────────────┐
  │ 1. SESSION INITIATION (FTE3600_IOC_CALIB_BEGIN)             │
  │    - Verify data->state == INITIALIZED_UNCALIBRATED (14)    │
  │    - Generate 32-bit random session_id                      │
  │    - Bind owner_filp = filp, owner_tgid = current->tgid     │
  │    - Set expire_jiffies = jiffies + msecs_to_jiffies(5000)  │
  │    - Set session->active = true                             │
  └──────────────────────────────┬──────────────────────────────┘
                                 │
                                 ▼
  ┌─────────────────────────────────────────────────────────────┐
  │ 2. TRANSACTIONAL STEPPING (FDT_SAMPLE, READ_FRAME)          │
  │    - Acquire data->lock                                     │
  │    - Verify session->active == true                         │
  │    - Verify filp == session->owner_filp                     │
  │    - Verify input.session_id == session->session_id         │
  │    - Verify time_before(jiffies, session->expire_jiffies)   │
  │    - Execute bounded SPI transfer (Max ~86 ms)              │
  │    - Refresh expire_jiffies deadline                        │
  │    - Release data->lock                                     │
  └──────────────────────────────┬──────────────────────────────┘
                                 │
            ┌────────────────────┴────────────────────┐
            │ Success                                 │ Error / Timeout / Abort
            ▼                                         ▼
  ┌───────────────────────────────┐         ┌───────────────────────────────┐
  │ 3. COMMIT (CALIB_COMMIT)      │         │ 4. ABORT / CRASH TEARDOWN     │
  │    - Verify all phases set    │         │    - Reset or force UNKNOWN   │
  │    - Program bank & arm strobe│         │    - Invalidate session token │
  │    - data->state = READY (4)  │         │    - session->active = false  │
  │    - Clear session->active    │         │    - data->state = UNKNOWN (7)│
  └───────────────────────────────┘         └───────────────────────────────┘
```

---

## 4. Concurrency & Protection Rules

### 4.1 Token Authentication & Stale Identifier Rejection
- Every calibration ioctl payload (`FDT_SAMPLE`, `READ_FRAME`, `SET_THRESHOLDS`, `COMMIT`, `ABORT`) requires `session_id` in its header.
- The kernel validates:
  ```c
  if (!data->session.active)
      return -ENODEV;
  if (filp != data->session.owner_filp)
      return -EBUSY;
  if (params.session_id != data->session.session_id)
      return -EACCES;
  ```
- If a userspace process crashes and restarts, or sends a stale token from a previous run, the call is rejected immediately with `-EACCES`.

### 4.2 Handling Multiple Processes & File Descriptors
- The character device already enforces a single-client open lease via `atomic_cmpxchg(&data->is_open)` (`kernel/fte3600.c:614`).
- Any second process attempting `open("/dev/fte3600")` is rejected with `-EBUSY` at open time.
- If a multi-threaded application shares the same open file descriptor:
  - Any thread attempting non-session operations (`FTE3600_IOC_INIT`, `FTE3600_IOC_RESET`) while `session.active` is true is rejected with `-EBUSY`.
  - Telemetry query `FTE3600_IOC_GET_STATE` is permitted, acquiring `data->lock` for $<5 \mu\text{s}$ between steps and returning state with `FTE3600_FLAG_CALIBRATION_PENDING`.

### 4.3 Inactivity Watchdog & Session Expiration
- Calibration is an interactive hardware procedure that must complete within a strict temporal bound.
- The driver enforces a default inactivity timeout of **5.0 seconds**:
  ```c
  if (time_after(jiffies, data->session.expire_jiffies)) {
      dev_err(&data->spi->dev, "Calibration session timed out; aborting\n");
      fte3600_calib_abort_locked(data);
      return -ETIMEDOUT;
  }
  ```
- Each successful step resets the watchdog deadline:
  `data->session.expire_jiffies = jiffies + msecs_to_jiffies(5000);`.
- If userspace hangs in an infinite loop, the next operation or close immediately tears down the session.

---

## 5. Crash Resilience & Process Teardown

A central danger of hybrid calibration is an unexpected userspace termination (e.g. `SIGSEGV`, `SIGKILL`, `abort()`) while the sensor is partially configured.

### 5.1 Teardown Hook in `fte3600_cdev_release()`
When a process terminates, the Linux kernel VFS unconditionally invokes `filp->f_op->release` (`fte3600_cdev_release`):

```c
static int fte3600_cdev_release(struct inode *inode, struct file *filp)
{
    struct fte3600_data *data = filp->private_data;

    mutex_lock(&data->lock);
    if (data->session.active && data->session.owner_filp == filp) {
        dev_warn(&data->spi->dev,
                 "Process %d terminated with active calibration session; aborting\n",
                 data->session.owner_tgid);
        fte3600_calib_abort_locked(data);
    }
    mutex_unlock(&data->lock);

    atomic_set(&data->is_open, 0);
    put_device(&data->dev);
    return 0;
}
```

### 5.2 Abort Actions (`fte3600_calib_abort_locked()`):
1. **Invalidates Session:** Sets `session.active = false`, `session.session_id = 0`, `session.owner_filp = NULL`.
2. **Hardware Invalidation:** Because the sensor may have received intermediate bias or timing commands, the driver transitions:
   ```c
   data->state = FTE3600_STATE_UNKNOWN;
   data->last_error = ECANCELED;
   ```
3. **Capture Lockdown:** State `UNKNOWN` strictly blocks `FTE3600_IOC_CAPTURE` (`-EPERM`).
4. **Recovery Path:** The only way to exit `UNKNOWN` is an active-low hardware reset pulse (`FTE3600_IOC_RESET`), ensuring the sensor silicon is electrically reset before another attempt.

---

## 6. Device Removal & Unbind Synchronization

If `rmmod fte3600` is executed or the SPI device is unbound via sysfs while userspace is midway through calibration:

1. `fte3600_remove()` is invoked by the SPI bus subsystem.
2. Step 1: `cdev_device_del()` removes `/dev/fte3600`, preventing new calls.
3. Step 2: `mutex_lock(&data->lock)` waits for any active in-flight step (max ~86 ms) to complete.
4. Step 3:
   ```c
   atomic_set(&data->offline, 1);
   data->state = FTE3600_STATE_DEVICE_REMOVAL;
   data->session.active = false;
   data->session.session_id = 0;
   data->spi = NULL;
   data->reset_gpiod = NULL;
   ```
5. Step 4: `mutex_unlock(&data->lock)`.
6. Any subsequent calibration step waiting for the mutex wakes up, observes `atomic_read(&data->offline) == 1`, and immediately aborts with `-ENODEV`.
7. When userspace finally closes its file descriptor, `put_device(&data->dev)` frees driver memory safely.
