# Stage 2E.9.2 Reset Semantics and Remaining Evidence Reconciliation Report

**Target System:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Target SPI Device:** `spi-FTE3600:00` on Intel LPSS SPI2 (`pxa2xx-spi.2`, CS0)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (x86_64, SMP PREEMPT_DYNAMIC)  
**Task Type:** Static-Only Reset Semantics & Evidence Reconciliation Audit  
**Status:** **STAGE B HARDWARE EXECUTION ON HOLD — STRICTLY STATIC AUDIT**

---

## 1. Reset Interruption Classification & Execution Trace

### 1.1 Complete Execution Call Graph Trace
The execution path for Phase 4 of [`scripts/stage-b-test.sh`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L1319) traverses through three software layers:

```
[scripts/stage-b-test.sh:1319]
       │
       ▼ (fork/execve)
[bin/fte3600-tool: main()]
       │
       ├─► [open("/dev/fte3600", O_RDWR)] ──► VFS chrdev_open() ──► fte3600_open()
       │
       ├─► [printf(">> Issuing FTE3600_IOC_RESET...\n")]
       │
       └─► [ioctl(fd, FTE3600_IOC_RESET)]
                 │
                 ▼ (sys_ioctl)
           [kernel/fte3600.c: fte3600_ioctl()]
                 │
                 ├─► mutex_lock(&data->lock)
                 ├─► fte3600_hw_reset_pulse(data)
                 │         ├─► state = FTE3600_STATE_RESETTING
                 │         ├─► gpiod_set_value_cansleep(data->reset_gpiod, 0)
                 │         ├─► msleep(10)
                 │         ├─► gpiod_set_value_cansleep(data->reset_gpiod, 1)
                 │         ├─► msleep(20)
                 │         ├─► gpiod_set_value_cansleep(data->reset_gpiod, 0)
                 │         ├─► msleep(10)
                 │         └─► state = FTE3600_STATE_RESET_SEQUENCE_ISSUED
                 ├─► data->flags |= FTE3600_FLAG_RESET_SEQUENCE_ISSUED; data->last_error = 0;
                 ├─► mutex_unlock(&data->lock)
                 └─► return 0
                 │
                 ▼ (syscall return)
           [bin/fte3600-tool: main()]
                 ├─► printf(">> Reset ioctl returned 0 (Success).\n")
                 ├─► close(fd) ──► VFS fte3600_release()
                 └─► return 0 (exit status 0)
       │
       ▼
[scripts/stage-b-test.sh:1321]
       └─► RESET_RC=$? (records 0)
```

---

### 1.2 Signal Arrival Taxonomy (SIGINT, SIGTERM, SIGHUP, SIGQUIT)

If an asynchronous signal arrives during or around line 1319, the exact state of hardware reset depends strictly on the execution sub-phase.

| Sub-Phase | Point of Signal Delivery | Hardware Reset Status | Diagnostic Classification |
| :--- | :--- | :--- | :--- |
| **Case A** | Before userspace tool starts (in shell prior to `execve` or during fork) | **Definitely did NOT occur** | `RESET DEFINITELY DID NOT OCCUR` |
| **Case B** | After tool starts, during ELF loading / libc init, before `open("/dev/fte3600")` | **Definitely did NOT occur** | `RESET DEFINITELY DID NOT OCCUR` |
| **Case C** | After `open()` returns `fd`, during `strcmp` / `printf`, before `ioctl()` syscall | **Definitely did NOT occur** | `RESET DEFINITELY DID NOT OCCUR` |
| **Case D** | During `ioctl(FTE3600_IOC_RESET)` execution inside the kernel | **Uncertain** (kernel executes uninterruptible `msleep()` / `mutex_lock()`, but dispatch point or signal delivery timing relative to GPIO assertion cannot be proven from userspace) | `RESET OUTCOME UNCERTAIN — DO NOT RETRY` |
| **Case E** | After `fte3600_hw_reset_pulse()` completes in kernel, during syscall return path before userspace resumes | **Uncertain to userspace** (pulse completed in hardware, but userspace was killed before receiving status; indistinguishable from Case D to userspace) | `RESET OUTCOME UNCERTAIN — DO NOT RETRY` |
| **Case F** | After `ioctl()` returns 0 to libc, but before shell records exit status `RESET_RC=$?` | **Uncertain to shell** (tool may have printed success to stdout, but if shell or tool is terminated before status collection, shell trap fires) | `RESET OUTCOME UNCERTAIN — DO NOT RETRY` |

