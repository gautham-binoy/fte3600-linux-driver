# Stage D8 — Error Handling Matrix & Fault Recovery Specification

**Target Device:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Status:** FROZEN ARCHITECTURAL SPECIFICATION FOR STAGE D8  
**Safety Gate:** STRICTLY READ-ONLY DESIGN — ZERO HARDWARE ACCESS AUTHORIZED

---

## 1. Executive Summary & Philosophy

In the FTE3600 Linux kernel driver, error handling follows the **fail-closed invariant**:
1. Any transport failure, timeout, bus fault, or protocol violation immediately transitions internal driver state to `FTE3600_STATE_UNKNOWN` (or latches the domain into quarantine).
2. Hardware recovery is never performed silently or automatically; it strictly requires an explicit userspace `FTE3600_IOC_RESET` cycle.
3. Every error returns a standardized Linux `errno`, updates `data->last_error`, and logs a structured kernel diagnostic message.
4. Timed-out or stranded DMA transactions are never touched by userspace or freed prematurely; they enter the global quarantine subsystem.

---

## 2. Complete Error Code to Failure Mode Mapping

| Linux `errno` | Numerical | Category / Trigger | Internal State Transition | Driver Action & Recovery |
| :--- | :---: | :--- | :--- | :--- |
| **`-EINVAL`** | `22` | Invalid UAPI argument: bad `version`, nonzero `reserved` fields, bad buffer length, out-of-range DAC, unsupported flags. | No state change. | Rejects syscall immediately. No hardware transactions issued. |
| **`-EACCES`** | `13` | Security / Session violation: invalid 64-bit session token, attempt by non-owner OFD to issue session ioctls. | No state change. | Rejects ioctl. Prevents unauthorized or cross-process session interference. |
| **`-EBUSY`** | `16` | Contention / Lease conflict: second OFD attempts `SESSION_BEGIN` while active lease exists, or overlapping ioctl. | No state change. | Rejects attempt. Caller must wait until existing session terminates or times out. |
| **`-EPERM`** | `1` | State violation: `IOC_INIT` issued before `RESET`, `SESSION_BEGIN` issued before `INITIALIZED_UNCALIBRATED`, `CAPTURE` issued before `READY`. | No state change. | Rejects operation. Userspace must follow state machine transition sequence. |
| **`-ENODEV`** | `19` | Hardware absence: device offline (`atomic_read(&data->offline) == 1`), `spi == NULL`, peripheral unbound, or controller removed. | Transitions to `FTE3600_STATE_DEVICE_REMOVAL` or rejects immediately. | Blocks all hardware ioctls. Only `GET_STATE` permitted. |
| **`-ETIMEDOUT`** | `110` | Timeout: SPI async wait timeout (250 ms), 5.0 s session watchdog expiry, 10 ms `HW_STATUS` poll timeout, FDT stability timeout. | Transitions to `FTE3600_STATE_UNKNOWN`. On SPI timeout, latches domain quarantine. | Aborts session, cancels timers, retains stranded buffers in DRAM ($\mathtt{FREED}(t) \equiv 0$). Requires `IOC_RESET`. |
| **`-EBADMSG`** | `74` | Protocol corruption: wire CRC-16 mismatch on Chip ID or FDT, invalid command complement, corrupted FIFO header. | Transitions to `FTE3600_STATE_UNKNOWN`. | Fails closed. Records error. Requires `IOC_RESET` to resynchronize silicon sequencer. |
| **`-EFAULT`** | `14` | Memory fault: `copy_from_user()` or `copy_to_user()` failure (bad userspace virtual pointer). | No state change. | Rejects syscall immediately. |
| **`-ENOMEM`** | `12` | Memory exhaustion: `kzalloc()` failure for `fte3600_async_xfer`, quarantine table full (`FTE3600_MAX_DOMAINS` reached). | State unchanged or `UNKNOWN`. | Fails closed without leaking memory. |
| **`-EOPNOTSUPP`**| `95` | Unsupported function: operation blocked by milestone gate or invalid hardware variant. | No state change. | Returns error code cleanly. |
| **`-ESHUTDOWN`** | `108` | Domain quarantined: SPI controller domain permanently quarantined after DMA timeout. | Remained latched `FTE3600_STATE_UNKNOWN`. | Blocks all further SPI submissions to this controller until system reboot. |
| **`-ERESTARTSYS`**| `512` | Interrupted lock: process received signal while waiting for `mutex_lock_interruptible(&data->lock)`. | No state change. | Handled by kernel VFS signal restart layer. |

