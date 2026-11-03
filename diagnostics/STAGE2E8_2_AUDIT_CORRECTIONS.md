# Stage 2E.8.2 Audit Corrections and Reset-Interruption Semantics

**Target System:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Target SPI Device:** `spi-FTE3600:00` on Intel LPSS SPI2 (`pxa2xx-spi.2`, CS0)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (x86_64)  
**Task Type:** Static-Only Audit Follow-Up & Correction Review  
**Status:** **STAGE B HARDWARE EXECUTION ON HOLD — STRICTLY STATIC WORK**

---

## 1. Executive Summary & Audit Scope

This document provides a static-only follow-up to the Stage 2E.8.1 independent implementation audit. It rigorously corrects the technical analysis of reset-interruption semantics, refines the no-unload invariant wording, analyzes Bash syntax-error and trap registration boundaries, and re-inspects the entire reachable call graph.

### Strict Safety Restrictions
- No Stage B hardware execution or script launching.
- No kernel module insertion (`insmod`), removal (`rmmod`), or parameter modifications.
- No SPI device binding or unbinding (`spi-FTE3600:00`).
- No device node access (`/dev/fte3600`) and no ioctl invocations (`RESET`, `GET_STATE`, `INIT`, `CAPTURE`).
- No SPI transactions, GPIO operations, or MMIO register accesses.
- No modifications to kernel source code or Stage B execution scripts.
- No git commits.

---

## 2. Detailed Source-Level Sequence of Reset Execution

