# Stage D7.3 — Architectural Comparison: In-Kernel vs Hybrid Calibration

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source Architecture Review (Zero Hardware Operations)  
**Deliverable:** `diagnostics/STAGE_D7_3_HYBRID_ARCHITECTURE_COMPARISON.md`  

---

## 1. Executive Summary

This report performs a comprehensive comparative analysis of three competing architectures for executing the complete FT9362 startup sequence:
$$\text{prefix}() \longrightarrow \text{fdt\_baseline}() \longrightarrow \text{image\_baseline}() \longrightarrow \text{arm}()$$

The candidate architectures evaluated are:
- **Design A (Monolithic In-Kernel Engine):** Single monolithic ioctl (`FTE3600_IOC_CALIBRATE`) executing the entire 1.15-second multi-frame calibration loop in kernel space under `data->lock`.
- **Design B (Stateless Bounded IOCTLs):** Userspace helper orchestrates primitive, bounded ioctls without explicit kernel session state (releasing `data->lock` between steps).
- **Design C (Session-Gated Hybrid Architecture):** Kernel-managed transactional calibration session with explicit `BEGIN`, bounded `STEP`, and `COMMIT`/`ABORT` operations, orchestrated by userspace.

**Definitive Recommendation:** **Design C (Session-Gated Hybrid Architecture)** is recommended. It eliminates long-held kernel mutexes, minimizes kernel RAM, cleanly separates policy from mechanism, and guarantees session exclusivity and fail-closed crash cleanup.

---

## 2. Comprehensive Comparative Matrix

| Evaluation Criterion | Design A (Monolithic In-Kernel) | Design B (Stateless Bounded IOCTLs) | Design C (Session-Gated Hybrid) |
|---|---|---|---|
| **SPI Exclusivity** | Continuous throughout entire calibration run. | Guaranteed only per-ioctl; vulnerable to interleaving between calls. | Guaranteed per-ioctl, with overall session strictly gated by session token & owner fd. |
| **Mutex Hold Duration** | **Unacceptable:** Mutex held for **350 ms to ~1.15 seconds**. | Bounded: Max ~85 ms per frame readout. | **Optimal:** Bounded: Max ~85 ms per frame readout. |
| **Concurrent IOCTL Impact** | Blocks desktop daemons (`fprintd`, `GET_STATE`) for >1 s. | Concurrent ioctls can interleave between steps and corrupt state. | `GET_STATE` returns immediately; conflicting calls rejected with `-EBUSY`. |
| **Process Interruption / Crash** | Kernel executes to completion even if userspace dies. | Leaves sensor in half-configured state with no owner to clean up. | `fte3600_cdev_release()` detects active session, aborts hardware, sets `UNKNOWN`. |
| **Device Removal Handling** | Long loop must poll `offline` on every iteration. | Simple: Next ioctl detects `offline` and returns `-ENODEV`. | Clean: Next ioctl detects `offline` and immediately terminates session. |
| **Kernel Memory Footprint** | Adds ~12–16 KB static RAM (dark frame cache + histogram). | Zero added kernel buffer memory. | Zero added kernel buffer memory (buffers live in userspace). |
| **Kernel Complexity** | High: Math algorithms, sorting, and binary search in kernel. | Low: Primitives only, but lacks safety enforcement. | Medium: State-enforcement session logic; zero image processing math. |
| **Rollback Capability** | Built-in abort logic in kernel loop. | Impossible without session tracking. | Explicit `ABORT` transition resets sensor or forces state `UNKNOWN`. |
| **Linux Philosophy** | Violates "Mechanism, not Policy" principle. | Adheres to Unix model, but violates kernel safety gating. | **Strict adherence:** Kernel owns mechanism & state; userspace owns policy. |
| **Testability** | Difficult: Requires simulated kernel environment. | High: Userspace unit tests can mock individual steps. | **Highest:** Session steps testable independently in userspace harness. |

---

## 3. Deep Dive: Candidate Architectural Designs

### 3.1 Design A: Monolithic In-Kernel Engine

```
  ┌────────────────────────────────────────────────────────┐
  │ Userspace: ioctl(fd, FTE3600_IOC_CALIBRATE)            │
  └────────────────────────┬───────────────────────────────┘
                           │ (Single blocking ioctl)
                           ▼
  ┌────────────────────────────────────────────────────────┐
  │ Kernel (fte3600.ko):                                   │
  │   mutex_lock(&data->lock);                             │
  │   - Allocates 10 KB dark frame buffer in GFP_KERNEL    │
  │   - Loop 1: FDT DAC binary search (10 iterations)      │
  │   - Loop 2: Image DAC binary search (4–13 frames)      │
  │   - Computes 5,120-pixel median & histogram in kernel  │
  │   - Writes threshold bank (0x80B0, 0x9A=0x5A)          │
  │   - Issues arm strobe (0xC2 0x3D)                      │
  │   - Sets data->state = READY                           │
  │   mutex_unlock(&data->lock);                           │
  └────────────────────────────────────────────────────────┘
```

#### Fatal Defects of Design A:
1. **Unacceptable Latency & Mutex Contention:**
   As mathematically established in Stage D7.2, streaming up to 13 frames across a 1 MHz SPI bus with hardware handshakes requires **~1.15 seconds of wall-clock time**. Holding `data->lock` continuously for 1.15 seconds stalls all desktop monitoring calls (`FTE3600_IOC_GET_STATE`), pauses device close operations, and triggers kernel lockup watchdog warnings if preempted.
