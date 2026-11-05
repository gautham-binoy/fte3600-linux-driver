# Stage D8 — State Machine Specification: 14-State Internal Model and Public UAPI Mapping

**Target Device:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Status:** FROZEN ARCHITECTURAL SPECIFICATION FOR STAGE D8  
**Safety Gate:** STRICTLY READ-ONLY DESIGN — ZERO HARDWARE ACCESS AUTHORIZED

---

## 1. Executive Summary

The `fte3600` driver employs a **dual-layer state model**:
1. **Public UAPI States (0 through 8):** Stable, backward-compatible states exposed to userspace telemetry and diagnostic tools via `FTE3600_IOC_GET_STATE`.
2. **Internal Extended States (9 through 14):** Fine-grained kernel synchronization states governing interrupt-driven touch detection, frame buffer preparation, power management, device unbind, and partial calibration.

This document formally specifies every state, the exact public mapping, and a comprehensive transition matrix defining triggers, preconditions, hardware bus actions, failure branches, and rollback semantics.

---

## 2. Complete 14-State Taxonomy and Public Mappings

```mermaid
graph TD
    subgraph Public UAPI Representation [Public UAPI Space (0..8)]
        P0[0: UNINITIALIZED]
        P1[1: RESETTING]
        P2[2: RESET_SEQUENCE_ISSUED]
        P3[3: INITIALIZING]
        P4[4: READY]
        P5[5: CAPTURING]
        P6[6: ERROR]
        P7[7: UNKNOWN]
        P8[8: IDENTIFIED]
    end

    subgraph Internal Driver States [Internal Extended Kernel Space (9..14)]
        I9[9: WAITING_FOR_TOUCH] -->|Maps to| P4
        I10[10: TOUCH_DETECTED] -->|Maps to| P4
        I11[11: FRAME_READY] -->|Maps to| P4
        I12[12: SUSPENDED] -->|Maps to| P7
        I13[13: DEVICE_REMOVAL] -->|Maps to| P0
        I14[14: INITIALIZED_UNCALIBRATED] -->|Maps to| P8
    end

    style P4 fill:#e8f5e9,stroke:#388e3c,stroke-width:2px
    style P8 fill:#e1f5fe,stroke:#0288d1,stroke-width:2px
    style P7 fill:#ffebee,stroke:#c62828,stroke-width:2px
```

| State Identifier | Enum Value | Scope | Public Mapping | Semantic Definition |
| :--- | :--- | :--- | :--- | :--- |
| `FTE3600_STATE_UNINITIALIZED` | 0 | Public | 0 (`UNINITIALIZED`) | Driver loaded, hardware un-reset, zero communications issued. |
| `FTE3600_STATE_RESETTING` | 1 | Public | 1 (`RESETTING`) | GPIO reset pulse sequence actively executing on Pad 14. |
| `FTE3600_STATE_RESET_SEQUENCE_ISSUED` | 2 | Public | 2 (`RESET_SEQUENCE_ISSUED`)| GPIO pulse completed on host. Silicon responsiveness unverified. |
| `FTE3600_STATE_INITIALIZING` | 3 | Public | 3 (`INITIALIZING`) | Active multi-step initialization or calibration sequence running. |
| `FTE3600_STATE_READY` | 4 | Public | 4 (`READY`) | **CANONICAL READY:** Full calibration complete; armed for touch. |
| `FTE3600_STATE_CAPTURING` | 5 | Public | 5 (`CAPTURING`) | SPI FIFO read active (transferring 10,246 bytes). |
| `FTE3600_STATE_ERROR` | 6 | Public | 6 (`ERROR`) | Recoverable transport or protocol error encountered. |
| `FTE3600_STATE_UNKNOWN` | 7 | Public | 7 (`UNKNOWN`) | **FAIL-CLOSED:** Transport failure, timeout, or ambiguous hardware state. |
| `FTE3600_STATE_IDENTIFIED` | 8 | Public | 8 (`IDENTIFIED`) | Chip ID (`0x9362`) and CRC (`0x1C53`) verified over SPI wire. |
| `FTE3600_STATE_WAITING_FOR_TOUCH` | 9 | Internal | 4 (`READY`) | Sensor armed; kernel waiting on GPIO Pad 229 interrupt. |
| `FTE3600_STATE_TOUCH_DETECTED` | 10 | Internal | 4 (`READY`) | GPIO Pad 229 asserted; finger touch confirmed by AFE. |
| `FTE3600_STATE_FRAME_READY` | 11 | Internal | 4 (`READY`) | Full frame in kernel transient buffer awaiting userspace copy. |
| `FTE3600_STATE_SUSPENDED` | 12 | Internal | 7 (`UNKNOWN`) | Host entering ACPI S3/S0ix; sensor clocks gated. |
| `FTE3600_STATE_DEVICE_REMOVAL` | 13 | Internal | 0 (`UNINITIALIZED`) | Driver unbinding; peripheral going offline (`FLAG_OFFLINE`). |
| `FTE3600_STATE_INITIALIZED_UNCALIBRATED`| 14 | Internal | 8 (`IDENTIFIED`) | Startup prefix completed, but FDT calibration not yet performed. |

