# Stage 2A — Operator Procedure and Execution Manual (v2.0 Pure spidev)

**Target Unit:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Operating System:** Omarchy Linux (`7.2.8-9-omarchy`, x86_64)  
**Target Hardware:** FocalTech Fingerprint Sensor on Intel GSPI #1 (`spi-FTE3600:00`)  
**Execution Script:** `scripts/run-stage2a-in-terminal.sh` (or `sudo scripts/stage2a-test.sh`)  
**Architecture:** Pure `spidev` (No `fte3600.ko`, No UIO, No GPIO manipulation)  
**Status:** MANUAL AUTHORIZATION GATE — DO NOT RUN WITHOUT EXPLICIT OPERATOR CONSENT

---

## 1. Purpose and Exact Scope of Stage 2A

### 1.1 Purpose
The sole purpose of Stage 2A is to safely and non-destructively interrogate the physical silicon connected to the SPI bus to identify the exact FocalTech chip model (e.g., `FT9362`, `FT9365`, `FT9391`, or `FT9395`).

### 1.2 Exact Scope
The identification utility executes up to four discrete SPI transactions over `/dev/spidev1.0`:
1. **Operation 1:** Write `0x01` to volatile clock-gating register `0xC6` (`09 F6 C6 01`, 4 bytes).
2. **Operation 2:** 4 ms stabilization delay with Chip Select (CS) held deasserted high.
3. **Operation 3:** Read back register `0xC6` (`08 F7 C6 00 00`, 5 bytes) and verify that `buffer[4] == 0x01`.
4. **Operation 4:** Read Chip ID register `0x1A8B` (`04 FB 9A 8B 00 00 00 00 00 00`, 10 bytes), validate framing, compute CRC-CCITT over payload bytes 6–7, and match against evidenced models.
5. **Operation 5 (Conditional):** Read sub-revision register `0x1816` (`04 FB 98 16 ...`, 10 bytes) **strictly if and only if** the base Chip ID is `0x9391` (to distinguish `FT9395` from `FT9391`).
6. **Teardown & Rollback:** Close SPI file descriptor, unbind device from `spidev`, clear `driver_override`, unload `spidev` kernel module, and exit cleanly.

### 1.3 Strict Operational Boundaries
- ❌ **Zero retries:** No transaction retry loops.
- ❌ **Zero recovery writes:** No blind re-initialization or error recovery packets.
- ❌ **Zero reset pulsing:** Hardware reset line (Pad 14) is never pulsed or toggled.
- ❌ **Zero GPIO manipulation:** Neither Pad 14 nor Pad 229 is claimed, leased, or reconfigured.
- ❌ **Zero out-of-tree modules:** `fte3600.ko` and `uio.ko` are completely omitted.
- ❌ **Zero commands:** Prohibited sleep commands (`0x5A/0xA5`), flash commands (`0xC0`), or AFE writes (`0xFD/0xFE`) are never issued.
- ❌ **Zero capture:** No frame capture or image acquisition operations.

---

## 2. Hardware and Software Prerequisites

Before executing the manual command, the operator must verify the following:

| Item | Requirement | How to Verify (Read-Only) |
| :--- | :--- | :--- |
| **User Privileges** | Root / `sudo` access | `sudo -v` |
| **Kernel Version** | `7.2.8-9-omarchy` | `uname -r` |
| **Target Device Node** | `spi-FTE3600:00` present in sysfs | `ls -d /sys/bus/spi/devices/spi-FTE3600:00` |
| **Pre-Test Binding** | SPI device must be `UNBOUND` | `ls -l /sys/bus/spi/devices/spi-FTE3600:00/driver 2>&1` (must return "No such file") |
| **Pre-Test Override** | `driver_override` must be empty | `cat /sys/bus/spi/devices/spi-FTE3600:00/driver_override` (must return empty / `\n`) |
| **Module State** | `spidev` and `fte3600` must be `ABSENT` | `lsmod \| grep -E "spidev\|fte3600"` (must return empty) |
| **Device Nodes** | No pre-existing `/dev/spidev*` | `ls -l /dev/spidev* 2>&1` (must return "No such file") |
| **Compiled Binary** | `bin/stage2a-identify` executable | `test -x bin/stage2a-identify && echo OK` |