The reset operation involves userspace binary [`tools/fte3600-tool.c`](file:///home/titan/fte3600-linux-driver/tools/fte3600-tool.c) and kernel driver [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c).

### 2.1 Complete Execution Flow
```
[User / Launcher]
       │
       ▼
scripts/stage-b-test.sh (lines 1309–1339)
       │
       ├─► 1. "$TOOL_BIN" get-state (Snapshot 1) ───────────► queries pre-reset state
       │
       ├─► 2. "$TOOL_BIN" reset (Phase 4)
       │       │
       │       ├─► open("/dev/fte3600", O_RDWR) (fte3600-tool.c:41)
       │       │       │
       │       │       └─► kernel: fte3600_cdev_open() (fte3600.c:368-389)
       │       │             - Check offline == 0
       │       │             - Lease: atomic_cmpxchg(&data->is_open, 0, 1) -> -EBUSY
       │       │             - get_device(&data->dev)
       │       │
       │       ├─► ioctl(fd, FTE3600_IOC_RESET) (fte3600-tool.c:57)
       │       │       │
       │       │       └─► kernel: fte3600_ioctl_common() (fte3600.c:402-457)
       │       │             - Check magic == FTE3600_IOC_MAGIC
       │       │             - Fast-path offline check
       │       │             - mutex_lock_interruptible(&data->lock) (line 437)
       │       │             - Check offline || !data->reset_gpiod (line 446)
       │       │             - fte3600_hw_reset_pulse(data) (lines 131-158)
       │       │                 * state = FTE3600_STATE_RESETTING
       │       │                 * gpiod_set_value_cansleep(reset_gpiod, 0)
       │       │                 * msleep(10)  [TASK_UNINTERRUPTIBLE]
       │       │                 * gpiod_set_value_cansleep(reset_gpiod, 1) [Assert LOW]
       │       │                 * msleep(20)  [TASK_UNINTERRUPTIBLE]
       │       │                 * gpiod_set_value_cansleep(reset_gpiod, 0) [Deassert HIGH]
       │       │                 * msleep(10)  [TASK_UNINTERRUPTIBLE]
       │       │                 * state = FTE3600_STATE_RESET_SEQUENCE_ISSUED
       │       │             - mutex_unlock(&data->lock) (line 455)
       │       │             - return 0
       │       │
       │       ├─► close(fd) (fte3600-tool.c:101)
       │       │       │
       │       │       └─► kernel: fte3600_cdev_release() (fte3600.c:391-399)
       │       │             - atomic_set(&data->is_open, 0)
       │       │             - put_device(&data->dev)
       │       │
       │       └─► return 0 (fte3600-tool.c:102)
       │
       └─► 3. "$TOOL_BIN" get-state (Snapshot 2) ───────────► queries post-reset state
```

### 2.2 Clarification on Tool State Queries
- **Important Finding:** `tools/fte3600-tool` when invoked with argument `reset` does **NOT** query initial state or final state. It executes strictly:
  1. `open("/dev/fte3600", O_RDWR)` ([`fte3600-tool.c:41`](file:///home/titan/fte3600-linux-driver/tools/fte3600-tool.c#L41))
  2. `printf(">> Issuing FTE3600_IOC_RESET...\n")` ([`fte3600-tool.c:56`](file:///home/titan/fte3600-linux-driver/tools/fte3600-tool.c#L56))
  3. `ioctl(fd, FTE3600_IOC_RESET)` ([`fte3600-tool.c:57`](file:///home/titan/fte3600-linux-driver/tools/fte3600-tool.c#L57))
  4. `printf(">> Reset ioctl returned 0 (Success).\n")` ([`fte3600-tool.c:63`](file:///home/titan/fte3600-linux-driver/tools/fte3600-tool.c#L63))
  5. `close(fd)` and `return 0` ([`fte3600-tool.c:101-102`](file:///home/titan/fte3600-linux-driver/tools/fte3600-tool.c#L101-L102))
- Initial state query (Snapshot 1, line 1310) and final state query (Snapshot 2, line 1334) are executed as separate, independent processes by [`scripts/stage-b-test.sh`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh).

---

## 3. Corrected Reset-Interruption Semantics

### 3.1 Four Distinct Signal & Error Timing Cases

| Case | Signal / Event Timing | Userspace Exit Status | Kernel Execution State | Hardware Reset Pulse on GPIO Pad 14 | Actual Hardware Reset Outcome |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Case A** | Signal (`SIGINT`/`SIGTERM`/`SIGHUP`) received **before** `ioctl(FTE3600_IOC_RESET)` is issued or during `mutex_lock_interruptible` | $rc \ge 128$ (e.g. 130 for SIGINT, 143 for SIGTERM) or $rc = 1$ (`EINTR`) | `fte3600_hw_reset_pulse()` was **never entered** | **No electrical pulse dispatched** (GPIO unaffected) | **RESET NOT PERFORMED** |
| **Case B** | Signal received **while** the kernel reset handler is executing inside `fte3600_hw_reset_pulse()` | $rc \ge 128$ (signal delivered upon syscall return to userspace) | `msleep(40)` in `TASK_UNINTERRUPTIBLE` ran to completion; signal marked pending and delivered on return | **Electrical pulse completed in full** (10ms + 20ms low + 10ms settle) | **RESET COMPLETED ON HARDWARE** |
| **Case C** | Signal received **after** ioctl completes successfully in kernel, but before `fte3600-tool` exits | $rc \ge 128$ (process terminated during `printf`, `close()`, or exit) | `ioctl` returned 0; `close()` executed or handled by kernel `do_exit()` | **Electrical pulse completed in full** | **RESET COMPLETED ON HARDWARE** |
| **Case D** | Tool exits normally (no fatal signal), but ioctl returns an error | $rc = 1$ ($0 < rc < 128$, `perror` logged) | Lock or offline check failed; `fte3600_hw_reset_pulse()` was aborted | **No electrical pulse dispatched** | **RESET FAILED / ABORTED** |

### 3.2 Indeterminacy of Exit Code $\ge 128$
- In Stage 2E.8.1, it was incorrectly implied that because `msleep(40)` is uninterruptible, an exit code $\ge 128$ meant the reset pulse completed.
- **Correction:** The caller observing exit status $rc \ge 128$ **cannot distinguish Case A from Case B or Case C**:
  - In **Case A**, $rc = 130$, and **no reset pulse occurred**.
  - In **Case B**, $rc = 130$, and **the reset pulse occurred**.
  - In **Case C**, $rc = 130$, and **the reset pulse occurred**.
- Therefore, inferring that $rc \ge 128$ proves the reset completed is **technically invalid**.
- If a signal interrupts the execution and the tool cannot establish whether the ioctl was entered and completed, the outcome must strictly be classified as:
  ```
  RESET OUTCOME UNCERTAIN — DO NOT RETRY
  ```
- **Why automatic retry is strictly prohibited:**
  If Case B or Case C occurred, the hardware was already pulsed. Issuing a second reset pulse could disrupt silicon in an unknown post-reset recovery state, violate power/settle timing windows, or cause electrical bus collisions.

### 3.3 What `msleep(40)` Establishes vs What It Does Not Establish

In [`kernel/fte3600.c:141-150`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L141-L150), `msleep()` is used:
```c
gpiod_set_value_cansleep(data->reset_gpiod, 0);
msleep(10);
gpiod_set_value_cansleep(data->reset_gpiod, 1);
msleep(20);
gpiod_set_value_cansleep(data->reset_gpiod, 0);
msleep(10);
```

In the Linux kernel, `msleep(msecs)` puts the calling task into `TASK_UNINTERRUPTIBLE` state via `schedule_timeout_uninterruptible()`.

#### What `msleep(40)` Establishes:
1. **Timing Atomicity:** If kernel execution reaches `fte3600_hw_reset_pulse()`, the 40 ms timing sequence cannot be cut short or fragmented by signals.
2. **Pulse Integrity:** The GPIO controller is guaranteed to hold Pad 14 asserted LOW for the intended 20 ms interval and allow the 10 ms settle intervals without being interrupted midway.
3. **Signal Deferral:** Signals delivered while inside `msleep()` are held pending and only delivered to userspace after `data->lock` is released and the syscall returns.

#### What `msleep(40)` Does NOT Establish:
1. **Does not establish that `fte3600_hw_reset_pulse()` was reached:** If a signal arrives in Case A (before `ioctl` or during `mutex_lock_interruptible`), `msleep(40)` is never executed.
2. **Does not prove physical pin voltage:** `gpiod_set_value_cansleep()` writes to the Intel PCH LPSS GPIO controller registers and returns `void`. It cannot detect an open circuit, disconnected flex cable, or broken trace.
3. **Does not prove sensor silicon responsiveness:** An electrical reset pulse sets silicon into its power-on state; it does not verify that the sensor accepted the reset, that its clocks are running, or that it is ready for SPI transfers. Sensor responsiveness is only proven by subsequent SPI chip ID/CRC interrogation.
4. **Does not guarantee userspace witnessed success:** A pending signal delivered upon syscall completion terminates the tool before it can report success to the shell.

---

## 4. Corrected No-Unload Invariant Wording

### 4.1 Previous Imprecise Wording
In Stage 2E.8.1, the report stated:
> *"On every termination path... the script leaves fte3600.ko loaded."*

This claim was factually imprecise. If the script aborts during Pre-flight Checks (before `insmod` is ever run), `fte3600.ko` was never loaded and is absent. Furthermore, if `fte3600.ko` was loaded prior to script execution, the harness does not "leave it loaded as its own" because it never owned the module.

### 4.2 The Correct, Precise Invariant
The definitive, unconditional invariant is:

$$\text{\bf THE HARNESS NEVER AUTOMATICALLY UNLOADS OR UNBINDS THE MODULE.}$$

This applies across all four distinct module states:

1. **Module Absent (Pre-flight Abort):**
   - *Condition:* Pre-flight Check (0 through 8) fails or execution is interrupted before `insmod`.
   - *Driver State:* `fte3600.ko` was never loaded and remains absent.
   - *Harness Action:* No unload is attempted because no module was inserted.
2. **Insertion Definitely Succeeded:**
   - *Condition:* Pre-flight passed, `insmod` returned 0, `INSERTION_STATE=DEFINITELY_SUCCEEDED`.
   - *Driver State:* `fte3600.ko` is loaded and bound.
   - *Harness Action:* The harness deliberately preserves the module loaded by Option 1 policy and logs manual removal commands for the administrator.
3. **Insertion Outcome Uncertain:**
   - *Condition:* Interruption during insertion window or inconclusive module query (`INSERTION_STATE=UNCERTAIN`).
   - *Driver State:* Module may or may not be present in the kernel.
   - *Harness Action:* The harness fails closed: it does NOT attempt removal. Manual inspection is required.
4. **Module Was Already Loaded Prior to Script Start:**
   - *Condition:* Pre-flight Check 7 observes `INITIAL_FTE3600_LOADED=1`.
   - *Driver State:* Module was loaded by an external administrator or system service.
   - *Harness Action:* The harness recognizes zero ownership and NEVER unloads or unbinds the pre-existing module.

---

## 5. Corrected Syntax-Error and Trap Limitations

### 5.1 Bash Parse-Time Syntax Errors vs Traps
- In Bash, script execution occurs in two distinct phases:
  1. **Lexical and Syntactical Parsing:** Bash reads the entire script into an internal abstract syntax tree (AST).
  2. **Execution:** Bash begins executing statements sequentially.
- If a script contains a parse-time syntax error (e.g. unclosed quotes, unbalanced brackets, missing `fi`/`done`, or unexpected tokens):
  - Bash writes an error to `stderr` and terminates immediately with exit status 2.
  - **Zero lines of shell script code are executed.**
  - Trap handlers registered on lines 1029–1032 ([`scripts/stage-b-test.sh:1029-1032`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L1029-L1032)) are **NEVER registered**.
  - `cleanup_handler()` does **NOT** execute.

### 5.2 Why the No-Unload Invariant Remains Fully Enforced
- The fact that traps do not run on parse-time syntax errors does **NOT** weaken or violate the no-auto-unload policy:
  1. If a parse-time syntax error occurs, the script never began execution; `insmod` was never called, so no module was loaded.
  2. The harness contains **ZERO** executable calls to `rmmod`, `modprobe -r`, `delete_module`, or `unbind` in any function or branch.
  3. Because unload commands simply do not exist in the codebase, the absence of cleanup execution can never trigger an unload or unbind.
- Therefore, the invariant holds unconditionally: even if a parse-time syntax error prevents cleanup traps from registering or executing, **the harness never automatically unloads or unbinds the module**.

---

## 6. Source Re-Inspection Across Complete Reachable Call Graph

All reachable source files were re-inspected line-by-line:

| File | Lines | Verified Properties |
| :--- | :--- | :--- |
| [`scripts/run-stage-b-in-terminal.sh`](file:///home/titan/fte3600-linux-driver/scripts/run-stage-b-in-terminal.sh) | 1–39 | Zero unload commands; invokes `stage-b-test.sh`; captures exit code; logs Option 1 policy banners; zero retries. |
| [`scripts/stage-b-test.sh`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh) | 1–1394 | Zero `rmmod`/`unbind`; cooperative lock FD 200 released cleanly; reset invoked strictly once; open descriptor check read-only. |
| [`tools/fte3600-tool.c`](file:///home/titan/fte3600-linux-driver/tools/fte3600-tool.c) | 1–103 | Strictly opens `/dev/fte3600`, issues single requested ioctl, closes `fd` on all paths; zero unloads/retries. |
| [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c) | 1–738 | Single-client lease; lockdep enforcement; `msleep(40)` in reset pulse; zero automatic unbind or self-removal. |
| [`tools/fw9369-mock/src/fw9369_main_hw.c`](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_main_hw.c) | 1–80 | Legacy safety gate binary source; permanently returns exit code 1 with sealed safety banner. |
| [`scratch/test_synthetic_suite.py`](file:///home/titan/fte3600-linux-driver/scratch/test_synthetic_suite.py) | 1–1044 | 27 offline tests; extracts active script functions; validates all 14 Option 1 paths; zero obsolete fingerprint tests. |

### Verification Invariants Confirmed:
1. **Zero Unload / Unbind Commands:** Re-verified with automated static search: 0 occurrences of `rmmod`, `modprobe -r`, `delete_module`, or driver `unbind` across all executable paths.
2. **Zero Process Termination:** No `kill`, `pkill`, or `killall` commands exist in the harness. `check_open_descriptors()` is strictly read-only.
3. **Zero Automatic Reset Retries:** `bin/fte3600-tool reset` is called at [`scripts/stage-b-test.sh:1319`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L1319) exactly once. There are no loops or retry constructs.
4. **Cooperative Lock Safety:** Lock file descriptor 200 is explicitly released via `flock -u 200` and `exec 200>&-` at [`scripts/stage-b-test.sh:1004-1005`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L1004-L1005) only after post-test diagnostics have completed.
5. **Secondary Reset Immunity:** Signal interruption during or after reset enters `cleanup_handler()`, which contains no hardware-facing operations and cannot re-trigger a reset.

---

## 7. Remaining Risks, Limitations, and Unresolved Questions

1. **Telemetry Message Distinction in `scripts/stage-b-test.sh`:**
   - *Current Source:* Line 1324–1326 checks `if [ "$RESET_RC" -ne 0 ]; then printf ... "[WARN] Reset ioctl failed with non-zero exit code ($RESET_RC)."; fi`.
   - *Limitation:* It does not explicitly emit the banner `RESET OUTCOME UNCERTAIN — DO NOT RETRY` when `$RESET_RC >= 128`.
   - *Impact:* The script does not automatically retry anyway (single execution only), but adding this explicit banner in future maintenance will provide even greater clarity for terminal operators.
2. **Static Scan Scope in `test_synthetic_suite.py` (Path 14):**
   - *Current Source:* Path 14 scans `scripts/stage-b-test.sh` and `scripts/run-stage-b-in-terminal.sh`.
   - *Limitation:* Does not include `tools/fte3600-tool.c` or mock C helper sources in its automated file scan list.
   - *Impact:* Manual source review has proven zero unload commands exist in the C tools; expanding Path 14 will formalize this in the automated test runner.
3. **Pre-flight Check 7 Policy:**
   - Pre-flight Check 7 detects if `fte3600` is already loaded before starting. It prints an informative banner and sets `INITIAL_FTE3600_LOADED=1`. It does not abort. If an operator requires testing only fresh module insertion, they must ensure the module is not loaded prior to launching.

---

## 8. Enforcement Confirmation: Option 1 Policy

The Option 1 policy ("Never Automatically Unload the Fingerprint Driver") is **strictly and comprehensively enforced**:
- No automatic unload or unbind exists anywhere in the codebase.
- No failure or signal condition can trigger an unload.
- Pre-existing modules are preserved untouched.
- Cleanups are strictly read-only and diagnostic.
- The cooperative lock is safely released without touching driver state.

---

## 9. Required Final Status Declarations

- **AUTOMATIC MODULE UNLOAD:** DISABLED
- **AUTOMATIC SPI UNBIND:** DISABLED
- **STAGE B HARDWARE EXECUTION:** NOT PERFORMED
- **STAGE B AUTHORIZATION:** ON HOLD
