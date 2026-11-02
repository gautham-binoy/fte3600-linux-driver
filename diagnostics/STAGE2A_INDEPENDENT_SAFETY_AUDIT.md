# Stage 2A Independent Implementation and Safety Audit

**Date:** 2026-11-02  
**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Operating System:** Omarchy Linux (`7.2.8-9-omarchy`, x86_64)  
**Target Silicon:** FocalTech SPI Fingerprint Sensor (`FTE3600:00`)  
**Scope:** Stage 2A Supervised Sensor Identification Implementation  
**Audit Type:** Static Source Code, Lifecycle, and Blast-Radius Audit (Zero Hardware Access)

---

## 1. Executive Verdict

### **Verdict: PASS WITH CONDITIONS**

The Stage 2A implementation (`tools/stage2a-identify.c`, `scripts/stage2a-test.sh`, `scripts/run-stage2a-in-terminal.sh`) has been independently audited line by line. The implementation adheres strictly to the approved operational boundaries established in `diagnostics/STAGE2A_EXECUTION_CHECKLIST.md` and `diagnostics/STAGE2A_FINAL_SAFETY_REVIEW.md`.

- **0 Critical Findings**
- **0 High Findings**
- **1 Medium Finding** (Resolved: SPI ioctl readback verification added to `stage2a-identify.c`)
- **1 Low Finding** (Resolved: Character device validation via `fstat(S_ISCHR)` added to `stage2a-identify.c`)

### Mandatory Operational Conditions Before Execution:
1. **Explicit User Authorization:** Hardware execution must not commence without explicit user consent.
2. **Dedicated Terminal Execution:** Execution must be performed via `scripts/run-stage2a-in-terminal.sh` under interactive user supervision so that all real-time outputs and rollback steps are visible.
3. **Pristine Baseline Prerequisite:** Execution must abort immediately if any pre-existing `/dev/spidev*` node exists, if `fte3600.ko` or `spidev` is loaded, or if `spi-FTE3600:00` is already bound.
4. **Post-Test Residual State Awareness:** The user must be informed that while host resources are 100% rolled back, sensor internal register `0xC6` is inferred to retain `0x01` until the next host power cycle or reboot.

---

## 2. Inspected Files and Functions

| Component | File Path | Functions / Sections Inspected |
| :--- | :--- | :--- |
| **Identification Tool** | `tools/stage2a-identify.c` | `calc_crc_ccitt()`, `print_hex_buffer()`, `main()`, SPI parameter setup, Step 1 write (`0xC6=1`), Step 2 delay (4 ms), Step 3 readback (`0xC6`), Step 4 Chip ID (`0x1A8B`), Step 5 sub-rev (`0x1816`), variant gating, and exit error paths. |
| **Test Harness** | `scripts/stage2a-test.sh` | Baseline audit checks (lines 477-561), deferred signal traps (`enter_critical`, `leave_critical`, lines 148-182), `cleanup()` 5-step rollback (lines 200-428), `uio` loading & ownership logic (lines 566-601), `spidev` binding & override (lines 603-673), `fte3600.ko` probe confirmation (lines 675-731), binary invocation & exit code propagation (lines 733-770). |
| **Terminal Launcher** | `scripts/run-stage2a-in-terminal.sh` | Shell environment setup, banner warnings, `sudo` execution, exit code capture, and user pause prompt. |
| **Reference Stage 1 Harness** | `scripts/stage1-test.sh` | Comparison of module state tracking, UIO ownership logic, and cleanup error handling. |
| **Kernel Glue Driver** | `kernel/fte3600.c` | Resource parsing (`fte3600_resource`), reset deassertion (`fte3600_deassert`), GPIO configuration (`fte3600_probe`), sysfs status attribute (`fte3600_status_show`), and module removal (`fte3600_remove`/`fte3600_cleanup`). |

---

## 3. Categorized Findings and Audit Log