---

## 3. Exact Manual Execution Command

The operator must execute the dedicated terminal launcher from the project root:

```bash
cd /home/titan/fte3600-linux-driver
./scripts/run-stage2a-in-terminal.sh
```

*(Alternatively, if running directly in an existing root terminal: `sudo ./scripts/stage2a-test.sh`)*

All outputs will be displayed in real time on the console and simultaneously recorded to:  
`diagnostics/stage2a-results.txt`

---

## 4. Expected Operations in Chronological Order

```
[Phase 1: Pre-Flight Safety Checks]
  1. Verify root privileges.
  2. Verify kernel version is 7.2.8-9-omarchy.
  3. Verify identification binary exists and is executable.
  4. Verify spi-FTE3600:00 exists in sysfs.
  5. Verify driver_override is clean ("").
  6. Verify spi-FTE3600:00 is UNBOUND.
  7. Verify spidev module is ABSENT.
  8. Verify no pre-existing /dev/spidev* character devices exist.
  9. Verify fte3600 module is ABSENT.
  10. Read-only sanity check of PCH Pad 14 (output high 0x84000201).

[Phase 2: Host Interface Activation]
  11. Load mainline spidev module (modprobe spidev).
  12. Verify spidev is PRESENT; record SPIDEV_LOADED_BY_US=1.
  13. Write "spidev" to spi-FTE3600:00/driver_override; record DRIVER_OVERRIDE_SET_BY_US=1.
  14. Bind spi-FTE3600:00 to spidev driver; record SPIDEV_BOUND_BY_US=1.
  15. Verify /dev/spidev1.0 is created and is a character device.

[Phase 3: Sensor Identification Execution]
  16. Open /dev/spidev1.0; configure SPI Mode 0, 8 bits/word, 1 MHz.
  17. Read back and verify SPI parameters via ioctl.
  18. Write 0xC6 = 0x01 (4 bytes: 09 F6 C6 01).
  19. Wait 4 ms (usleep(4000)) with CS held deasserted high.
  20. Read register 0xC6 (5 bytes: 08 F7 C6 00 00).
  21. Verify rx[4] == 0x01 (Transceiver clock active).
  22. Read Chip ID 0x1A8B (10 bytes: 04 FB 9A 8B 00 00 00 00 00 00).
  23. Check for bus float (0x0000 / 0xFFFF).
  24. Validate CRC-CCITT over payload bytes 6-7.
  25. Match Chip ID against variant gate (0x9362, 0x9365, 0x9391).
  26. (If ID == 0x9391): Read sub-revision 0x1816 (10 bytes: 04 FB 98 16 ...).
  27. Close /dev/spidev1.0; print identification summary; exit 0.

[Phase 4: Controlled Host Rollback]
  28. Unbind spi-FTE3600:00 from spidev driver. Verify UNBOUND.
  29. Restore driver_override to empty (""). Verify readback.
  30. Unload spidev module (modprobe -r spidev). Verify ABSENT.
  31. Verify /dev/spidev* character devices are destroyed.
  32. Output residual sensor state notice.
  33. Terminate cleanly with tool's return code.
```

---

## 5. Expected Successful Output (Illustrative Example)

