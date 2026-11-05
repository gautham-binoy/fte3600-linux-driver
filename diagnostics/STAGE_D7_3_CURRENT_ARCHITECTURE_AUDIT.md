# Stage D7.3 — Current Architecture, State Machine & Concurrency Audit

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source Architecture Audit (Zero Hardware Operations)  
**Deliverable:** `diagnostics/STAGE_D7_3_CURRENT_ARCHITECTURE_AUDIT.md`  

---

## 1. Executive Summary

This report establishes the verified baseline of the current Linux kernel driver architecture (`kernel/fte3600.c`, `kernel/fte3600.h`, `kernel/fte3600-policy.h`) as of Stage D7.2. It analyzes the public and internal state machine, synchronization primitives, file descriptor lifetime management, ioctl validation contracts, SPI transaction serialization, and capture gating.

Every claim in this report is anchored to exact source file lines in the active driver codebase.

---

## 2. Public and Internal Driver State Machine

The driver implements a dual-tier state machine defining 9 public UAPI states and 6 extended internal synchronization states (`kernel/fte3600.h:31–53`).

### 2.1 Public UAPI States (`enum fte3600_state`, 0 through 8)
- `FTE3600_STATE_UNINITIALIZED` (0): Driver bound, passive state; zero hardware transactions issued.
- `FTE3600_STATE_RESETTING` (1): Actively driving Pad 14 low for hardware reset.
- `FTE3600_STATE_RESET_SEQUENCE_ISSUED` (2): Electrical reset pulse completed; Pad 14 restored high.
- `FTE3600_STATE_INITIALIZING` (3): Transient state while executing identification or startup prefix.
- `FTE3600_STATE_READY` (4): Fully operational and calibrated state (capture permitted).
- `FTE3600_STATE_CAPTURING` (5): Transient state during active matrix exposure / FIFO clocking.
- `FTE3600_STATE_ERROR` (6): Fatal unrecoverable driver/hardware failure.
- `FTE3600_STATE_UNKNOWN` (7): Hardware in indeterminate state (e.g. timeout or failed startup).
- `FTE3600_STATE_IDENTIFIED` (8): Sensor responsiveness verified, chip ID `0x9362` and CRC `0x1C53` validated.

### 2.2 Extended Internal Driver States (9 through 14)
- `FTE3600_STATE_WAITING_FOR_TOUCH` (9): Autonomous FDT comparator monitoring armed.
- `FTE3600_STATE_TOUCH_DETECTED` (10): Pad 229 interrupt signaled; finger contact latched.
- `FTE3600_STATE_FRAME_READY` (11): Image exposure completed; status `0x80 == 0x54` asserted.
- `FTE3600_STATE_SUSPENDED` (12): Device in low-power ACPI sleep state.
- `FTE3600_STATE_DEVICE_REMOVAL` (13): Unbind triggered; hardware pointers nullified.
- `FTE3600_STATE_INITIALIZED_UNCALIBRATED` (14): Volatile startup prefix completed; analog baseline and threshold arming pending.

### 2.3 State Translation & Clamping (`kernel/fte3600.h:78–103`)
Public queries via `FTE3600_IOC_GET_STATE` never expose internal raw integers $\ge 9$:
- Internal state 14 (`INITIALIZED_UNCALIBRATED`) maps to public state 8 (`IDENTIFIED`).
- Internal states 9–11 (touch/frame lifecycle) map to public state 4 (`READY`).
- Internal state 12 (`SUSPENDED`) maps to public state 7 (`UNKNOWN`).
- Internal state 13 (`DEVICE_REMOVAL`) maps to public state 0 (`UNINITIALIZED`).

```
  STATE TRANSITION GRAPH (Current Implementation):

  [UNINITIALIZED (0)]
          │
          │ ioctl(FTE3600_IOC_RESET)
          ▼
    [RESETTING (1)]
          │
          │ 10 ms pulse complete
          ▼
  [RESET_SEQUENCE_ISSUED (2)]
          │
          │ ioctl(FTE3600_IOC_INIT, flags=IDENTIFY_ONLY)
          ▼
   [IDENTIFIED (8)]
          │
          │ ioctl(FTE3600_IOC_INIT, flags=FULL_STARTUP)
          ▼
   [INITIALIZING (3)]
          │
          ├─────────────────────────────────────────┐
          │ (Success: prefix completed)             │ (Failure: any bus/status error)
          ▼                                         ▼
  [INITIALIZED_UNCALIBRATED (14)]             [UNKNOWN (7)]
  (Capture strictly rejected: -EPERM)               │
          │                                         │ ioctl(FTE3600_IOC_RESET)
          │ (CALIBRATION GAP: Unreachable)          ▼
          ▼                                   [RESETTING (1)]
     [READY (4)]
```

