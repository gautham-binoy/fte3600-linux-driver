# Stage 2E.2 — Restricted Unified Kernel Driver Software Implementation Report

**Target System:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Kernel Release:** `7.2.8-9-omarchy` (SMP preempt mod_unload)  
**Host Compiler:** `gcc (GCC) 16.2.1 20260810`  
**Document ID:** `STAGE2E2_KERNEL_IMPLEMENTATION_REPORT`  
**Execution Mode:** STRICTLY SOFTWARE-ONLY (Kernel Source Implementation, Compilation, and Offline Test Harness)  
**Hardware Safety Gate:** **CLOSED (`bin/fw9369-hw-tool` exits with code 1 unconditionally)**  

---

## 1. Executive Summary & Authoritative Stage Verdict

Stage 2E.2 delivers the first software-only implementation of the unified Linux kernel driver `fte3600.ko`. The driver replaces the fragmented Stage 1 platform-glue / spidev notifier model with a native, direct ACPI `spi_driver` architecture managing SPI transport, active-low GPIO reset, and a single-client character device node (`/dev/fte3600`).

The kernel module was compiled directly against running kernel build headers (`/lib/modules/7.2.8-9-omarchy/build`) with **zero compiler warnings and zero errors**. All UAPI structures, state machine transitions, concurrency primitives, and reference lifetimes were validated by a dedicated 10-test offline harness.

### Authoritative Stage Verdict

$$\mathbf{SOFTWARE\;IMPLEMENTATION\;COMPILED\;—\;HARDWARE\;UNTESTED}$$

---

## 2. Implemented Driver Architecture

### 2.1 Native ACPI SPI Driver Registration
- **Direct Matching:** Implemented `struct spi_driver fte3600_spi_driver` with `.acpi_match_table` matching ACPI ID `"FTE3600"` (`kernel/fte3600.c:42-45`).
- **Elimination of Stage 1 Glue:** Removed `bus_register_notifier(&spi_bus_type, ...)` and spidev child-binding hooks.
- **Probe Semantics (Policy A):** Probe is strictly passive:
  - Registers ACPI driver GPIO mapping table (`fte3600_acpi_gpios`) on `&spi->dev`.
  - Acquires reset descriptor via `devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_LOW)`.
  - Configures SPI Mode 0, 8-bit words, and 1 MHz clock via `spi_setup()`.
  - Allocates and registers character device `/dev/fte3600`.
  - Sets driver state to `FTE3600_STATE_UNINITIALIZED`.
  - **Zero reset pulses are driven and zero SPI transactions are issued during probe.**

```
ACPI FP01 ("FTE3600") ──► Linux SPI Core
                                │
                        fte3600.ko (spi_driver)
                        ├── devm_acpi_dev_add_driver_gpios()
                        ├── devm_gpiod_get("reset", GPIOD_OUT_LOW)
                        ├── spi_setup(Mode 0, 8-bit, 1 MHz)
                        ├── struct mutex lock (Reset & SPI serialization)
                        └── /dev/fte3600 (Character Device Node)
```

---

## 3. UAPI Specification (`kernel/fte3600.h`)

All UAPI structures are fixed-width, 64-bit aligned, and identical across 32-bit compat and 64-bit native ABIs:

```c
#define FTE3600_UAPI_VERSION      1
#define FTE3600_IOC_MAGIC         'F'

/* Telemetry Snapshot */
struct fte3600_state_snapshot {
    __u32 version;        /* Must equal FTE3600_UAPI_VERSION */
    __u32 state;          /* enum fte3600_state */
    __u32 flags;          /* Status bitmask (FTE3600_FLAG_OFFLINE, etc.) */
    __u32 last_error;     /* Most recent errno */
    __u32 reserved[4];    /* Must be 0 */
}; /* Exactly 32 bytes */

/* Initialization Parameters */
struct fte3600_init_params {
    __u32 version;        /* Must equal FTE3600_UAPI_VERSION */
    __u32 flags;          /* Control flags */
    __u32 reserved[6];    /* Must be 0 */
}; /* Exactly 32 bytes */

/* Frame Buffer Description */
struct fte3600_frame_buffer {
    __u32 version;        /* Must equal FTE3600_UAPI_VERSION */
    __u32 length;         /* Must equal FTE3600_IMAGE_BYTES (10240) */
    __u64 buffer_ptr;     /* Userspace virtual address (64-bit aligned) */
    __u32 reserved[4];    /* Must be 0 */
}; /* Exactly 32 bytes */

/* IOCTL Definitions */
#define FTE3600_IOC_GET_STATE _IOR(FTE3600_IOC_MAGIC, 0x01, struct fte3600_state_snapshot)
#define FTE3600_IOC_RESET     _IO(FTE3600_IOC_MAGIC, 0x02)
#define FTE3600_IOC_INIT      _IOWR(FTE3600_IOC_MAGIC, 0x03, struct fte3600_init_params)
#define FTE3600_IOC_CAPTURE   _IOWR(FTE3600_IOC_MAGIC, 0x04, struct fte3600_frame_buffer)
```

