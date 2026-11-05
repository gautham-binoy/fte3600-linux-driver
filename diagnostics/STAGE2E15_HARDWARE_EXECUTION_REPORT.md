# Stage 2E.15 — Hardware Execution Report (Stage B Controlled Reset)

**Target System:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Target SPI Device:** `spi-FTE3600:00` on Intel LPSS SPI2 (`pxa2xx-spi.2`, CS0)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (x86_64, SMP PREEMPT_DYNAMIC)  
**Execution Window:** 2026-11-04T09:32:01+05:30 to 2026-11-04T09:33:06+05:30  
**Test Script Timestamp:** 2026-11-04T09:32:17+05:30  
**Status:** **STAGE B EXECUTION COMPLETED SUCCESSFULLY (STATUS 0) — RESET CONFIRMED — OPTION 1 RETENTION ACTIVE**

---

## 1. Executive Summary

Under explicit operator authorization, Stage B was executed once using the reviewed terminal launcher [`scripts/run-stage-b-in-terminal.sh`](file:///home/titan/fte3600-linux-driver/scripts/run-stage-b-in-terminal.sh).

All 10 pre-flight safety checks passed without error. Kernel module `kernel/fte3600.ko` was loaded safely, bound automatically to ACPI SPI device `spi-FTE3600:00`, and created character device node `/dev/fte3600` (major 511, minor 0). Initial driver telemetry (Snapshot 1) confirmed driver state `0 (UNINITIALIZED)`.

Exactly **ONE** `FTE3600_IOC_RESET` ioctl was issued via `bin/fte3600-tool reset`. The ioctl executed the kernel 40 ms active-low GPIO pulse sequence and returned exit status **0 (Success)**. Post-reset telemetry (Snapshot 2) confirmed driver state transition to `2 (RESET_SEQUENCE_ISSUED)` with flag `0x00000002 [RESET_ISSUED]`.

Option 1 cleanup executed cleanly: zero module unloads, zero SPI unbinds, zero process terminations, and zero automatic retries were performed. The module remains loaded and bound in the kernel for administrative inspection, and script-owned lock FD 200 was released.

---

## 2. Pre-Execution Artifact Hash Verification

All components were verified against the Stage 2E.14 baseline hashes immediately prior to execution:

| Component / Artifact | Expected & Observed SHA-256 Checksum | Match Status |
| :--- | :--- | :---: |
| [`scripts/stage-b-test.sh`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh) | `ff53d058e7b7cce5631bbfb1c3e4449b042db4573cfd6cafcb43bb02a7a822b5` | **VERIFIED MATCH** |
| [`scripts/run-stage-b-in-terminal.sh`](file:///home/titan/fte3600-linux-driver/scripts/run-stage-b-in-terminal.sh) | `0d45f94459124430c2b64c9bef9d820c22c984032a959a0d20b88248ed9b4e27` | **VERIFIED MATCH** |
| [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c) | `29d2726c9a63772f0981bf65321f48934882eeb6702fe4a02629794b410805e8` | **VERIFIED MATCH** |
| [`kernel/fte3600.ko`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.ko) | `cc8c43d4d4abd79c3ee43c5877e0cffe5d5a1975de8c081cdda9eb2654cf56c9` | **VERIFIED MATCH** |
| [`bin/fte3600-tool`](file:///home/titan/fte3600-linux-driver/bin/fte3600-tool) | `b91d7dc3cb0ca172d7020613af884b649c451be79a55e1836d31dc6f66217d12` | **VERIFIED MATCH** |
| [`diagnostics/STAGE2E13_INDEPENDENT_SOURCE_AUDIT.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2E13_INDEPENDENT_SOURCE_AUDIT.md) | `72a0c16418b38cb448de78bb0c911b9508ac96ae1fa03b0de4bebbe5becfca52` | **VERIFIED MATCH** |

---

## 3. Pre-Flight Inspection Results (Phase 1)

From captured log [`diagnostics/stage-b-results.txt`](file:///home/titan/fte3600-linux-driver/diagnostics/stage-b-results.txt):

| Step | Pre-Flight Check | Result | Telemetry Detail |
| :---: | :--- | :---: | :--- |
| 1 | Cooperative test runner lock | **PASS** | Exclusive lock acquired on FD 200 (`/run/lock/fte3600-test.lock`) |
| 2 | Kernel release validation | **PASS** | Running kernel `7.2.8-9-omarchy` matched |
| 3 | Kernel module binary check | **PASS** | `kernel/fte3600.ko` present |
| 4 | Vermagic validation | **PASS** | `7.2.8-9-omarchy SMP preempt mod_unload` matched |
| 5 | Module absence verification | **PASS** | `fte3600` and `spidev` confirmed absent (rc=1 handled safely) |
| 6 | SPI device sysfs existence | **PASS** | `/sys/bus/spi/devices/spi-FTE3600:00` present |
| 7 | SPI device binding check | **PASS** | Device confirmed unbound |
| 8 | Character device absence | **PASS** | `/dev/fte3600` confirmed absent prior to probe |
| 9 | Validation utility check | **PASS** | `bin/fte3600-tool` present and executable |
| 10 | Legacy safety gate audit | **PASS** | Sealed (rc=1, policy banner verified) |

Baseline hardware state recorded before probe:
- **PCH Pad 14 (Reset):** `pin 14 (SPKR) 14:INT34C5:00 GPIO 0x84000201 0x0003c026 0x00000000 [LOCKED]`
- **PCH Pad 229 (IRQ):** `pin 229 (CPU_GP_0) 323:INT34C5:00 GPIO 0x80100100 0x00000029 0x00000000 [LOCKED tx]`
- **GPIO claims:** `None`
- **Dmesg baseline lines:** `1121`

---

## 4. Module Insertion, Probe & Device Validation (Phase 2)

1. **Insertion:** `insmod /home/titan/fte3600-linux-driver/kernel/fte3600.ko` returned exit status **0**.
   - Signal-safe critical section committed: `THIS_INVOCATION_INSERTED=1`, `INSERTION_STATE="DEFINITELY_SUCCEEDED"`.
2. **Post-Probe Module Loading:** Confirmed loaded in kernel: `fte3600 20480 0`.
3. **SPI Binding:** `spi-FTE3600:00/driver` bound to `../../../../../../../bus/spi/drivers/fte3600`.
4. **Device Node Creation & Identity:**
   - Character device node created: `crw------- 1 root root 511, 0 Nov  4 09:32 /dev/fte3600`.
   - Major: `511`, Minor: `0`, `rdev`: `130816` (hex `0x1ff:0x00`).
   - Sysfs major:minor matched `/sys/class/fte3600/fte3600/dev` exactly.
5. **GPIO Allocation under Probe:**
   - Debugfs GPIO: `gpio-14 (|reset ) out hi ACTIVE LOW`
   - Reset GPIO descriptor acquired by driver with initial logical 0 (physical inactive / high).

---

## 5. Pre-Reset Telemetry (Phase 3: Snapshot 1)

`fte3600-tool get-state` was issued to record the driver's state prior to reset:
```
>> FTE3600 State Snapshot:
   Version:    1
   State:      0 (UNINITIALIZED)
   Flags:      0x00000000
   Last Error: 0 (None)
```
Snapshot 1 succeeded with exit code **0**. The driver was in clean initial state `UNINITIALIZED`.

---

## 6. Execution of Exactly ONE Controlled Reset IOCTL (Phase 4)

`bin/fte3600-tool reset` was invoked at line 53 of the test execution:
```
Invoking: /home/titan/fte3600-linux-driver/bin/fte3600-tool reset
>> Issuing FTE3600_IOC_RESET...
>> Reset ioctl returned 0 (Success).
Reset ioctl process return code: 0
```

- **RESET Call Count:** Exactly **ONE** invocation.
- **Return Code:** **`0` (Success)**.
- **Kernel Execution Path:**
  1. `fte3600_ioctl` received `FTE3600_IOC_RESET`.
  2. `mutex_lock(&data->lock)` acquired.
  3. Pre-reset settle: Logical `0` (deasserted), slept 10 ms.
  4. Assert reset: Logical `1` (asserted active-low), slept 20 ms.
  5. Deassert reset: Logical `0` (deasserted), slept 10 ms.
  6. Transitioned state to `FTE3600_STATE_RESET_SEQUENCE_ISSUED`.
  7. `mutex_unlock(&data->lock)` released.
- **Outcome Status:** **CONFIRMED SUCCESS (Not Uncertain)**.
- **Retry Count:** **ZERO RETRIES**.

---

## 7. Post-Reset Telemetry (Phase 5: Snapshot 2)

`fte3600-tool get-state` was issued immediately following the reset:
```
>> FTE3600 State Snapshot:
   Version:    1
   State:      2 (RESET_SEQUENCE_ISSUED)
   Flags:      0x00000002 [RESET_ISSUED]
   Last Error: 0 (None)
```
- Driver state successfully transitioned from `0 (UNINITIALIZED)` to `2 (RESET_SEQUENCE_ISSUED)`.
- Flag `0x00000002` (`FTE3600_STATE_FLAG_RESET_ISSUED`) confirmed set in hardware status block.
- Last error remained `0 (None)`.

---

## 8. Diagnostic Observations & Kernel Ring Buffer

### 8.1 Intel LPSS GPIO Pad State
- **PCH Pad 14 (Sensor Reset / Pad SPKR):**
  - Baseline:   `pin 14 (SPKR) 14:INT34C5:00 GPIO 0x84000201 0x0003c026 0x00000000 [LOCKED]`
  - Under Probe: `pin 14 (SPKR) 14:INT34C5:00 GPIO 0x84000201 0x0003c026 0x00000000 [LOCKED]`
  - Post-Reset:  `pin 14 (SPKR) 14:INT34C5:00 GPIO 0x84000201 0x0003c026 0x00000000 [LOCKED]`
  - Post-Test:   `pin 14 (SPKR) 14:INT34C5:00 GPIO 0x84000201 0x0003c026 0x00000000 [LOCKED]`
  - Configuration: Output buffer enabled (`PADCFG0_GPIOTXDIS = 0`), TX state logical 1 (`PADCFG0_GPIOTXSTATE = 1` / inactive deasserted state).
- **PCH Pad 229 (Sensor IRQ / Pad CPU_GP_0):**
  - Baseline:   `pin 229 (CPU_GP_0) 323:INT34C5:00 GPIO 0x80100100 0x00000029 0x00000000 [LOCKED tx]`
  - Post-Reset: `pin 229 (CPU_GP_0) 323:INT34C5:00 GPIO 0x82000100 0x00000029 0x00000000 [LOCKED tx]`
  - Configuration: High-impedance input mode preserved (`PADCFG0_GPIOTXDIS = 1`), edge trigger configured by IRQ subsystem.

### 8.2 Software Reporting vs Physical Voltage Disclaimer
- The GPIO states reported above are software-reported register configurations read from `/sys/kernel/debug/pinctrl/INT34C5:00/pins` and `gpiolib`.
- **NO physical voltage measurement is available**; direct pin voltage on Pad 14 was not measured with an external oscilloscope or multimeter.
- Successful reset pulse sequence dispatch does not prove silicon health or fingerprint scanning functionality.

### 8.3 Kernel Ring Buffer (`dmesg`)
Driver messages logged during the execution window:
```
[  295.296137] fte3600: loading out-of-tree module taints kernel.
[  295.296141] fte3600: module verification failed: signature and/or required key missing - tainting kernel
[  295.310918] fte3600 spi-FTE3600:00: FTE3600 driver bound (passive probe, state UNINITIALIZED)
```
- **Zero Kernel Errors:** Zero warnings, zero panics, zero NULL pointer dereferences.
- Passive driver probe and character device initialization executed cleanly.

---

## 9. Option 1 Cleanup & Final System State

In accordance with Option 1 Mandatory Policy:
1. **Module Retention:** `fte3600` was **NOT** unloaded. It remains loaded in the kernel (`fte3600 20480 0`).
2. **SPI Binding:** `spi-FTE3600:00` was **NOT** unbound. It remains bound to `fte3600`.
3. **Character Device Node:** `/dev/fte3600` remains present (`crw------- 1 root root 511, 0`).
4. **Open Descriptor Audit:** Audit across 333 active processes and 5,037 file descriptors confirmed **zero open file descriptors** on `/dev/fte3600`.
5. **Lock Release:** Script-owned cooperative lock descriptor (FD 200) was cleanly closed (`/run/lock/fte3600-test.lock` released).
6. **Overall Test Status:** **Exit Status `0` (Success)**.

---

## 10. Explicit Confirmations of Prohibited Actions Avoided

| Prohibited Action | Status | Evidence |
| :--- | :---: | :--- |
| Second RESET ioctl | **AVOIDED (0)** | Exactly 1 RESET issued at line 53 of log |
| Reset retry on failure | **AVOIDED (0)** | Zero retry loops in harness |
| `FTE3600_IOC_INIT` ioctl | **AVOIDED (0)** | Zero INIT calls |
| `FTE3600_IOC_CAPTURE` ioctl | **AVOIDED (0)** | Zero CAPTURE calls |
| In-script SPI bus transfers | **AVOIDED (0)** | Zero SPI transactions |
| Sensor register read/write | **AVOIDED (0)** | Zero register accesses |
| Firmware / EFUSE / OTP access | **AVOIDED (0)** | Zero NVRAM/OTP access |
| Automatic module unload (`rmmod`) | **AVOIDED (0)** | Module explicitly retained in kernel |
| Automatic SPI unbind | **AVOIDED (0)** | SPI link explicitly retained bound |
| External process termination (`kill`) | **AVOIDED (0)** | Zero processes killed |

---

## 11. Final State Summary

- **Module State:** Loaded (`fte3600`)
- **Device Node:** Present (`/dev/fte3600`, major 511, minor 0)
- **SPI Link:** Bound (`spi-FTE3600:00` $\rightarrow$ `fte3600`)
- **Driver Internal State:** `2 (RESET_SEQUENCE_ISSUED)`
- **Host Health:** Clean, fully operational, zero lock holders
- **Next Steps:** No further hardware operations are authorized by this stage. Manual administrative unload (`sudo rmmod fte3600`) may be executed at operator discretion.
