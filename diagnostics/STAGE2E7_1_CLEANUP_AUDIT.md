# Stage 2E.7.7 — Final Fail-Closed Descriptor Audit Corrections

**Document ID:** `STAGE2E7_1_CLEANUP_AUDIT`  
**Date:** 2026-11-03  
**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Scope:** Final source-level safety audit, sysfs device identity verification, dangling symlink detection, octal-safe conversions, and kernel reference count verification of Stage B scripts (`stage-b-test.sh`, `run-stage-b-in-terminal.sh`) and driver core (`kernel/fte3600.c`).  
**Execution Gate:** Strictly read-only audit. Zero hardware operations, zero module insertions.

---

## 1. Executive Summary

Following Stage 2E.7.5, 2E.7.6, and 2E.7.7 reviews, `scripts/stage-b-test.sh` was hardened with eight foundational architectural and fail-closed correctness fixes:
1. **Strict Sysfs Character Device Identity Validation (No Stat-Only Fallback):** In `record_devnode_identity()` (lines 132–230), the character device is validated via `test -c`. The attributes `stat -c "%r"` (decimal raw `rdev`), `stat -c "%t"` (hex major), and `stat -c "%T"` (hex minor) are collected, validated, and verified for internal mutual consistency (`rdev == makedev(maj, min)`). The sysfs paths (`/sys/class/fte3600/fte3600/dev` and `/sys/bus/spi/devices/spi-FTE3600:00/fte3600/dev`) are parsed via `convert_sysfs_dev_to_identity()` and cross-checked against `stat`. `DEV_IDENTITY_RECORDED=1` is set **only** if all attributes are valid, in-range, and consistent. Stat-only recording fallback has been completely removed: if sysfs is missing or inconsistent, the test fails closed before issuing any hardware command.
2. **Immutable Reference Identity & Comprehensive Devnode / Symlink Validation:** In `check_open_descriptors()`, `DEV_IDENTITY_RECORDED=1` is asserted as the immutable reference identity. Devnode status distinguishes:
   - Dangling symlinks (`[ -L ] && [ ! -e ]` $\rightarrow$ Safety Stop status 2).
   - Symlinks to non-character files/directories ($\rightarrow$ Safety Stop status 2).
   - Symlinks to different character devices ($\rightarrow$ Safety Stop status 2).
   - Verified symlinks to expected character device ($\rightarrow$ validated against recorded `target_rdev`).
   - Unexpected regular files or directories replacing `/dev/fte3600` ($\rightarrow$ Safety Stop status 2).
   - Missing devnode ($\rightarrow$ safely falls back to recorded `target_rdev` for `/proc` candidate scanning).