> [!IMPORTANT] Illustrative Example Only — Actual Silicon Model Unverified
> The console transcript below depicts a hypothetical run where an FT9395 sensor (`0x9391`, sub-revision `0x0FFF`) is detected. This is an **illustrative example of the expected transcript format, NOT a prediction or claim of the actual sensor identity**.
> 
> The physical sensor installed on this Dynabook unit is formally unverified until this test executes. It may report any of the evidenced models (`FT9362`, `FT9365`, `FT9391`, or `FT9395`).
> 
> Furthermore, **chip identification is NOT fingerprint capture**. Successful execution merely reads static silicon identification registers and releases all host resources. No fingerprint scanning, image acquisition, or biometric enrollment occurs.

A successful run will produce console output structured as follows:

```text
========================================================
 FTE3600 Stage 2A Supervised Sensor Identification (Pure spidev)
 Host: Dynabook Satellite Pro C40-J
 Kernel: 7.2.8-9-omarchy (x86_64)
 Date: 2026-11-02T...
 Architecture: Pure spidev (No fte3600.ko, No UIO, No GPIO manipulation)
========================================================

--- 1. Pre-Execution Safety Audits (Read-Only) ---
[PASS] Running kernel matches expectation: 7.2.8-9-omarchy
[PASS] Identification binary verified: .../bin/stage2a-identify
[PASS] SPI device spi-FTE3600:00 exists.
[PASS] driver_override on spi-FTE3600:00 is clean ('').
[PASS] spi-FTE3600:00 is confirmed UNBOUND.
[PASS] spidev is confirmed ABSENT.
[PASS] No pre-existing /dev/spidev* device nodes.
[PASS] fte3600 is confirmed ABSENT (pure spidev architecture).
[INFO] PCH Pad 14 pinctrl state: pin 14 (SPKR) 14:INT34C5:00 GPIO 0x84000201 0x0003c026 ...
[PASS] Pad 14 matches verified firmware output-high state (0x84000201).

--- 2a. Loading spidev module ---
[PASS] spidev loaded successfully.

--- 2b. Setting driver_override and Binding spidev ---
[PASS] driver_override set to spidev.
[PASS] spi-FTE3600:00 confirmed bound to spidev driver.
[PASS] Target spidev character device identified: /dev/spidev1.0

========================================================
 Executing Stage 2A Minimal Sensor Identification Sequence
 Target: /dev/spidev1.0
========================================================

>> Stage 2A FocalTech Identification Tool
>> Target device: /dev/spidev1.0
>> SPI configuration verified: Mode 0 (CPOL=0, CPHA=0), 8 bits/word, 1000000 Hz

--- Operation 1: Writing volatile register 0xC6 = 0x01 ---
MOSI: [ 0x09 0xF6 0xC6 0x01 ]
>> Operation 1 completed (CS deasserted high).

--- Operation 2: Delaying 4 ms (transceiver stabilization) ---
>> Delay complete.

--- Operation 3: Reading register 0xC6 for verification ---
MOSI: [ 0x08 0xF7 0xC6 0x00 0x00 ]
MISO: [ 0x?? 0x?? 0x?? 0x?? 0x01 ]
>> Register 0xC6 readback value: 0x01
>> Register 0xC6 verified: SPI transceiver clock active.

--- Operation 4: Interrogating Chip ID register 0x1A8B ---
MOSI: [ 0x04 0xFB 0x9A 0x8B 0x00 0x00 0x00 0x00 0x00 0x00 ]
MISO: [ 0x?? 0x?? 0x?? 0x?? 0x?? 0x?? 0x93 0x91 0xC3 0x2F ]
>> Raw Chip ID word (buffer[6..7]): 0x9391
>> Received CRC-CCITT (buffer[8..9]): 0xC32F (Calculated: 0xC32F)
>> CRC validation: VERIFIED (CRC matches)
>> Preliminary Identification: Chip ID 0x9391 -> FT9391 family ...

--- Operation 5: Reading sub-revision register 0x1816 ---
MOSI: [ 0x04 0xFB 0x98 0x16 0x00 0x00 0x00 0x00 0x00 0x00 ]
MISO: [ 0x?? 0x?? 0x?? 0x?? 0x?? 0x?? 0x0F 0xFF 0x13 0xC1 ]
>> Sub-revision word (buffer[6..7]): 0x0FFF
>> Sub-revision CRC (buffer[8..9]): 0x13C1 (Calculated: 0x13C1)
>> Final Silicon Model: FT9395 (Sub-revision 0x0FFF identified)

===================================================================
STAGE 2A IDENTIFICATION RESULT: SUCCESS
Identified Chip ID: 0x9391
Sub-Revision Word: 0x0FFF
Silicon Classification: FT9395 (Sub-revision 0x0FFF identified)
CRC Verification: PASS (Exact match)
===================================================================

>> [STAGE 2A RESULT: SUCCESS] Sensor identification completed successfully.

--- 4. Initiating Controlled Rollback ---

========================================================
 Stage 2A Rollback Harness (Pure spidev)
 Timestamp: 2026-11-02T...
========================================================
>> Rollback step 1: Unbinding spidev from spi-FTE3600:00...
   [OK] spi-FTE3600:00 unbind succeeded. Confirmed UNBOUND.
>> Rollback step 2: Restoring driver_override on spi-FTE3600:00...
   [OK] driver_override restored to ''.
>> Rollback step 3: Unloading spidev module...
   [OK] modprobe -r spidev succeeded. Module ABSENT.

--- Post-Rollback State Verification ---
>> Checking spidev module state post-rollback: ABSENT
>> Checking SPI device driver link: UNBOUND
>> Checking /dev device nodes:
ls: cannot access '/dev/spidev*': No such file or directory

>> Sensor Internal State Notice:
   Host-side resources are fully released, bus clock is halted, and CS is deasserted high.
   PCH Pad 14 (reset) and Pad 229 (IRQ) were never claimed or altered.
   The internal state of register 0xC6 cannot be confirmed as restored by host cleanup.
   Whether 0xC6 retains 0x01, autonomously decays, or resets via an undocumented internal
   watchdog timer is unknown. Host cleanup guarantees host isolation only.

========================================================
 ROLLBACK COMPLETED CLEANLY (0 ERRORS)
 Verified: spidev ABSENT, spi-FTE3600:00 UNBOUND, /dev/spidev* ABSENT
========================================================
```