2. **Kernel Memory Bloat:**
   Retaining the dark reference matrix requires allocating a persistent 10,240-byte buffer (`uint16_t[5120]`) plus temporary search buffers and a 1,024-bin histogram array in kernel space, adding ~16 KB of pinned kernel RAM.
3. **Violation of Linux Architecture:**
   Implementing median sorting algorithms, histogram analysis, and DAC convergence heuristics directly inside the kernel violates the Linux architectural boundary: *the kernel provides mechanisms, not policy*.

---

### 3.2 Design B: Stateless Bounded IOCTLs

```
  ┌────────────────────────────────────────────────────────┐
  │ Userspace Helper:                                      │
  │   ioctl(fd, FTE3600_IOC_FDT_SAMPLE, &dac);             │
  │   [Runs binary search algorithm in userspace]          │
  │   ioctl(fd, FTE3600_IOC_READ_RAW_FRAME, &buf);         │
  │   [Calculates 5,120-pixel median in userspace]         │
  │   ioctl(fd, FTE3600_IOC_SET_THRESHOLDS, &th);          │
  │   ioctl(fd, FTE3600_IOC_ARM);                          │
  └────────────────────────────────────────────────────────┘
```

#### Fatal Defects of Design B:
1. **The Interleaving Hazard:**
   Simply releasing `data->lock` between individual ioctl calls is fundamentally unsafe without a session management layer. While the driver currently enforces a single-client open lease (`atomic_cmpxchg(&data->is_open)`), multi-threaded userspace applications or subprocesses sharing the file descriptor could issue `FTE3600_IOC_RESET` or query state concurrently, corrupting the sensor's physical state midway through DAC adjustment.
2. **Crash & Abandonment Vulnerability:**
   If the userspace helper crashes or is terminated by `SIGKILL` after adjusting the analog bias DAC (`0x1801`) but before programming the threshold bank, the sensor is left in an electrically modified, uncalibrated state. The stateless kernel has no awareness that a calibration sequence was abandoned.
3. **Premature READY Risk:**
   Without tracking that all intermediate stages were executed in the required sequence, a rogue userspace process could skip image baseline acquisition and directly invoke `FTE3600_IOC_ARM`, marking the sensor READY with all-zero thresholds.

---

### 3.3 Design C: Session-Gated Hybrid Architecture (Recommended)

```
  ┌──────────────────────────────────────────────────────────────────┐
  │ Userspace Helper:                                                │
  │ 1. ioctl(FTE3600_IOC_CALIB_BEGIN, &session_params)               │
  │    --> Kernel verifies state, generates session_id, sets timeout │
  │ 2. ioctl(FTE3600_IOC_CALIB_FDT_SAMPLE, &step_fdt)                │
  │    --> Kernel verifies session_id, scans 4 ADC channels (~1 ms)  │
  │ 3. ioctl(FTE3600_IOC_CALIB_READ_FRAME, &step_frame)              │
  │    --> Kernel verifies session_id, streams 1 frame (~86 ms)      │
  │ 4. ioctl(FTE3600_IOC_CALIB_SET_THRESHOLDS, &thresholds)         │
  │    --> Kernel validates [300, 700], writes 0x80B0 bank           │
  │ 5. ioctl(FTE3600_IOC_CALIB_COMMIT, &commit_params)               │
  │    --> Kernel verifies all phases complete, arms, sets READY     │
  └──────────────────────────────────────────────────────────────────┘
```

#### Core Architectural Virtues of Design C:
1. **Bounded Kernel Mutex Hold Duration:**
   `data->lock` is held strictly for the duration of a single hardware transaction (max ~86 ms for a 10,246-byte frame readout). Between steps, the mutex is unlocked, allowing concurrent non-conflicting queries (such as `GET_STATE`) to execute without multi-second delays.
2. **Session Exclusivity & Token Validation:**
   The kernel tracks `data->session`:
   - Binds to the specific `struct file *filp` that invoked `BEGIN`.
   - Issues a cryptographic 32-bit random `session_id`.
   - Rejects any command presenting an invalid or expired `session_id` with `-EACCES`.
   - Rejects any other process or file descriptor with `-EBUSY`.
3. **Fail-Closed Crash Teardown:**
   If the userspace process crashes, hangs, or closes `filp` before calling `COMMIT`, `fte3600_cdev_release()` intercepts the event:
   - Aborts the active session.
   - Transitions `data->state` immediately to `FTE3600_STATE_UNKNOWN` (7).
   - Prevents the device from ever reporting `READY`.
4. **Strict Phase Enforcement:**
   `COMMIT` checks a bitmask (`session->completed_phases`). If FDT sampling, image baseline, or threshold programming was skipped, `COMMIT` returns `-EPERM` and marks the session failed.

---

## 4. Architectural Recommendation & Justification

**Verdict:** **Design C (Session-Gated Hybrid Architecture)** is definitively recommended.

### Justification Summary:
1. **Safety:** Prevents 1.15-second kernel mutex lockups while maintaining total session exclusivity.
2. **Cleanliness:** Allocates zero large buffers in kernel space; image sorting and median calculations remain in userspace where they belong.
3. **Resilience:** Guaranteed automated cleanup upon userspace crashes via `fte3600_cdev_release()`.
4. **Upstream Alignment:** Follows the established Linux kernel subsystem pattern for multi-step hardware calibration (e.g. `v4l2`, `iio`).
