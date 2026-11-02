# FTE3600 Linux Driver — Stage 1 Dependency Fix Report

**Date/Time:** 2026-11-02T10:11:00+05:30  
**Target Kernel:** `7.2.8-9-omarchy` (x86_64)  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Modified File:** [`scripts/stage1-test.sh`](file:///home/titan/fte3600-linux-driver/scripts/stage1-test.sh)

---

## 1. Original Failure and Root Cause

- **Failure Message:**
  ```text
  insmod: ERROR: could not insert module /home/titan/fte3600-linux-driver/kernel/fte3600.ko: Unknown symbol in module
  Error: Failed to insert /home/titan/fte3600-linux-driver/kernel/fte3600.ko (code 1)!
  ```
- **Root Cause:**
  - `kernel/fte3600.c` references 3 Userspace I/O symbols: `uio_event_notify`, `__uio_register_device`, and `uio_unregister_device`.
  - In the running Omarchy Linux kernel (`7.2.8-9-omarchy`), Userspace I/O is built as a module (`CONFIG_UIO=m`), not built into the core kernel (`CONFIG_UIO=y`).
  - `stage1-test.sh` previously invoked `insmod "$MODULE_PATH"`. Unlike `modprobe`, `insmod` executes the raw `finit_module` syscall without resolving or loading prerequisite modules from `modules.dep`.
  - Because `uio.ko.zst` was absent from the kernel module list, the in-kernel module loader rejected `fte3600.ko` with `-ENOENT` (`Unknown symbol in module`).
  - Analysis confirmed that **zero** driver source modifications were needed; all other 70 undefined symbols are exported directly by `vmlinux`.

---

## 2. Files Modified

- **Modified:** [`scripts/stage1-test.sh`](file:///home/titan/fte3600-linux-driver/scripts/stage1-test.sh)
- **Unmodified:**
  - [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c) (retained bit-for-bit)
  - [`kernel/fte3600-policy.h`](file:///home/titan/fte3600-linux-driver/kernel/fte3600-policy.h) (retained bit-for-bit)
  - [`kernel/Makefile`](file:///home/titan/fte3600-linux-driver/kernel/Makefile) (retained bit-for-bit)
  - [`kernel/fte3600.ko`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.ko) (retained bit-for-bit)
  - [`scripts/run-stage1-in-terminal.sh`](file:///home/titan/fte3600-linux-driver/scripts/run-stage1-in-terminal.sh) (retained bit-for-bit)

---

## 3. Exact Implementation Changes

### A. UIO Baseline State Detection (Check 7)
Before modifying any system state, `scripts/stage1-test.sh` executes Check 7 using independent three-state inspection (`get_module_state "uio"`):
- **`ABSENT`:** Marks `UIO_PRE_EXISTING=0` (eligible to be loaded on demand and tracked for rollback).
- **`PRESENT`:** Marks `UIO_PRE_EXISTING=1` (pre-existing module; will be preserved and **never** unloaded during rollback).
- **`UNKNOWN`:** Aborts script execution immediately before any modifying commands.

### B. Prerequisite Module Loading (Section 2a)
If `UIO_PRE_EXISTING=0`:
- Executes `modprobe uio` inside a critical section (`enter_critical` / `leave_critical`) with deferred signal handling.
- Reconciles actual module state using `get_module_state "uio"`.
- Sets `UIO_LOADED_BY_US=1` **only** upon confirmed `PRESENT` state.
- If post-load state is `UNKNOWN` or `ABSENT`, sets `STATE_UNCERTAIN=1`, halts execution, and initiates rollback.
- If `UIO_PRE_EXISTING=1`, skips loading and logs that pre-existing UIO is preserved.

### C. Dependency-Safe Rollback Ordering (Cleanup Step 5)
Strict teardown ordering is enforced:
1. **Rollback Step 1:** Remove `fte3600` via `rmmod fte3600` → verify `fte_confirmed_absent=1`.
2. **Rollback Step 2:** Unbind `spi-FTE3600:00` from `spidev` (strictly gated on `fte_confirmed_absent=1`) → verify `spi_confirmed_unbound=1`.
3. **Rollback Step 3:** Restore `driver_override` on `spi-FTE3600:00` (strictly gated on `spi_confirmed_unbound=1`).
4. **Rollback Step 4:** Unload `spidev` via `modprobe -r spidev` (strictly gated on `fte_confirmed_absent=1` AND `spi_confirmed_unbound=1`).
5. **Rollback Step 5 (New):** Unload `uio` via `modprobe -r uio`:
   - Gated on `UIO_LOADED_BY_US=1` and `fte_confirmed_absent=1`.
   - Never executed if `UIO_PRE_EXISTING=1`.
   - Never forced (`modprobe -r uio` respects in-kernel module reference counts).
   - Verifies postcondition with three-state inspection:
     - `ABSENT`: clears `UIO_LOADED_BY_US=0`.
     - `PRESENT`: logs failure, increments `CLEANUP_ERRORS`, retains `UIO_LOADED_BY_US=1`.
     - `UNKNOWN`: logs failure, increments `CLEANUP_ERRORS`, sets `STATE_UNCERTAIN=1`, retains `UIO_LOADED_BY_US=1`.

### D. Post-Rollback State Audit
- Audits `fte3600`, `spidev`, and `uio` independently.
- If `UIO_PRE_EXISTING=1`, verifies that `uio` remains `PRESENT`.
- If `UIO_PRE_EXISTING=0`, verifies that `uio` is `ABSENT`.
- Requires `fte3600 ABSENT`, `spidev ABSENT`, `spi-FTE3600:00 UNBOUND`, and `uio clean` before reporting clean rollback.

---

## 4. Ownership Tracking & Safeguard Invariants

| Safety Invariant | Implementation in `stage1-test.sh` | Status |
| :--- | :--- | :---: |
| **Three-State Authority** | Every module and driver state evaluation uses `PRESENT`, `ABSENT`, or `UNKNOWN`. `UNKNOWN` never counts as success. | **Preserved** |
| **Deferred Signals** | Critical sections defer SIGINT/SIGTERM until state flags are updated; cleanup executes with proper exit codes (130/143). | **Preserved** |
| **Postcondition Authority** | Rollback success is judged solely by postcondition inspection, not by command exit status alone. | **Preserved** |
| **Gated Dependent Unloads** | Unbinding `spidev` requires confirmed `fte3600` absence; unloading `spidev` requires confirmed unbind; unloading `uio` requires confirmed `fte3600` absence. | **Preserved** |
| **Pre-Existing Module Preservation** | If `uio` was loaded before the test, `UIO_PRE_EXISTING=1` prevents `modprobe -r uio` from ever being attempted. | **Preserved** |

---

## 5. Validation Results

- `bash -n scripts/stage1-test.sh`: **PASS (exit code 0, 0 syntax errors)**
- `bash -n scripts/run-stage1-in-terminal.sh`: **PASS (exit code 0, 0 syntax errors)**
- Zero files modified outside `scripts/stage1-test.sh`.

---

## 6. Remaining Risks

- **Host Concurrency:** As userspace bash lacks an atomic system-wide kernel lock, external udev events or concurrent administrator commands during test execution must be avoided.
- **Root Credentials:** Execution requires interactive `sudo` in the user's terminal.

---

## 7. Final Readiness Verdict

### **READY FOR MANUAL EXECUTION**

The Stage 1 test harness is fully dependency-aware, strictly gated, and ready for manual execution by the user:

```bash
cd /home/titan/fte3600-linux-driver
./scripts/run-stage1-in-terminal.sh
```
*(Or directly: `sudo ./scripts/stage1-test.sh`)*
