# Stage D8 — Session Management Specification: Tokens, File Description Ownership and Concurrency

**Target Device:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Status:** FROZEN ARCHITECTURAL SPECIFICATION FOR STAGE D8  
**Safety Gate:** STRICTLY READ-ONLY DESIGN — ZERO HARDWARE ACCESS AUTHORIZED

---

## 1. Executive Summary

In a hybrid fingerprint driver, multi-step calibration and capture operations require stateful progression across multiple userspace ioctls. Leaving the kernel in an indeterminate intermediate state due to process crashes, thread hijacking, or abandoned sessions invites severe hardware deadlock or memory corruption.

This specification defines the **Session-Gated Hybrid Management Subsystem**:
1. **Cryptographic/Monotonic Session Tokens:** 64-bit session handle validating every transactional ioctl.
2. **Open File Description (OFD) Ownership:** Binding session authority to `struct file *`, correctly handling `dup()`, `fork()`, and `SCM_RIGHTS` descriptor passing.
3. **Inactivity Watchdog:** A 5.0-second kernel timer terminating abandoned sessions and failing closed.
4. **Milestone Progress Tracking:** Strict bitmask gating state machine progression through the 5 calibration milestones.

---

## 2. Session Data Structure (`struct fte3600_session`)

Embedded directly within per-device private data (`struct fte3600_data`):

```c
struct fte3600_session {
    bool                valid;              /* True if session is currently active */
    u64                 token;              /* 64-bit session identifier */
    struct file        *owner_filp;         /* Bound Open File Description pointer */
    u32                 generation;         /* Monotonic session generation counter */
    u32                 milestones;         /* Bitmask of completed calibration stages */
    unsigned long       last_activity_jiffies; /* Timestamp of most recent valid ioctl */
    struct delayed_work watchdog_work;      /* Inactivity watchdog work item */
};
```

### 2.1 Calibration Milestones Bitmask

```c
#define FTE3600_MILESTONE_IDENTIFY      (1U << 0)  /* Chip ID & CRC validated */
#define FTE3600_MILESTONE_IDLE_HANDSHAKE(1U << 1)  /* Wakeup dialog 5A A5 completed */
#define FTE3600_MILESTONE_FDT_CALIB     (1U << 2)  /* Stability window & thresholds set */
#define FTE3600_MILESTONE_BASELINE_FRAME(1U << 3)  /* Baseline raw frame read & verified */
#define FTE3600_MILESTONE_ARMED         (1U << 4)  /* Threshold bank written & arm strobe */
#define FTE3600_MILESTONES_ALL_READY    0x0000001FU /* All 5 milestones required for READY */
```

---

## 3. Open File Description (OFD) Ownership Architecture

In Linux VFS, a file descriptor (`int fd`) is a per-process index into a table pointing to an **Open File Description** (`struct file`).

```mermaid
graph TD
    subgraph Process A [Process A]
        FD1[fd 3]
        FD2[fd 4 (dup of 3)]
    end

    subgraph Process B [Process B (forked child / SCM_RIGHTS)]
        FD3[fd 3]
    end

    subgraph VFS Core [Linux VFS Layer]
        OFD["struct file (Open File Description)<br/>Bound to Session Owner"]
    end

    subgraph Kernel Driver [fte3600 Kernel Subsystem]
        Session["struct fte3600_session<br/>owner_filp == struct file *"]
    end

    FD1 --> OFD
    FD2 --> OFD
    FD3 --> OFD
    OFD --> Session

    style OFD fill:#e1f5fe,stroke:#0288d1,stroke-width:2px
    style Session fill:#e8f5e9,stroke:#388e3c,stroke-width:2px
```

### 3.1 Handling Shared Descriptors (`dup`, `fork`, `SCM_RIGHTS`)
- **Invariant:** Session ownership is tied to `struct file *filp`, **NOT** to userspace PID or thread ID.
- **Shared Access:**
  - If a process calls `dup(fd)`, both integer descriptors share the identical `struct file *`. Both can legally issue session ioctls.
  - If a process forks a child, the child inherits the file table entries pointing to the same `struct file *`. Both can legally issue session ioctls.
  - If a process passes `fd` over a UNIX domain socket using `SCM_RIGHTS`, the recipient receives a reference to the same `struct file *`.