---

## 3. Comprehensive State Transition Matrix

The table below governs all valid state transitions in the driver. Any transition not explicitly listed is **STRICTLY PROHIBITED** and must trigger `-EINVAL` or fail closed to `UNKNOWN`.

```
Abbreviations:
- Sub: Submitter (ioctl process context)
- IRQ: Interrupt Service Routine / Tasklet
- WQ: Kernel Workqueue
```

| Source State | Target State | Trigger / Operation | Preconditions | Hardware Bus / GPIO Activity | Failure Branch & Result | Recovery Behavior |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `UNINITIALIZED` | `RESETTING` | `FTE3600_IOC_RESET` | Not offline; `data->lock` held | Pre-settle 10ms, Assert Pad 14 low 20ms, Deassert 10ms | `-ENODEV` if GPIO absent | Remains `UNINITIALIZED` |
| `RESETTING` | `RESET_SEQUENCE_ISSUED` | Reset pulse completion | Electrical timing elapsed | GPIO writes completed to Intel PCH LPSS | None (void return) | Moves to `RESET_SEQUENCE_ISSUED` |
| `RESET_SEQUENCE_ISSUED` | `INITIALIZING` | `FTE3600_IOC_INIT` | `is_open == 1`; not offline | Beginning SPI sequence | `-ENODEV` if SPI invalid | Transitions to `UNKNOWN` |
| `INITIALIZING` | `IDENTIFIED` | Legacy Identify (`flags=0`)| `RESET_SEQUENCE_ISSUED` | Write `0xC6=1`, readback `0xC6`, read `0x1A8B` (expect `0x9362`, `0x1C53`) | Mismatch $\to$ `UNKNOWN`, returns `-EIO` | Requires new `RESET` sequence |
| `INITIALIZING` | `INITIALIZED_UNCALIBRATED`| Full Startup Prefix | `RESET_SEQUENCE_ISSUED` | Wake `5A A5 00`, Poll `0x80 == 0x50`, Lock `A5 5A 00` | Timeout/Err $\to$ `UNKNOWN`, returns `-EIO` | Host-side invalidation; reboot |
| `IDENTIFIED` / `INITIALIZED_UNCALIBRATED`| `INITIALIZING` | `SESSION_BEGIN` | Valid open fd; no active lease | Zero SPI traffic (allocates session token) | `-EBUSY` if active session | User retries or aborts |
| `INITIALIZING` | `INITIALIZING` | `SESSION_FDT_SAMPLE` | Session active; token matches | DAC sweep on `0x1801`; 10 samples read; CRC verified | Out of range $\to$ `ERROR`, returns `-EAGAIN`| Adjust DAC bounds or retry |
| `INITIALIZING` | `INITIALIZING` | `SESSION_READ_FRAME` | Session active; FDT complete | Burst FIFO read: 10,246 bytes (Prefix + 10,240 B payload) | Short read $\to$ `UNKNOWN`, returns `-EIO` | Aborts session fail-closed |
| `INITIALIZING` | `INITIALIZING` | `SESSION_SET_THRESHOLDS`| Session active; Frame read | Write 22-byte SRAM bank at `0x80E0` | Transport err $\to$ `UNKNOWN` | Aborts session |
| `INITIALIZING` | `READY` | `SESSION_COMMIT` | All 4 milestones completed | Transmit Arm strobe `{0xC2, 0x3D, 0x00}` | Error $\to$ `UNKNOWN`, returns `-EIO` | Aborts session |
| `READY` | `WAITING_FOR_TOUCH` | Background IRQ arm | Driver armed; poll() called | Enables GPIO Pad 229 falling edge IRQ | `-EINVAL` if IRQ invalid | Remains `READY` |
| `WAITING_FOR_TOUCH` | `TOUCH_DETECTED` | IRQ Triggered (Hardirq) | Sensor asserts Pad 229 low | Silicon touch detected by AFE | False trigger $\to$ re-arm | Hardware re-arms |
| `TOUCH_DETECTED` | `CAPTURING` | Frame capture start | Submitter invokes capture | SPI FIFO burst read (10,246 bytes) | SPI hung $\to$ `UNKNOWN` (Quarantine)| Buffer retained $\mathtt{FREED} \equiv 0$ |
| `CAPTURING` | `FRAME_READY` | Capture complete | Full 10,246 bytes received | Copies payload to userspace via `copy_to_user` | Copy fail $\to$ returns `-EFAULT` | Frame discarded; returns to `READY` |
| `FRAME_READY` | `READY` | Delivery acknowledged | Userspace read returned | Zero bus traffic | None | Ready for next touch |
| *ANY STATE* | `DEVICE_REMOVAL` | Sysfs unbind / PCI remove | Driver core unbind event | Cancels watchdog outside lock; marks offline | None | Terminal state until re-probe |
| *ANY ACTIVE* | `UNKNOWN` | Timeout (250 ms) / Bus Hung | Hardware does not reply | Halts software wait; preserves DMA buffers | Returns `-ETIMEDOUT` | **Permanent Quarantine Pin** |