| ID | Severity | File & Line | Description | Status |
| :--- | :--- | :--- | :--- | :--- |
| **F-01** | **Medium** | `tools/stage2a-identify.c:101-143` | **SPI Configuration Readback Verification:** Original implementation wrote SPI mode, bits per word, and max speed via `SPI_IOC_WR_*` ioctls but did not read back the settings via `SPI_IOC_RD_*` to verify that the kernel SPI controller driver actually committed them. | **RESOLVED:** Added `SPI_IOC_RD_MODE`, `SPI_IOC_RD_BITS_PER_WORD`, and `SPI_IOC_RD_MAX_SPEED_HZ` ioctl checks with strict value matching; failures exit immediately with `EXIT_ERR_SPI_CONFIG` (2). |
| **F-02** | **Low** | `tools/stage2a-identify.c:88-99` | **Device Node Type Validation:** Original implementation opened `device_path` directly with `open(O_RDWR)`. If an unprivileged user passed a regular file or FIFO, the program would fail later on the first ioctl with `ENOTTY` rather than reporting an invalid invocation. | **RESOLVED:** Added `fstat()` check validating `S_ISCHR(st.st_mode)`. If the path is not a character device, it halts immediately with `EXIT_ERR_INVOCATION` (1). |
| **F-03** | **Low** | `scripts/stage2a-test.sh:699-706` | **Probe Readiness Timing:** Harness checks `/sys/bus/platform/devices/fte3600-glue.*/fte3600_status` for `"ready"`. The sysfs group is registered synchronously in `fte3600_probe()`, ensuring immediate availability upon `insmod`. | **VERIFIED:** Static inspection of `kernel/fte3600.c:716-720` confirms `fte3600_status` sysfs node is created and marked `"ready"` before probe returns 0. |
| **F-04** | **Informational** | `scripts/stage2a-test.sh:399-403` | **Residual Silicon Clock State:** Host cleanup restores all kernel and bus state, but register `0xC6` remains set to `0x01` on the sensor until cold reboot. | **VERIFIED & DOCUMENTED:** Harness explicitly logs this residual state notice during rollback. |

---

## 4. SPI Packet Verification Table

Every SPI transaction implemented in `tools/stage2a-identify.c` was audited against the FocalTech reference driver (`ft93xx.c`) and protocol specification:

| Op # | Purpose | Reference Command | MOSI Packet Bytes | Length | CS Behavior | Expected MISO Offset & Content | Error Exit Code |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **1** | Clock Enable | `ft93xx_write_byte(0xC6, 0x01)` | `0x09 0xF6 0xC6 0x01` | 4 bytes | Asserted during transfer; deasserted high upon completion. | MOSI write only (`rx_buf = 0`). | Code 3 (`EXIT_ERR_WRITE_TRANSPORT`) |
| **2** | Stabilization | `msleep(4)` | None (Inter-transaction delay) | N/A | Deasserted high (`usleep(4000)`). | N/A | N/A |
| **3** | Clock Readback | `ft93xx_read_byte(0xC6, &val)` | `0x08 0xF7 0xC6 0x00 0x00` | 5 bytes | Asserted during transfer; deasserted high upon completion. | `rx[4] == 0x01`. Mismatch halts immediately. | Code 4 (`EXIT_ERR_READ_TRANSPORT`) / Code 5 (`EXIT_ERR_VERIFY_MISMATCH`) |
| **4** | Chip ID Query | `ft93xx_read_word(0x1A8B, &id)` | `0x04 0xFB 0x9A 0x8B 0x00 0x00 0x00 0x00 0x00 0x00` | 10 bytes | Asserted during transfer; deasserted high upon completion. | `rx[6..7]` = Chip ID word;<br>`rx[8..9]` = CRC-CCITT.<br>Halt if `0x0000` or `0xFFFF`. | Code 4 (`READ_TRANSPORT`) / Code 6 (`BUS_FLOAT`) / Code 7 (`CRC_MISMATCH`) / Code 8 (`UNKNOWN_CHIP_ID`) |
| **5** | Sub-Revision *(Conditional: only if ID=0x9391)* | `ft93xx_read_word(0x1816, &sub)` | `0x04 0xFB 0x98 0x16 0x00 0x00 0x00 0x00 0x00 0x00` | 10 bytes | Asserted during transfer; deasserted high upon completion. | `rx[6..7]` = Sub-revision word;<br>`rx[8..9]` = CRC-CCITT.<br>`0x0FFF` -> FT9395; other -> FT9391. | Code 9 (`EXIT_ERR_SUBREV_FAILURE`) / Code 7 (`CRC_MISMATCH`) |