---

## 3. Concurrency, Locking & Serialization Architecture

### 3.1 Mutex Serialization (`struct mutex lock`)
- Defined in `struct fte3600_data` (`kernel/fte3600.c:39`) and initialized in `fte3600_probe()` (`line 867`).
- Protects:
  - All SPI command strobes, register reads, and multi-byte writes (`lines 492, 655, 676, 715, 768`).
  - All hardware state machine transitions (`data->state`).
  - Active-low hardware reset pulsing (`data->reset_gpiod`).
  - Driver shutdown and resource nullification during module unbind (`lines 952–969`).
- Every ioctl handler acquires `mutex_lock_interruptible(&data->lock)` (`lines 655, 676, 715, 768`), returning `-ERESTARTSYS` if interrupted by a signal.

### 3.2 Single-Client Exclusive Lease (`atomic_t is_open`)
- In `fte3600_cdev_open()` (`kernel/fte3600.c:614`):
  ```c
  if (atomic_cmpxchg(&data->is_open, 0, 1) != 0)
      return -EBUSY;
  ```
- The character device node `/dev/fte3600` strictly permits only **one open file descriptor at a time** across the entire system. Any concurrent `open()` call from a second process or thread is rejected with `-EBUSY`.
- Released in `fte3600_cdev_release()` via `atomic_set(&data->is_open, 0)` (`line 633`).

### 3.3 Device Unbind & Offline Invariant (`atomic_t offline`)
- Initialized to 0 during probe (`kernel/fte3600.c:870`).
- Checked at the entry of `fte3600_cdev_open()` (`line 610`) and on the fast-path of `fte3600_ioctl_common()` (`line 648`).
- Set to 1 under `data->lock` in `fte3600_remove()` (`line 958`).
- Every ioctl re-verifies `atomic_read(&data->offline)` after acquiring `data->lock` (`lines 685, 719, 772`), aborting with `-ENODEV` if unbind occurred while waiting for the mutex.

### 3.4 Poll Synchronization & Waitqueue (`wait_queue_head_t event_wq`)
- Defined in `struct fte3600_data` (`kernel/fte3600.c:40`) and initialized in `probe()` (`line 868`).
- `fte3600_cdev_poll()` registers the calling thread via `poll_wait(filp, &data->event_wq, wait)` (`line 822`).
- If `offline` is set, returns `EPOLLERR | EPOLLHUP` (`line 825`).
- During module removal, `wake_up_all(&data->event_wq)` awakens all sleeping pollers (`line 974`).

---

## 4. File Descriptor & Object Lifetime Accounting

The driver employs an embedded `struct device dev` to decouple character device unregistration from memory deallocation (`kernel/fte3600.c:38, 879–880, 946, 983, 1000–1006`):

1. **Probe Allocation:** `kzalloc(sizeof(*data))` allocates the device context. `device_initialize(&data->dev)` sets its initial reference count to 1 (`line 879`).
2. **Open Acquisition:** `fte3600_cdev_open()` increments the reference counter via `get_device(&data->dev)` (`line 623`).
3. **Close Release:** `fte3600_cdev_release()` decrements the reference counter via `put_device(&data->dev)` (`line 634`).
4. **Driver Unbind (`fte3600_remove()`):**
   - Calls `cdev_device_del(&data->cdev, &data->dev)` (`line 946`), unlinking `/dev/fte3600` from sysfs and character device lookup.
   - Nullifies `data->spi` and `data->reset_gpiod` under `data->lock` (`lines 960–961`).
   - Drops the probe's initial reference via `put_device(&data->dev)` (`line 983`).
5. **Memory Teardown (`fte3600_device_release()`):**
   Invoked automatically when the final reference is dropped:
   ```c
   static void fte3600_device_release(struct device *dev)
   {
       struct fte3600_data *data = container_of(dev, struct fte3600_data, dev);
       kfree(data);
   }
   ```
   This guarantees that an open file descriptor racing against device unbind cannot trigger a use-after-free.

---

## 5. Current IOCTL Interface Contracts

All ioctl structures are statically asserted to 32 bytes (`kernel/fte3600.h:167–169`):

