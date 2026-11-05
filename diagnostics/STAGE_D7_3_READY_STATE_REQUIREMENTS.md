# Stage D7.3 — READY State Requirements & Gating Specifications

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source Architecture Review (Zero Hardware Operations)  
**Deliverable:** `diagnostics/STAGE_D7_3_READY_STATE_REQUIREMENTS.md`  

---

## 1. Executive Summary

This report establishes the definitive technical specifications and strict prerequisites governing entry into the operational state **`FTE3600_STATE_READY`** (4).

Under the safety architecture established in Stage D6.4 and hardened in Stage D7.1, **`READY` must never be asserted prematurely**. Prefix completion alone merely transitions the device to `INITIALIZED_UNCALIBRATED` (14), where capture is strictly rejected. This design ensures that `READY` is reachable exclusively through an atomic commit that validates every analog, optical, and electrical calibration prerequisite.

---

## 2. Mandatory Pre-Conditions for READY State Entry

To transition `data->state` to `FTE3600_STATE_READY` (4), the kernel driver must verify that all ten of the following milestones have succeeded:

```
  ┌────────────────────────────────────────────────────────────────────────┐
  │ MANDATORY MILESTONES FOR READY ENTRY:                                  │
  │                                                                        │
  │ 1. Hardware Identification & Volatile Prefix Complete                  │
  │    - Pad 14 deasserted high, Chip ID 0x9362 verified, Fab ID read.     │
  │    - Internal state == FTE3600_STATE_INITIALIZED_UNCALIBRATED (14).    │
  │                                                                        │
  │ 2. FDT Analog Bias DAC Converged                                       │
  │    - Successive approximation DAC search converged (1 <= DAC <= 125).  │
  │    - Analog bias programmed into register 0x1801.                      │
  │                                                                        │
  │ 3. 4-Channel Baseline Minima Validated                                 │
  │    - 10 stability samples measured across 4 physical ADC channels.      │
  │    - All 4 channel minima strictly satisfy: 300 <= minima[c] <= 700.   │
  │                                                                        │
  │ 4. Touch Thresholds Calculated & Bounds-Checked                        │
  │    - Threshold formula applied: value = minima[c] - 30.                │
  │    - Bounds verification: 270 <= threshold[c] <= 670.                  │
  │                                                                        │
  │ 5. Threshold SRAM Bank Written & Locked                                │
  │    - Unlocked via 0x9A = 0x5A, 8 words written to 0x80B0, relocked.    │
  │                                                                        │
  │ 6. Image Bias DAC Search Converged                                     │
  │    - Multi-frame median search converged to 512 (0 <= image_dac <= 127)│
  │                                                                        │
  │ 7. Dark Frame Reference Matrix Cached                                  │
  │    - Second acquired image frame retained as baseline dark frame.      │
  │                                                                        │
  │ 8. Linux Interrupt Event Routing Configured                            │
  │    - Interrupt mask register 0x1A83 programmed with 0x0016.            │
  │                                                                        │
  │ 9. Initial Interrupt Status Cleared                                    │
  │    - Acknowledged via write 0x1A84 = 0x002F.                           │
  │                                                                        │
  │ 10. Autonomous Touch Comparator Strobe Emitted                         │
  │     - Mode set to Down Arm (0x1881 |= 0x81).                           │
  │     - Command strobe {0xC2, 0x3D, 0x00} successfully transmitted.      │
  └────────────────────────────────────────────────────────────────────────┘
```

If **any single condition** is unmet or returns an error, the transition to `READY` is denied, the calibration session is aborted, and the device enters `FTE3600_STATE_UNKNOWN` (7).

---

## 3. Strict Gating Invariants

### 3.1 Gating Capture Against Incomplete Calibration
In `kernel/fte3600.c:786–789`:
```c
if (data->state != FTE3600_STATE_READY) {
    mutex_unlock(&data->lock);
    return -EPERM;
}
```
- In all non-READY states (`UNINITIALIZED`, `RESET_SEQUENCE_ISSUED`, `IDENTIFIED`, `INITIALIZING`, `INITIALIZED_UNCALIBRATED`, `UNKNOWN`, `ERROR`), `FTE3600_IOC_CAPTURE` returns **`-EPERM`**.
- This guarantee is preserved unchanged in the proposed architecture.

### 3.2 Preventing Touch Monitoring Before Valid Thresholds
- Issuing the arm strobe `{0xC2, 0x3D}` while threshold registers contain uncalibrated all-zero values triggers massive false-positive interrupt storms that saturate the host CPU.
- The kernel's `FTE3600_IOC_CALIB_COMMIT` handler enforces:
  ```c
  if (!(session->completed_phases & FTE3600_CALIB_PHASE_THRESHOLDS_SET)) {
      dev_err(&data->spi->dev, "Cannot arm: threshold bank unprogrammed\n");
      return -EPERM;
  }
  ```
- Command strobe `{0xC2, 0x3D}` is never issued unless verified thresholds have been successfully written to SRAM bank `0x80B0`.

### 3.3 Preventing READY After Incomplete or Abandoned Sessions
- `FTE3600_IOC_GET_STATE`, `FTE3600_IOC_RESET`, and `FTE3600_IOC_INIT` are structurally incapable of setting `data->state = FTE3600_STATE_READY`.
- The only function permitted to assign `READY` is `fte3600_calib_commit_locked()`.
- If a session is aborted, times out, or fails:
  - `session->active` is cleared.
  - `data->state` is set to `FTE3600_STATE_UNKNOWN` (7).
  - Recovery requires an explicit hardware reset pulse (`FTE3600_IOC_RESET`).

### 3.4 Preventing READY After Device Removal
- In `fte3600_remove()`, the driver executes under `data->lock`:
  ```c
  atomic_set(&data->offline, 1);
  data->state = FTE3600_STATE_DEVICE_REMOVAL;
  data->session.active = false;
  ```
- Once `offline` is asserted, any in-flight or subsequent `COMMIT` ioctl returns `-ENODEV` without touching hardware.

---

## 4. Preservation of Current Capture Stub Behavior

In this design stage (Stage D7.3), the existing character device capture stub in `kernel/fte3600.c:790–794` remains strictly preserved:
```c
/* Physical FIFO capture deferred in Stage 2E.2 software milestone */
mutex_unlock(&data->lock);
return -EOPNOTSUPP;
```
Once the sensor reaches `READY`, calling `FTE3600_IOC_CAPTURE` returns `-EOPNOTSUPP` (Operation not supported). Physical image readout into userspace buffers will be implemented only in a separately authorized future stage.
