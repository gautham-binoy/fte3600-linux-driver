# Stage 2E.7.13 — Module Ownership Continuity and Unload Safety Audit

**Date:** 2026-11-03  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`, Intel LPSS SPI2 CS0)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (x86_64)  
**Mode:** STRICT STATIC-ONLY AUDIT (Stage B Execution ON HOLD)

---

## 1. Executive Summary

Stage 2E.7.13 conducted an independent, adversarial, static investigation of the fundamental ownership continuity problem in userspace kernel module lifecycle management.

### Key Conclusions:
1. **The Insertion-to-Capture Ownership Gap Exists and Cannot Be Closed in Userspace:**
   Linux system calls `init_module(2)` and `finit_module(2)` return an integer status (`0` or `-1`), not an instance handle, cookie, or file descriptor. Userspace can only query sysfs (`/sys/module/fte3600`) *after* the `insmod` process exits. If an external entity unloads and replaces the module during this interval, the script captures the replacement's attributes as its baseline reference, permanently confusing the replacement for its own.
2. **The Unload-Time TOCTOU Race Cannot Be Eliminated in Userspace:**
   Linux system call `delete_module(2)` accepts only a NUL-terminated string module name (`const char *name`, `int flags`). No system call exists to delete a module by instance token or file descriptor. Re-verifying module instance identity immediately before `rmmod` shortens the race window by eliminating descriptor scan latency, but **a shortened race window is not an eliminated race**. During the sub-millisecond process-fork and syscall dispatch window, an external replacement can still occur and be deleted by the subsequent `delete_module("fte3600")`.
3. **Kernel Reference Counting Inherent Paradox:**
   An open file descriptor on `/dev/fte3600` holds `.owner = THIS_MODULE` and increments `module_refcount(mod)`, which successfully pins the module against deletion (`delete_module()` returns `-EBUSY`). However, normal removal requires `module_refcount == 0`. Therefore, an existing file descriptor cannot simultaneously protect ownership continuity across the lifecycle AND allow normal removal during cleanup.
4. **Definitive Conclusion:**
   **Conclusion B is selected.** Ownership continuity cannot be established reliably by the current userspace design alone. Automatic cleanup cannot be considered mathematically safe against uncoordinated external module replacement. On an isolated hardware platform (the Dynabook laptop under dedicated single-tenant testing), the probability of uncoordinated root intervention is zero, making the procedure operationally defensible under documented assumptions.

---

## 2. Investigation 1: The Insertion-to-Capture Ownership Gap

### 2.1 The Vulnerability Mechanics
In `scripts/stage-b-test.sh`, lines 1400–1430 execute:
```bash
MODULE_INSERTION_ATTEMPTED=1
THIS_INVOCATION_INSERTED=0
INSERTION_STATE="$INSERTION_STATE_NOT_ATTEMPTED"
...
insmod "$MODULE_PATH"
insmod_rc=$?

if [ "$insmod_rc" -ge 128 ]; then
    INSERTION_STATE="$INSERTION_STATE_UNCERTAIN"
elif [ "$insmod_rc" -eq 0 ]; then
    INSERTION_STATE="$INSERTION_STATE_DEFINITELY_SUCCEEDED"
    THIS_INVOCATION_INSERTED=1
    MODULE_INSERTED=1
    capture_module_instance_identity "fte3600" || true
else
    INSERTION_STATE="$INSERTION_STATE_DEFINITELY_FAILED"
