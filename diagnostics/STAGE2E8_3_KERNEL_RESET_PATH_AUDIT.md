# Stage 2E.8.3 Kernel Reset-Path and GPIO Timing Audit

**Target System:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Target SPI Device:** `spi-FTE3600:00` on Intel LPSS SPI2 (`pxa2xx-spi.2`, CS0)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (x86_64)  
**Task Type:** Static-Only Source Audit of Kernel Reset Implementation  
**Status:** **STAGE B HARDWARE EXECUTION ON HOLD — STRICTLY STATIC AUDIT**

---

## 1. Executive Summary & Safety Restrictions

This document provides a static-only source audit of the kernel reset implementation in `kernel/fte3600.c` and its userspace interface in `tools/fte3600-tool.c` prior to any consideration of hardware testing.

### Strict Safety Boundaries
- No physical hardware testing performed.
- No kernel module insertion (`insmod`), removal (`rmmod`), or driver unbinding (`unbind`).
- No device node opening (`/dev/fte3600`) or ioctl execution.
- No SPI transactions, GPIO line toggling, MMIO accesses, or sensor register communications.
- No modifications to kernel source code, userspace tools, or test scripts.
- No git commits.

---

## 2. Complete RESET ioctl Call Graph

The reset sequence traverses from the userspace test harness down to the Intel PCH LPSS GPIO controller.

### 2.1 Trace of Complete Execution Path

```
Step 1: Userspace Command Dispatch
        scripts/stage-b-test.sh (lines 1314–1321)
          └─► Invokes: bin/fte3600-tool reset

Step 2: Userspace Process Entry
        tools/fte3600-tool.c:34 (main)
          └─► Validates argc >= 2 (lines 36–39)

Step 3: Character Device Open
        tools/fte3600-tool.c:41: open("/dev/fte3600", O_RDWR)
          └─► VFS dispatch: fte3600_fops.open -> fte3600_cdev_open (kernel/fte3600.c:368-389)
                ├─► kernel/fte3600.c:373: check atomic_read(&data->offline) -> -ENODEV
                ├─► kernel/fte3600.c:377: atomic_cmpxchg(&data->is_open, 0, 1) -> -EBUSY
                ├─► kernel/fte3600.c:381: re-check atomic_read(&data->offline) -> -ENODEV
                ├─► kernel/fte3600.c:386: get_device(&data->dev) (increments kobject refcount)
                └─► kernel/fte3600.c:387-388: filp->private_data = data; return 0

Step 4: Userspace IOCTL Call
        tools/fte3600-tool.c:57: ioctl(fd, FTE3600_IOC_RESET)
          └─► Defined in kernel/fte3600.h:95: _IO('F', 0x02)

Step 5: Kernel IOCTL Dispatcher
        kernel/fte3600.c:548: fte3600_fops.unlocked_ioctl -> fte3600_cdev_ioctl (line 515)
          └─► Calls fte3600_ioctl_common(filp, cmd, arg) (kernel/fte3600.c:402-457)
                ├─► Line 407: validates _IOC_TYPE(cmd) == FTE3600_IOC_MAGIC ('F') -> -ENOTTY
                ├─► Line 411: fast-path check atomic_read(&data->offline) -> -ENODEV
                └─► Line 436: case FTE3600_IOC_RESET:

Step 6: Mutex Acquisition & Safety Validation
        kernel/fte3600.c:437: mutex_lock_interruptible(&data->lock)
          ├─► If interrupted by signal: returns -ERESTARTSYS (libc ioctl returns -1, errno=EINTR)
          └─► Line 446: validates !atomic_read(&data->offline) && data->reset_gpiod != NULL
                └─► If invalid: mutex_unlock(&data->lock); return -ENODEV

Step 7: Reset Helper Execution
        kernel/fte3600.c:451: ret = fte3600_hw_reset_pulse(data)
          └─► Definition in kernel/fte3600.c:131-158
                ├─► Line 133: lockdep_assert_held(&data->lock)
                ├─► Line 135: defensive check (atomic_read(&data->offline) || !data->reset_gpiod)
                ├─► Line 138: data->state = FTE3600_STATE_RESETTING;
                ├─► Line 141-142: gpiod_set_value_cansleep(data->reset_gpiod, 0); msleep(10);
                ├─► Line 145-146: gpiod_set_value_cansleep(data->reset_gpiod, 1); msleep(20);
                ├─► Line 149-150: gpiod_set_value_cansleep(data->reset_gpiod, 0); msleep(10);
                ├─► Line 156: data->state = FTE3600_STATE_RESET_SEQUENCE_ISSUED;
                └─► Line 157: return 0;

Step 8: Mutex Release & Syscall Completion
        kernel/fte3600.c:455: mutex_unlock(&data->lock)
          └─► Line 512: return ret; (returns 0 to userspace)

Step 9: Userspace Success Handling
        tools/fte3600-tool.c:63: printf(">> Reset ioctl returned 0 (Success).\n")

Step 10: Character Device Release
        tools/fte3600-tool.c:101: close(fd)
          └─► VFS dispatch: fte3600_fops.release -> fte3600_cdev_release (kernel/fte3600.c:391-399)
                ├─► Line 396: atomic_set(&data->is_open, 0)
                ├─► Line 397: put_device(&data->dev) (drops kobject refcount)
                └─► Line 398: return 0
```