- **Rogue / Distinct Open Access:**
  - If an unrelated process (or the same process) calls `open("/dev/fte3600")`, VFS creates a **distinct** `struct file *`.
  - When that distinct `filp` attempts any session ioctl:
    $$\mathtt{filp} \neq \mathtt{session.owner\_filp} \implies \text{Driver immediately aborts with } \mathbf{-EACCES}$$
  - If it attempts `SESSION_BEGIN`:
    $$\mathtt{session.valid} == \mathbf{true} \implies \text{Driver immediately aborts with } \mathbf{-EBUSY}$$

---

## 4. Inactivity Watchdog Lifecycle and Deadlock Safety

To prevent orphaned sessions from blocking the hardware indefinitely if userspace hangs or crashes without closing the descriptor:

### 4.1 Watchdog Mechanics
1. **Timeout Duration:** 5.0 seconds (`msecs_to_jiffies(5000)`).
2. **Re-arming:** Every successful session ioctl calls `mod_delayed_work(system_wq, &data->session.watchdog_work, timeout)`.
3. **Expiry Callback (`fte3600_watchdog_work`):**
   ```c
   static void fte3600_watchdog_work(struct work_struct *work)
   {
       struct fte3600_data *data = container_of(to_delayed_work(work), ...);
       
       mutex_lock(&data->lock);
       if (data->session.valid) {
           pr_warn("fte3600: Calibration session 0x%llx expired; timing out fail-closed\n",
                   data->session.token);
           data->session.valid = false;
           data->session.owner_filp = NULL;
           data->session.token = 0;
           data->state = FTE3600_STATE_UNKNOWN; /* Fail-closed */
       }
       mutex_unlock(&data->lock);
   }
   ```

### 4.2 Mandatory Deadlock Remediation Rule
**Critical Locking Invariant:** In `fte3600_release()` and `fte3600_spi_remove()`, calling `cancel_delayed_work_sync(&data->session.watchdog_work)` while holding `data->lock` causes a catastrophic AB-BA self-deadlock if the watchdog worker is currently blocking on `mutex_lock(&data->lock)`.

**Mandatory Rule for Stage D8:**
```c
/* CORRECT SYNCHRONIZATION SEQUENCE IN TEARDOWN / RELEASE: */
cancel_delayed_work_sync(&data->session.watchdog_work); /* <--- OUTSIDE data->lock */
mutex_lock(&data->lock);
/* Perform cleanup */
mutex_unlock(&data->lock);
```

---

## 5. Session Abort and Release Mechanics

### 5.1 Explicit Abort (`FTE3600_IOC_SESSION_ABORT`)
- Invoked by userspace when an unexpected condition or noise threshold failure occurs.
- Cancels watchdog.
- Invalidates session token.
- Sets `data->state = FTE3600_STATE_UNKNOWN` (forces clean electrical cycle before retry).
- Returns 0 to userspace.

### 5.2 Implicit Release on File Close (`fte3600_release`)
- When a process exits, crashes, or calls `close(fd)`, Linux VFS invokes the driver's `.release` file operation.
- If `filp == data->session.owner_filp`:
  - Cancels watchdog timer synchronously.
  - Clears `data->session.valid = false`.
  - Clears `data->session.owner_filp = NULL`.
  - Releases single-client lease: `atomic_set(&data->is_open, 0)`.
- If an asynchronous SPI transaction was in-flight at the moment of exit, it continues under its own 250 ms wait. Buffers remain protected.

---

## 6. Conclusion and Stage D8 Freeze Sign-Off

The Session Management Subsystem provides complete transactional isolation:
- Tokens and OFD ownership eliminate unauthorized cross-talk.
- Deadlock-free watchdog cancellation guarantees robust background timeout handling.
- The specification is frozen and ready for Stage D8 implementation.