---

## 6. Expected Failure Conditions (Strict Immediate STOP)

If ANY of the following conditions occur, the script halts immediately, initiates automated rollback, and exits with a distinct non-zero exit code:

| Exit Code | Failure Reason | Symptom / Diagnostic Message | Mandatory Action |
| :---: | :--- | :--- | :--- |
| **1** | Invocation / Parameter / Pre-flight Error | Missing root, kernel mismatch, or target node missing/invalid (`!S_ISCHR`). | Halts immediately. Zero hardware changes. |
| **2** | SPI Configuration Error | `ioctl(SPI_IOC_WR_* / RD_*)` fails to configure Mode 0, 8-bit, or 1 MHz. | Closes fd. Full rollback invoked. |
| **3** | Write Transport Error | `ioctl(SPI_IOC_MESSAGE)` returns `< 4` on `0xC6` write. | Closes fd. Full rollback invoked. |
| **4** | Read Transport Error | `ioctl(SPI_IOC_MESSAGE)` returns `< 5` or `< 10` during read. | Closes fd. Full rollback invoked. |
| **5** | Verification Mismatch | Readback of register `0xC6` returns `!= 0x01`. | Closes fd. Full rollback invoked. |
| **6** | Bus Ground / Floating Error | Chip ID returns `0x0000` (grounded) or `0xFFFF` (floating/unpowered). | Closes fd. Full rollback invoked. |
| **7** | CRC Mismatch | Computed CRC-CCITT does not match received CRC bytes (non-zero). | Closes fd. Full rollback invoked. |
| **8** | Unknown / Unsupported Chip ID | Chip ID is not `0x9362`, `0x9365`, or `0x9391`. | Closes fd. Full rollback invoked. |
| **9** | Sub-Revision Failure | Transport or CRC error while querying register `0x1816`. | Closes fd. Full rollback invoked. |
| **130** | User Cancel (`SIGINT` / Ctrl+C) | Operator pressed Ctrl+C. | Signal deferred until atomic step finishes; full rollback invoked. |
| **143** | Process Terminated (`SIGTERM`) | System kill signal received. | Signal deferred until atomic step finishes; full rollback invoked. |