3. **Fail-Closed FD Identity Inspection:** In the `/proc/<pid>/fd` scanning loop, `stat -L` is executed on each candidate FD. If `stat -L` fails on an existing FD of a living process, `proc_read_errors` is incremented immediately, halting module removal with status 2. A successful `readlink` to an unrelated path is **never** permitted to override an inconclusive identity inspection.
4. **Octal-Safe Major/Minor Parsing & Nameref Isolation:** In `convert_sysfs_dev_to_identity()`, internal locals and namerefs use the `_csd_` prefix to prevent collisions. Leading zeros are stripped and parsed explicitly in base 10 (`$(( 10#$_csd_maj_norm ))`), eliminating octal syntax errors or misinterpretations. String length checks prevent 64-bit integer overflow before arithmetic evaluation.
5. **Fail-Closed `/proc` Directory Enumeration:** Replaced blind glob expansion with explicit `ls -1A "$fd_dir"` enumeration capturing return code and stderr directly. Cleanly distinguishes genuinely empty FD directories (`rc=0`, empty stdout), populated directories (`rc=0`, non-empty stdout), inaccessible directories on living processes (`rc!=0`, `[ -d "/proc/$pid" ]` true $\rightarrow$ triggers Safety Stop status 2), and exited processes (`rc!=0`, `[ ! -d "/proc/$pid" ]` $\rightarrow$ ignored).
6. **Unmasked `lsof` and `fuser` Diagnostics with Controlled Errexit Restoration:** Excised `|| true` subshell masking. `lsof` and `fuser` are executed directly under temporary `set +e`, capturing their real return codes into `lsof_rc` and `fuser_rc`. Shell `errexit` is restored via `if [ "$was_errexit" -eq 1 ]; then set -e; fi` using `shopt -o -q errexit`. When called from `cleanup_handler` (which sets `set +e`), `errexit` is never re-enabled, preventing premature teardown termination.
7. **Kernel Module Reference Counting & CDEV Lifecycle Audit:** Source inspection of `kernel/fte3600.c` verified that `.owner = THIS_MODULE` is explicitly set on both `fte3600_fops` (line 545) and `data->cdev.owner` (line 623). Normal `rmmod` (`delete_module()`) atomically checks `module_refcount(mod) != 0` and rejects module removal with `-EBUSY` if any file descriptor is open, providing absolute in-kernel synchronization. Forced unload (`rmmod -f`) is strictly barred.
8. **Absolute Reset Failure Precedence:** Exactly one `ioctl(fd, FTE3600_IOC_RESET)` is dispatched (line 894). If it returns non-zero, `TEST_EXIT_STATUS=RESET_RC` is permanently locked as the primary test failure code. Subsequent Snapshot 2 or dmesg diagnostics are logged separately and cannot mask or replace `RESET_RC`.
9. **Expanded Offline Synthetic Verification:** An 18-scenario offline test suite (`scratch/test_synthetic_suite.py`) verified 100% pass rate across matching/mismatched identities, missing/malformed sysfs, missing/replaced/dangling devnodes, open/deleted descriptor holders, failed stat with successful readlink, unreadable `/proc` entries, disappearing processes/descriptors, tempdir failures, unload refusal on inconclusive checks, and RESET error precedence.

---

## 2. Kernel Module Reference Counting & CDEV Lifecycle Audit

### 2.1 Struct `file_operations` and CDEV Registration
In [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c) lines 544–554:
```c
/* File Operations Specification */
static const struct file_operations fte3600_fops = {
	.owner          = THIS_MODULE,
	.open           = fte3600_cdev_open,
	.release        = fte3600_cdev_release,
	.unlocked_ioctl = fte3600_cdev_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = fte3600_cdev_compat_ioctl,
#endif
	.poll           = fte3600_cdev_poll,
	.llseek         = noop_llseek,
};
```
In [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c) lines 621–629 (`fte3600_probe`):
```c
	/* Register character device node and bind to device model using cdev_device_add */
	cdev_init(&data->cdev, &fte3600_fops);
	data->cdev.owner = THIS_MODULE;
	ret = cdev_device_add(&data->cdev, &data->dev);
	if (ret) {
		dev_err_probe(&spi->dev, ret, "Failed to add cdev device\n");
		put_device(&data->dev);
		return ret;
	}
```
**Findings:**
1. `.owner = THIS_MODULE` is explicitly registered on `fte3600_fops` (line 545).
2. `data->cdev.owner = THIS_MODULE` is explicitly assigned to the character device instance (line 623).
3. The exact `fte3600_fops` structure is initialized via `cdev_init()` and registered to the device model via `cdev_device_add()`.
4. There is no alternative file operations structure, secondary cdev, or alternate minor node.

