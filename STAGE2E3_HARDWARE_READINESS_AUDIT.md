# Stage 2E.3 — Final Hardware-Readiness Audit for First Fingerprint Sensor Identification

**Target System:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Host Kernel:** `7.2.8-9-omarchy` (SMP preempt mod_unload x86_64)  
**Host Compiler:** `gcc (GCC) 16.2.1 20260810`  
**Document ID:** `STAGE2E3_HARDWARE_READINESS_AUDIT`  
**Audit Scope:** Comprehensive static hardware-readiness and protocol audit of the unified kernel driver prior to any physical hardware interaction  
**Hardware Safety Gate:** **CLOSED (`bin/fw9369-hw-tool` exits with code 1 unconditionally)**  

---

## 1. Executive Summary & Authoritative Stage Verdict

This comprehensive static audit evaluates the software readiness of the unified Linux kernel driver `fte3600.ko` for a potential first controlled physical hardware identification test.

The investigation encompassed full source inspection of `kernel/fte3600.c`, `kernel/fte3600.h`, and `kernel/Makefile`, verification of ACPI and SPI controller ownership, electrical GPIO reset semantics, the previously verified Stage 2A identification protocol, error and recovery state transitions, and kernel SPI controller constraints.

### Key Audit Findings

1. **Hardware Ownership & ACPI Mapping Verified:** The physical device `FTE3600:00` on SPI bus `pxa2xx-spi.2` (`spi1.0`) is currently completely unbound. No conflicting driver (`spidev`, `focal_spi`) is loaded or bound. ACPI CRS index 0 and 1 correctly map Pad 14 (reset) and Pad 229 (IRQ) with `active_low = true` for reset.
2. **Device-Model Lifetime & Teardown Proven:** The `cdev_device_add()` embedded `struct device` architecture and Hardware-Resource Shutdown Protocol proven in Stage 2E.2.3 guarantee that no devres-managed resources (`reset_gpiod`, `spi`) can be dereferenced post-unbinding.
3. **Reset GPIO Sequence Ready in Principle:** `fte3600_hw_reset_pulse()` implements the exact reference timing (10 ms pre-settle, 20 ms active-low assert, 10 ms post-settle). However, this sequence has **never been executed through the unified kernel driver**.
4. **PRIMARY BLOCKER — Identification Sequence Unimplemented in Driver:** While the SPI transfer helper `fte3600_spi_sync_transfer()` and CRC calculator `fte3600_crc16_wire()` are implemented and hardened, they remain marked `__maybe_unused` with **zero call sites** in `kernel/fte3600.c`. `FTE3600_IOC_INIT` unconditionally returns `-EOPNOTSUPP`. The kernel driver currently has no code to issue the wake write (`0xC6=0x01`), perform the 4 ms delay, verify readback, read Chip ID register `0x1A8B`, or validate wire CRC.
5. **Physical Test Blocked by Missing Driver Logic:** Because the driver cannot execute the identification transactions, the proposed first hardware test cannot be performed until the identification sequence is implemented in driver software in a subsequent stage.

### Authoritative Stage Verdict

$$\mathbf{NOT\;READY\;—\;SOFTWARE\;IMPLEMENTATION\;INCOMPLETE}$$

*Hardware ownership, ACPI resource mappings, and device lifetime protections are fully verified. However, the driver does not yet implement the identification transaction sequence (FTE3600_IOC_INIT returns -EOPNOTSUPP, and transport helpers are unlinked). Hardware testing cannot proceed until this software logic is implemented.*

---

## 2. Driver Architecture & Lifetime Audit

The latest source code in [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c) was independently inspected to verify the retention of all Stage 2E.2.2 and Stage 2E.2.3 fixes:

### 2.1 Character Device & Device Model Lifetime
- **Embedded `struct device`:** [`struct fte3600_data`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L34-L46) embeds `struct device dev` and `struct cdev cdev`.
- **Probe Registration:** [`fte3600_probe()`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L396-L473) initializes `data->dev` via `device_initialize(&data->dev)`, configures class, parent, and release callback [`fte3600_device_release()`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L98-L104), and registers via `cdev_device_add(&data->cdev, &data->dev)`.
- **Reference Balancing:**
  - `fte3600_cdev_open()` calls `get_device(&data->dev)`.
  - `fte3600_cdev_release()` calls `put_device(&data->dev)`.
  - `fte3600_remove()` calls `put_device(&data->dev)` for the probe reference.
  - Every error path in `probe()` after `device_initialize()` calls `put_device(&data->dev)`.
