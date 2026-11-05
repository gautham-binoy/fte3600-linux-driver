# Stage D7.6 — State Machine, Abort & Recovery Semantics Adversarial Audit

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only State Machine & Hardware Recovery Audit  
**Deliverable:** `diagnostics/STAGE_D7_6_STATE_AND_RECOVERY_AUDIT.md`  
**Date:** 2026-11-04  

---

## 1. Executive Summary & Objective

In Stage D7.5, the progression from `INITIALIZED_UNCALIBRATED` to `READY` was governed by a 6-bit kernel progress bitmask and an explicit `CALIB_COMMIT` ioctl.

This adversarial audit scrutinizes the state machine transitions, failure recovery paths, and command semantics to guarantee that:
1. `ABORT` is strictly software-only and never mischaracterized as physical hardware rollback.
2. Incomplete or failed calibrations can never leave the sensor in a counterfeit `READY` state.
3. The 6 progression milestone bits cannot be skipped, spoofed, or replayed out of order.
4. Physical hardware recovery is strictly decoupled into hardware reset (`FTE3600_IOC_RESET`).
5. Emitting `{0xC2, 0x3D, 0x00}` is recognized strictly as an arm command, not physical finger contact.

---

## 2. Distinction: Software Cleanup vs. Physical Hardware Recovery

| Domain | Mechanism | Effect on Hardware | Effect on Kernel Driver |
| :--- | :--- | :--- | :--- |
| **Software Abort (`CALIB_ABORT`)** | Software IOCTL (`0x25`) | **ZERO SPI packets sent**. Sensor registers remain in whatever partially configured state they were left in. | Session marked `active = false; id = 0; progress_mask = 0;`. State demoted to `UNKNOWN`. |
| **Physical Hardware Recovery (`RESET`)** | Hardware IOCTL (`0x01`) | Asserts physical active-low GPIO reset pin for 20 ms. Forces sensor into clean electrical power-on reset (POR). | State set to `RESET` (state 2). Counters and registers purged. |

> [!IMPORTANT]
> The vendor source code contains **zero hardware rollback routines**. Once a register write or strobe is sent over SPI, it cannot be "undone" via software. If calibration fails midway, physical recovery requires an electrical reset pulse (`FTE3600_IOC_RESET`).

---

## 3. The 6-Bit Kernel Progression Bitmask

To prevent userspace from skipping steps or committing partial calibrations, the kernel maintains an internal, immutable bitmask:

```c
#define FTE3600_PROG_PREFIX          BIT(0) /* 0x01: Startup prefix completed */
#define FTE3600_PROG_FDT_COMPLETE     BIT(1) /* 0x02: FDT DAC & stability verified */
#define FTE3600_PROG_FRAME_DISCARD   BIT(2) /* 0x04: Residue frame discarded */
#define FTE3600_PROG_FRAME_BASELINE  BIT(3) /* 0x08: Dark baseline frame captured */
#define FTE3600_PROG_THRESHOLDS_SET  BIT(4) /* 0x10: 8-channel threshold bank written */
#define FTE3600_PROG_ARM_EMITTED     BIT(5) /* 0x20: Arm strobe {0xC2, 0x3D, 0x00} emitted */

#define FTE3600_PROG_FULL_MASK       0x3F   /* Mandatory mask for COMMIT */
```

### 3.1. Strict Sequential Transition Rules:
1. **Bit 0 (`PREFIX`):** Inherited from `FTE3600_IOC_INIT`. If device is not in `INITIALIZED_UNCALIBRATED` (state 14), `CALIB_BEGIN` returns `-EPERM`.
2. **Bit 1 (`FDT_COMPLETE`):** Set only when `CALIB_FDT_SAMPLE` completes with valid stability and convergence. Requires Bit 0.
3. **Bit 2 (`FRAME_DISCARD`):** Set on the first successful call to `CALIB_READ_FRAME`. Requires Bit 1.
4. **Bit 3 (`FRAME_BASELINE`):** Set on the second successful call to `CALIB_READ_FRAME`. Requires Bit 2.
5. **Bit 4 (`THRESHOLDS_SET`):** Set when `CALIB_SET_THRESHOLDS` writes the 16-byte bank (`0x80B0`/`0x80E0`). Requires Bit 3.
6. **Bit 5 (`ARM_EMITTED`):** Set when `CALIB_COMMIT` successfully emits the arm sequence `{0xC2, 0x3D, 0x00}`. Requires Bits 0..4.

---

## 4. Failure Invalidation & Demotion Rules

If any of the following failures occur during an active session:
- SPI bus error or controller timeout
- Status register polling timeout (poll count = 10)
- `copy_to_user` memory fault
- Watchdog timer expiry (5 seconds inactivity)
- Userspace `CALIB_ABORT` invocation
- File descriptor closure (`cdev_release`)

The kernel immediately executes **Destructive Session Teardown**:
```c
session->active = false;
session->id = 0;
session->progress_mask = 0;
session->owner_filp = NULL;
data->state = FTE3600_STATE_UNKNOWN;
data->last_error = <specific_error_code>;
```
- **Immunity from Silent Promotion:** If userspace subsequently calls `CALIB_COMMIT`, the kernel checks `session->progress_mask == 0x3F`. Because the mask is 0, `COMMIT` immediately fails with `-EPERM`. `READY` is **never** assigned.

---

## 5. Arm Strobe Semantics vs. Physical Touch

In `fw9369_release.c:279`, the vendor code transmits `{0xC2, 0x3D, 0x00}`:
- **What it does:** Programs the chip's internal analog comparators to monitor the capacitive matrix and drive the interrupt pin when capacitance exceeds the programmed threshold bank.
- **What it does NOT do:** It does **not** signify that a finger is touching the sensor, nor does it guarantee that the host interrupt controller will successfully latch the GPIO edge.
- **Driver State Meaning:** Setting `data->state = FTE3600_STATE_READY` signifies only that the hardware is **armed and capture-ready**. Actual capture leasing remains gated behind subsequent interrupt or touch-polling events.

---

## 6. Evidence Classification

| Property | Evidence Classification | Proof Reference |
| :--- | :--- | :--- |
| Zero hardware rollback in vendor source | `SOURCE-VERIFIED` | Full codebase search across `vendor/` |
| Physical electrical reset requires GPIO toggle | `SOURCE-VERIFIED` | [`kernel/fte3600.c:134-161`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L134) |
| Monotonic 6-bit progression mask enforcement | `ARCHITECTURAL-SPECIFICATION` | Formally specified in Section 3 |
| Immediate mask destruction on error | `ARCHITECTURAL-SPECIFICATION` | Formally specified in Section 4 |
| Arm strobe `{0xC2, 0x3D, 0x00}` configures analog monitoring | `SOURCE-VERIFIED` | [`fw9369_release.c:279`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_release.c#L279) |
