# Stage D7.1 — Kernel Integration Review & Hardware Interrupt Verification

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source and Documentation Audit  
**Audit Classification:** Adversarial Challenge & Evidence Qualification  

---

## 1. Executive Summary

This report delivers an adversarial review of the kernel driver integration challenges for Stage D8, rigorously evaluating:
1. The physical hardware status and reality of the Pad 229 interrupt line.
2. The concurrency, locking, memory, and cancellation requirements for a future calibration implementation.
3. The architectural trade-offs between an autonomous In-Kernel Calibration Engine (Option A) versus a Hybrid Kernel/Userspace Architecture (Option B).

---

## 2. Reassessing the Pad 229 Interrupt Reality

In previous analyses, Pad 229 (`GPP_D13` / `SPI2_CS1#` / `FTE3600_INT`) has been described as an active interrupt delivery line. This adversarial audit separates what is documented from what has physically occurred on this laptop:

```
  ┌────────────────────────────────────────────────────────────────────────┐
  │ 1. ACPI DSDT Description                                               │
  │    Declares GpioInt(Edge, ActiveLow, ExclusiveAndWake, PullDefault)    │
  │    Connected to GPP_D13 (Pad 229).                                     │
  ├────────────────────────────────────────────────────────────────────────┤
  │ 2. Linux Pinctrl & IRQ Subsystem                                       │
  │    Linux assigns IRQ 153 to GPP_D13.                                   │
  │    Kernel gpiolib configures PADCFG0 = 0x82000100 (falling-edge).      │
  ├────────────────────────────────────────────────────────────────────────┤
  │ 3. Vendor Software Intent                                              │
  │    Vendor code sets 0x1A83 bits to route touch events to the host.     │
  ├────────────────────────────────────────────────────────────────────────┤
  │ 4. Physical Silicon Hardware Reality                                   │
  │    ZERO interrupts have EVER been generated or observed on Pad 229     │
  │    on this laptop. The physical line behavior remains UNVERIFIED.      │
  └────────────────────────────────────────────────────────────────────────┘
```

### 2.1 Critical Evidentiary Distinctions
1. **No Hardware Touch Observed:** Because the sensor has only ever been initialized through Stage 2E.15 (wake + chip ID read), the FDT comparator engine has never been armed on physical hardware.
2. **Electrical Unknowns:**  
   - Whether the sensor drives Pad 229 as an active push-pull output or an open-drain line requiring an external pull-up.
   - Whether the physical pulse duration satisfies the minimum pulse-width requirement of the Intel PCH LPSS GPIO filter (typically >1–2 microseconds).
   - Whether spurious interrupts occur upon power transitions or during SPI bus traffic.
3. **Audit Verdict:** Physical interrupt delivery on Pad 229 remains an **unverified hypothesis** until explicit hardware arming is authorized.

---

## 3. Concurrency & Lifetime Integration Challenges

Implementing calibration and baseline acquisition in the Linux kernel driver introduces significant concurrency complexities:

### 3.1 Long-Running Execution & Mutex Contention
- As proven in Stage D7.1, `image_baseline()` transfers between 40 KB and 133 KB across the SPI bus at 1 MHz, requiring up to **1.1 seconds of execution time**.
- If `data->lock` (the primary driver mutex) is held continuously for 1.1 seconds:
  - Any concurrent `FTE3600_IOC_GET_STATE` call from desktop daemons (`fprintd`, status monitors) will block in the kernel for over a second.
  - Device close (`fte3600_release`) will stall until calibration completes.
  - Solution: Calibration must either execute in an interruptible background workqueue or acquire `data->lock` per-transaction, using state flags to serialize concurrent ioctl requests.

### 3.2 Device Removal & Asynchronous Unbind
- If the user unbinds the driver (`echo spi-FTE3600:00 > unbind`) or the system enters ACPI suspend during a 1.1-second calibration loop:
  - The driver must immediately abort SPI transfers without attempting subsequent search iterations.
  - Every loop iteration must check `if (data->offline || kthread_should_stop()) return -ENODEV;`.

### 3.3 State Rollback on Calibration Failure
- As established in Stage D7.1, vendor code contains zero hardware rollback on calibration failure.
- If the kernel driver encounters an error during FDT or Image calibration:
  - Leaving the state at `INITIALIZED_UNCALIBRATED` is dangerous, as subsequent calls might assume partial registers are valid.
  - **Mandatory Policy:** Any failure during calibration must transition `data->state` immediately to `FTE3600_STATE_UNKNOWN` (7). The only allowable recovery action from `UNKNOWN` is an active-low hardware reset (`FTE3600_IOC_RESET`).

---

## 4. Architectural Comparison: Option A vs Option B

```
  OPTION A: Autonomous In-Kernel Calibration
  ┌────────────────────────────────────────────────────────┐
  │ Userspace: ioctl(FTE3600_IOC_INIT, flags=CALIBRATE)    │
  └────────────────────────┬───────────────────────────────┘
                           │ (Single ioctl entry)
                           ▼
  ┌────────────────────────────────────────────────────────┐
  │ Kernel (fte3600.ko):                                   │
  │ - Allocates 10 KB baseline buffer                      │
  │ - Implements DAC binary search & pixel histogram math  │
  │ - Streams 2–13 frames from FIFO 0x9A05                 │
  │ - Programs threshold bank under 0x9A=0x5A              │
  │ - Advances state to FTE3600_STATE_READY                │
  └────────────────────────────────────────────────────────┘

  OPTION B: Hybrid Kernel/Userspace Architecture
  ┌────────────────────────────────────────────────────────┐
  │ Userspace (Daemon / libfprint helper):                 │
  │ - Executes DAC binary search algorithm                 │
  │ - Computes 5,120-pixel median intensity                │
  │ - Calls primitive ioctls: READ_FDT, READ_RAW_FRAME     │
  │ - Calculates threshold bank & baseline CRC             │
  │ - Calls ioctl(FTE3600_IOC_SET_CALIBRATION, &calib)     │
  └────────────────────────┬───────────────────────────────┘
                           │ (Uploads verified parameters)
                           ▼
  ┌────────────────────────────────────────────────────────┐
  │ Kernel (fte3600.ko):                                   │
  │ - Validates bounds (300 <= thresholds <= 700)          │
  │ - Writes threshold bank under 0x9A=0x5A                │
  │ - Executes arm() strobe (0xC2 0x3D)                    │
  │ - Advances state to FTE3600_STATE_READY                │
  └────────────────────────────────────────────────────────┘
```

### 4.1 Evaluation & Recommendation

| Criterion | Option A (In-Kernel) | Option B (Hybrid) |
|---|---|---|
| **Kernel Memory Footprint** | Adds ~12–16 KB per device (buffer + histogram) | Zero added buffer RAM in kernel |
| **Kernel Code Complexity** | High (search algorithms, median math in kernel) | Low (kernel remains pure hardware controller) |
| **Unix Philosophy** | Policy & algorithms embedded in kernel | Clean separation of mechanism and policy |
| **Vendor Alignment** | Diverges from vendor (`focal_spi` was pure transport)| Closely aligns with vendor architecture |
| **Userspace Simplicity** | Very simple (single ioctl) | Requires userspace calibration tool/daemon |
| **Safety Blast Radius** | Kernel executes multiple unverified FIFO transfers | Kernel only writes bounded, validated thresholds |

**Architectural Recommendation:**  
Option B (Hybrid Architecture) is significantly cleaner, safer, and more maintainable from a Linux kernel engineering perspective. However, if userspace tooling must remain minimal, Option A can be implemented using an interruptible kernel thread or workqueue.