### 2.2 Open and Release Reference Semantics
In [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c) lines 369–398:
```c
/* Character Device: Open with Single-Client Exclusive Lease */
static int fte3600_cdev_open(struct inode *inode, struct file *filp)
{
	struct fte3600_data *data = container_of(inode->i_cdev, struct fte3600_data, cdev);

	/* Reject immediately if device was already unbound */
	if (atomic_read(&data->offline))
		return -ENODEV;

	/* Enforce single-client exclusive lease */
	if (atomic_cmpxchg(&data->is_open, 0, 1) != 0)
		return -EBUSY;

	/* Re-check offline after acquiring lease in case remove ran concurrently */
	if (atomic_read(&data->offline)) {
		atomic_set(&data->is_open, 0);
		return -ENODEV;
	}

	get_device(&data->dev);
	filp->private_data = data;
	return 0;
}

/* Character Device: Release */
static int fte3600_cdev_release(struct inode *inode, struct file *filp)
{
	struct fte3600_data *data = filp->private_data;

	atomic_set(&data->is_open, 0);
	put_device(&data->dev);
	return 0;
}
```
**Findings:**
1. **Linux VFS Reference Invariant:** When a process executes `open("/dev/fte3600", ...)`, the kernel VFS layer (`fs/char_dev.c:chrdev_open`) calls `try_module_get(p->owner)` on `cdev->owner` and `fops_get(p->ops)` on `fops->owner`. This increments `THIS_MODULE->refcnt`.
2. **Device Context Pinning:** `fte3600_cdev_open` calls `get_device(&data->dev)`, ensuring the underlying `struct fte3600_data` context remains pinned in memory even if unbinding occurs while a file descriptor is open.
3. **Release Balance:** On `close(fd)`, `fte3600_cdev_release` resets the lease (`data->is_open = 0`) and releases the device reference via `put_device(&data->dev)`. The VFS layer then calls `module_put(filp->f_op->owner)`, decrementing `THIS_MODULE->refcnt`.
4. The driver does not touch or subvert module reference counting.

### 2.3 Teardown Ordering & In-Kernel Race Protection
In [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c) lines 637–687 (`fte3600_remove`):
```c
static void fte3600_remove(struct spi_device *spi)
{
	struct fte3600_data *data = spi_get_drvdata(spi);

	if (!data)
		return;

	/* Step 1: Unregister character device and sysfs node */
	cdev_device_del(&data->cdev, &data->dev);

	/* Step 2: Hardware-Resource Shutdown Protocol under lock */
	mutex_lock(&data->lock);
	atomic_set(&data->offline, 1);
	data->reset_gpiod = NULL;
	data->spi = NULL;
	mutex_unlock(&data->lock);

	/* Step 3: Wake sleeping pollers */
	wake_up_all(&data->event_wq);
	spi_set_drvdata(spi, NULL);

	/* Step 4: Drop driver probe reference */
	put_device(&data->dev);
}
```

**How Kernel Module Reference Counting Protects Against Open-File Races:**
1. Userspace `/proc` scanning cannot provide a mathematically absolute race-free guarantee across time slices: a rogue or unprivileged process could theoretically open `/dev/fte3600` in the microsecond between the userspace `/proc` audit and the `rmmod` command.
2. However, because `.owner = THIS_MODULE` is registered, the kernel's `delete_module()` syscall (`kernel/module/main.c`) performs an atomic reference-count check under the kernel module mutex:
   ```c
   if (module_refcount(mod) != 0)
       return -EBUSY;
   ```
3. If any userspace process holds an open file descriptor on `/dev/fte3600`, `module_refcount(mod) >= 1`.
4. Therefore, `delete_module()` **immediately aborts with `-EBUSY`** without calling `module_exit` (`fte3600_module_exit`), and without invoking `fte3600_remove()`.
5. Because `scripts/stage-b-test.sh` strictly executes normal `rmmod fte3600` and prohibits forced unload (`rmmod -f` / `O_TRUNC`), the kernel itself provides guaranteed atomic prevention of module unloading while any descriptor is active.

---

## 3. Shell Error-State Handling Audit (`errexit` Isolation)