---

### 1.3 Correction of Stage 2E.9.1 Statement

> [!CAUTION]
> **Correction of Prior Assertion:**  
> In Stage 2E.9.1 Section 1.3, scenario 4 stated:  
> *"SIGINT/SIGTERM/SIGHUP during/after line 1319 $\rightarrow$ 1 reset"*  
> **This was an inaccurate oversimplification.**  
>
> If a signal terminates the execution of line 1319, it may have terminated the process in Case B, C, or early D (where reset **never occurred**), or during Case D or E (where reset **completed or partially executed in kernel**, but userspace cannot confirm it). Userspace observes only process termination via signal ($128 + N$).  
>
> Therefore, a signal received during line 1319 **does not prove that a reset occurred**. The only mathematically and operationally correct classification is:  
> **`RESET OUTCOME UNCERTAIN — DO NOT RETRY`**  
>
> In all such uncertain cases, the test harness enforces a strict **fail-closed, no-retry policy**: execution aborts directly to [`cleanup_handler()`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L895-L1026), which never attempts to re-issue the ioctl or re-pulse the hardware.

---

## 2. Resolution of Reset Helper Timing Discrepancy

### 2.1 Verbatim Source Excerpt of Current Implementation
The helper function is implemented in [`kernel/fte3600.c:131-158`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L131-L158):

```c
static int fte3600_hw_reset_pulse(struct fte3600_data *data)
{
	lockdep_assert_held(&data->lock);

	if (atomic_read(&data->offline) || !data->reset_gpiod)
		return -ENODEV;

	data->state = FTE3600_STATE_RESETTING;

	/* Pre-reset settle interval (Logical 0 = physical HIGH / inactive) */
	gpiod_set_value_cansleep(data->reset_gpiod, 0);
	msleep(10);

	/* Assert reset: Logical 1 = physical LOW / asserted */
	gpiod_set_value_cansleep(data->reset_gpiod, 1);
	msleep(20); /* Reference hold duration (recovery.c:19) */

	/* Deassert reset: Logical 0 = physical HIGH / inactive */
	gpiod_set_value_cansleep(data->reset_gpiod, 0);
	msleep(10); /* Reference settle latency (factory_startup.c:341) */

	/*
	 * Electrical reset pulse sequence completed on GPIO controller.
	 * State is RESET_SEQUENCE_ISSUED. Sensor silicon responsiveness is unverified.
	 */
	data->state = FTE3600_STATE_RESET_SEQUENCE_ISSUED;
	return 0;
}
```

---

### 2.2 Sequence, Logic, and Timing Analysis

1. **GPIO Logical Values in Order:**
   - Step 1: Logical `0` (Pre-reset settle)
   - Step 2: Logical `1` (Assert reset)
   - Step 3: Logical `0` (Deassert reset)
2. **Sleep Calls in Order:**
   - Step 1: `msleep(10)`
   - Step 2: `msleep(20)`
   - Step 3: `msleep(10)`
3. **Total Nominal Requested Sleep Duration:**
   $$\text{Total Nominal Sleep} = 10\text{ ms} + 20\text{ ms} + 10\text{ ms} = 40\text{ ms}$$
4. **Active-Low Interpretation of Each Logical Value:**
   - The reset descriptor `reset_gpiod` is acquired with active-low semantics (`GPIOD_OUT_LOW`). Under the Linux `gpiod_*` consumer API:
     - **Logical 0:** Inactive state $\rightarrow$ Physical line is driven **HIGH** (3.3V / reset deasserted).
     - **Logical 1:** Active state $\rightarrow$ Physical line is driven **LOW** (0V / reset asserted).
   - Therefore, Step 1 sets physical HIGH, Step 2 drives physical LOW for 20 ms, and Step 3 restores physical HIGH.
5. **State Updates Before, During, and After Pulse:**
   - **Entry / Before Pulse:** Prior driver state (e.g. `FTE3600_STATE_UNINITIALIZED` = 0).
   - **At Start of Pulse:** `data->state = FTE3600_STATE_RESETTING;` (state = 1).
   - **During Pulse:** State remains `FTE3600_STATE_RESETTING`.
   - **At Completion of Pulse:** `data->state = FTE3600_STATE_RESET_SEQUENCE_ISSUED;` (state = 2).
   - **In Caller (`fte3600_ioctl`):** `data->flags |= FTE3600_FLAG_RESET_SEQUENCE_ISSUED; data->last_error = 0;`.