---

## 3. Precise Reset Pulse Sequence and GPIO Timing

### 3.1 GPIO Descriptor Acquisition and ACPI Translation
- **Descriptor Acquisition:** In [`kernel/fte3600.c:602`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L602):
  ```c
  data->reset_gpiod = devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_LOW);
  ```
- **ACPI Mapping Table:** In [`kernel/fte3600.c:61-68`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L61-L68):
  ```c
  static const struct acpi_gpio_params fte3600_reset_gpios = { 0, 0, true };
  static const struct acpi_gpio_mapping fte3600_acpi_gpios[] = {
      { "reset-gpios", &fte3600_reset_gpios, 1 },
      ...
  };
  ```
  The third parameter `true` sets `active_low = true`.
- **Gpiolib Polarity Translation:**
  Because the GPIO is declared `active_low = true`, gpiolib automatically maps:
  - **Logical 0 (Inactive / Deasserted)** $\longrightarrow$ **Physical Electrical HIGH** (VDD / 3.3V)
  - **Logical 1 (Active / Asserted)** $\longrightarrow$ **Physical Electrical LOW** (GND / 0V)
- **Initial State at Probe:**
  `GPIOD_OUT_LOW` requests initial logical value 0 $\longrightarrow$ physical pin is initialized **HIGH** (inactive / deasserted).

### 3.2 Reset Pulse Step-by-Step Sequence

| Step | Operation in `kernel/fte3600.c` | Line | Logical Value | Physical Pin Level | Requested Delay | Purpose |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **0** | `data->state = FTE3600_STATE_RESETTING` | 138 | N/A | Pre-existing (HIGH) | None | Driver state transition |
| **1** | `gpiod_set_value_cansleep(data->reset_gpiod, 0)` | 141 | `0` | **HIGH (Inactive)** | None | Enforce clean deasserted state |
| **2** | `msleep(10)` | 142 | `0` | **HIGH (Inactive)** | 10 ms | Pre-reset settle interval |
| **3** | `gpiod_set_value_cansleep(data->reset_gpiod, 1)` | 145 | `1` | **LOW (Asserted)** | None | Assert active-low reset pulse |
| **4** | `msleep(20)` | 146 | `1` | **LOW (Asserted)** | 20 ms | Reference reset hold (`recovery.c:19`) |
| **5** | `gpiod_set_value_cansleep(data->reset_gpiod, 0)` | 149 | `0` | **HIGH (Inactive)** | None | Deassert reset pulse |
| **6** | `msleep(10)` | 150 | `0` | **HIGH (Inactive)** | 10 ms | Reference settle latency (`startup.c:341`) |
| **7** | `data->state = FTE3600_STATE_RESET_SEQUENCE_ISSUED` | 156 | `0` | **HIGH (Inactive)** | None | Mark electrical pulse completed |

