# Stage D6.6 — OFFLINE State Mapping & Userspace UAPI Contract

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Environment:** Linux `7.2.8-9-omarchy` (x86_64)  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Driver State:** Stage D6.6 Source-Only Remediation  

---

## 1. Executive Summary

In kernel driver lifecycle management, physical unplug, ACPI power events, or driver unbinding can occur while an application holds an open file descriptor (`/dev/fte3600`). Exposing internal teardown states (such as `FTE3600_STATE_DEVICE_REMOVAL` = 13) across the UAPI boundary violates ABI state guarantees (where valid public states are strictly bounded to `[0..8]`).

Stage D5.2 established the internal-to-public mapping, and Stage D6.6 formally specifies the userspace contract, non-revival guarantees, and repeated call semantics.

---

## 2. Kernel Mapping Implementation

In `kernel/fte3600.c`, `fte3600_state_to_uapi()` converts internal driver states to public UAPI states:

```c
static __u32 fte3600_state_to_uapi(enum fte3600_state state)
{
	switch (state) {
	case FTE3600_STATE_UNINITIALIZED:
	case FTE3600_STATE_RESET:
	case FTE3600_STATE_IDENTIFIED:
	case FTE3600_STATE_RESETTING:
	case FTE3600_STATE_INITIALIZING:
	case FTE3600_STATE_CALIBRATING:
	case FTE3600_STATE_READY:
	case FTE3600_STATE_CAPTURING:
	case FTE3600_STATE_UNKNOWN:
		return (__u32)state;
	case FTE3600_STATE_UNCALIBRATED:
		/*
		 * Internal state 14 represents completion of the vendor volatile
		 * prefix sequence, awaiting calibration and baseline acquisition.
		 * Map to public FTE3600_STATE_IDENTIFIED (8) with the telemetry
		 * flag FTE3600_FLAG_STARTUP_PREFIX_COMPLETED asserted.
		 */
		return FTE3600_STATE_IDENTIFIED;
	case FTE3600_STATE_DEVICE_REMOVAL:
	default:
		/*
		 * Internal state 13 (DEVICE_REMOVAL) must never leak across UAPI.
		 * When offline, the driver returns FTE3600_STATE_UNINITIALIZED (0)
		 * and asserts FTE3600_FLAG_OFFLINE in snapshot.flags.
		 */
		return FTE3600_STATE_UNINITIALIZED;
	}
}
```

When building the snapshot for `FTE3600_IOC_GET_STATE`:
```c
	snap.state = fte3600_state_to_uapi(data->state);
	snap.flags = 0;
	if (data->offline)
		snap.flags |= FTE3600_FLAG_OFFLINE;
```

---

## 3. Userspace Contract & Client Rules

### 3.1 Precedence of `FTE3600_FLAG_OFFLINE`

When an application receives a `struct fte3600_state_snapshot` via `ioctl(fd, FTE3600_IOC_GET_STATE, &snap)`:

1. **Mandatory First Check:**  
   The application **MUST** test:
   ```c
   if (snap.flags & FTE3600_FLAG_OFFLINE) {
       /* Device is disconnected, unmounted, or removed */
       /* DO NOT interpret snap.state as an actionable operational state */
   }
   ```
2. **State Subordination:**  
   While `snap.state` will report `0` (`FTE3600_STATE_UNINITIALIZED`), this is an ABI range-isolation placeholder. The `OFFLINE` flag takes absolute precedence over all state fields.

### 3.2 Non-Revival Invariant

Once `data->offline` is set to `true` (via `fte3600_remove()`):
1. **Irreversible State:**  
   The device can never be restored to an online state on that file descriptor.
2. **Immediate Rejection of Hardware IOCTLs:**  
   Every hardware ioctl (`FTE3600_IOC_RESET`, `FTE3600_IOC_INIT`, `FTE3600_IOC_CAPTURE`) tests `data->offline` under `data->lock` and returns `-ENODEV` immediately:
   ```c
   if (data->offline) {
       mutex_unlock(&data->lock);
       return -ENODEV;
   }
   ```
3. **Zero SPI Accesses:**  
   Under no circumstances can an offline descriptor issue SPI messages or toggle GPIO lines.
4. **RESET Cannot Revive:**  
   Issuing `FTE3600_IOC_RESET` on an offline descriptor returns `-ENODEV` and does not alter the driver state or clear the offline flag.

---

## 4. Reset & Repeated Call Semantics

### 4.1 Repeated INIT Rejection

If userspace calls `FTE3600_IOC_INIT` when the driver is in an invalid state:
- **From UNINITIALIZED:** Rejected with `-EPERM` (caller must call `RESET` first).
- **From IDENTIFIED:** Accepted if repeating identify, or advancing with `FULL_STARTUP`.
- **From UNCALIBRATED:** Repeated `FULL_STARTUP` or identify returns `-EPERM` (already completed prefix).
- **From UNKNOWN:** Rejected with `-EPERM` (fail-closed; caller must issue `RESET` to clear latch).
- **From OFFLINE:** Rejected with `-ENODEV` (permanent).

In all rejected cases, zero SPI transfers occur.

### 4.2 Reset Recovery Semantics

If the device encounters a transport failure (e.g. timeout or CRC error), it enters internal state `FTE3600_STATE_UNKNOWN` (8). In `UNKNOWN`, all capture and init ioctls are rejected with `-EPERM`. The only legal recovery ioctl is `FTE3600_IOC_RESET`, which resets internal software state and issues the hardware reset line.

---

## 5. Verification & Test Evidence

Test Suite 10 Scenario 10 (`test_d6_6_offline_contract_and_reset_rejection`) comprehensively verified:
1. `GET_STATE` returns `state = FTE3600_STATE_UNINITIALIZED` (0) and `flags = FTE3600_FLAG_OFFLINE` (1).
2. `INIT` returns `-ENODEV` with 0 SPI transfers.
3. `CAPTURE` returns `-ENODEV` with 0 SPI transfers.
4. `RESET` returns `-ENODEV` with 0 SPI transfers.
5. Subsequent `GET_STATE` confirms `snap.flags & FTE3600_FLAG_OFFLINE` remains set and state remains 0 (no revival).

This test passed with 100% compliance.
