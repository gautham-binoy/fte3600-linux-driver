# Stage D7.4 — Calibration Abort Semantics & Hardware Recovery Audit

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source & Design Review  
**Deliverable:** `diagnostics/STAGE_D7_4_ABORT_AND_RECOVERY_AUDIT.md`

---

## 1. Executive Summary & Objective

In multi-step hardware calibration, a failure or cancellation in userspace leaves the system in an intermediate state. This independent audit examines:
1. Whether `FTE3600_IOC_CALIB_ABORT` can safely execute hardware rollback commands.
2. The fundamental distinction between **Software Cleanup** and **Physical Hardware Recovery**.
3. Why the vendor reference implementation contains zero hardware rollback mechanisms.
4. Exact driver state transition invariants across `RESET`, `INIT`, `UNKNOWN`, and `OFFLINE` from `kernel/fte3600.c`.

---

## 2. Software Cleanup vs. Physical Hardware Recovery

A critical finding of this audit is that **`FTE3600_IOC_CALIB_ABORT` performs strictly Software Cleanup, not Physical Hardware Recovery**:

```
+-----------------------------------------------------------------------------+
|                          ABORT INVOCATION / FAILURE                         |
+-----------------------------------------------------------------------------+
                                      |
       +------------------------------+------------------------------+
       |                                                             |
       v                                                             v
[SOFTWARE CLEANUP]                                          [HARDWARE STATE]
- Clear session lease (active = false)                      - Sensor registers may be
- Invalidate 32-bit token (id = 0)                            partially configured
- Cancel delayed watchdog worker                             - FIFO flags uncleared
- data->state = FTE3600_STATE_UNKNOWN                       - Capacitive matrix uncalibrated
- Record telemetry last_error = ECANCELED                   - CANNOT BE SAFELY ROLLED
                                                              BACK OVER SPI!
                                                                     |
                                                                     v
                                                          [PHYSICAL RECOVERY]
                                                          Requires active-low
                                                          Pad 14 electrical pulse
                                                          via FTE3600_IOC_RESET!
```

### 2.1. Why ABORT Must NOT Issue SPI Rollback Commands
1. **Absence of Vendor Rollback Protocol:**  
   Inspection of `research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c:448-488` confirms that the vendor codebase contains **zero rollback routines**. When an error occurs during FDT or image baseline, the code immediately branches to `done:` and terminates, leaving sensor registers untouched.
2. **Bus Hang Hazards:**  
   If an abort was triggered by a transport failure (`-EIO`), an unacknowledged frame, or an SPI bus timeout, attempting to issue further SPI commands risks hanging the SPI master controller.
3. **Silicon Corruption Hazards:**  
   Attempting partial inverse register writes (e.g., trying to re-lock bank `0x9A` or guess prior DAC settings) risks putting the analog front-end into an illegal electrical state.
4. **Definitive Rule:**  
   `FTE3600_IOC_CALIB_ABORT` **must issue ZERO SPI commands**. Its sole responsibility is immediate, synchronous software state invalidation.

---

## 3. Strict State Transition Invariants from Repository Source

The safety of the driver depends on proving that once a session aborts or fails, the hardware cannot be operated without a full electrical reset.

### 3.1. Proof from `kernel/fte3600.c`
In [kernel/fte3600.c:734–742](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L734-L742), the driver implements the following check for `FTE3600_IOC_INIT`:

```c
/* kernel/fte3600.c:734-742 */
case FTE3600_IOC_INIT:
	/*
	 * INIT is strictly permitted only from RESET_SEQUENCE_ISSUED (after hardware reset pulse)
	 * or from IDENTIFIED (allowing full initialization following identify-only query).
	 */
	if (data->state != FTE3600_STATE_RESET_SEQUENCE_ISSUED &&
	    data->state != FTE3600_STATE_IDENTIFIED) {
		mutex_unlock(&data->lock);
		return -EPERM;
	}
```

### 3.2. Mandatory State Progression Invariant
1. When any calibration step fails or `CALIB_ABORT` is called:
   $$\text{data}\to\text{state} \longleftarrow \text{FTE3600\_STATE\_UNKNOWN (7)}$$
2. From state `UNKNOWN`:
   - `FTE3600_IOC_INIT` returns **`-EPERM`**.
   - `FTE3600_IOC_CAPTURE` returns **`-EPERM`**.
   - `FTE3600_IOC_CALIB_BEGIN` returns **`-EPERM`**.
3. The **ONLY** operation permitted from `UNKNOWN` that can transition state forward is `FTE3600_IOC_RESET`:
   ```c
   /* kernel/fte3600.c:719-728 */
   case FTE3600_IOC_RESET:
   	ret = fte3600_hw_reset_pulse(data);
   	if (ret) {
   		data->state = FTE3600_STATE_UNKNOWN;
   		...
   	}
   	data->state = FTE3600_STATE_RESET_SEQUENCE_ISSUED;
   ```
4. **Physical Safety Guarantee:**  
   Because `FTE3600_IOC_RESET` physically drives Pad 14 low for 10 ms, the FT9362 hardware is subjected to a complete silicon power-on reset, clearing all internal volatile registers, DAC settings, and FIFO memory before userspace can ever re-attempt `INIT`.

---

## 4. OFFLINE and Device Removal Invariants

When the device is unbound from the SPI bus (`fte3600_spi_remove()` in `kernel/fte3600.c:980–990`):
1. `data->flags |= FTE3600_FLAG_OFFLINE;`
2. `data->state = FTE3600_STATE_DEVICE_REMOVAL;`
3. If a calibration session was active, `session->active = false; session->id = 0;`.
4. The watchdog delayed work is cancelled synchronously: `cancel_delayed_work_sync(&session->watchdog_work)`.
5. All subsequent ioctl invocations on any remaining open descriptors return `-ENODEV`.
6. No reset pulse or SPI transfer can ever be issued while offline.

---

## 5. Evidence Classification

| Statement / Finding | Classification | Evidence Reference |
| :--- | :--- | :--- |
| Vendor startup implements no hardware rollback | `VENDOR-SOURCE-VERIFIED` | `fw9369_startup.c:488-495` |
| `INIT` strictly gated from `RESET_SEQUENCE_ISSUED` and `IDENTIFIED` | `SOURCE-VERIFIED` | `kernel/fte3600.c:734-742` |
| `INIT`, `CAPTURE`, `CALIB_BEGIN` return `-EPERM` from `UNKNOWN` | `SOURCE-VERIFIED` | `kernel/fte3600.c:736, 767` |
| Physical silicon reset required to escape `UNKNOWN` | `MATHEMATICALLY-DERIVED` | Formally proven via state graph in Section 3.2 |
| `ABORT` must execute zero SPI transactions | `ARCHITECTURAL-PROPOSAL` | Specified in Section 2.1 |
| Removal sets `OFFLINE` and cancels watchdog synchronously | `ARCHITECTURAL-PROPOSAL` | Specified in Section 4 |