---

## 4. Reset Implementation and Unverified Hardware Status

### 4.1 Reset Execution Sequence
Reset is executed only when explicitly requested via `ioctl(fd, FTE3600_IOC_RESET)` (`kernel/fte3600.c:92-114`):
1. Acquires `mutex_lock_interruptible(&data->lock)`.
2. Sets state to `FTE3600_STATE_RESETTING`.
3. Settle line: `gpiod_set_value_cansleep(data->reset_gpiod, 0)` (Logical 0 = physical HIGH), wait 10 ms.
4. Assert reset: `gpiod_set_value_cansleep(data->reset_gpiod, 1)` (Logical 1 = physical LOW), hold 20 ms.
5. Deassert reset: `gpiod_set_value_cansleep(data->reset_gpiod, 0)` (Logical 0 = physical HIGH), settle 10 ms.
6. Sets state to `FTE3600_STATE_RESET_COMPLETE`.
7. Releases mutex.

### 4.2 Unverified Hardware Status
- A successful return from `gpiod_set_value_cansleep()` proves solely that the PCH LPSS GPIO pad register accepted the write.
- Physical voltage on Pad 14 has **NOT** been measured or verified.
- The reset pulse does **NOT** transition state to `READY`. Transition to `READY` strictly requires subsequent verified SPI chip ID and CRC identification.
- The reset ioctl was implemented and unit-tested in software simulation, but **was never invoked against physical hardware**.

---

## 5. SPI Transport Implementation & Wire CRC Validation

### 5.1 Transport Engine
- In-kernel synchronous transfer helper `fte3600_spi_sync_transfer()` (`kernel/fte3600.c:117-142`):
  - Uses `struct spi_transfer` with `cs_change = 0`.
  - Dispatches via `spi_sync(data->spi, &m)`.
  - Allocated dynamically with standard `GFP_KERNEL` (prohibiting `GFP_DMA`).
  - Automatically transitions state to `FTE3600_STATE_UNKNOWN` on transport error.

### 5.2 Wire CRC-16 Calculation
- Implemented `fte3600_crc16_wire()` (`kernel/fte3600.c:60-77`):
  - CCITT polynomial `0x1021`, initial seed `0xFFFF`, MSB-first.
  - Matches test vector `[ 0x93, 0x62 ] -> 0x1C53`.

---

## 6. Deferred FIFO and IRQ Features

In accordance with strict safety mandates from Stage 2E.1.2:

1. **FIFO Frame Capture Deferred:**
   - Prefix discrepancy (`[06 F9 ...]` vs `[10 EF ...]`) and PXA2xx DMA constraints remain under review.
   - `FTE3600_IOC_CAPTURE` validates parameters and unconditionally returns **`-EOPNOTSUPP`**. Zero physical FIFO burst transfers are executed.
2. **Interrupt Servicing Deferred:**
   - Physical interrupt acknowledgement via `0x1A84` remains **BLOCKED**.
   - No IRQ handler registers for Pad 229 in this milestone.
   - No writes to register `0x1A84` or `0x1A82` occur.
   - Direct memory-mapped access to PCH Pad 229 `PADCFG` registers is strictly barred.

---

## 7. Concurrency, Lifetime, and Teardown Model

1. **Atomic Single-Client Open:**
   - `fte3600_cdev_open()` executes `atomic_cmpxchg(&data->is_open, 0, 1)`.
   - Concurrent open attempts immediately fail with **`-EBUSY`**.
