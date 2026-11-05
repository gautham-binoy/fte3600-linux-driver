# Stage D7 — Kernel Integration Gap Analysis & Architecture Review

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Investigation Mode:** Read-Only Static Source Analysis (Zero Physical Hardware Communication)  

---

## 1. Executive Summary

This report compares the vendor calibration and baseline architecture against the current Linux kernel driver implementation (`kernel/fte3600.c`), identifies architectural integration gaps, and evaluates structural options for transitioning from `INITIALIZED_UNCALIBRATED` (state 14) to `READY` (state 4) while strictly maintaining all existing Stage D6.6 UAPI, ABI, and safety contracts.

A fundamental design divergence exists between the vendor reference implementation and our kernel driver:
- In the vendor ecosystem, the Linux kernel driver (`focal_spi.c`) is merely a **thin pass-through transport module** exposing raw character-device SPI read/write and GPIO reset ioctls. The complex calibration algorithms (binary DAC search, 10-sample FDT averaging, image median calculation, and baseline frame buffering) were implemented entirely in userspace (`libfprint-fte3600`).
- In our project architecture, `fte3600.ko` is an autonomous, fail-closed state-machine driver managing device lifecycle, hardware safety gates, and serialized transport.

This report analyzes how this algorithmic complexity can be resolved cleanly without compromising kernel safety or ABI stability.

---

## 2. Status of Stage D6.6 Contracts & Invariants

Any future calibration implementation must preserve all established Stage D6.6 contracts:

| Contract / Invariant | Current Stage D6.6 Baseline | Compatibility Requirement for Future Calibration |
|---|---|---|
| **Public UAPI States** | Strictly bounded to `[0..8]` | Maintained; internal calibration milestones must map into `[0..8]` |
| **Internal State 14** | `INITIALIZED_UNCALIBRATED` | Retained as the post-prefix, pre-calibration state |
| **UAPI Telemetry Flags** | `STARTUP_PREFIX_COMPLETED` (`0x04`), `CALIBRATION_PENDING` (`0x08`) | Retained; `CALIBRATION_PENDING` cleared only upon full calibration |
| **Capture Gating** | `data->state != FTE3600_STATE_READY -> -EPERM` | Retained; capture strictly gated until operational `READY` |
| **Three-Tier Capture Errno** | `-ENODEV` (offline) -> `-EPERM` (non-READY) -> `-EOPNOTSUPP` (feature stub) | Retained without alteration |
| **INIT Flag Validation** | Flags outside `FTE3600_INIT_FLAGS_MASK` reject with `-EINVAL` | Retained; future flags (e.g. `INIT_FLAG_CALIBRATE`) must expand mask cleanly |
| **OFFLINE Contract** | Unreversible; `RESET` / `INIT` return `-ENODEV` | Retained; offline devices cannot be calibrated or revived |
| **3-Byte Command Strobe** | `{cmd, ~cmd, 0x00}` wire framing | Retained for all command strobes (`0x5A/0xA5`, `0xC2/0x3D`, `0xC4/0x3B`) |
| **Interrupt W1TC Logic** | Targeted bit clear (`0x1A84 = 0x0020`, `0x0008`) or read-then-echo | Enforced; avoid indiscriminate `0xFFFF` writes during runtime |

---

## 3. Kernel Integration Gaps

To advance from `INITIALIZED_UNCALIBRATED` (14) to `READY` (4), three distinct capability gaps must be resolved:

```
  Current Driver Baseline:
  [RESET] ──► [IDENTIFIED] ──► [INITIALIZED_UNCALIBRATED] (State 14)
                                         │
                   ══════════════════════╪══════════════════════
                               GAP 1: FDT Baseline
                               - Binary search on DAC 0x1801
                               - 10-sample FDT averaging
                               - Channel minima [300, 700] validation
                               - Threshold calculation (minima - 30)
                   ══════════════════════╪══════════════════════
                               GAP 2: Image Baseline
                               - Multi-capture DAC binary search
                               - Median intensity computation
                               - 10,246-byte frame buffer capture
                               - Dark background matrix storage
                   ══════════════════════╪══════════════════════
                               GAP 3: Sensor Arming
                               - Threshold bank programming (0x9A unlock)
                               - FDT comparator mode setup (0x1881)
                               - Event routing & targeted W1TC ACK
                               - Command strobe 0xC2 0x3D
                   ══════════════════════╪══════════════════════
                                         ▼
                               [STATE_READY] (State 4)
```