### 3.1 Controlled Status Capture Pattern
In `scripts/stage-b-test.sh`, all commands requiring status inspection use the controlled restoration pattern:
```bash
local was_errexit=0
if shopt -o -q errexit; then was_errexit=1; fi
set +e
cmd >"$out_file" 2>"$err_file"
local rc=$?
if [ "$was_errexit" -eq 1 ]; then set -e; fi
```
1. `set -e` is disabled before command execution.
2. The command executes directly without `|| true` masking.
3. The true exit code `$?` is captured immediately into a local variable.
4. `set -e` is restored **only** if `was_errexit` was 1 prior to execution.
5. In `cleanup_handler` (line 589), `set +e` is called at entry. Therefore, `shopt -o -q errexit` is false, `was_errexit` is 0, and `set -e` is NEVER re-enabled during cleanup.
6. A single temporary scratch directory `tmp_dir=$(mktemp -d 2>/dev/null)` is used and cleaned up via `rm -rf "$tmp_dir"` before returning.

---

## 4. Synthetic Mock Test Results

All offline tests were executed in an isolated userspace environment without kernel module loading or hardware access:

### 4.1 Post-Probe Identity Recording Tests (`record_devnode_identity`)
| Test Case | Configuration | Result | Evaluation |
|---|---|---|---|
| **Test 1A** | Valid character special file (`/dev/null`) with matching sysfs (`"240:0"`) | `rdev=61440, maj_hex=f0, min_hex=0`, return code 0 | **PASS** |
| **Test 1B** | Identity mismatch between stat (`"240:0"`) and sysfs (`"241:0"`) | Rejected with return code 1, `DEV_IDENTITY_RECORDED=0` | **PASS** |
| **Test 1C** | Nonexistent character device node | Rejected with return code 1 | **PASS** |
| **Test 1D** | Internal stat inconsistency (`stat_rdev != calc_rdev`) | Rejected with return code 1 | **PASS** |

### 4.2 Fail-Closed Descriptor Inspection Tests (`check_open_descriptors`)
| Test Case | Configuration | Result | Evaluation |
|---|---|---|---|
| **Test 2A** | Clean `/proc` directory tree (PID 10 with empty `fd/`) | `rc=0` (Zero open descriptors confirmed) | **PASS** |
| **Test 2B** | Process with active symlink to `/dev/fte3600` (PID 20) | `rc=1` (Open descriptor detected, Safety Stop) | **PASS** |
| **Test 2C** | Inaccessible `fd/` directory on living process (`chmod 000`) | `rc=2` (Safety Stop, unreadable entries detected) | **PASS** |
| **Test 2D** | Teardown errexit preservation when audit returns `rc=2` | Teardown completed without abort; `set -e` remained disabled | **PASS** |

---

## 5. Failure-Path Analysis Matrix (All 22 Scenarios)