### Verification of Complement Bytes vs. Register Addresses
- In Operation 1 (`09 F6 C6 01`): Byte 0 is opcode `0x09`; Byte 1 is bitwise NOT `~0x09 = 0xF6`; Byte 2 is register `0xC6`. **`0xF6` is the protocol complement byte, NOT a write to register `0xF6`.**
- In Operation 3 (`08 F7 C6 00 00`): Byte 0 is opcode `0x08`; Byte 1 is bitwise NOT `~0x08 = 0xF7`; Byte 2 is register `0xC6`. **`0xF7` is the protocol complement byte, NOT register `0xF7`.**
- In Operation 4 (`04 FB 9A 8B ...`): Byte 0 is opcode `0x04`; Byte 1 is bitwise NOT `~0x04 = 0xFB`; Bytes 2-3 are address `0x9A8B` (`0x1A8B | 0x8000`).

---

## 5. Driver Binding and Module Lifecycle Analysis

### Critical Architectural Question:
**Does Stage 2A actually need to modify driver bindings or load the Stage 1 glue driver (`fte3600.ko`)?**

### Analysis and Detailed Technical Justification:

#### 1. Why `spidev` binding and `driver_override` modification are strictly necessary:
- The sensor ACPI node `FTE3600:00` is enumerated as an SPI device (`spi-FTE3600:00`) on Intel LPSS SPI controller 1 (`spi1.0`).
- By default, the Linux kernel `spidev` driver does not bind to `spi-FTE3600:00` because `spidev` does not include `"FTE3600"` in its ACPI ID table (mainline Linux intentionally prevents automatic binding of userspace SPI to ACPI devices for device safety).
- In order to create the userspace character device `/dev/spidev1.0` through which SPI ioctls can be delivered, userspace must explicitly write `"spidev"` to `/sys/bus/spi/devices/spi-FTE3600:00/driver_override` and bind the device via `/sys/bus/spi/drivers/spidev/bind`.
- **Can it be avoided?** No. Without this, no character device `/dev/spidev*` exists, and userspace cannot communicate over the SPI bus.

#### 2. Why loading `fte3600.ko` is technically and electrically necessary:
- In the Dynabook ACPI DSDT, `Device (FTE0)` defines:
  - SPI serial bus resource (`ConnectionSpeed: 1000000`, 4-wire, Mode 0).
  - GpioIo resource (Pin 14 / Pad 14 on Intel PCH `GPP_B14`): Hardware Reset line.
  - GpioInt resource (Pin 323 / Pad 229 on Intel PCH `CPU_GP_0`): Interrupt line.
- When `fte3600.ko` loads and executes `fte3600_probe()` (`kernel/fte3600.c:652-665`):
  1. It claims the reset GPIO descriptor: `f->lines[FTE3600_RESET] = fwnode_gpiod_get_index(..., "reset", 0, GPIOD_ASIS, ...)`.
  2. It verifies the line is configured active-low: `gpiod_is_active_low(f->lines[FTE3600_RESET])`.
  3. It calls `fte3600_deassert(f)`:
     ```c
     static int fte3600_deassert(struct fte3600 *f)
     {
         return gpiod_direction_output(f->lines[FTE3600_RESET], 0);
     }
     ```
  4. Because the line is active-low, a logical value of `0` directs the Intel PCH GPIO controller to actively drive Pad 14 to **physical HIGH (3.3V)**.
- **What happens if `fte3600.ko` is NOT loaded?**
  - If `fte3600.ko` is omitted, Pad 14 remains in its unmanaged baseline state (`0x0003c026`, input mode with an internal 20k pull-up resistor).
  - An input pin with a weak internal pull-up is susceptible to bus capacitance and electrical noise. If the reset pin is not actively held high by an output driver, the sensor's internal reset circuitry may assert or float during SPI bus toggling, causing transactions to return `0x00` or `0xFF`.
  - Furthermore, `fte3600.ko` validates that ACPI resources match the exact expected fingerprint sensor configuration before declaring `"ready"` in sysfs (`fte3600_status == "ready"`).