### 3.3 Execution Boundaries and Feedback Limitations
- **Exact Beginning:** The reset sequence begins at line 138 when `data->state = FTE3600_STATE_RESETTING` is assigned and line 141 dispatches the pre-reset settle write.
- **Exact Completion:** The sequence completes at line 156 when the post-reset settle sleep finishes and `data->state = FTE3600_STATE_RESET_SEQUENCE_ISSUED` is assigned.
- **Error Impossibility Post-Start:**
  In [`kernel/fte3600.c:141-157`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L141-L157), `gpiod_set_value_cansleep()` and `msleep()` both return `void`. There are zero conditional checks or error return paths after line 141. Once the sequence begins, **it unconditionally runs through to completion and returns 0**.
- **Absence of Electrical Feedback:**
  `gpiod_set_value_cansleep()` writes to Intel PCH LPSS GPIO pad configuration registers (PADCFG0 / PADCFG1). It provides **zero electrical feedback**:
  - Does not read back pin line voltage.
  - Cannot detect open traces, cold solder joints, or a disconnected flex cable.
  - Cannot verify whether the sensor silicon accepted the reset pulse or booted properly.

---

## 4. Error and Interruption Semantics

### 4.1 Exhaustive Return-Path Analysis

| Situation | Can reset helper be entered? | Can GPIO transition occur? | What can userspace know? | Outcome Classification |
| :--- | :--- | :--- | :--- | :--- |
| **Device open fails** (`open()` returns `< 0`) | **No** | **No** | Device node missing (`ENOENT`) or unbinding (`ENODEV`); `ioctl` was never issued | `RESET NOT PERFORMED` |
| **Device busy** (`open()` returns `-EBUSY`) | **No** | **No** | Another process holds single-client lease; `ioctl` was never issued | `RESET NOT PERFORMED` |
| **Signal before ioctl** (fatal signal terminates process before `ioctl`) | **No** | **No** | Process killed before entering kernel; exit code $rc \ge 128$; no kernel code ran | `RESET NOT PERFORMED` |
| **Mutex acquisition interrupted** (`mutex_lock_interruptible` returns `-ERESTARTSYS`) | **No** | **No** | Signal arrived while blocked on mutex; lock was never acquired; `ioctl` returns `-1` with `errno == EINTR` ($rc = 1$) or process killed ($rc \ge 128$) | `RESET NOT PERFORMED` |
| **Invalid ioctl** (magic mismatch or unknown ioctl code) | **No** | **No** | Dispatcher rejected command with `-ENOTTY`; `ioctl` returns `-1` ($rc = 1$) | `RESET NOT PERFORMED` |
| **Pre-reset check fails under lock** (`offline == 1` or `reset_gpiod == NULL`) | **No** | **No** | Driver was unbound concurrently; dispatcher returns `-ENODEV`; `ioctl` returns `-1` ($rc = 1$) | `RESET NOT PERFORMED` |
| **Reset helper begins** (lines 138–156) | **Yes** | **Yes** | Helper executes unconditionally to completion; returns 0 | `RESET COMPLETED ON HARDWARE` |
| **Signal during reset sleep** (signal delivered during `msleep(40)`) | **Yes** | **Yes** | Kernel sleeps in `TASK_UNINTERRUPTIBLE` ran to completion; signal delivered upon return to userspace; process killed ($rc \ge 128$) | `RESET OUTCOME UNCERTAIN — DO NOT RETRY` |
| **Signal after ioctl completion** (signal arrives after `ioctl` returns 0, during `printf` or `close`) | **Yes** | **Yes** | Pulse succeeded; process killed during userspace cleanup ($rc \ge 128$) | `RESET OUTCOME UNCERTAIN — DO NOT RETRY` |
| **Normal ioctl return** (`ioctl` returns 0; process exits normally) | **Yes** | **Yes** | Pulse dispatched and completed; tool prints success and exits $rc = 0$ | `RESET COMPLETED (SUCCESS)` |