### 5.1 `FTE3600_IOC_GET_STATE` (`_IOR('F', 0x01, struct fte3600_state_snapshot)`)
- Structure: `version` (4 B), `state` (4 B), `flags` (4 B), `last_error` (4 B), `reserved[4]` (16 B). Total: 32 bytes.
- Preconditions: None. Executes even when `offline` is set (`kernel/fte3600.c:648`).
- Returns: Canonical state [0..8], status flags (`OFFLINE`, `RESET_SEQUENCE_ISSUED`, `STARTUP_PREFIX_COMPLETED`, `CALIBRATION_PENDING`), and `last_error`.

### 5.2 `FTE3600_IOC_RESET` (`_IO('F', 0x02)`)
- Preconditions: Device must not be offline (`data->reset_gpiod != NULL`).
- Permitted States: Callable from **any valid driver state** (`UNINITIALIZED`, `RESET_SEQUENCE_ISSUED`, `IDENTIFIED`, `INITIALIZED_UNCALIBRATED`, `ERROR`, `UNKNOWN`).
- Action: Asserts Pad 14 low for 10 ms, restores high, waits 20 ms settling delay.
- Postcondition: Sets `data->state = FTE3600_STATE_RESET_SEQUENCE_ISSUED` (2); clears `data->last_error = 0`.

### 5.3 `FTE3600_IOC_INIT` (`_IOWR('F', 0x03, struct fte3600_init_params)`)
- Structure: `version` (4 B), `flags` (4 B), `reserved[6]` (24 B). Total: 32 bytes.
- Validation: Rejects `version != 1`, undefined flags (`flags & ~FTE3600_INIT_FLAGS_MASK`), or non-zero reserved fields with `-EINVAL` (`kernel/fte3600.c:704–713`).
- Permitted States:
  - State 2 (`RESET_SEQUENCE_ISSUED`): Executes `fte3600_hw_identify()`. If flag `FTE3600_INIT_FLAG_FULL_STARTUP` is set, immediately executes `fte3600_hw_startup()`.
  - State 8 (`IDENTIFIED`): Executes `fte3600_hw_startup()`.
  - Any other state: Returns `-EPERM` (`line 741`).
- Postcondition: On success, state enters `INITIALIZED_UNCALIBRATED` (14). On failure, state enters `UNKNOWN` (7).

### 5.4 `FTE3600_IOC_CAPTURE` (`_IOWR('F', 0x04, struct fte3600_frame_buffer)`)
- Structure: `version` (4 B), `length` (4 B), `buffer_ptr` (8 B), `reserved[4]` (16 B). Total: 32 bytes.
- Validation: Rejects `version != 1`, `length != 10240`, or non-zero reserved fields with `-EINVAL`.
- **Capture-Gating Rule (`lines 786–789`):**
  ```c
  if (data->state != FTE3600_STATE_READY) {
      mutex_unlock(&data->lock);
      return -EPERM;
  }
  ```
  Returns `-EPERM` in all states except `READY` (4). In `READY`, returns `-EOPNOTSUPP` pending physical FIFO implementation.

---

## 6. Architectural Audit Findings

| Component | Verified Implementation Fact | Source Evidence | Classification |
|---|---|---|---|
| **Public State Range** | Public UAPI states are strictly bounded to `[0, 8]`. | `kernel/fte3600.h:65, 78–103` | **SOURCE-VERIFIED** |
| **Startup Terminal State**| `fte3600_hw_startup()` terminates in state 14 (`INITIALIZED_UNCALIBRATED`). | `kernel/fte3600.c:591` | **SOURCE-VERIFIED** |
| **READY Accessibility** | State 4 (`READY`) is **completely unreachable** in current driver code. | `grep -rn "STATE_READY" kernel/` | **SOURCE-VERIFIED** |
| **Capture Gating** | `FTE3600_IOC_CAPTURE` returns `-EPERM` in state 14. | `kernel/fte3600.c:786` | **SOURCE-VERIFIED** |
| **Single-Client Access** | Character device enforces single-client lease via `atomic_cmpxchg(&data->is_open)`. | `kernel/fte3600.c:614` | **SOURCE-VERIFIED** |
| **Locking Granularity** | `data->lock` held continuously across entire identification/startup sequence (~15–20 ms). | `kernel/fte3600.c:715–747` | **SOURCE-VERIFIED** |
| **Teardown Safety** | Unbind nullifies `spi` and `reset_gpiod` under `data->lock`, avoiding dangling devres access. | `kernel/fte3600.c:952–969` | **SOURCE-VERIFIED** |
| **Recovery from UNKNOWN**| State `UNKNOWN` (7) rejects `INIT` with `-EPERM`; recovery requires `RESET` (2). | `kernel/fte3600.c:734–742` | **SOURCE-VERIFIED** |
