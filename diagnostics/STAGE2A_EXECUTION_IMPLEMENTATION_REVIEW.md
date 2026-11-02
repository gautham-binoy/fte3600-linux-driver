# FTE3600 Linux Driver — Stage 2A Execution Implementation Review

**Date/Time:** 2026-11-02T15:20:00+05:30  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Device:** `spi-FTE3600:00` on Intel GSPI #1 (`pxa2xx-spi.2`)  
**Phase:** Phase 1 — Pre-Execution Preparation  
**Hardware Access Status:** **ZERO HARDWARE ACCESS OCCURRED.** No kernel modules were loaded, no devices bound, and no SPI transactions executed.

---

## 1. Overview and Files Created

In preparation for Stage 2A supervised fingerprint sensor identification, dedicated userspace tools and harness scripts were implemented strictly following the authorized specification in `diagnostics/STAGE2A_EXECUTION_CHECKLIST.md`:

| File | Purpose | Status |
| :--- | :--- | :---: |
| [`tools/stage2a-identify.c`](file:///home/titan/fte3600-linux-driver/tools/stage2a-identify.c) | Dedicated C utility for SPI Mode 0 / 1 MHz ioctl message dispatch, response parsing, and CRC-CCITT validation. | Created & Source Audited |
| [`bin/stage2a-identify`](file:///home/titan/fte3600-linux-driver/bin/stage2a-identify) | Compiled ELF executable built with `gcc -O2 -Wall -Wextra -Werror`. Tested for invocation errors. | Compiled (Userspace Only) |
| [`scripts/stage2a-test.sh`](file:///home/titan/fte3600-linux-driver/scripts/stage2a-test.sh) | Execution harness with atomic critical sections, preflight checks, UIO dependency handling, and automated rollback. | Created & Syntax Verified |
| [`scripts/run-stage2a-in-terminal.sh`](file:///home/titan/fte3600-linux-driver/scripts/run-stage2a-in-terminal.sh) | Sudo terminal launcher matching Stage 1 interactive pattern. | Created & Syntax Verified |
| [`diagnostics/STAGE2A_EXECUTION_IMPLEMENTATION_REVIEW.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2A_EXECUTION_IMPLEMENTATION_REVIEW.md) | This implementation review document. | Created |

---

## 2. Exact Commands to Be Executed (Pending Explicit Authorization)

When explicitly authorized, Stage 2A will be launched via:
```bash
./scripts/run-stage2a-in-terminal.sh
```
which executes:
```bash
sudo ./scripts/stage2a-test.sh
```

### Command Execution Breakdown by Step:

1. **Pre-Execution Baseline Verification:**
   - `uname -r` -> Verify `7.2.8-9-omarchy`.
   - `cat /sys/bus/spi/devices/spi-FTE3600:00/driver_override` -> Verify clean (`""`).
   - `readlink -f /sys/bus/spi/devices/spi-FTE3600:00/driver` -> Verify `UNBOUND`.
   - `lsmod` -> Verify `fte3600` and `spidev` are absent; record baseline status of `uio`.
   - `ls -d /dev/spidev*` -> Verify absent.
2. **Prerequisite Loading & Device Binding:**
   - `modprobe --first-time uio` (executed only if `uio` was absent at baseline; tracked via `UIO_LOADED_BY_US=1`).
   - `modprobe spidev` (tracked via `SPIDEV_LOADED_BY_US=1`).
   - `echo "spidev" > /sys/bus/spi/devices/spi-FTE3600:00/driver_override` (tracked via `DRIVER_OVERRIDE_SET_BY_US=1`).
   - `echo "spi-FTE3600:00" > /sys/bus/spi/drivers/spidev/bind` (tracked via `SPIDEV_BOUND_BY_US=1`).
   - `insmod kernel/fte3600.ko` (tracked via `FTE3600_LOADED_BY_US=1`).
   - Read `/sys/bus/spi/devices/spi-FTE3600:00/fte3600-glue.3.auto/fte3600_status` -> Verify `ready`.
3. **Sensor Identification Execution:**
   - Execute binary:
     ```bash
     ./bin/stage2a-identify /dev/spidev1.0
     ```
4. **Controlled Host-Side Rollback (Always Executed):**
   - Step 1: `rmmod fte3600`
   - Step 2: `echo "spi-FTE3600:00" > /sys/bus/spi/drivers/spidev/unbind`
   - Step 3: `echo "" > /sys/bus/spi/devices/spi-FTE3600:00/driver_override`
   - Step 4: `modprobe -r spidev`
   - Step 5: `modprobe -r uio` (executed only if loaded by us)
   - Step 6: Post-rollback verification: check module absence and unbound status.

---

## 3. Exact SPI Byte Sequences Dispatched by `bin/stage2a-identify`

The C utility issues the exact sequence reviewed in the Stage 2A checklist, with zero deviation:

```
[Transaction 1: Write 0xC6 = 0x01]
  CS Asserted Low -> Clock 4 bytes:
  MOSI: [ 0x09, 0xF6, 0xC6, 0x01 ]
  MISO: [ 0x00, 0x00, 0x00, 0x00 ] (Ignored)
  CS Deasserted High

[Inter-Transaction Delay: 4 ms]
  CS Held High -> usleep(4000)

[Transaction 2: Read 0xC6 Verification]
  CS Asserted Low -> Clock 5 bytes:
  MOSI: [ 0x08, 0xF7, 0xC6, 0x00, 0x00 ]
  MISO: [  ?? ,  ?? ,  ?? ,  ?? , 0x01 ]
  CS Deasserted High
  -> Evaluates buffer[4]: Must equal 0x01. If != 0x01, STOP immediately.

[Transaction 3: Read Chip ID 0x1A8B]
  CS Asserted Low -> Clock 10 bytes:
  MOSI: [ 0x04, 0xFB, 0x9A, 0x8B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 ]
  MISO: [  ?? ,  ?? ,  ?? ,  ?? ,  ?? ,  ?? ,  D1 ,  D0 , CRCH, CRCL ]
  CS Deasserted High
  -> Extracts: chip_id = (D1 << 8) | D0
  -> Evaluates:
     - If chip_id == 0x0000 or 0xFFFF -> STOP immediately (Bus inactive/float).
     - Validates CRC-CCITT (poly 0x1021, seed 0xFFFF) across D1 and D0.
     - Enforces variant gate: Must be 0x9362, 0x9365, or 0x9391. Any other ID -> STOP.

[Transaction 4: Conditional Read Sub-Revision 0x1816 (Only if Chip ID == 0x9391)]
  CS Asserted Low -> Clock 10 bytes:
  MOSI: [ 0x04, 0xFB, 0x98, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 ]
  MISO: [  ?? ,  ?? ,  ?? ,  ?? ,  ?? ,  ?? ,  E1 ,  E0 , CRCH, CRCL ]
  CS Deasserted High
  -> Extracts: sub_rev = (E1 << 8) | E0
  -> Validates CRC-CCITT on E1 and E0.
  -> Evaluates: if sub_rev == 0x0FFF -> Identifies as FT9395. Otherwise FT9391.
```

---

## 4. Error Handling and Rollback Behavior

The implementation enforces a **strict deterministic STOP policy**:

1. **Granular Exit Codes from `bin/stage2a-identify`:**
   - `0`: Success (Identification completed, CRC validated, known silicon).
   - `1`: Invocation / file open error.
   - `2`: SPI parameter configuration ioctl failure.
   - `3`: Write transport error on register `0xC6`.
   - `4`: Read transport error on `0xC6` or `0x1A8B`.
   - `5`: Verification value mismatch (`buffer[4] != 0x01`).
   - `6`: Bus float / inactive (`0x0000` or `0xFFFF`).
   - `7`: CRC mismatch.
   - `8`: Unknown or unproven Chip ID.
   - `9`: Sub-revision read failure.
2. **Harness Trap & Rollback Response:**
   - Any non-zero exit code halts execution immediately.
   - No retry loops exist.
   - No recovery commands (`0x5A/0xA5/0xC0`) are sent.
   - The shell script trap immediately triggers `cleanup()`, ensuring all kernel modules (`fte3600`, `spidev`, `uio`) are removed, `driver_override` is cleared, and `spi-FTE3600:00` is unbound.
3. **Signal Protection:**
   - `SIGINT` (Ctrl+C) and `SIGTERM` are deferred during atomic state changes and intercepted to trigger emergency host rollback.

---

## 5. Unresolved Risks and Constraints

1. **Inferred Sensor Residual State:**  
   Writing `0xC6 = 0x01` sets the internal SPI clock-gating flip-flop inside the sensor. Because the test harness does not cycle motherboard power and does not pulse hardware reset Pad 14, **register `0xC6` is inferred to remain set to `0x01` inside the sensor until the next cold reboot / power cycle**. The presence or absence of an internal auto-sleep timer is undocumented by FocalTech.
2. **Zero-CRC Heuristic:**  
   If the sensor returns `0x0000` in the CRC field, the reference-driver heuristic treats it as acceptable. The test tool logs this explicitly as `UNVERIFIED (Zero-CRC Exception)` rather than asserting data integrity.
3. **Hardware Documentation Absence:**  
   All timing and framing behaviors are derived from open-source vendor driver sources (`ft93xx.c`, `fw9369_startup.c`, `factory_startup.c`) and test suite mocks. No authoritative silicon datasheet exists.

---

## 6. Pre-Execution Statement

**STATEMENT OF COMPLIANCE:**  
Phase 1 preparation is strictly complete. **Zero hardware operations were conducted.** No kernel modules were loaded, no sysfs attributes were modified, and no bus transactions occurred.

**NEXT ACTION:**  
Stop and await explicit user confirmation in this conversation before executing Phase 3.