6. **Physical / Electrical Timing Disclaimers:**
   - `gpiod_set_value_cansleep()` dispatches MMIO register writes to the Intel PCH LPSS GPIO controller. Because `gpiod_set_value_cansleep()` returns `void`, successful execution proves that register writes were issued to the controller; it does **not** verify physical pin voltage on Pad 14 or sensor silicon acceptance.
   - `msleep()` calls request software timeouts based on kernel jiffies and timer slack. They represent **nominal requested durations**, not exact electrical oscilloscope guarantees.

---

### 2.3 Cause of the Discrepancy
- **Earlier Audit Status ([`STAGE2E8_3`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2E8_3_KERNEL_RESET_PATH_AUDIT.md)):**  
  Correctly described the three-step sequence: Logical 0 (10 ms), Logical 1 (20 ms), Logical 0 (10 ms), total 40 ms.
- **Stage 2E.9.1 Status:**  
  Loosely summarized the sequence as *"asserts active-low reset, sleeps `msleep(20)`, deasserts, sleeps `msleep(20)`"*.
- **Git Commit Provenance:**  
  `git log -1 --format="%h %cd" kernel/fte3600.c` confirms commit `c855381` (Nov 3 2026). `git diff HEAD -- kernel/` shows **0 lines changed**.
- **Conclusion:**  
  The discrepancy was **strictly an audit report-writing error** in the Stage 2E.9.1 summary text. The underlying C code in `kernel/fte3600.c` has remained unchanged and identical to the 10 ms / 20 ms / 10 ms implementation throughout.

---

## 3. Reconciliation of RESET Invocation Count

A clear distinction must be maintained between the **upper bound invariant** and the **successful execution guarantee**:

### 3.1 Claim 1: The harness can invoke the RESET ioctl at most once ($\le 1$)
- **Source Verification:**
  - Across the entire repository, there is exactly one invocation of the reset utility: [`scripts/stage-b-test.sh:1319`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L1319) (`"$TOOL_BIN" reset`).
  - There are no loops around line 1319.
  - There are no retry branches or wrapper scripts that re-invoke reset.
  - The cleanup handler [`cleanup_handler()`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L895-L1026) contains 0 ioctls and 0 invocations of `$TOOL_BIN`.
- **Proof Across All Paths:**
  - If pre-checks, insertion, devnode validation, or Snapshot 1 fail: Line 1319 is never reached $\rightarrow$ **0 invocations**.
  - If a signal terminates the script before line 1319: Line 1319 is never reached $\rightarrow$ **0 invocations**.
  - If a signal terminates the script during line 1319: Execution stops; classified as **`RESET OUTCOME UNCERTAIN — DO NOT RETRY`** $\rightarrow$ **$\le 1$ invocation**.
  - If line 1319 completes: Exactly **1 invocation**.
  - Subsequent steps (Snapshot 2, dmesg logging, cleanup) never call reset $\rightarrow$ total remains **1 invocation**.
- **Conclusion:** **The upper bound of $\le 1$ is mathematically and structurally proven.**

### 3.2 Claim 2: The RESET ioctl definitely executes exactly once on a successful run ($= 1$)
- **Source Verification:**
  - On a clean, uninterrupted run where:
    1. Pre-flight checks pass;
    2. Driver inserts cleanly and creates `/dev/fte3600`;
    3. Snapshot 1 completes with status 0;
    4. Line 1319 runs `"$TOOL_BIN" reset`;
    5. Userspace opens `/dev/fte3600`, calls `ioctl(fd, FTE3600_IOC_RESET)`, and receives 0;
    6. Tool prints `>> Reset ioctl returned 0 (Success).` and exits 0;
  - The ioctl entered the kernel, acquired `&data->lock`, executed the 40 ms pulse sequence, updated driver state to `RESET_SEQUENCE_ISSUED`, and released the lock.
- **Proof Boundary:**
  - Only when line 1319 exits with status 0 and outputs success does the test harness know that reset executed. If the tool is interrupted before entering the kernel, reset did not occur.
- **Conclusion:** **Exact execution of $= 1$ is guaranteed if and only if Phase 4 completes with exit status 0.**

---

## 4. Kernel Lifetime Verification Boundaries

To maintain absolute architectural integrity, the evidence supporting driver and character device lifetimes is separated into distinct verification categories:

| Evidence Level | Subject / Components | Exact Local Evidence Available | Status / Scope |
| :--- | :--- | :--- | :--- |
| **1. Directly Inspected Driver Implementation** | `kernel/fte3600.c`, `kernel/fte3600.h` | Complete C source code inspected locally. Verified: `cdev_init`, `cdev_set_parent`, `cdev_device_add`, `cdev_device_del`, `atomic_read(&data->offline)`, `mutex_lock(&data->lock)`, `devm_gpiod_get`, module refcounting (`.owner = THIS_MODULE`). | **Directly Verified locally from source** |
| **2. Installed Header / API Evidence** | `/lib/modules/7.2.8-9-omarchy/build/include/` | Header files inspected: `linux/cdev.h`, `linux/device.h`, `linux/fs.h`, `linux/gpio/consumer.h`. Verified: type definitions (`struct cdev`, `struct device`), function prototypes (`cdev_device_add`, `cdev_device_del`). | **Directly Verified locally from installed headers** |
| **3. Exact Running-Kernel Implementation Evidence** | Linux `7.2.8-9-omarchy` kernel runtime | `/proc/kallsyms` inspected: symbols `cdev_device_add`, `cdev_device_del`, `cdev_del`, `chrdev_open` are exported in running kernel. `/proc/version` confirms running kernel version and PREEMPT_DYNAMIC build. | **Directly Verified locally from runtime kernel interfaces** |
| **4. Inferences Based on Upstream Linux Semantics** | Linux Character Device & Driver Core Subsystems (`fs/char_dev.c`, `drivers/base/core.c`) | Standard upstream Linux architecture (since commit `233ed097f436` / Linux 4.12): `cdev_device_add()` pins parent device; `chrdev_open()` takes module reference via `try_module_get()` and device reference via `cdev_get()`; `cdev_del()` revokes device node from `cdev_map`. | **Inferred from established upstream Linux kernel architecture** |
| **5. Remaining Unverified Assumptions** | Distribution-specific C source files for `7.2.8-9-omarchy` | Full core kernel `.c` source files (`fs/char_dev.c`, `drivers/base/core.c`) are not installed on disk under `/lib/modules/7.2.8-9-omarchy/build/`. Any distribution-specific modifications made by "omarchy" distributor to core VFS cannot be read from disk. | **Unverified locally due to source unavailability** |

---

### 4.1 Impact on Single RESET Test Safety
**Does any unresolved lifetime uncertainty create a concrete reason to block this single RESET test?**

**Answer: NO.**

**Technical Proof:**
1. Under **Option 1 (fail-closed, never unload)**:
   - Automatic module unloading is permanently disabled (`rmmod` / `modprobe -r` disabled).
   - Automatic SPI unbinding is permanently disabled (`unbind` disabled).
2. During the single controlled RESET test:
   - `fte3600_remove()` is **never called** by the test harness under any outcome.
   - The theoretical open-vs-remove race condition analyzed in Stage 2E.8.4 and 2E.8.5 requires `fte3600_remove()` to execute concurrently with `open()`.
   - Because `fte3600_remove()` is never invoked during the test, this race condition **cannot occur**.
3. Single-threaded serialization:
   - The test harness is protected by exclusive cooperative lock (`/run/lock/fte3600-stage-b.lock`).
   - Only a single instance of `fte3600-tool` executes at any time.
4. Conclusion:
   - The theoretical lack of local `fs/char_dev.c` source code does not create an operational safety hazard for the single RESET test because the driver removal path is completely deactivated.

---

## 5. Final Result and Status

### **Selected Result: A. No source defect found; remaining limitations are explicitly bounded.**

**Justification:**
1. **Signal Interruption:** Corrected the classification of interrupted reset execution to `RESET OUTCOME UNCERTAIN — DO NOT RETRY`. The source code correctly implements a fail-closed, single-invocation structure with no retry paths.
2. **Timing Discrepancy:** Proved that the 10 ms / 20 ms / 10 ms sequence in `kernel/fte3600.c` never changed; the discrepancy was purely an audit report-writing error in Stage 2E.9.1.
3. **Invocation Count:** Reconciled the upper-bound guarantee ($\le 1$) from the clean execution guarantee ($= 1$), proving both from source.
4. **Kernel Lifetime:** Established an honest, structured separation of verification evidence, confirming that Option 1 safely deactivates the removal race during Stage B execution.

---

STAGE B HARDWARE EXECUTION: NOT PERFORMED  
AUTOMATIC MODULE UNLOAD: DISABLED  
AUTOMATIC SPI UNBIND: DISABLED