### 4.2 Ambiguity of $rc \ge 128$
- When userspace observes an exit code $rc \ge 128$ (e.g. 130 for `SIGINT`, 143 for `SIGTERM`):
  - In *Signal before ioctl*, the reset pulse **did not occur**.
  - In *Signal during reset sleep* and *Signal after ioctl completion*, the reset pulse **did occur**.
- Exit status $rc \ge 128$ is completely ambiguous from userspace.
- Therefore, any signal-interrupted execution must strictly be treated as:
  $$\text{\bf RESET OUTCOME UNCERTAIN — DO NOT RETRY}$$

---

## 5. GPIO Lifetime and Driver State Management

### 5.1 Resource Allocation and Detach Lifecycle
1. **Acquisition:** Acquired in `fte3600_probe()` ([line 602](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L602)) via `devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_LOW)`.
2. **Devres Management:** Managed by the device resource manager bound to `&spi->dev`.
3. **Automatic Release on Detach:** Automatically released when the driver is detached.
4. **Hardware-Resource Shutdown Protocol:**
   To eliminate use-after-free races during concurrent unbind:
   [`kernel/fte3600.c:655-664`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L655-L664):
   ```c
   mutex_lock(&data->lock);
   atomic_set(&data->offline, 1);
   data->reset_gpiod = NULL;
   data->spi = NULL;
   mutex_unlock(&data->lock);
   ```
   Under `data->lock`, `reset_gpiod` is set to `NULL` before `fte3600_remove()` returns. Any waiting ioctl will acquire `data->lock`, observe `!data->reset_gpiod`, and cleanly abort with `-ENODEV`.

### 5.2 Probe vs IOCTL Race Safety
- `cdev_device_add(&data->cdev, &data->dev)` ([line 624](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L624)) is the very last call in `fte3600_probe()`.
- The device node `/dev/fte3600` is inaccessible to userspace until probe has completely succeeded.
- It is impossible for userspace to issue `FTE3600_IOC_RESET` before probe completes.

### 5.3 Concurrency and In-Flight Removal Protection
- **Concurrent Opens:** Blocked by `atomic_cmpxchg(&data->is_open, 0, 1)` ([line 377](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L377)). Only one process may hold `/dev/fte3600` open at a time.
- **Concurrent IOCTLs:** Strictly serialized under `mutex_lock_interruptible(&data->lock)` ([line 437](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L437)).
- **Driver Removal During Active IOCTL:**
  If `fte3600_remove()` is triggered while an ioctl is running:
  1. `cdev_device_del()` ([line 649](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L649)) unlinks the character device.
  2. `mutex_lock(&data->lock)` ([line 655](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L655)) **blocks and waits** until the active ioctl finishes and releases the mutex.
  3. `data` structure memory is protected by `get_device(&data->dev)` ([line 386](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L386)) and is not freed until userspace calls `close(fd)`.
- **Module Reference Protection:**
  - `fte3600_fops.owner = THIS_MODULE` ([line 545](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L545))
  - `data->cdev.owner = THIS_MODULE` ([line 623](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L623))
  - Guarantees `rmmod fte3600` is blocked with `-EBUSY` while `/dev/fte3600` is held open by any process.

---

## 6. Review of Timing Claims and Kernel Semantics

### 6.1 `msleep()` in `TASK_UNINTERRUPTIBLE`
- In the Linux kernel, `msleep(msecs)` calls `schedule_timeout_uninterruptible()`.
- The calling task is placed into `TASK_UNINTERRUPTIBLE`.
- Signals delivered to the process (e.g. `SIGINT`, `SIGTERM`) do **not** awaken the task early.
- The thread yields the CPU and sleeps until the kernel timer expires.

### 6.2 Sequence is NOT Atomic
- In Linux kernel terminology, an "atomic context" prohibits sleeping or yielding the CPU (e.g. spinlocks, RCU read locks, hardirq handlers).
- The reset pulse executes in *process context* with a sleepable `struct mutex` held.
- The thread calls `msleep()` three times (total 40 ms), during which other kernel and userspace threads run on the CPU.
- Describing the sequence as "atomic" is technically inaccurate. It is **uninterruptible by signals**, but not atomic.