---

## 7. What the Script Does During Rollback

Regardless of whether the test succeeds or fails at any step, `cleanup()` executes the following:
1. **Unbinds `spi-FTE3600:00`:** Writes `spi-FTE3600:00` to `/sys/bus/spi/drivers/spidev/unbind`. Verifies that the driver symlink is destroyed (`UNBOUND`).
2. **Restores `driver_override`:** Writes `""` back to `/sys/bus/spi/devices/spi-FTE3600:00/driver_override`. Verifies readback confirms empty string.
3. **Unloads `spidev` Module:** Executes `modprobe -r spidev` (without `-f`). Verifies `spidev` is confirmed `ABSENT` in `/sys/module` and `/proc/modules`.
4. **Post-Rollback Audit:** Verifies `/dev/spidev*` character devices are gone, confirms `spi-FTE3600:00` is unbound, and checks error counters.

---

## 8. What the Script Cannot Guarantee About Sensor State

The operator must distinguish host-side restoration from physical sensor silicon state:
- **Host State (100% Guaranteed):** Kernel module unloaded, character device removed, driver unbound, override cleared, SPI bus clock stopped, and Chip Select held deasserted high. PCH Pad 14 and Pad 229 were never claimed or modified.
- **Sensor Internal State (Uncertain):** Writing `0xC6 = 0x01` ungated the sensor's internal transceiver clock.
  - The physical persistence, decay, or autonomous reset behavior of register `0xC6` is **undocumented** by FocalTech.
  - Whether register `0xC6` retains `0x01`, decays over time, or resets via an internal watchdog timer is an **unknown internal sensor state**.
  - Host cleanup releases host resources, but **cannot confirm the internal state of the sensor** without issuing further SPI writes (which would violate cleanup isolation).
  - To guarantee a return to the pristine, unclocked power-on baseline, the operator should perform a full ACPI S5 cold reboot or power cycle.

---

## 9. Read-Only Post-Run Verification Commands

After the script finishes, the operator can independently confirm complete host restoration by running these non-privileged, read-only commands:

```bash
# 1. Confirm spidev and fte3600 modules are completely absent:
lsmod | grep -E "spidev|fte3600|uio"
# (Expected: No output)

# 2. Confirm SPI device is completely unbound:
ls -l /sys/bus/spi/devices/spi-FTE3600:00/driver 2>&1
# (Expected: ls: cannot access ...: No such file or directory)

# 3. Confirm driver_override is clean:
cat /sys/bus/spi/devices/spi-FTE3600:00/driver_override
# (Expected: empty line)

# 4. Confirm no /dev/spidev character devices exist:
ls -l /dev/spidev* 2>&1
# (Expected: ls: cannot access ...: No such file or directory)
```

---

## 10. Information to Collect for Stage 3 Planning

Upon completion, the operator should inspect `diagnostics/stage2a-results.txt` and report:
1. **Raw Chip ID Word:** Sensed 16-bit value from register `0x1A8B` (bytes 6 and 7).
2. **Sub-Revision Word:** Sensed 16-bit value from register `0x1816` (if queried).
3. **CRC Validation Telemetry:** Received CRC word vs computed CRC-CCITT (and whether exact match or zero-exception was observed).
4. **Exact Model Identification:** Final silicon model classification (`FT9362`, `FT9365`, `FT9391`, or `FT9395`).
5. **Rollback Error Count:** Confirmed 0 rollback errors.