---

## 4. The Canonical Readiness Invariant

### 4.1 Strict Readiness Gate

$$\boxed{\mathtt{State} = \mathtt{READY} \iff (\mathtt{Milestone}_1 \land \mathtt{Milestone}_2 \land \mathtt{Milestone}_3 \land \mathtt{Milestone}_4 \land \mathtt{Milestone}_5)}$$

The five mandatory milestones that **MUST ALL BE COMPLETED** before `data->state` can transition to `FTE3600_STATE_READY` are:
1. **$\mathtt{Milestone}_1$ (Identification):** Chip ID `0x9362` and CRC `0x1C53` verified.
2. **$\mathtt{Milestone}_2$ (MCU Idle Handshake):** Wakeup dialog `5A A5 00` completed and status `0x80 == 0x50` verified.
3. **$\mathtt{Milestone}_3$ (FDT Calibration):** 10-sample stability window passed; minima within $[300, 700]$; CRC valid.
4. **$\mathtt{Milestone}_4$ (Baseline Frame):** Cold image frame acquired and validated; median pixel count verified.
5. **$\mathtt{Milestone}_5$ (Thresholds & Arm):** 22-byte threshold bank written; Arm strobe `{0xC2, 0x3D, 0x00}` dispatched.

**Rule:** Completing the startup prefix alone sets `FTE3600_STATE_INITIALIZED_UNCALIBRATED` (public `IDENTIFIED`). It **NEVER sets `READY`**.

---

## 5. Software Abort vs. Hardware Rollback

- **Software Abort:** When `SESSION_ABORT` or watchdog expiry occurs, the kernel invalidates the userspace session handle (`data->session_valid = 0`), unlocks the mutex, and returns the state machine to `INITIALIZED_UNCALIBRATED` or `UNKNOWN`.
- **Hardware Reality:** The physical sensor silicon **remains in its last programmed electrical state**. Software invalidation does **NOT** roll back hardware registers. To prevent undefined MCU states, subsequent admissions require a full cycle starting from `RESET`.

---

## 6. Conclusion and Stage D8 Freeze Sign-Off

The 14-state internal architecture and public UAPI mapping are mathematically closed, fail-closed, and frozen for Stage D8 implementation.
