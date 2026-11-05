# Stage D7.7 — VFS File Descriptor Ownership Policy Audit

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Linux VFS & Session Security Audit  
**Deliverable:** `diagnostics/STAGE_D7_7_FILE_OWNERSHIP_POLICY_AUDIT.md`  
**Date:** 2026-11-05  

---

## 1. Executive Summary & Objective

In multi-threaded or multi-process systems, multiple processes may open `/dev/fte3600` or inherit file descriptors via `fork()`. If session ownership is poorly defined, unauthorized processes could tamper with calibration, hijack session tokens, or corrupt device states.

This audit critically evaluates the semantics of Linux file descriptors, compares three distinct ownership policies, and defines the simplest, most secure ownership model.

---

## 2. Linux VFS File Descriptor Semantics

In the Linux kernel, there is a fundamental distinction between a **file descriptor (integer `fd`)** and an **open file description (`struct file *filp`)**:

```
Process A (PID 1001)                     Process B (PID 1002 - child of A)
+-----------------------+                +-----------------------+
| fd 3 -----------------+----+      +----+----------------- fd 3 |
+-----------------------+    |      |    +-----------------------+
                             v      v
                 +-------------------------------+
                 | struct file *filp             |  (Shared via fork() or dup())
                 | - f_count = 2                 |
                 | - private_data = data         |
                 +-------------------------------+
                                 ^
                                 | (Separate open)
Process C (PID 2000)             |
+-----------------------+        |
| fd 4 -----------------+--------+--- [DOES NOT SHARE filp!]
+-----------------------+             Creates NEW struct file *filp_C
```

### Key VFS Invariants:
1. **`dup()` / `dup2()` / `dup3()`:** Creates a new integer descriptor in the same process pointing to the **exact same `struct file`**.
2. **`fork()` / `clone()`:** Child processes inherit open file descriptors pointing to the **exact same `struct file`**, incrementing `filp->f_count`.
3. **`SCM_RIGHTS` (Unix Sockets):** Passing an fd across a UNIX socket transfers a reference to the **exact same `struct file`**.
4. **Separate `open()`:** Every call to `open("/dev/fte3600")` allocates a **new, distinct `struct file`** with its own reference count.

---

## 3. Comparison of Ownership Policies

### Policy A: Open File Description Ownership (`session->owner_filp = filp`)
- **Mechanism:** The calibration session records the pointer `struct file *filp` passed to `FTE3600_IOC_CALIB_BEGIN`.
- **Authorization Check:** Subsequent ioctls verify `if (session->owner_filp != filp) return -EACCES;`.
- **Pros:**
  - Perfectly handles multi-threaded processes (all threads share `filp`).
  - Seamlessly supports `fork()` and helper subprocesses holding inherited fds.
  - Immune to PID recycling and Linux PID namespaces.
  - Automatically isolates independent processes: a separate `open()` gets a different `filp`, completely preventing session hijacking even if the 32-bit token is leaked or guessed.
- **Cons:** Any process intentionally given the file descriptor (via `fork` or `SCM_RIGHTS`) can issue calibration calls (desirable behavior).

### Policy B: Process Identity Ownership (`session->owner_tgid = current->tgid`)
- **Mechanism:** Records the thread group ID (`tgid`) of the process calling `CALIB_BEGIN`.
- **Flaws & Fatal Pitfalls:**
  1. **PID Recycling:** PIDs in Linux wrap around (default limit 32,768). If the owner crashes and its PID is recycled to another daemon, access could be falsely granted.
  2. **PID Namespaces:** Containers and namespaces assign virtual PIDs that differ from global kernel PIDs, introducing subtle authentication bugs.
  3. **Broken Subprocesses:** If a calibration tool invokes a helper child process or uses `system()`, the child has a different `tgid` and is blocked from operating the session despite holding the valid descriptor.

### Policy C: Capability / Credential Model (`struct cred`)
- **Mechanism:** Enforces specific Linux capabilities (e.g. `CAP_SYS_ADMIN` or `CAP_SYS_RAWIO`).
- **Flaws:** Overly complex and unnecessary. Access to `/dev/fte3600` is already restricted by filesystem permissions (`0600 root:root`). Adding internal capability tokens adds friction without adding security.

---

## 4. Architectural Recommendation: Policy A (Open File Description)

### 4.1. Concrete Implementation Specification
In `kernel/fte3600.c`:
```c
/* Calibration session state */
struct fte3600_calib_session {
    bool active;
    u32 id;                     /* Ephemeral 32-bit nonce */
    struct file *owner_filp;    /* Bound open file description */
    u32 progress_mask;
    unsigned long jiffies_deadline;
};

/* In ioctl dispatcher: */
static long fte3600_ioctl_calib_step(struct fte3600_data *data, struct file *filp,
                                     u32 session_id, ...)
{
    mutex_lock(&data->lock);

    /* 1. Verify session active and token matches */
    if (!data->session.active || data->session.id != session_id) {
        mutex_unlock(&data->lock);
        return -ESTALE;
    }

    /* 2. Verify open file description ownership */
    if (data->session.owner_filp != filp) {
        mutex_unlock(&data->lock);
        return -EACCES;
    }

    ...
}
```

### 4.2. Security Guarantees:
1. **Hijack Immunity:** A separate process running as `root` that opens `/dev/fte3600` independently has a distinct `struct file *`. Any attempt to call calibration ioctls is immediately rejected with `-EACCES`.
2. **Crash Resilience:** When the process holding the file descriptor terminates, VFS calls `cdev_release()` when the last fd reference drops. `cdev_release()` detects `session->owner_filp == filp`, invalidates the session, and aborts any active transaction.