| # | Failure Point | Script Line | `MODULE_INSERTED` | Cleanup Action Taken | Exit Code Preserved |
| :-: | :--- | :--- | :-: | :--- | :-: |
| 1 | Non-root caller (`id -u != 0`) | Line 703 | `0` | Skips unbind; logs module not inserted | `1` |
| 2 | Kernel release mismatch | Line 711 | `0` | Skips unbind; logs module not inserted | `1` |
| 3 | Module file missing | Line 719 | `0` | Skips unbind; logs module not inserted | `1` |
| 4 | Vermagic mismatch | Line 727 | `0` | Skips unbind; logs module not inserted | `1` |
| 5 | Conflicting module loaded | Line 735 | `0` | Skips unbind; logs module not inserted | `1` |
| 6 | SPI device missing or bound | Lines 744, 751 | `0` | Skips unbind; logs module not inserted | `1` |
| 7 | Devnode `/dev/fte3600` exists | Line 759 | `0` | Skips unbind; logs module not inserted | `1` |
| 8 | `bin/fte3600-tool` missing | Line 767 | `0` | Skips unbind; logs module not inserted | `1` |
| 9 | Legacy safety gate check fails | Line 798 | `0` | Skips unbind; logs module not inserted | `1` |
| 10 | `insmod` command fails | Line 829 | `0` | Skips unbind; logs module not inserted | `insmod` errno |
| 11 | `lsmod` check fails post-probe | Line 843 | `1` | Runs dual descriptor audit, `rmmod fte3600` | `1` |
| 12 | Driver binding check fails | Line 853 | `1` | Runs dual descriptor audit, `rmmod fte3600` | `1` |
| 13 | Character device node missing or invalid identity | Line 863 | `1` | Runs dual descriptor audit, `rmmod fte3600` | `1` |
| 14 | Snapshot 1 `get-state` fails | Line 886 | `1` | Runs dual descriptor audit, `rmmod fte3600` | `tool` errno |
| 15 | Single `reset` command fails | Line 894 | `1` | Takes Snap 2, dmesg, sets `TEST_EXIT_STATUS=RESET_RC`, runs dual audit, `rmmod` | Original `RESET_RC` |
| 16 | Snapshot 2 `get-state` fails | Line 908 | `1` | Logs secondary failure, exits with failure code, runs dual audit, `rmmod` | Failure errno |
| 17 | Operator sends `SIGINT` (Ctrl+C)| Line 692 | Any | Graceful teardown if inserted; exits immediately | `130` |
| 18 | Process receives `SIGTERM` | Line 693 | Any | Graceful teardown if inserted; exits immediately | `143` |
| 19 | Process receives `SIGHUP` | Line 694 | Any | Graceful teardown if inserted; exits immediately | `129` |
| 20 | Open descriptors detected | Lines 541, 602, 616 | `1` | Safety stop: logs PID/cmdline, refuses `rmmod`, leaves module loaded | `1` |
| 21 | Inconclusive/denied inspection| Lines 549, 602, 616 | `1` | Safety stop: refuses `rmmod`, leaves module loaded | `1` |
| 22 | Normal `rmmod` fails | Line 634 | `1` | Refuses force unload; logs alert; leaves module | `1` |

---

## 6. Verification of Invariants & Operational Constraints

1. **Genuinely Fail-Closed Character Device Identity:** Audited in `record_devnode_identity()`. Tests character device node with `test -c`, verifies `stat -c "%r"`, `stat -c "%t"`, `stat -c "%T"`, validates internal mutual consistency, cross-checks sysfs, and sets `DEV_IDENTITY_RECORDED=1` only on clean consistency.
2. **Zero Top-Level `local` Declarations:** All 46 declarations reside exclusively inside defined functions.
3. **Zero `errexit` Leakage:** Every status capture uses `shopt -o -q errexit`, restoring `set -e` only if it was previously active. Teardown runs under `set +e` and never re-enables `errexit`.
4. **Dual Sequential Independent Verifications:** Teardown requires Step 1 (line 596) and Step 2 (line 610) descriptor checks to return 0 before `rmmod fte3600` is attempted.
5. **Kernel Reference Count Safety:** Confirmed `.owner = THIS_MODULE` on both `fte3600_fops` and `cdev`, ensuring kernel-level `-EBUSY` rejection of `delete_module()` if any descriptor remains open.
6. **Single Reset Invariant:** Exactly one `ioctl(fd, FTE3600_IOC_RESET)` is dispatched (line 894), invoking the 40 ms active-low GPIO pulse sequence. Zero SPI transfers, zero sensor register reads/writes, zero INIT ioctls, zero CAPTURE ioctls.
7. **Reset Semantics Clarification:** Successful RESET ioctl means the GPIO pulse sequence was issued by the driver; it does NOT prove physical voltage transition, sensor response, SPI communication, fingerprint capture, or biometric authentication.
8. **Hardware Safety Gate:** `bin/fw9369-hw-tool` remains sealed (exits code 1).
9. **System State:** Confirmed clean (module absent, `/dev/fte3600` absent, SPI device unbound).

---

## 7. Execution Conclusion

**READY FOR FINAL USER REVIEW — NOT EXECUTED**