### Gap 1: Algorithmic Search in Kernel Space
- `fdt_baseline()` requires a 10-step successive approximation binary search over SPI to find the DAC value where the average of 4 channels crosses 512.
- While computationally trivial (integer addition and division by 4), it requires sequential SPI transactions interleaved with 1 ms settling delays and polling loops against register `0x1A82`.

### Gap 2: Memory & Image Processing Overhead
- `image_baseline()` requires capturing physical 10,246-byte image frames from FIFO `0x9A05`.
- In-kernel median calculation over 5,120 16-bit pixels requires either a 1,024-bin histogram array (4 KB RAM) or pixel sorting.
- Retaining the dark reference matrix requires allocating a persistent 10,240-byte buffer (`uint16_t baseline[5120]`) within `struct fte3600_data`.

### Gap 3: Interrupt Service Routine (ISR) Architecture
- Operational sensing relies on physical interrupt line Pad 229 (Intel LPSS GPIO IRQ).
- When armed, the sensor asserts Pad 229 on finger contact.
- Currently, `fte3600.ko` claims the IRQ in probe but has no threaded interrupt handler or poll wakeup integration tied to the event status register `0x1A82`.

---

## 4. Architectural Implementation Strategies

Two architectural pathways exist for bridging these gaps in future development stages:

### Option A: Fully Autonomous In-Kernel Calibration Engine
In this model, the kernel driver implements `fdt_baseline()`, `image_baseline()`, and `arm()` internally within kernel space:
- **Mechanism:** Triggered via a new initialization flag: `FTE3600_INIT_FLAG_CALIBRATE` (bit 1).
- **Advantages:**
  - Completely self-contained kernel module.
  - Userspace tools remain simple (issuing `ioctl(fd, FTE3600_IOC_INIT, &params)` with calibration flag).
  - Eliminates context-switching overhead across multiple SPI transfers during binary search.
- **Challenges:**
  - Requires embedding 10 KB baseline storage and histogram median calculation inside kernel space.
  - Kernel execution time for `INIT` extends by ~100–300 ms during calibration search loops.

### Option B: Split Kernel/Userspace Calibration (Hybrid Model)
In this model, the kernel driver handles SPI transport, hardware safety gates, and state tracking, while userspace (`libfprint` or a privileged helper utility) performs the statistical search algorithms:
- **Mechanism:**
  1. Kernel provides low-level primitive ioctls: `FTE3600_IOC_FDT_SCAN` and `FTE3600_IOC_ACQUIRE_RAW_FRAME`.
  2. Userspace tool executes the DAC search loops and calculates thresholds and baseline matrices.
  3. Userspace uploads the calculated parameters to the kernel via `FTE3600_IOC_SET_CALIBRATION(struct fte3600_calib_data)`.
  4. Kernel validates bounds (e.g. thresholds in `[300, 700]`), writes the threshold bank to hardware, executes `arm()`, and transitions to `FTE3600_STATE_READY`.
- **Advantages:**
  - Adheres strictly to the Unix philosophy: complex data processing remains in userspace, hardware control remains in the kernel.
  - Matches the vendor reference architecture (`focal_spi` + `libfprint`).
  - Zero large memory footprint in kernel space.
- **Challenges:**
  - Requires defining a new UAPI ioctl payload (`struct fte3600_calib_data`).

---

## 5. Architectural Recommendations for Next Stages

1. **Retain Current State Machine Isolation:**  
   The decision in Stage D6.4 and D6.6 to stop at `INITIALIZED_UNCALIBRATED` is fully vindicated. Operational readiness cannot exist without calibrated baselines.
2. **Evaluate Option A vs Option B in Design Review:**  
   Stage D8 should formulate the detailed implementation design, selecting between the autonomous in-kernel engine (Option A) and the hybrid userspace-assisted model (Option B).
3. **Preserve Existing ABI:**  
   Any future calibration parameters must fit cleanly into the fixed 32-byte UAPI structures or use a dedicated, backward-compatible ioctl number (`0x05`).
