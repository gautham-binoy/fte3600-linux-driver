# Stage D6 — Final Source Audit and Live Invariant Verification

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Audit Scope:** Full diff of Stage D6 changes across `kernel/fte3600.h`, `kernel/fte3600.c`, and `kernel/fte3600-test.c`  
**Audit Date:** 2026-11-04  
**Audit Classification:** `PASS — SOURCE-ONLY INITIALIZATION IMPLEMENTATION COMPLETE`  
**Document Ref:** `diagnostics/STAGE_D6_FINAL_SOURCE_AUDIT.md`  

---

## 1. Static Source Code Audit

### 1.1 Concurrency & Locking
- In `kernel/fte3600.c`, `fte3600_hw_startup()` explicitly asserts `lockdep_assert_held(&data->lock)` on entry.
- The `FTE3600_IOC_INIT` dispatch switch in `fte3600_ioctl_common()` holds `mutex_lock(&data->lock)` throughout the entire identification and volatile startup procedures.
- Concurrent reset attempts, subsequent init calls, or state queries are strictly serialized under `data->lock`.

### 1.2 Fail-Closed Error Transitions
- Every SPI transaction in `fte3600_hw_startup()` evaluates the return code.
- If any SPI write or read returns non-zero (`ret != 0`), execution immediately branches to `err:`:
  ```c
  err:
      data->state = FTE3600_STATE_UNKNOWN;
      data->last_error = (u32)(-ret);
      return ret;
  ```
- The controller is never left in `INITIALIZING` or `READY` upon a communications fault.
- Once in `UNKNOWN`, only a fresh `FTE3600_IOC_RESET` can restore the device to `RESET_SEQUENCE_ISSUED`.

### 1.3 Memory & Lifetime Safety
- DMA and cache alignment: `tx_buf` and `rx_buf` in `struct fte3600_data` remain annotated with `____cacheline_aligned`.
- Object reference balance: `fte3600_device_release` manages memory deallocation via `get_device(&data->dev)` and `put_device(&data->dev)`.
- Devres resource protection: on unbind/removal (`fte3600_remove`), `atomic_set(&data->offline, 1)` and `data->state = FTE3600_STATE_DEVICE_REMOVAL` are committed, and `reset_gpiod` and `spi` are set to `NULL` before unregistration returns.
- Poller synchronization: unbind signals `wake_up_interruptible_all(&data->event_wq)`, cleanly unblocking any waiting threads with `EPOLLERR | EPOLLHUP`.

### 1.4 ABI & UAPI Compatibility
- `sizeof(struct fte3600_init_params)` is verified to be exactly 32 bytes via `static_assert`.
- Reserved fields in `struct fte3600_init_params` (fields `reserved[0..5]`) are validated to be zero:
  ```c
  for (i = 0; i < 6; i++) {
      if (params.reserved[i] != 0)
          return -EINVAL;
  }
  ```
- Bit 0 of `params.flags` is allocated for `FTE3600_INIT_FLAG_FULL_STARTUP`. All remaining bits in `params.flags` are preserved for future extensions.
- Public state snapshots queried via `FTE3600_IOC_GET_STATE` are filtered through `fte3600_state_to_uapi()`, strictly constraining returned values to `[0..8]`.

---

## 2. Passive Hardware Invariant Verification

As mandated by Stage D6 safety instructions, the physical sensor was kept in the pre-existing Option 1 retention state.

### 2.1 Live Invariant Audit (Command Execution Output)

```bash
$ grep fte3600 /proc/modules
fte3600 20480 0 - Live 0xffffffffc09c2000 (O)

$ cat /sys/bus/spi/devices/spi-FTE3600:00/statistics/transfers
3

$ cat /sys/bus/spi/devices/spi-FTE3600:00/statistics/bytes
19

$ ls -l /dev/fte3600
crw------- 1 root root 511, 0 Nov  4 10:15 /dev/fte3600
```

### 2.2 Live Invariant Verification Findings
1. **Module Residency:** `fte3600.ko` remains loaded and live (`20480 0`).
2. **Device Node:** `/dev/fte3600` exists with character major 511, minor 0.
3. **SPI Hardware Invariants:**
   - SPI transfers: **exactly 3 transfers**.
   - SPI bytes transferred: **exactly 19 bytes**.
   - **Zero new SPI transfers** occurred during the entire Stage D6 implementation.
4. **Hardware Safety Restrictions:** 100% compliant.