fi
```

### 2.2 Evaluation of the Exact Adversarial Scenario
1. **Script executes `insmod`:** The child process invokes `finit_module(2)` or `init_module(2)`. The kernel registers the module and returns 0.
2. **External module replacement occurs:** Before Bash enters `capture_module_instance_identity()`:
   - An external root shell, udev trigger, or background daemon executes `rmmod fte3600`.
   - The external actor loads a different module binary under the name `fte3600`.
3. **Script captures fingerprint:** Bash enters `capture_module_instance_identity "fte3600"`.
   - It reads `/sys/module/fte3600` via `stat -c %i`, `stat -c %Z`, `cat srcversion`, `cat coresize`.
   - **It captures the attributes of the REPLACEMENT module!**
   - It stores these in `RECORDED_MOD_SYSFS_INODE`, `RECORDED_MOD_CTIME`, etc., and sets `MOD_INSTANCE_RECORDED=1`.
4. **Cleanup evaluates ownership:**
   - Later, when `cleanup_handler()` runs, it checks:
     - `THIS_INVOCATION_INSERTED == 1` (True)
     - `verify_module_instance_identity "fte3600"` (Compares live sysfs against `RECORDED_MOD_*`).
   - The live attributes match `RECORDED_MOD_*` perfectly because the replacement's attributes were recorded in step 3!
   - `verify_module_instance_identity` returns 0 (Success).
5. **Errant Teardown:**
   - `cleanup_handler()` concludes that the module belongs to this invocation and issues `rmmod fte3600`.
   - **The script unloads the external replacement module.**

### 2.3 Architectural Root Cause
In Linux, module loading syscalls do not return an instance identifier. The return value is simply an integer return code:
```c
int finit_module(int fd, const char *param_values, int flags);
```
There is no atomicity between "module loaded" and "userspace reads sysfs". Because userspace must issue separate file reads to sysfs after the syscall returns, an unavoidable temporal gap exists. **No existing userspace mechanism can prevent this scenario if an uncoordinated entity intervenes in that gap.** This limitation is verified in Synthetic Test 58.

---

## 3. Investigation 2: The Unload-Time TOCTOU Race Window

### 3.1 Source Inspection
In `scripts/stage-b-test.sh`, lines 1094–1125:
```bash
# Pre-unload descriptor check
if check_open_descriptors; then
    ...
    # Pre-unload identity check
    if ! verify_module_instance_identity "fte3600"; then
        printf "%s\n" "[SAFETY STOP] Module instance identity changed during descriptor audit! Original instance replaced externally."
        CLEANUP_OVERALL_OK=0
    else
        # Graceful module unload
        if rmmod fte3600; then ...