- **Can it be avoided?**
  - Avoiding `fte3600.ko` would require either relying on an indeterminate, weakly pulled-up reset line, or manipulating PCH Pad 14 directly via `/dev/gpiochip*` from userspace, which introduces additional dependencies and bypasses ACPI arbitration.
  - Therefore, loading `fte3600.ko` during Stage 2A is the cleanest, most reliable, and safest architectural method to ensure the hardware reset line is actively driven high while SPI transactions execute.
- **Rollback guarantees:** When Stage 2A finishes, `rmmod fte3600` releases the GPIO descriptor, safely returning Pad 14 to its pristine baseline state.

---

## 6. Failure-Path and Rollback Analysis

### Exit Codes and Halting Behavior in `tools/stage2a-identify.c`

| Exit Code | Constant | Trigger Condition | Rollback Action |
| :--- | :--- | :--- | :--- |
| **0** | `EXIT_IDENTIFY_SUCCESS` | All 5 operations succeed; valid Chip ID identified. | Normal complete rollback. |
| **1** | `EXIT_ERR_INVOCATION` | Incorrect arguments, failed `open()`, or target is not a character device (`!S_ISCHR`). | Halts immediately; trap initiates full rollback. |
| **2** | `EXIT_ERR_SPI_CONFIG` | Failed to write or verify SPI mode, word bits, or 1 MHz speed via ioctl. | Closes fd, halts immediately; trap initiates full rollback. |
| **3** | `EXIT_ERR_WRITE_TRANSPORT` | `ioctl(SPI_IOC_MESSAGE)` error or byte count mismatch on register `0xC6` write. | Closes fd, halts immediately; trap initiates full rollback. |
| **4** | `EXIT_ERR_READ_TRANSPORT` | `ioctl(SPI_IOC_MESSAGE)` error or byte count mismatch during read transactions. | Closes fd, halts immediately; trap initiates full rollback. |
| **5** | `EXIT_ERR_VERIFY_MISMATCH` | Register `0xC6` readback value is not `0x01`. | Closes fd, halts immediately; trap initiates full rollback. |
| **6** | `EXIT_ERR_BUS_FLOAT` | Chip ID returned `0x0000` (grounded/inactive) or `0xFFFF` (floating/unpowered). | Closes fd, halts immediately; trap initiates full rollback. |
| **7** | `EXIT_ERR_CRC_MISMATCH` | Non-zero CRC-CCITT does not match computed value over received payload. | Closes fd, halts immediately; trap initiates full rollback. |
| **8** | `EXIT_ERR_UNKNOWN_CHIP_ID` | Chip ID word is not in approved list (`0x9362`, `0x9365`, `0x9391`). | Closes fd, halts immediately; trap initiates full rollback. |
| **9** | `EXIT_ERR_SUBREV_FAILURE` | Read transport failure while querying sub-revision register `0x1816`. | Closes fd, halts immediately; trap initiates full rollback. |

### Shell Trap Architecture in `scripts/stage2a-test.sh`:
- **Atomic Critical Sections (`enter_critical` / `leave_critical`):**
  - Every state modifying operation (`modprobe uio`, `modprobe spidev`, setting `driver_override`, binding `spidev`, `insmod fte3600.ko`) is wrapped in a critical section.
  - If `SIGINT` (Ctrl+C) or `SIGTERM` arrives during a critical section, the signal is deferred (`PENDING_SIGNAL=130` or `143`) until state variables (`UIO_LOADED_BY_US`, `SPIDEV_BOUND_BY_US`, etc.) are updated. Once reconciled, the deferred signal is dispatched to trigger `cleanup()`.
- **Signal-Protected Rollback Routine (`cleanup`):**
  - Cleanup trap is registered for `EXIT`, `INT`, `TERM`.
  - Step 1: `rmmod fte3600` (verified ABSENT).
  - Step 2: Unbind `spi-FTE3600:00` from `spidev` (refuses if bound to an unexpected driver; verified UNBOUND).
  - Step 3: Restore `driver_override` to original empty string (verified restored).
  - Step 4: `modprobe -r spidev` (verified ABSENT).
  - Step 5: `modprobe -r uio` if loaded by script; preserved if pre-existing at baseline (verified ABSENT if loaded by script).
  - Signals received during cleanup are deferred so each step runs to completion without interruption.
  - Post-rollback verification confirms zero lingering modules, zero lingering bindings, and zero orphan `/dev/spidev*` device nodes.

---