---

## 3. Detailed Failure Scenarios & State Recovery Matrix

### 3.1 Scenario 1: SPI Asynchronous Transfer Timeout (250 ms Expired)
- **Path:** `fte3600_spi_async_transfer()` dispatches `spi_async()` and sleeps on `wait_for_completion_timeout(..., 250ms)`.
- **Fault:** Completion not signaled within 250 ms.
- **Handling:**
  1. Driver marks transfer `timed_out = true`.
  2. Domain is latched into quarantine (`data->domain->quarantined = true`).
  3. `data->state` set to `FTE3600_STATE_UNKNOWN`, `data->last_error = ETIMEDOUT`.
  4. Returns `-ETIMEDOUT` to caller.
  5. The transfer object and buffers are **retained permanently in DRAM** ($\mathtt{FREED}(t) \equiv 0$).
  6. Subsequent SPI operations on this domain return `-ESHUTDOWN`.

### 3.2 Scenario 2: Session Watchdog Expiry (5000 ms Inactivity)
- **Path:** Session active; client process hangs, crashes, or stalls.
- **Fault:** `session.watchdog_work` delayed work timer fires after 5000 ms.
- **Handling:**
  1. Worker executes outside ioctl context. Acquires `data->lock`.
  2. Increments `session.generation` (invalidating the active token).
  3. Clears `session.active = false` and `session.owner_filp = NULL`.
  4. `data->state` transitions to `FTE3600_STATE_UNKNOWN`, `data->last_error = ETIMEDOUT`.
  5. Subsequent calls with the old session token return `-EACCES`.
  6. Recovery requires a new `FTE3600_IOC_RESET`.

### 3.3 Scenario 3: Process Abrupt Exit / File Descriptor Close
- **Path:** Client holds active session and crashes or calls `close(fd)`.
- **Fault:** `fte3600_cdev_release()` invoked while session active.
- **Handling:**
  1. `cancel_delayed_work_sync(&data->session.watchdog_work)` called **prior to taking `data->lock`** (deadlock prevention invariant).
  2. Driver acquires `data->lock`.
  3. If `filp == data->session.owner_filp`, session is invalidated immediately (`active = false`, `generation++`).
  4. Driver state transitions to `FTE3600_STATE_UNKNOWN`.
  5. `atomic_set(&data->is_open, 0)` releases single-client lease.
  6. Mutex unlocked; device refcount decremented via `put_device(&data->dev)`.

### 3.4 Scenario 4: Hardware Status Mismatch During Startup Prefix
- **Path:** `fte3600_hw_startup()` Step 3 reads SFR `0x80`, expected `0x50`.
- **Fault:** Value read is `0x00`, `0xFF`, or non-idle status.
- **Handling:**
  1. Driver transmits abort strobe `{ 0xC0, 0x3F, 0x00 }` to resync sequencer.
  2. Sleeps 1 ms.
  3. Re-reads SFR `0x80`.
  4. If still not `0x50`, transitions state to `FTE3600_STATE_UNKNOWN`, sets `data->last_error = EIO`, returns `-EIO`.
  5. No additional blind writes performed.

### 3.5 Scenario 5: Wire CRC-16 Checksum Failure
- **Path:** `fte3600_hw_identify()` reads Chip ID register `0x1A8B`.
- **Fault:** Wire CRC `RX[8..9]` does not match `fte3600_crc16_wire(&RX[6], 2)`.
- **Handling:**
  1. Log warning: `Wire CRC mismatch: rx 0x%04x != computed 0x%04x`.
  2. State set to `FTE3600_STATE_UNKNOWN`.
  3. `data->last_error = EBADMSG`.
  4. Returns `-EBADMSG` to userspace.
  5. Silicon reset required.

---

## 4. Telemetry and Diagnostic Logging Contract

Every failure path updates driver telemetry:
```c
static void fte3600_record_error(struct fte3600_data *data, int err, const char *msg)
{
    lockdep_assert_held(&data->lock);
    data->state = FTE3600_STATE_UNKNOWN;
    data->last_error = (u32)(err < 0 ? -err : err);
    if (data->spi)
        dev_err(&data->spi->dev, "Fault [%d]: %s (state -> UNKNOWN)\n", err, msg);
}
```
Userspace can interrogate `FTE3600_IOC_GET_STATE` at any time (even when offline or in error) to inspect `state`, `flags`, and `last_error`.

---

## 5. Conclusion and Sign-Off

The error handling matrix, state recovery paths, and errno contracts are formally frozen for Stage D8.