2. **Signal Interruption:**
   - All ioctls acquire mutexes via `mutex_lock_interruptible(&data->lock)`. Processes receiving `SIGINT` or `SIGTERM` abort cleanly with `-ERESTARTSYS`.
3. **Reference-Counted Lifetime (`kref`):**
   - Probe initializes `kref = 1`. Each `open()` increments `kref`. Each `release()` decrements `kref`.
   - On driver unbind (`fte3600_remove()`), the probe reference is dropped, but memory is preserved until all active file descriptors are closed (`fte3600_destroy_context()`).
4. **Shutdown / Offline State:**
   - `fte3600_remove()` sets `atomic_set(&data->offline, 1)` and calls `wake_up_all(&data->event_wq)`.
   - All subsequent open, ioctl, and poll calls return **`-ENODEV`** or `EPOLLHUP | EPOLLERR`.

---

## 8. Actual Kernel Compilation Results

### 8.1 Build Environment
- **Running Kernel:** `7.2.8-9-omarchy`
- **Build Directory:** `/lib/modules/7.2.8-9-omarchy/build`
- **Compiler:** `gcc (GCC) 16.2.1 20260810`
- **Build Command:**
  ```bash
  make -C /lib/modules/7.2.8-9-omarchy/build M=/home/titan/fte3600-linux-driver/kernel clean
  make -C /lib/modules/7.2.8-9-omarchy/build M=/home/titan/fte3600-linux-driver/kernel modules
  ```

### 8.2 Compiler Output
```text
make: Entering directory '/usr/lib/modules/7.2.8-9-omarchy/build'
make[1]: Entering directory '/home/titan/fte3600-linux-driver/kernel'
  CC [M]  fte3600.o
  MODPOST Module.symvers
  CC [M]  fte3600.mod.o
  CC [M]  .module-common.o
  LD [M]  fte3600.ko
  BTF [M] fte3600.ko
make[1]: Leaving directory '/home/titan/fte3600-linux-driver/kernel'
make: Leaving directory '/usr/lib/modules/7.2.8-9-omarchy/build'
```
**Compilation Result: ZERO WARNINGS, ZERO ERRORS (Exit code 0).**

### 8.3 Module Metadata (`modinfo`)
```text
filename:       /home/titan/fte3600-linux-driver/kernel/fte3600.ko
version:        1.0.0
license:        GPL
author:         Antigravity Project
description:    Unified Linux Driver for FocalTech FT9362 / FW9369 Fingerprint Sensor
srcversion:     83F535855562FA1E1E74DA3
alias:          acpi*:FTE3600:*
depends:        
name:           fte3600
retpoline:      Y
vermagic:       7.2.8-9-omarchy SMP preempt mod_unload 
```

---

## 9. Offline Verification Results

### 9.1 Stage 2E.2 Kernel Unit Test Suite (`kernel/fte3600-test`)
A dedicated offline test program (`kernel/fte3600-test.c`) was authored and executed:

```text
===================================================================
 FTE3600 Unified Kernel Driver Software Test Suite (Stage 2E.2)
 Mode: Software Implementation Validation (Zero Hardware Access)
===================================================================

--- Test Suite 1: UAPI & Data Structure Alignment ---
  Testing: UAPI structures exact 32-byte size and alignment ... [PASS]
  Testing: UAPI protocol constants (10246 B wire, 10240 B image, v1) ... [PASS]
  Testing: IOCTL command encodings (magic 'F', sizes, directions) ... [PASS]

--- Test Suite 2: Protocol CRC Vector Validation ---
  Testing: Wire CRC (0x1C53) and Word LE CRC (0x313E, 0x5B7C, 0xE4AA) ... [PASS]

--- Test Suite 3: State Machine Transitions & Invariants ---
  Testing: 8-state model, UNKNOWN fail-closed invariant, reset recovery ... [PASS]

--- Test Suite 4: Character Device Concurrency & Single-Open ---
  Testing: Single-open atomic lease rejects concurrent open with -EBUSY ... [PASS]
  Testing: Threaded concurrent IOCTLs serialized strictly under mutex ... [PASS]

--- Test Suite 5: Reference Lifetime & Teardown Mechanics ---
  Testing: kref refcount keeps context alive until all handles close ... [PASS]
  Testing: Driver offline flag rejects subsequent opens with -ENODEV ... [PASS]

--- Test Suite 6: UAPI Validation & Security Boundaries ---
  Testing: IOCTL validation, version check, reserved fields, and -EOPNOTSUPP ... [PASS]

===================================================================
 Stage 2E.2 Test Results: 10 / 10 Passed (100.0%)
===================================================================
```