- **Single-Client Exclusive Lease:**
  - `atomic_cmpxchg(&data->is_open, 0, 1)` strictly prevents concurrent opens, returning `-EBUSY`.
  - Post-lease re-check of `data->offline` ensures clean rollback if removal races during open.

### 2.2 Hardware-Resource Shutdown Protocol
In [`fte3600_remove()`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L476-L526):
1. `cdev_device_del(&data->cdev, &data->dev)` unmaps character device and sysfs entries, preventing new opens.
2. `mutex_lock(&data->lock)` serializes with any running or pending hardware operation.
3. `atomic_set(&data->offline, 1)` asserts the offline flag.
4. `data->reset_gpiod = NULL; data->spi = NULL;` invalidates hardware pointers while holding `data->lock`.
5. `mutex_unlock(&data->lock)` releases lock.
6. `wake_up_all(&data->event_wq)` wakes pollers to return `EPOLLERR | EPOLLHUP`.
7. `put_device(&data->dev)` drops probe reference.
8. Function returns $\to$ driver core calls `devres_release_all(&spi->dev)`.

In [`fte3600_ioctl_common()`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L252-L352):
- `case FTE3600_IOC_RESET:` acquires `data->lock`, re-checks `atomic_read(&data->offline) || !data->reset_gpiod`, and returns `-ENODEV` if unbind has run.
- `case FTE3600_IOC_GET_STATE:` reads retained software context under lock and asserts `FTE3600_FLAG_OFFLINE` if offline, without touching hardware.

---

## 3. ACPI & SPI Controller Ownership Verification