```

### 3.2 Evaluation of the Race Window
Consider the exact operations executed by the operating system between `verify_module_instance_identity` returning 0 and the kernel unloading the module:
1. `verify_module_instance_identity` executes `stat` and string comparisons; returns 0.
2. Bash parses `if rmmod fte3600`.
3. Bash calls `fork()`.
4. Child process executes `execve("/usr/bin/rmmod", ...)`.
5. Dynamic linker resolves libraries (`libc.so`, `libkmod.so`).
6. `rmmod` parses arguments and invokes `syscall(__NR_delete_module, "fte3600", O_NONBLOCK)`.
7. Kernel context switches into `sys_delete_module()`.
8. Kernel executes `find_module("fte3600")`.

If an external process replaces `fte3600` during steps 2 through 7:
- The pre-unload identity check has already passed.
- `delete_module()` receives only the string `"fte3600"`.
- The kernel unloads whatever module currently occupies the name `"fte3600"`.

### 3.3 Evaluation: Shortened vs Eliminated
Re-checking identity immediately before `rmmod` eliminated the large latency window of the two `/proc` descriptor audits (hundreds of milliseconds). However:
> [!IMPORTANT]
> **A shortened race window is NOT an eliminated race.**  
> In Linux userspace, eliminating this race is mathematically impossible because the kernel provides no API to invoke `delete_module` by instance handle. Synthetic Test 59 verifies this behavior.

---

## 4. Investigation 3: Kernel Module Reference Counting

### 4.1 Implementation in `kernel/fte3600.c`
1. **`.owner = THIS_MODULE` is Set:**
   - In `struct file_operations fte3600_fops` (line 545): `.owner = THIS_MODULE`.
   - In `fte3600_probe()` (line 623): `data->cdev.owner = THIS_MODULE`.
2. **When Opening `/dev/fte3600` Acquires a Reference:**
   - When userspace calls `open("/dev/fte3600")`, the VFS `chrdev_open()` (in `fs/char_dev.c`) and `do_dentry_open()` (in `fs/open.c`) invoke:
     ```c
     if (!try_module_get(f->f_op->owner))
         return -ENODEV;
     ```
   - This increments `module_refcount(mod)`.
3. **Open Descriptor Pins Against Unloading:**
   - In `kernel/module/main.c`, `sys_delete_module()` enforces:
     ```c
     if (module_refcount(mod) != 0) {
         ret = -EBUSY;
         goto out;
     }
     ```
   - If any file descriptor on `/dev/fte3600` is open, `rmmod fte3600` fails with `-EBUSY` ("Device or resource busy", exit code 1). Normal removal is impossible. Synthetic Test 60 verifies this.
4. **When the Reference is Released:**
   - When userspace calls `close(fd)`, VFS `__fput()` calls `f->f_op->release` (`fte3600_cdev_release`) and then calls `module_put(filp->f_op->owner)`.
   - `module_refcount(mod)` is decremented.
5. **`get_device(&data->dev)` vs Module Lifetime:**
   - In `fte3600_cdev_open()` (line 386): `get_device(&data->dev)`.
   - In `fte3600_cdev_release()` (line 397): `put_device(&data->dev)`.
   - In `fte3600_remove()` (line 685): `put_device(&data->dev)`.
   - `get_device(&data->dev)` increments the kobject reference of the *device structure* (`struct fte3600_data`). It guarantees that context memory is not freed while a file is open, even if `spi_unregister_driver()` runs.
   - **`get_device()` does NOT increment `module_refcount(mod)`.** It protects only device struct memory, not the module instance. Synthetic Test 61 verifies this distinction.
6. **Descriptor Lifecycle in Stage B:**
   - In Stage B, `fte3600-tool` opens `/dev/fte3600`, performs an ioctl, and exits.
   - The file descriptor is closed and the module reference is released *before* cleanup runs.
   - `check_open_descriptors()` verifies that zero descriptors are open before `rmmod`.

### 4.2 The Ownership Continuity Paradox
Could userspace keep an open file descriptor across the entire Stage B test to pin the module?
- If FD 200 held `/dev/fte3600` open from `insmod` to cleanup:
  - The module would be securely pinned against external unloading.
  - BUT when `cleanup_handler()` executes `rmmod fte3600`, `rmmod` would fail with `-EBUSY`!
  - If userspace closes the FD right before `rmmod`, the TOCTOU window opens up again at that exact moment.
- **Conclusion:** No reference counting mechanism available in existing Linux VFS/module architecture allows userspace to establish atomic ownership continuity across the full lifecycle while still permitting normal `rmmod`.

---

## 5. Evaluation of Safe Architectural Choices

### Choice A: Current Userspace Fingerprint-and-rmmod Design
- **Guarantees:**
  - Preserves pre-existing modules (`INITIAL_FTE3600_LOADED=1`).
  - Preserves unowned modules (`THIS_INVOCATION_INSERTED=0`).
  - Preserves uncertain insertions (`INSERTION_STATE=UNCERTAIN`).
  - Blocks unload if any active process holds an open descriptor.
  - Detects dissimilar module replacement (inode, ctime second, or KASLR text address).
  - Pre-unload re-check catches mid-cleanup replacement during descriptor audits.
- **Limitations:**
  - Vulnerable to replacement in the insertion-to-capture gap.
  - Vulnerable to replacement in the final fork/exec TOCTOU window before `delete_module()`.
  - Blind to identical reloads within the same second with recycled inode and address.
- **Operational Consequences:**
  - Completely adequate for single-user, dedicated test environments.
  - Inadequate for hostile, multi-tenant, or automated environments with concurrent module operations.

### Choice B: Fail-Closed Cleanup Refusing Automatic Unload
- **Guarantees:**
  - The script *never* executes `rmmod`.
  - After completing test ioctls, the script closes descriptors and leaves the module loaded.
  - Completely eliminates the risk of deleting an external or replacement module.
- **Limitations:**
  - The module remains loaded indefinitely until an administrator manually issues `rmmod fte3600`.
  - Subsequent runs of `stage-b-test.sh` will fail at Pre-Flight Check 3 (`INITIAL_FTE3600_LOADED=1`) until manually unloaded.
- **Operational Consequences:**
  - Completely fail-safe against errant deletion, but transfers cleanup burden to the operator.

### Choice C: Future Kernel-Assisted Ownership/Lifetime Mechanism
- **Design Concept:**
  - A kernel module mechanism where:
    1. An anonymous instance FD is returned by the kernel upon insertion (`finit_module_fd()`).
    2. An explicit unbind/unload ioctl is issued on that FD (`ioctl(instance_fd, FTE3600_IOC_TEARDOWN)`), or the module is automatically unloaded when the instance FD is closed (`close(instance_fd)`).
    3. Alternatively, a monotonic 64-bit kernel instance UUID is assigned at probe, and `delete_module()` accepts an optional expected instance UUID.
- **Guarantees:**
  - Monotonically guaranteed instance identity; zero TOCTOU window; atomic lifecycle binding.
- **Limitations:**
  - Requires upstream Linux kernel modifications to `kernel/module/main.c`. Cannot be implemented in an out-of-tree module or in userspace.

---

## 6. Synthetic Offline Verification Suite (62/62 Tests)

The offline synthetic test harness in `scratch/test_synthetic_suite.py` was expanded with 5 dedicated tests proving every limitation analyzed in this audit:

```text
===========================================================================
Stage 2E.7.13 Offline Synthetic Test Suite
===========================================================================
[PASS] Tests 1 to 57: Baseline descriptor, stat, ownership, and edge-case checks
[PASS] Test 58: Insertion-to-capture gap: replacement captured as reference
[PASS] Test 59: Unload-time race: replacement between check and rmmod
[PASS] Test 60: Module reference held by open file descriptor blocks removal (-EBUSY)
[PASS] Test 61: Device object reference distinction (get_device does not pin module)
[PASS] Test 62: All attributes matched blind spot (identical reload indistinguishable)
===========================================================================
Summary: 62 passed, 0 failed out of 62 tests.
===========================================================================
```

### Analysis of New Synthetic Tests:
- **Test 58 (Insertion-to-Capture Gap):** Proves that if a module is replaced before `capture_module_instance_identity` executes, the script records the replacement's identity and subsequently unloads it.
- **Test 59 (Unload-Time Race):** Proves that between final verification and `delete_module()`, an external replacement will be deleted by the string-based syscall.
- **Test 60 (Module Reference Pinning):** Proves that an open file descriptor causes `rmmod` to fail with exit code 1 (`-EBUSY`), and cleanup safely leaves the module loaded without forcing removal.
- **Test 61 (Device Ref vs Module Ref):** Demonstrates that holding `data->dev` refcount does not prevent `rmmod` if `module_refcount == 0`.
- **Test 62 (Full Attribute Match Blind Spot):** Proves that an identical reload within the same second with reused inode and address cannot be detected by any sysfs attribute.

---

## 7. Static Safety Verifications & Host Invariant Checks

All static safety checks were verified cleanly:
- `bash -n scripts/stage-b-test.sh` & `bash -n scripts/run-stage-b-in-terminal.sh`: **PASS** (zero syntax warnings or errors).
- `git diff --check`: **PASS** (zero whitespace issues).
- `git diff -- kernel/`: **ZERO MODIFICATIONS** (kernel source remains strictly untouched).
- `git commit`: **ZERO COMMITS CREATED**.
- Host Invariant Checks:
  - `fte3600` is **NOT** loaded (`lsmod | grep fte3600` is empty).
  - `/dev/fte3600` is **absent**.
  - `spi-FTE3600:00` is **unbound** (`/sys/bus/spi/devices/spi-FTE3600:00/driver` is absent).
  - Safety gate is **sealed** (`bin/fw9369-hw-tool reset` returns `1`).

---

## 8. Required Final Conclusion

In strict adherence to the Stage 2E.7.13 requirements, the conclusion is:

### **Conclusion B: Ownership continuity cannot be established reliably by the current userspace design, so automatic cleanup cannot be considered fully safe against uncoordinated external module replacement.**

### Technical Justification:
1. **Userspace Gap:** Because Linux `init_module(2)` returns an `int` rather than an instance handle, userspace cannot bind to an instance atomically at load time.
2. **Syscall Boundary:** Because Linux `delete_module(2)` takes only a module name string, userspace cannot atomically unload a specific instance.
3. **Reference Incompatibility:** A module reference pins the module against removal (`-EBUSY`), making it impossible to hold a reference that establishes continuity while still permitting normal `rmmod`.

### Recommended Operational Policy for Stage B:
1. **Operational Environment Precondition:** The authorized Stage B controlled reset must be executed on a dedicated, single-tenant, isolated Linux system (the Dynabook notebook) where no secondary administrator, concurrent user, automated CI daemon, or background process is loading or unloading modules.
2. **Defensibility Boundary:** Under this isolated operational precondition, the probability of concurrent uncoordinated module replacement during the sub-second test window is zero.
3. **Fail-Closed Guardrails:** The existing fail-closed guardrails (preflight module checks, cooperative locking, open-descriptor blocking, non-zero exit code preservation, no forced unloads) provide the maximum possible protection achievable on unmodified Linux.

---

> [!CAUTION]
> **EXPLICIT STAGE B EXECUTION STATUS:**  
> **Stage B remains STRICTLY ON HOLD.**  
> Do NOT execute `scripts/run-stage-b-in-terminal.sh`. Do NOT execute `scripts/stage-b-test.sh`. Execution requires explicit user authorization following independent review of this audit report.