### 6.3 Nominal Delays vs Measured Electrical Timing
- Exact microsecond timing cannot be claimed from source inspection:
  - `msleep()` resolution depends on the kernel system timer frequency (`CONFIG_HZ`).
  - Timer wheel slack, scheduler latency, and CPU wake-up jitter mean actual elapsed time exceeds the nominal requested values:
    - Requested 10 ms settle $\longrightarrow$ nominal $\ge 10\text{ ms}$ (typically 10.5 ms – 14 ms).
    - Requested 20 ms hold $\longrightarrow$ nominal $\ge 20\text{ ms}$ (typically 20.5 ms – 24 ms).
  - Register writes to the Intel PCH LPSS GPIO controller traverse the internal MMIO bus with sub-microsecond propagation delays.
  - Therefore, durations must strictly be documented as **requested nominal intervals**, not measured physical timings.

---

## 7. Verification of Zero Unintended Operations

Exhaustive traversal of the call graph reachable from `case FTE3600_IOC_RESET:` confirms that the RESET path **strictly and exclusively** performs GPIO register writes and timer delays.

| Prohibited Operation | Verified Reachability from RESET Path | Evidence |
| :--- | :--- | :--- |
| **SPI read/write transfers** | **NONE** | `spi_sync()` and `fte3600_spi_sync_transfer()` are **never** called from `case FTE3600_IOC_RESET`. |
| **INIT handler / Silicon ID** | **NONE** | `fte3600_hw_identify()` is called exclusively from `case FTE3600_IOC_INIT`. |
| **CAPTURE handler / FIFO read**| **NONE** | `case FTE3600_IOC_CAPTURE` returns `-EOPNOTSUPP` and is unreachable from reset. |
| **Firmware loading** | **NONE** | `request_firmware()` does not exist in `kernel/fte3600.c`. |
| **Register access** | **NONE** | Zero SPI commands or register transactions dispatched. |
| **Sensor bus communication** | **NONE** | CS0 remains HIGH (inactive); clock remains silent; MOSI/MISO silent. |
| **IRQ handler execution** | **NONE** | Driver does not bind or register an interrupt handler in probe or reset. |
| **Automatic retries / Loops** | **NONE** | `fte3600_hw_reset_pulse()` contains no loops or recursive calls. Single execution only. |

---

## 8. Final Readiness Assessment

### **Selected Finding: Conclusion A**

$$\text{\bf Conclusion A: The source-level reset path is internally consistent.}$$

**Justification:**
1. The kernel reset path enforces single-client lease, serializes under `data->lock`, uses `TASK_UNINTERRUPTIBLE` sleeps, and transitions driver state cleanly.
2. The Hardware-Resource Shutdown Protocol prevents use-after-free races against concurrent driver unbind.
3. Module unloading is prevented while the device is open via `.owner = THIS_MODULE`.
4. Zero SPI transactions, register operations, or secondary reset pulses are issued.
5. No blocking source-level bugs or safety vulnerabilities were identified in the reset execution path.

*Note: Selecting Conclusion A verifies source-level consistency only; it does not verify physical pin voltage on Pad 14 or silicon responsiveness, which can only be evaluated when hardware testing is authorized.*

---

## 9. Remaining Limitations

1. **Hardware Open-Circuit Blindness:**
   Because `gpiod_set_value_cansleep()` returns `void`, the driver cannot detect broken PCB traces, cold solder joints, or a detached flex cable on Pad 14.
2. **Userspace Telemetry Enhancement Opportunity:**
   When `tools/fte3600-tool reset` exits with $rc \ge 128$, `scripts/stage-b-test.sh:1325` logs a general warning. A dedicated banner (`RESET OUTCOME UNCERTAIN — DO NOT RETRY`) should be added in a future update to make this policy explicit to operators.

---

## 10. Required Final Status Declarations

- **AUTOMATIC MODULE UNLOAD:** DISABLED
- **AUTOMATIC SPI UNBIND:** DISABLED
- **KERNEL RESET PATH AUDIT:** COMPLETED
- **STAGE B HARDWARE EXECUTION:** NOT PERFORMED
- **STAGE B AUTHORIZATION:** ON HOLD