### 3.1 Hardware Topology & Static ACPI Declarations
From inspection of [`diagnostics/privileged/acpi-fte3600-only.dsl`](file:///home/titan/fte3600-linux-driver/diagnostics/privileged/acpi-fte3600-only.dsl) and DSDT `Device (FP01)`:

```text
ACPI Scope: \_SB.PC00.SPI1.FP01
├── _HID: "FTE3600"
├── _CID: "FTE3600"
├── _STA: 0x0F (Present, Enabled, Functioning, Decodes Resources)
├── _INI: SHPO(0x090E0003, 1), SHPO(0x0900000E, 1) [Host OS Ownership Switch]
└── _CRS:
    ├── SpiSerialBusV2 (CS 0, Active-Low, 4-Wire, 8-bit, 1 MHz, Mode 0, "\_SB.PC00.SPI1")
    ├── GpioIo (Index 0, OutputOnly, PullUp, "\_SB.GPI0", Pin GNUM(0x0900000E) -> Pad 14)
    └── GpioInt (Index 1, Edge, ActiveHigh, PullNone, "\_SB.GPI0", Pin GNUM(0x090E0003) -> Pad 229)
```

### 3.2 Linux Subsystem Ownership
1. **SPI Controller:** Intel Tiger Lake-LP PCH LPSS SPI2 controller (`pci0000:00/0000:00:1e.3/pxa2xx-spi.2`).
2. **SPI Master / Bus:** Registered as `spi1`.
3. **SPI Slave Device:** `spi-FTE3600:00` at `spi1.0`.
4. **Current Binding Status:** Unbound. Sysfs node `/sys/bus/spi/devices/spi-FTE3600:00/driver` does not exist.
5. **Conflict Analysis:**
   - In-tree `spidev.ko` is not loaded.
   - Stage 1 glue module `fte3600.ko` is not loaded.
   - No conflicting userspace daemon (`fprintd`, `open-fprintd`) is installed or running.
   - The unified SPI driver matches ACPI ID `"FTE3600"` and will bind exclusively to `spi-FTE3600:00`.
6. **GPIO Translation Mapping:**
   Because ACPI lacks `_DSD` named GPIO entries, the driver's translation table [`fte3600_acpi_gpios`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L60-L67) (`reset-gpios` $\to$ CRS index 0, `irq-gpios` $\to$ CRS index 1) is mandatory. Proved that `devm_acpi_dev_add_driver_gpios()` must be called against `&spi->dev` before `devm_gpiod_get()`.

---

## 4. Reset Implementation Readiness & Semantics

### 4.1 Reset Pulse Sequence
[`fte3600_hw_reset_pulse()`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L106-L146) implements:
1. `lockdep_assert_held(&data->lock);`
2. `if (atomic_read(&data->offline) || !data->reset_gpiod) return -ENODEV;`
3. Pre-settle: `gpiod_set_value_cansleep(data->reset_gpiod, 0); msleep(10);` (Logical 0 = Physical HIGH 3.3V / inactive).
4. Assert: `gpiod_set_value_cansleep(data->reset_gpiod, 1); msleep(20);` (Logical 1 = Physical LOW 0V / asserted, matching `recovery.c:19`).
5. Deassert: `gpiod_set_value_cansleep(data->reset_gpiod, 0); msleep(10);` (Logical 0 = Physical HIGH 3.3V / inactive, matching `factory_startup.c:341`).
6. State update: `data->state = FTE3600_STATE_RESET_COMPLETE;`

### 4.2 Technical Limitations & Status
- **`gpiod_set_value_cansleep()` returns `void`:** Calling it confirms solely that the register write was posted to the Intel PCH LPSS GPIO controller. It does not measure physical pin voltage on Pad 14.
- **State Invariant:** State transitions to `FTE3600_STATE_RESET_COMPLETE` (aliased as `FTE3600_STATE_RESET_SEQUENCE_ISSUED`). It does **not** transition to `FTE3600_STATE_READY`.
- **Status:** The reset pulse logic is ready in principle, but has **never been tested on physical hardware** through the unified driver.

---

## 5. Identification Protocol Verification & Implementation Gap

### 5.1 Protocol Specification (from Stage 2A & Vendor DLL Analysis)

The verified Stage 2A identification sequence consists of 3 distinct transactions:

```text
1. WAKE WRITE:
   MOSI: [ 0x09, 0xF6, 0xC6, 0x01 ] (Opcode 0x09 Write Byte, ~Opcode 0xF6, Reg 0xC6, Val 0x01)
   MISO: [ 0x00, 0x00, 0x00, 0x00 ]
   CS asserted LOW for 4 bytes, then deasserted HIGH.

2. TRANSCEIVER STABILIZATION DELAY:
   Hold CS deasserted HIGH for 4 ms.

3. WAKE READBACK VERIFICATION:
   MOSI: [ 0x08, 0xF7, 0xC6, 0x00, 0x00 ] (Opcode 0x08 Read Byte, ~Opcode 0xF7, Reg 0xC6, 2 dummy bytes)
   MISO: [ 0x00, 0x00, 0x00, 0x00, 0x01 ] (Byte 4 must equal 0x01)
   CS asserted LOW for 5 bytes, then deasserted HIGH.

4. CHIP ID INTERROGATION:
   MOSI: [ 0x04, 0xFB, 0x9A, 0x8B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 ] (Opcode 0x04 Read Word, Reg 0x1A8B, count 0)
   MISO: [ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x93, 0x62, 0x1C, 0x53 ]
   Payload:
     - Chip ID Word: rx[6..7] = 0x9362
     - Hardware CRC: rx[8..9] = 0x1C53
   CS asserted LOW for 10 bytes, then deasserted HIGH.

5. WIRE CRC-16 VALIDATION:
   Computed over rx[6..7] using CCITT 0x1021, seed 0xFFFF, MSB-first.
   calc_crc([0x93, 0x62]) = 0x1C53.
   CRC matches rx[8..9] exactly.
```

### 5.2 Primary Blocker: Missing Driver Implementation
A complete audit of `kernel/fte3600.c` reveals:

1. **Unlinked Transport Helper:** [`fte3600_spi_sync_transfer()`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L148-L193) is marked `__maybe_unused` and has **zero callers**.
2. **Unlinked CRC Helper:** [`fte3600_crc16_wire()`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L69-L95) is marked `__maybe_unused` and has **zero callers**.
3. **INIT IOCTL Returns `-EOPNOTSUPP`:** In [`fte3600_ioctl_common()`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L308-L325), `case FTE3600_IOC_INIT:` validates version and reserved fields, but unconditionally returns **`-EOPNOTSUPP`**.
4. **Missing Code:** The driver currently lacks:
   - An in-kernel identification function (e.g. `fte3600_hw_identify()`).
   - Implementation of the wake write, 4 ms delay, wake readback, and Chip ID query inside `fte3600.c`.
   - Wiring of `FTE3600_IOC_INIT` to invoke this sequence.
   - Transition of `data->state` to `FTE3600_STATE_READY` upon successful identification.

**Conclusion:** The driver is software-incomplete. It cannot execute an identification test in its current state.

---

## 6. Failure Modes & Recovery State Machine Analysis

The driver's state machine and error handling were audited across 11 deterministic failure scenarios:

| Failure Scenario | Driver Detection Point | Error Returned | Resulting State | Recovery Path |
| :--- | :--- | :--- | :--- | :--- |
| **GPIO Acquisition Fails** | `fte3600_probe()` line 442 | `-ENODEV` / PTR_ERR | Driver fails probe | Driver unbinds cleanly, probe aborted |
| **Reset Called Offline** | `fte3600_ioctl_common()` line 296 | `-ENODEV` | Preserved | None; device offline |
| **SPI Sync Error** | `fte3600_spi_sync_transfer()` line 201 | Negative errno | `FTE3600_STATE_UNKNOWN` | User must issue `FTE3600_IOC_RESET` |
| **SPI Incomplete Status** | `fte3600_spi_sync_transfer()` line 208 | `m.status` | `FTE3600_STATE_UNKNOWN` | User must issue `FTE3600_IOC_RESET` |
| **Wake Readback != 0x01** | Future `fte3600_hw_identify()` | `-EIO` | `FTE3600_STATE_UNKNOWN` | User must issue `FTE3600_IOC_RESET` |
| **Unexpected Chip ID** | Future `fte3600_hw_identify()` | `-ENODEV` | `FTE3600_STATE_UNKNOWN` | User must issue `FTE3600_IOC_RESET` |
| **Wire CRC Mismatch** | Future `fte3600_hw_identify()` | `-EBADMSG` | `FTE3600_STATE_UNKNOWN` | User must issue `FTE3600_IOC_RESET` |
| **Process Closes / Exits** | `fte3600_cdev_release()` | `0` | Preserved | Re-opens allowed via atomic lease clear |
| **Driver Unbind During Op** | `fte3600_remove()` under lock | In-flight finishes | `offline = 1` | Resources nullified before unbind returns |
| **Bus Float (0xFFFF)** | Future `fte3600_hw_identify()` | `-ENODEV` | `FTE3600_STATE_UNKNOWN` | User must issue `FTE3600_IOC_RESET` |
| **Bus Ground (0x0000)** | Future `fte3600_hw_identify()` | `-ENODEV` | `FTE3600_STATE_UNKNOWN` | User must issue `FTE3600_IOC_RESET` |

### Recovery Guarantees
- Any transport or protocol fault transitions `data->state = FTE3600_STATE_UNKNOWN`.
- Once in `UNKNOWN`, all subsequent initialization or capture operations fail closed with `-EPERM`.
- Recovery strictly requires an explicit userspace `FTE3600_IOC_RESET`.
- **Zero Automatic Retries:** No blind retries or undocumented recovery writes exist in `fte3600.c`.
- **Power State Independence:** Driver does not assume ACPI S5 clears volatile register states; it forces explicit re-identification.

---

## 7. Target Kernel & SPI Controller Constraints

- **Bus Mode:** Mode 0 (`CPOL=0, CPHA=0`). Verified by ACPI CRS and configured via `spi->mode = SPI_MODE_0` in probe.
- **Word Size:** 8 bits per word. Configured via `spi->bits_per_word = 8`.
- **Clock Speed:** 1,000,000 Hz (1 MHz). Configured via `spi->max_speed_hz = 1000000`. Matches ACPI `_CRS` specification `0x000F4240`.
- **PXA2xx DMA Limits:**
  - `MAX_DMA_LEN = 64 KiB` (65,536 bytes) in `spi-pxa2xx.c`.
  - All identification packets (4 B, 5 B, 10 B) are well within controller FIFO / PIO and DMA boundaries.
- **`spidev.bufsiz` Irrelevance:**
  - `spidev.bufsiz` applies solely to the character device interface of `spidev.ko`.
  - `fte3600.ko` is an in-kernel SPI driver submitting `struct spi_transfer` directly to the Linux SPI core. `spidev.bufsiz` has **zero relevance or effect** on `fte3600.ko`.
- **FIFO Capture Status:** Remains completely disabled (`return -EOPNOTSUPP;`). The command-prefix discrepancy (`[06 F9 ...]` vs `[10 EF ...]`) must be resolved before any FIFO read is attempted.

---

## 8. Software Isolation Verification

- **Interrupt Servicing Isolated:** 0 occurrences of `request_threaded_irq` or `request_irq`.
- **Dangerous Registers Absent:** 0 occurrences of registers `0x1A84`, `0x1A82`, `0xF1`, `0xF4`, or `0x9A` across `fte3600.c` and `fte3600.h`.
- **PCH Pad Control:** Direct memory-mapped access to PCH Pad 14 / Pad 229 `PADCFG` registers is completely absent.
- **FIFO Path Disabled:** `FTE3600_IOC_CAPTURE` returns `-EOPNOTSUPP`.
- **No Raw SPI IOCTL:** No raw SPI transfers can be submitted from userspace.
- **Hardware Safety Gate:** `bin/fw9369-hw-tool` unconditionally exits with code 1.

---

## 9. Offline Verification Results

| Test Target | Command | Result | Details |
| :--- | :--- | :--- | :--- |
| **Hardware Safety Gate** | `bin/fw9369-hw-tool` | **Exit Code 1** | PASS — Execution gate strictly sealed |
| **Kernel Build Cleanliness** | `make -C kernel all` | **Exit Code 0** | Clean build against `7.2.8-9-omarchy`, 0 warnings, 0 errors |
| **Kernel Unit Test Suite** | `make -C kernel check` | **17 / 17 PASS (100%)** | 11 mandatory lifecycle scenarios validated |
| **ACPI Glue Policy Test** | `./kernel/fte3600-policy-test` | **9 / 9 PASS (100%)** | Resource ordering, polarity, CS capability |
| **Protocol Mock Suite** | `bin/fw9369-tests` | **227 / 227 PASS (100%)** | Zero regression across 14 offline suites |

---

## 10. Proposed First Hardware Test Specification

When authorized in a future stage (after software implementation of the identification sequence):

### Strictly Permitted Scope:
1. Load `fte3600.ko` and bind to `spi-FTE3600:00`.
2. Verify probe completes with state `UNINITIALIZED`.
3. Open `/dev/fte3600`.
4. Issue `ioctl(fd, FTE3600_IOC_RESET)`: drive active-low pulse (10 ms settle, 20 ms assert, 10 ms settle).
5. Verify state transitions to `RESET_COMPLETE`.
6. Issue `ioctl(fd, FTE3600_IOC_INIT)` to trigger identification sequence:
   - Write `0xC6 = 0x01` (4 bytes).
   - Sleep 4 ms.
   - Read back `0xC6` (5 bytes; verify byte 4 == `0x01`).
   - Read Chip ID register `0x1A8B` (10 bytes; extract word at `rx[6..7]`, CRC at `rx[8..9]`).
   - Validate wire CRC-16 matches `0x1C53` and Chip ID matches `0x9362`.
   - Verify state transitions to `READY`.
7. Issue `ioctl(fd, FTE3600_IOC_GET_STATE)` to verify snapshot telemetry.
8. Close `/dev/fte3600`.
9. Unbind driver and unload module.
10. **STOP. Test complete.**

### Strictly Prohibited Scope:
- Zero FIFO burst transfers or frame capture (`FTE3600_IOC_CAPTURE`).
- Zero fingerprint enrollment or image processing.
- Zero interrupt handling or Pad 229 IRQ requests.
- Zero writes to registers `0x1A84`, `0x1A82`, `0xF1`, `0xF4`, or `0x9A`.
- Zero automatic retries or undocumented recovery commands.

**Crucial Caveat:** This proposed test **CANNOT be executed at this time** because the kernel driver does not yet implement step 6. Stage 2E.4 must implement the identification sequence before this test can be reviewed.

---

## 11. Exact File & Line References

- [`kernel/fte3600.c#L34-L46`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L34-L46): `struct fte3600_data` context definition.
- [`kernel/fte3600.c#L69-L95`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L69-L95): `fte3600_crc16_wire()` standalone helper.
- [`kernel/fte3600.c#L106-L146`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L106-L146): `fte3600_hw_reset_pulse()` reset sequence.
- [`kernel/fte3600.c#L148-L193`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L148-L193): `fte3600_spi_sync_transfer()` hardened transfer engine (uncalled).
- [`kernel/fte3600.c#L196-L239`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L196-L239): `fte3600_cdev_open()` with lease and `get_device()`.
- [`kernel/fte3600.c#L286-L306`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L286-L306): `case FTE3600_IOC_RESET:` reset dispatch under lock.
- [`kernel/fte3600.c#L308-L325`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L308-L325): `case FTE3600_IOC_INIT:` returns `-EOPNOTSUPP` (gap identified).
- [`kernel/fte3600.c#L396-L473`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L396-L473): `fte3600_probe()` passive probe and `cdev_device_add()`.
- [`kernel/fte3600.c#L476-L526`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L476-L526): `fte3600_remove()` shutdown protocol and `cdev_device_del()`.
- [`kernel/fte3600.h#L46`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.h#L46): `FTE3600_STATE_RESET_SEQUENCE_ISSUED` alias.
- [`tools/stage2a-identify.c#L158-L260`](file:///home/titan/fte3600-linux-driver/tools/stage2a-identify.c#L158-L260): Reference packet encodings for wake, readback, and Chip ID.
- [`diagnostics/privileged/acpi-fte3600-only.dsl#L1-L51`](file:///home/titan/fte3600-linux-driver/diagnostics/privileged/acpi-fte3600-only.dsl#L1-L51): ACPI DSDT `Device (FP01)` resource template.