## 7. Sensor Safety and Blast-Radius Analysis

1. **Non-Volatile Memory (NVM / OTP / EFUSE / Flash):**
   - The implementation issues **zero** writes to OTP or EFUSE registers (`0xF0` through `0xFF`).
   - The implementation issues **zero** flash programming commands (`0xC0`).
   - The single write performed is to volatile register `0xC6` (SPI transceiver clock gating).
2. **Analog Front-End (AFE) and Calibration Protection:**
   - Registers `0xFD` and `0xFE` (analog bias, gain, and sensing matrix thresholds) are **never accessed**.
3. **Sequencer and Image Capture Isolation:**
   - Registers `0x1A84` and `0x1A8E` (frame start, scan mode, pixel capture triggers) are **never accessed**.
   - No buffer read commands or pixel polling loops exist in the codebase.
4. **Command Protocol Compliance:**
   - Prohibited sleep/standby commands (`0x5A`, `0xA5`) are **never transmitted**.
   - CS line is held asserted strictly for the duration of each individual transaction and deasserted high between transactions.

---

## 8. Remaining Uncertainties

1. **Residual Silicon Clock State (`0xC6 = 0x01`):**
   - The FocalTech reference driver contains no command to disable the SPI transceiver clock once enabled.
   - Register `0xC6` is inferred to retain `0x01` while the laptop remains powered.
   - This state is electrically passive (CS is held high with internal pull-up), but complete reset to factory unclocked state requires an ACPI S5 power cycle (cold reboot or power off).
2. **Silicon Variant Representation:**
   - The Dynabook Satellite Pro C40-J is expected to contain an `FT9362`, `FT9365`, `FT9391`, or `FT9395`.
   - If the installed silicon returns an unlisted Chip ID, the tool halts immediately with exit code 8 and does not guess or attempt generic communication.
3. **CRC Zero Exception Heuristic:**
   - Reference driver code indicates certain FocalTech firmware revisions return `0x0000` for the CRC word. The tool accepts `0x0000` but explicitly logs it as `UNVERIFIED` rather than treating it as proof of mathematical CRC verification.

---

## 9. Offline Verification Evidence

The following verification steps were executed strictly offline without accessing hardware:

1. **C Source Compilation:**
   ```bash
   gcc -O2 -Wall -Wextra -Werror -o bin/stage2a-identify tools/stage2a-identify.c
   ```
   *Result:* Exited 0 with 0 warnings.
2. **Bash Syntax Verification:**
   ```bash
   bash -n scripts/stage2a-test.sh
   bash -n scripts/run-stage2a-in-terminal.sh
   ```
   *Result:* Exited 0 with 0 syntax errors.
3. **Argument & Invocation Validation:**
   ```bash
   bin/stage2a-identify
   # Exited 1: Usage printed.
   bin/stage2a-identify /dev/nonexistent
   # Exited 1: Cannot open file.
   bin/stage2a-identify README.md
   # Exited 1: README.md is not a character device!
   ```
   *Result:* Verified immediate exit with code 1.
4. **CRC-CCITT Algorithm Verification:**
   - Computed offline over test vectors:
     - `0x9362` -> `0x1C53`
     - `0x9365` -> `0x6CB4`
     - `0x9391` -> `0xC32F`
     - `0x0FFF` -> `0x13C1`
   - Verified that `calc_crc_ccitt()` matches the standard CCITT `0x1021` polynomial with `0xFFFF` seed.

---

## 10. Explicit Confirmation of Zero Hardware Access

**I hereby confirm:**
- No kernel modules (`fte3600.ko`, `spidev`, `uio`) were loaded or unloaded.
- No devices were bound or unbound.
- No SPI transactions were executed.
- No `/dev/spidev*`, `/dev/uio*`, or `/dev/gpiochip*` nodes were opened or manipulated.
- Neither `scripts/stage2a-test.sh` nor `scripts/run-stage2a-in-terminal.sh` was executed.
- The hardware state of the Dynabook Satellite Pro C40-J remains in its exact baseline state.

---

## 11. Final Authorization Boundary

**STOP.**  
This audit concludes the review of the Stage 2A implementation. The verdict is **PASS WITH CONDITIONS**.  
Do **NOT** execute Stage 2A or authorize Phase 3 without separate, explicit user instructions.