### 9.2 Existing Stage 2C.5C Mock Test Suite (`bin/fw9369-tests`)
Reported separately, verifying zero regression in userspace protocol simulation:
$$\mathbf{Final\;Test\;Results:\;227\;/\;227\;Passed\;(100.0\%)}$$

---

## 10. Hardware Execution Gate & Safety Verification

- Hardware Execution Gate:
  ```bash
  bin/fw9369-hw-tool ; echo "Gate exit code: $?"
  ```
  Result: **Exit Code 1 (PASS — Gate Strictly Sealed)**
- **Zero Hardware Execution:**
  - `fte3600.ko` was **NOT** loaded into the kernel.
  - Physical SPI device node `/dev/spidev1.0` was not accessed.
  - Reset Pad 14 and IRQ Pad 229 were not toggled.
  - No physical SPI transactions or sensor register accesses occurred.

---

## 11. Files Created and Modified

| File | Status | Description |
| :--- | :--- | :--- |
| `kernel/fte3600.h` | Created | Unified UAPI header with 32-byte fixed-width structures, versioning, and ioctls. |
| `kernel/fte3600.c` | Rewritten | Native ACPI `spi_driver`, Policy A passive probe, active-low reset, single-open cdev. |
| `kernel/fte3600-test.c` | Created | 10-test unit test harness for UAPI, concurrency, lifetime, CRC, and state machine. |
| `kernel/Makefile` | Modified | Updated to build `fte3600.ko`, `fte3600-test`, and run `check`. |
| `STAGE2E2_KERNEL_IMPLEMENTATION_REPORT.md` | Created | Full Stage 2E.2 implementation report (this document). |
| `diagnostics/STAGE2E2_KERNEL_IMPLEMENTATION_REPORT.md` | Created | Mirrored implementation report in diagnostics repository. |
| `PROGRESS.md` | Updated | Appended Stage 2E.2 milestone record with historical continuity. |

---

## 12. Final Git Status

```text
On branch master
Your branch is up to date with 'origin/master'.

Changes not staged for commit:
  (use "git add <file>..." to update what will be committed)
  (use "git restore <file>..." to discard changes in working directory)
	modified:   PROGRESS.md
	modified:   kernel/Makefile
	modified:   kernel/fte3600.c

Untracked files:
  (use "git add <file>..." to include in what will be committed)
	STAGE2D1_BLOCKER_VERIFICATION.md
	STAGE2D2_MINIMAL_INITIALIZATION_SAFETY_AUDIT.md
	STAGE2D3_TEARDOWN_AND_KERNEL_RESET_REVIEW.md
	STAGE2D_HARDWARE_READINESS_AUDIT.md
	STAGE2E1_1_ARCHITECTURE_CORRECTION_REPORT.md
	STAGE2E1_2_FINAL_IMPLEMENTATION_PRECONDITIONS.md
	STAGE2E1_KERNEL_ARCHITECTURE_DESIGN.md
	STAGE2E2_KERNEL_IMPLEMENTATION_REPORT.md
	diagnostics/STAGE2D1_BLOCKER_VERIFICATION.md
	diagnostics/STAGE2D2_MINIMAL_INITIALIZATION_SAFETY_AUDIT.md
	diagnostics/STAGE2D3_TEARDOWN_AND_KERNEL_RESET_REVIEW.md
	diagnostics/STAGE2D_HARDWARE_READINESS_AUDIT.md
	diagnostics/STAGE2E1_1_ARCHITECTURE_CORRECTION_REPORT.md
	diagnostics/STAGE2E1_2_FINAL_IMPLEMENTATION_PRECONDITIONS.md
	diagnostics/STAGE2E1_KERNEL_ARCHITECTURE_DESIGN.md
	diagnostics/STAGE2E2_KERNEL_IMPLEMENTATION_REPORT.md
	kernel/fte3600-test
	kernel/fte3600-test.c
	kernel/fte3600.h

no changes added to commit (use "git add" and/or "git commit -a")
```
*(No Git commit was created in this stage, in accordance with project instructions.)*
