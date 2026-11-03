# Stage 2E.5 Hardware Test Plan and Independent Safety Review

**Project:** FocalTech FT9362 / FW9369 Unified Linux Driver (`fte3600.ko`)  
**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Host Controller:** Intel Tiger Lake-LP PCH LPSS SPI2 Controller (`pxa2xx-spi.2`, ACPI ID `FTE3600:00` at `spi1.0`)  
**Operating Environment:** Linux `7.2.8-9-omarchy` (SMP preempt mod_unload x86_64)  
**Safety Gate Status:** CLOSED (`bin/fw9369-hw-tool` exit code 1; zero hardware execution authorized)  
**Execution Policy:** PLAN AND STATIC REVIEW ONLY. Absolutely zero hardware operations authorized in this stage.  
**Authoritative Verdict:** `DOCUMENTATION FINALIZED — READY FOR USER AUTHORIZATION DECISION`

---

## 1. Executive Summary & Purpose

Following the completion of Stage 2E.4.2 software corrections and Stage 2E.4.3 static audit, this document establishes a risk-controlled, multi-stage hardware validation plan for the unified `fte3600.ko` kernel driver on the target Dynabook Satellite Pro C40-J laptop.

### Core Objectives:
1. Divide runtime hardware validation into **three strictly decoupled, independently authorized stages**:
   - **Stage A:** Module Probe & Device Registration (software-passive, electrically non-passive).
   - **Stage B:** Explicit Reset IOCTL Dispatch.
   - **Stage C:** Explicit Chip Identification IOCTL Sequence.
2. Establish a clear **hardware baseline** distinguishing verified physical evidence (Stage 2A) from historical deviations and unknown hardware states.
3. Establish comprehensive **rollback and recovery protocols** for each stage, documenting manual operator procedures, sysfs verification, host resource release, and hardware state limitations.
4. Define a detailed **13-case failure matrix** governing driver reporting, execution blocking, host cleanup, and sensor state evaluation using strictly evidence-based classifications.
5. Provide a standalone, read-only userspace verification tool specification for issuing ioctls and recording telemetry without scripts or hidden layers.

> [!IMPORTANT]
> **Safety Notice:** This document is a planning and review artifact. It does **not** authorize module loading, device binding, GPIO manipulation, or SPI communication. Hardware execution remains barred until separate user authorization is explicitly granted.

---

## 2. Hardware Baseline & Historical Deviations

### 2.1 Verified Physical Evidence (Stage 2A)
On November 2, 2026, Stage 2A executed a supervised, pure-`spidev` sensor identification on this specific host (`diagnostics/stage2a-results.txt`):
- **Host System:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), BIOS version verified.
- **Kernel:** `7.2.8-9-omarchy` (x86_64).
- **SPI Interface:** `spi1.0` (`spi-FTE3600:00`) on Intel LPSS SPI2 controller (`pxa2xx-spi.2`).
- **Bus Parameters:** SPI Mode 0 (CPOL=0, CPHA=0), 8 bits per word, 1,000,000 Hz (1 MHz).
- **Physical Sensor Interrogation Results:**
  - Operation 1 (Wake Write): Transmitted `[0x09, 0xF6, 0xC6, 0x01]`.
  - Operation 2 (Delay): 4 ms CS-high transceiver stabilization delay.
  - Operation 3 (Wake Readback): Transmitted `[0x08, 0xF7, 0xC6, 0x00, 0x00]`, received `rx[4] == 0x01`.
  - Operation 4 (Chip ID Read): Transmitted `[0x04, 0xFB, 0x9A, 0x8B, 0x00 * 6]`, received Chip ID `0x9362` at `rx[6..7]` and wire CRC-16 `0x1C53` at `rx[8..9]`.
  - Wire CRC Verification: `0x1C53` matched calculated CCITT polynomial (`0x1021`, seed `0xFFFF`).
  - Silicon Identity: Positively identified as **FocalTech FT9362 / FW9369**.
- **Stage 2A Rollback:** Unbound `spidev`, restored `driver_override` to empty `''`, unloaded `spidev.ko`. All host-side resources were released cleanly with 0 errors.

### 2.2 Historical Deviations & Hardware Unknowns
The test plan does **not** assume the Stage 2A state persists:
1. **Pad 229 (IRQ) Configuration Deviation:**
   - BIOS Boot Baseline: `0x80100100 0x00000029` (Level triggered, routed to IOxAPIC).
   - Post-Stage 1 State: `0x82000100 0x00000029` (Edge triggered, non-IOxAPIC).
   - Investigation ([`STAGE1_GPIO_RESTORATION_INVESTIGATION.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE1_GPIO_RESTORATION_INVESTIGATION.md)): The transition was programmed by `pinctrl-intel` when an earlier prototype requested an edge IRQ. The Linux GPIO/pinctrl subsystem does not restore BIOS register states on descriptor release.
   - **Baseline Rule:** Pad 229 differs from BIOS boot state and **must not be described as restored**. It remains in high-impedance input mode (`PADCFG0_GPIOTXDIS = 1`), unclaimed in `debugfs/gpio`.
2. **Pad 14 (Reset) Physical Voltage Unmeasured:**
   - Intel PCH register value: `0x84000201 0x0003c026` (`SPKR`, GPIO output, locked).
   - While the host controller register indicates output-high, the physical voltage on the motherboard trace to the sensor has **never been measured with an oscilloscope or multimeter**. No static audit may claim that the physical pin or sensor trace actually reached HIGH.
3. **Sensor Internal Silicon State Unknown:**
   - Stage 2A wrote `0xC6 = 0x01` to un-gate the sensor SPI transceiver clock.
   - Host rollback halted SPI clock and deasserted CS, but did not assert reset.
   - Whether register `0xC6` retains `0x01`, decayed upon power saving, or reset via an undocumented internal watchdog timer is **completely unknown**.
   - Host cleanup guarantees host isolation only; it cannot restore or verify sensor-internal silicon state.

---

## 3. Independent Multi-Stage Hardware Test Procedures

Testing is divided into three distinct stages. Each stage requires independent user review and explicit command-line execution. An unexpected result in any stage immediately halts the procedure.

```
┌─────────────────────────────────────────────────────────────┐
│                 STAGE A: PROBE & REGISTRATION               │
│  - insmod fte3600.ko                                        │
│  - Verify ACPI match, SPI binding, /dev/fte3600 creation   │
│  - Query initial state via GET_STATE (must be UNINITIALIZED)│
│  - Zero SPI transfers; zero GPIO reset pulses               │
│  - WARNING: devm_gpiod_get requests output HIGH (Pad 14)    │
│  - Electrically NON-PASSIVE: may release active reset clamp │
└──────────────────────────────┬──────────────────────────────┘
                               │ User Authorization
                               ▼
┌─────────────────────────────────────────────────────────────┐
│                 STAGE B: EXPLICIT RESET                     │
│  - Issue FTE3600_IOC_RESET ioctl                            │
│  - Pulse Pad 14: 10 ms pre, 20 ms assert, 10 ms post        │
│  - Verify state == RESET_SEQUENCE_ISSUED (2)                │
│  - Zero SPI transfers; STOP immediately                     │
│  - Evaluates pulse dispatch, NOT physical silicon recovery  │
└──────────────────────────────┬──────────────────────────────┘
                               │ User Authorization
                               ▼
┌─────────────────────────────────────────────────────────────┐
│             STAGE C: EXPLICIT IDENTIFICATION                │
│  - Issue FTE3600_IOC_INIT ioctl                             │
│  - Step A: 0xC6=0x01 write (4 B)                            │
│  - Step B: 4 ms stabilization delay                         │
│  - Step C: 0xC6 readback verify rx[4]==0x01 (5 B)           │
│  - Step D: 0x1A8B read (10 B) -> Chip ID 0x9362, CRC 0x1C53 │
│  - Verify state == IDENTIFIED (8), NOT READY                │
│  - Verify FTE3600_IOC_CAPTURE returns -1 (errno=EOPNOTSUPP) │
└─────────────────────────────────────────────────────────────┘
```

---

### 3.1 Stage A — Module Probe & Device Registration

#### 3.1.1 Objective & Scope
Confirm that `fte3600.ko` loads cleanly, matches ACPI device `FTE3600:00`, binds to `spi-FTE3600:00`, acquires the reset GPIO descriptor, configures SPI bus parameters, creates `/dev/fte3600`, and reports `FTE3600_STATE_UNINITIALIZED` (0) to userspace.

#### 3.1.2 Driver Probe Execution & Electrical Side Effects
Source inspection of [`fte3600_probe()`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L557-L634) establishes the exact software and electrical behavior of probe:

1. **Software-Passive Operations:**
   - **Zero SPI Transfers:** `fte3600_spi_sync_transfer()` is **never called** in `fte3600_probe()`. Chip Select remains deasserted, and no clocks or data are transmitted over MOSI.
   - **Zero Reset Pulses:** `fte3600_hw_reset_pulse()` is **never called** in `fte3600_probe()`. The 10ms-20ms-10ms timing pulse is not issued.
   - **SPI Bus Configuration:** `spi_setup(spi)` configures host LPSS controller registers (Mode 0, 8-bit, 1 MHz) without activating the SPI bus.
   - **Driver State Initialization:** `data->state` is initialized strictly to `FTE3600_STATE_UNINITIALIZED` (0).

2. **Active Electrical Side Effects (Reset GPIO Acquisition):**
   - Line 602 executes:
     ```c
     data->reset_gpiod = devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_LOW);
     ```
   - **Direction & Logical Value Request:** In Linux `gpiolib` (`include/linux/gpio/consumer.h`, `drivers/gpio/gpiolib.c`), the `GPIOD_OUT_LOW` request flag sets `GPIOD_FLAGS_BIT_DIR_SET | GPIOD_FLAGS_BIT_DIR_OUT` and logical value 0. This instructs the GPIO subsystem to actively configure the pad as an **output** pin with an initial logical value of 0.
   - **Active-Low Transformation & Expected Polarity:**
     - The ACPI GPIO mapping table specifies `active_low = true` for `reset-gpios` (`fte3600_reset_gpios = { 0, 0, true }`).
     - Inside `gpiod_direction_output()`, `test_bit(FLAG_ACTIVE_LOW, &desc->flags)` is true, so logical 0 is inverted (`value = !value`), requesting raw physical value `1`.
     - The expected physical polarity driven onto the line is therefore **physical HIGH** (`TXSTATE = 1`), which corresponds to the inactive reset voltage level.
   - **Callbacks and MMIO Register Operations:**
     - `gpiolib` executes `gpiod_direction_output_raw_commit(desc, 1)`, which invokes the Intel GPIO controller callback `chip->direction_output()` (`intel_gpio_direction_output()` in `drivers/pinctrl/intel/pinctrl-intel.c`).
     - This callback performs active **MMIO register writes** to Pad 14's `PADCFG0` register on Intel Tiger Lake GPIO controller `INT34C5:00`: clearing `PADCFG0_GPIOTXDIS` (bit 8 = 0, enabling the output transmitter) and setting `PADCFG0_GPIOTXSTATE` (bit 0 = 1, asserting high output level).
   - **Pin State Alteration & Non-Preservation:**
     - Linux `gpiolib` does **not** read or preserve the prior hardware state of Pad 14 before applying the requested configuration. It unconditionally enforces the requested direction and output value.
     - If the pin was previously configured as high-impedance input (tri-stated) or was driving physical LOW, acquiring the descriptor actively reconfigures the direction and forces the physical output to HIGH.
   - **Electrical Impact on Sensor Silicon (Potential Startup Release):**
     - Because the sensor reset line is active-low, driving the physical line HIGH **releases hardware reset**.
     - If the sensor silicon was previously held in reset (e.g. held low by a pull-down resistor, hardware power-on clamp, EC signal, or prior system state), driving physical HIGH removes the reset clamp.
     - Releasing the reset clamp can permit the sensor's internal silicon circuits, internal oscillator, or power-management state machine to begin autonomous startup or idle sequencing.
   - **Explicit Plan Disclosure:**
     Stage A may change the sensor's reset condition and electrical state even though it issues no deliberate reset pulse and no SPI transfers. The probe is **software-passive**, but **electrically non-passive**.
   - **Semantic Distinctions:**
     - *GPIO Request & Configuration:* Software call `devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_LOW)`.
     - *Logical GPIO Value:* Logical 0 (deasserted reset in driver logic).
     - *Expected Physical Polarity:* Physical HIGH (due to `active_low = true` inversion).
     - *Independently Measured Voltage:* **NEVER MEASURED** (no oscilloscope or multimeter verification has ever been performed; static audit cannot claim that the sensor pin actually reached HIGH).
     - *Sensor Internal State:* **UNKNOWN** (the plan strictly refrains from claiming that the sensor actually starts, resets, or reaches any particular silicon state).

#### 3.1.3 Pre-Execution Verification Commands
Before loading the module, the operator verifies host clean state:

```bash
# 1. Verify target kernel
uname -r
# Expected: 7.2.8-9-omarchy

# 2. Verify device is unbound and override is clean
cat /sys/bus/spi/devices/spi-FTE3600:00/driver_override || true
# Expected: (null) or blank

# 3. Verify driver directory does not exist on device
test ! -d /sys/bus/spi/devices/spi-FTE3600:00/driver && echo "DEVICE UNBOUND: OK"

# 4. Verify no conflicting module is loaded
lsmod | grep -E "fte3600|spidev" || echo "MODULES ABSENT: OK"

# 5. Verify character device node does not exist
test ! -e /dev/fte3600 && echo "DEVNODE ABSENT: OK"
```

#### 3.1.4 Manual Execution Commands
```bash
# Load the unified kernel driver
sudo insmod /home/titan/fte3600-linux-driver/kernel/fte3600.ko
```

#### 3.1.5 Post-Execution Inspection Commands
```bash
# 1. Inspect kernel ring buffer
dmesg | tail -n 20
# Expected log line:
# fte3600 spi1.0: FTE3600 driver bound (passive probe, state UNINITIALIZED)

# 2. Verify SPI driver binding
readlink /sys/bus/spi/devices/spi-FTE3600:00/driver
# Expected: ../../../../bus/spi/drivers/fte3600

# 3. Verify character device node creation
ls -l /dev/fte3600
# Expected: crw------- 1 root root <major>, <minor> /dev/fte3600

# 4. Query initial driver state via GET_STATE
/home/titan/fte3600-linux-driver/bin/fte3600-tool get-state
# Expected:
#   State: 0 (UNINITIALIZED)
#   Flags: 0x00000000
#   Last Error: 0
```

#### 3.1.6 Rollback Procedure (Stage A)
If any check fails or to complete Stage A testing:

```bash
# 1. Ensure no process holds /dev/fte3600 open
lsof /dev/fte3600 2>/dev/null || true

# 2. Unload module gracefully (rmmod -f is strictly prohibited)
sudo rmmod fte3600

# 3. Verify clean release
test ! -e /dev/fte3600 && echo "DEVNODE REMOVED: OK"
test ! -d /sys/bus/spi/devices/spi-FTE3600:00/driver && echo "DEVICE UNBOUND: OK"
lsmod | grep fte3600 || echo "MODULE UNLOADED: OK"
```

---

### 3.2 Stage B — Explicit Reset IOCTL

#### 3.2.1 Objective & Scope
Invoked **only after separate user authorization**. Confirms that `FTE3600_IOC_RESET` executes `fte3600_hw_reset_pulse()`, issues the 40 ms active-low pulse sequence to the GPIO controller, and transitions driver state to `FTE3600_STATE_RESET_SEQUENCE_ISSUED` (2).

#### 3.2.2 Execution Conditions, Signal Interruption & Error Semantics
Source inspection of [`fte3600_ioctl_common()`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L436-L457) and [`fte3600_hw_reset_pulse()`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L131-L158) establishes exact execution conditions and limits:

1. **Acceptance Conditions:**
   - Accepted from **any** state (`UNINITIALIZED`, `RESET_SEQUENCE_ISSUED`, `IDENTIFIED`, `ERROR`, or `UNKNOWN`).
   - Requires `atomic_read(&data->offline) == 0`.
   - Requires `data->reset_gpiod != NULL`.
   - Requires successful acquisition of `mutex_lock_interruptible(&data->lock)`.

2. **Signal Interruption Handling (`mutex_lock_interruptible`):**
   - If a signal interrupts a thread waiting on `mutex_lock_interruptible(&data->lock)`, the driver handler immediately aborts and returns the internal kernel errno `-ERESTARTSYS`.
   - **Confirmed Driver Invariant:** An interrupted `mutex_lock_interruptible()` returns early before `data->lock` is acquired and before the reset operation (`fte3600_hw_reset_pulse()`) executes. No GPIO MMIO writes occur, and `data->state` remains **completely unchanged**.
   - **Qualified Userspace Outcome:** The exact behavior observed by userspace depends on signal configuration and kernel syscall restart handling. If the signal handler was registered with `SA_RESTART`, the kernel may automatically restart the syscall; if not, or on terminating signals, the kernel may translate `-ERESTARTSYS` to `-EINTR`, causing libc `ioctl()` to return `-1` with `errno == EINTR`. The driver guarantees only that hardware access was prevented before lock acquisition.

3. **Offline / Missing Descriptor Failure:**
   - If `data->offline` is 1 or `data->reset_gpiod` is NULL, lines 446–449 abort under lock:
     ```c
     if (atomic_read(&data->offline) || !data->reset_gpiod) {
         mutex_unlock(&data->lock);
         return -ENODEV;
     }
     ```
   - The kernel returns `-ENODEV`. The userspace C library `ioctl()` wrapper returns `-1` with `errno == ENODEV`.
   - Driver state is **completely unchanged** (it does **not** transition to `UNKNOWN`).

4. **Pulse Execution & State Transition:**
   - Once `fte3600_hw_reset_pulse()` begins execution, `gpiod_set_value_cansleep()` returns `void`; the sequence executes to completion without mid-pulse error returns.
   - On pulse execution, state transitions to `FTE3600_STATE_RESETTING` (1).
   - After the 10 ms pre-delay, 20 ms assert (physical LOW), and 10 ms post-delay (physical HIGH), state transitions to `FTE3600_STATE_RESET_SEQUENCE_ISSUED` (2).
   - The ioctl completes with return code `0` (Success).

5. **What `RESET_SEQUENCE_ISSUED` Proves:**
   - Proves **strictly** that the software driver issued the active-low timing sequence MMIO register writes to the host Intel LPSS GPIO controller.

6. **What `RESET_SEQUENCE_ISSUED` Does NOT Prove:**
   - Does **not** prove physical trace voltage on Pad 14 changed (physical voltage has never been measured with an oscilloscope).
   - Does **not** prove the sensor silicon accepted the reset pulse.
   - Does **not** prove internal sensor silicon registers or state machines were re-initialized.
   - Does **not** prove the sensor silicon is alive, powered, or responsive.

7. **Reset from `UNKNOWN` is an Attempted Recovery:**
   - When the driver is in `FTE3600_STATE_UNKNOWN`, invoking `FTE3600_IOC_RESET` transitions state to `RESETTING` and then `RESET_SEQUENCE_ISSUED`.
   - This operation is strictly an **attempted recovery operation**, **not a verified recovery**.
   - Silicon recovery and responsiveness remain completely unproven until a subsequent Stage C `FTE3600_IOC_INIT` succeeds in waking the sensor and validating Chip ID and wire CRC.
   - Stage B halts immediately after state verification without issuing any SPI transactions.

#### 3.2.3 Manual Execution Commands
```bash
# Issue reset ioctl via userspace tool
/home/titan/fte3600-linux-driver/bin/fte3600-tool reset

# Query state immediately after reset
/home/titan/fte3600-linux-driver/bin/fte3600-tool get-state
```

#### 3.2.4 Expected Output
- **Reset command return:** `0` (Success).
- **Reported Telemetry:**
  - `State: 2 (RESET_SEQUENCE_ISSUED)` (or compatibility alias `RESET_COMPLETE`).
  - `Flags: 0x00000002 (RESET_SEQUENCE_ISSUED)`.
  - `Last Error: 0`.
- **Dmesg:** Zero error messages.

#### 3.2.5 Failure Handling & Rollback (Stage B)
If the ioctl fails (returns `-1` in userspace) or reported state is not `2`:
1. Record error code from `errno` and `last_error` from telemetry.
2. Do **not** attempt `INIT` or repeated resets.
3. Execute Stage A Rollback (`sudo rmmod fte3600`).
4. Consider sensor internal state uncertain.

---

### 3.3 Stage C — Explicit Identification IOCTL

#### 3.3.1 Objective & Scope
Invoked **only after separate user authorization** following successful Stage B. Confirms that `FTE3600_IOC_INIT` executes `fte3600_hw_identify()`, communicates over SPI at 1 MHz, validates register `0xC6` readback, reads Chip ID `0x9362`, validates wire CRC `0x1C53`, and transitions driver state to `FTE3600_STATE_IDENTIFIED` (8).

#### 3.3.2 Transaction Protocol Trace
The kernel driver will execute the exact verified 6-step sequence:
1. **Step A (Wake Write):** Transmits `[0x09, 0xF6, 0xC6, 0x01]` (4 bytes).
2. **Step B (Stabilization):** `usleep_range(4000, 5000)` (4 ms delay, CS deasserted HIGH).
3. **Step C (Wake Readback):** Transmits `[0x08, 0xF7, 0xC6, 0x00, 0x00]` (5 bytes), validates `rx_buf[4] == 0x01`.
4. **Step D (Chip ID Read):** Transmits `[0x04, 0xFB, 0x9A, 0x8B, 0x00 * 6]` (10 bytes), extracts MSB Chip ID at `rx[6..7]` and wire CRC at `rx[8..9]`.
5. **Step E (CRC Verification):** Computes CRC-16 CCITT over `&rx[6..7]`, asserts match with `rx[8..9]`.
6. **Step F (Variant Gate):** Verifies Chip ID equals `0x9362`. Transitions state to `FTE3600_STATE_IDENTIFIED` (8).

#### 3.3.3 Semantic Proof Boundary: Identification vs. Capture Readiness
- A successful identification sequence proves:
  1. Sensor silicon is powered and responsive to SPI commands.
  2. The volatile SPI transceiver clock gate register `0xC6` accepts writes and reads back `0x01`.
  3. The silicon identity matches FocalTech FT9362 (`0x9362`) with valid wire CRC (`0x1C53`).
- A successful identification sequence does **NOT** prove:
  1. That the sensor is ready for fingerprint capture.
  2. Analog Front End (AFE) calibration, DAC offset tuning, and pixel gain trim have not been performed.
  3. Background pixel baseline subtraction has not been calibrated.
  4. FIFO image read sequencing and interrupt generation have not been configured.
- State transitions to `FTE3600_STATE_IDENTIFIED` (8), strictly **NOT** `FTE3600_STATE_READY` (4).

#### 3.3.4 Prohibited Operations in Stage C
- **NO AFE Calibration:** No DAC configuration, gain adjustments, or baseline calculations.
- **NO FIFO Burst Read:** `FTE3600_IOC_CAPTURE` is tested to confirm it returns failure (`-1` with `errno == EOPNOTSUPP`).
- **NO Interrupt Handling:** No IRQ line requests or interrupt service routines.
- **NO OTP/EFUSE/Flash Operations:** No access to registers `0xF1`, `0xF4`, `0x1A84`, `0x1A82`, or write to `0x9A`.

#### 3.3.5 Manual Execution Commands
```bash
# Issue initialization/identification ioctl
/home/titan/fte3600-linux-driver/bin/fte3600-tool init

# Query state immediately after identification
/home/titan/fte3600-linux-driver/bin/fte3600-tool get-state

# Verify capture ioctl continues to return -1 (errno == EOPNOTSUPP)
/home/titan/fte3600-linux-driver/bin/fte3600-tool capture || true
```

#### 3.3.6 Expected Output
- **Init ioctl return:** `0` (Success).
- **Dmesg log line:**
  `fte3600 spi1.0: FocalTech FT9362 identified (ID 0x9362, CRC 0x1c53, state IDENTIFIED)`
- **Telemetry Query:**
  - `State: 8 (IDENTIFIED)` — **NOT `READY` (4)**.
  - `Flags: 0x00000000`.
  - `Last Error: 0`.
- **Capture Test:** System call returns `-1` with `errno == EOPNOTSUPP` (`Operation not supported`).

#### 3.3.7 Post-Identification Clean Shutdown
Once verified, clean host shutdown is executed:
```bash
# 1. Close userspace tool handle
# 2. Unload module cleanly
sudo rmmod fte3600

# 3. Verify clean unbind
test ! -e /dev/fte3600 && echo "CLEANUP VERIFIED: OK"
```

---

## 4. Rollback and Recovery Analysis

### 4.1 Host-Side Teardown Protocol & Resource Lifetime
The driver enforces the Hardware-Resource Shutdown Protocol proven in Stage 2E.2.3:

1. **Distinction Between Sysfs Device Unbind and Module Unload:**
   - **Ordinary Module Unload (`rmmod fte3600`):** `.owner = THIS_MODULE` is populated on both [`fte3600_fops`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L544) and `data->cdev.owner` to enable VFS module reference tracking via `try_module_get(THIS_MODULE)` on `open()`. When references are active, ordinary `rmmod fte3600` is expected to return `-EBUSY`. However, userspace tools must not treat module reference tracking as an infallible safety net across all kernel configurations. The practical safety rule is absolute: **always close all userspace file descriptors before attempting ordinary module unload**, and verify with `lsof /dev/fte3600`. Never attempt forced module unload (`rmmod -f`).
   - **Sysfs Device Unbind (`echo ... > unbind`):** Sysfs device unbind operates on the driver-device binding model independently of the module reference counter. It immediately invokes `device_release_driver()` -> [`fte3600_remove()`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L637) even while userspace holds open file descriptors. Module unload and device unbind must never be confused.
2. **Handling Open File Descriptors During Device Removal:**
   - When device removal runs (via sysfs unbind or after all fds close during module unload), `cdev_device_del(&data->cdev, &data->dev)` removes `/dev/fte3600` from the filesystem and cdev hash table, guaranteeing that **no new opens** can succeed.
   - Any already-open file descriptor (`struct file`) remains valid in userspace until closed.
3. **Serialization of In-Flight Operations:**
   - Any active ioctl holds `data->lock`.
   - `fte3600_remove()` calls `mutex_lock(&data->lock)`, blocking until any in-flight transfer or reset operation finishes.
4. **Invalidation of Hardware Access:**
   - Under `data->lock`, `fte3600_remove()` executes:
     ```c
     atomic_set(&data->offline, 1);
     data->reset_gpiod = NULL;
     data->spi = NULL;
     ```
   - It then unlocks `data->lock` and wakes sleeping pollers via `wake_up_all(&data->event_wq)`.
5. **Rejection of Future Operations:**
   - Subsequent ioctls on existing open file descriptors hit the fast-path check:
     `if (atomic_read(&data->offline) && cmd != FTE3600_IOC_GET_STATE) return -ENODEV;`
     returning negative `-ENODEV` (userspace receives `-1` with `errno == ENODEV`).
   - Blocked threads that acquire `data->lock` observe `data->offline == 1` and null pointers, immediately releasing the mutex and returning `-ENODEV`.
6. **Devres Cleanup Timing vs. Struct Lifetime:**
   - `fte3600_remove()` drops the probe reference via `put_device(&data->dev)`.
   - When `fte3600_remove()` completes, `device_release_driver()` invokes devres cleanup, releasing `reset_gpiod` and driver ACPI GPIO mappings.
   - Because `data->reset_gpiod` and `data->spi` were set to `NULL` under mutex, no userspace operation can dereference released devres pointers.
   - Memory for `struct fte3600_data` remains allocated because the open file descriptor holds a kobject reference on `data->dev` acquired via `get_device(&data->dev)` in `fte3600_cdev_open()`.
   - When userspace finally closes the file descriptor, `fte3600_cdev_release()` calls `put_device(&data->dev)`, dropping the final reference count to 0, which invokes `fte3600_device_release()` to execute `mutex_destroy(&data->lock)` and `kfree(data)`.

### 4.2 Safe Unbind & Unload Commands
```bash
# 1. Verify no userspace process holds the device node open
lsof /dev/fte3600 2>/dev/null || true

# 2. Graceful module unload (prevented with EBUSY if fd is open)
sudo rmmod fte3600

# Alternative explicit sysfs unbind (if module is kept loaded)
echo "spi-FTE3600:00" | sudo tee /sys/bus/spi/drivers/fte3600/unbind
```

> [!CAUTION]
> **Forced Unloading Prohibited:** Never use `rmmod -f` (forced removal). Forced removal violently unlinks kernel modules while bypassing kobject reference counts and open file descriptor locks, causing immediate kernel panic or slab corruption.

### 4.3 Hardware State & Power-Cycle Limitations
1. **Host-Side vs Sensor-Internal Boundary:**
   - Unloading `fte3600.ko` releases Linux kernel memory, descriptors, and bus controllers.
   - **It does NOT reset or restore sensor internal silicon registers.**
   - Register `0xC6` remains in the state last written by the SPI bus (`0x01` if identification ran).
2. **Attempted Recovery via Reset IOCTL:**
   - If an SPI transfer fails or sensor state becomes uncertain, issuing `FTE3600_IOC_RESET` is the primary host-side recovery attempt.
   - As established in Section 3.2, this is strictly an **attempted recovery**, not a guaranteed recovery.
3. **Power Removal Behavior is Officially UNKNOWN:**
   - On the Dynabook Satellite Pro C40-J, the laptop contains an internal rechargeable battery. Disconnecting the AC adapter does **not** cut power to system rails.
   - On modern Intel Tiger Lake-LP platforms, auxiliary power rails (`VREG_FP`, `VCC_AUX`) often remain powered during standard ACPI S5 (shutdown) or warm reboot to support Modern Standby or wake-on-fingerprint.
   - There are **no schematics, electrical measurements, or verified procedures** establishing that system shutdown or AC removal cuts power to the fingerprint sensor rails.
   - Therefore, sensor power-rail decay and cold power-cycle behavior are classified as **UNKNOWN**.
   - A proven physical sensor power-cycle method has **not yet been established** on this platform.

---

## 5. Comprehensive Failure Matrix

The failure matrix classifies each condition strictly using evidence-based separations:
- **Observed Transfer Result:** The raw software / bus observation.
- **Protocol Validation Result:** The protocol check that evaluated the observation.
- **Possible Explanations:** Plausible physical or software causes.
- **Conclusions That Cannot Be Established:** Inferences that static or wire data cannot prove.

| ID | Failure Mode | Observed Software Error (Kernel / Userspace) | Protocol Validation Result | Possible Explanations | Conclusions That Cannot Be Established | Required Action |
| :---: | :--- | :--- | :--- | :--- | :--- | :--- |
| **FL-01** | **ACPI match failure** | `fte3600_probe()` returns `-ENODEV`. Module load fails (`insmod` returns error). | ACPI companion missing or HID mismatch. | DSDT table modified; wrong device binding. | Cannot determine hardware presence. | **HALT.** Inspect `dmesg`. |
| **FL-02** | **GPIO acquisition failure** | `devm_gpiod_get("reset")` returns `-EPROBE_DEFER` or error. Probe unwinds. | Descriptor lookup failed. | `pinctrl-tigerlake` not bound; resource conflict. | Cannot determine Pad 14 physical voltage. | **HALT.** Unwind probe cleanly. |
| **FL-03** | **SPI configuration failure** | `spi_setup()` returns negative error. Probe unwinds cleanly. | Host controller rejected SPI parameters. | Unsupported bus frequency or Mode 0 settings. | Cannot determine sensor bus reception. | **HALT.** Verify controller clock. |
| **FL-04** | **Character device creation failure** | `cdev_device_add()` returns error. Probe unwinds cleanly. | Device node registration failed. | Major/minor conflict or sysfs name collision. | Cannot determine hardware state. | **HALT.** Verify devt allocation. |
| **FL-05** | **Module load succeeds but probe fails** | `insmod` returns error code. `/dev/fte3600` absent. | Probe unwind returned error to module loader. | Any probe step 1–4 failure. | Sensor state untouched by driver. | **HALT.** Check `dmesg`. |
| **FL-06** | **Reset ioctl fails** | `ioctl(RESET)` returns error if interrupted, or `-1` (`errno == ENODEV`) if offline. State is **NOT** set to `UNKNOWN`; remains unchanged. | Interrupted mutex returns before acquiring lock and before reset execution; or offline descriptor check failed. | Thread interrupted by signal before lock acquisition; driver in teardown; descriptor freed. | Does **not** prove physical pulse failure; does not change driver state. | **HALT.** Do not proceed to `INIT`. Unload cleanly. |
| **FL-07** | **SPI transfer returns error (`ret < 0`)** | `spi_sync()` fails. Driver sets state `UNKNOWN`, returns `ret` (`ioctl()` returns `-1`, `errno == -ret`). | Bus transaction aborted mid-transfer. | Host FIFO overrun, bus contention, DMA timeout. | Cannot determine whether sensor processed partial bytes. | **HALT.** Next `INIT` rejected (`-EPERM`). |
| **FL-08** | **Short transfer (`actual_length < len`)** | `m.actual_length != len`. Driver sets state `UNKNOWN`, returns `-EIO` (`errno == EIO`). | Packet truncated; RX buffer discarded. | Host controller terminated transfer early. | Cannot determine sensor silicon internal state. | **HALT.** Next `INIT` rejected (`-EPERM`). |
| **FL-09** | **Wake readback not `0x01`** | Readback `rx[4] != 0x01`. Driver sets state `UNKNOWN`, returns `-EIO` (`errno == EIO`). | Register 0xC6 readback check failed. | Transceiver clock gate did not un-gate; wake delay insufficient. | Cannot prove sensor is dead or unpowered. | **HALT.** Next `INIT` rejected (`-EPERM`). |
| **FL-10a** | **Chip ID bus fault (`0x0000` / `0xFFFF`)** | Bus grounded/floating. Driver sets state `UNKNOWN`, returns `-ENODEV` (`errno == ENODEV`). | Early fault check in Step F failed. | Hypotheses: MISO trace held low/grounded (`0x0000`) or floating high (`0xFFFF`); unpowered sensor; active reset clamp; output buffer inactive. | **These values do not diagnose the underlying electrical or power condition.** Trace connectivity, supply voltage, reset state, and silicon vitality remain unproven. | **HALT.** Halt testing. Check power & traces. |
| **FL-10b** | **CRC-valid, nonmatching chip ID** | Valid non-zero ID; CRC matches. Driver sets state `UNKNOWN`, returns `-ENODEV` (`errno == ENODEV`). | **SPI & CRC checks PASSED.** Variant equality check failed. | Evidence of a protocol-level response from silicon implementing this read command and CCITT CRC, but with an unexpected chip identifier. | Whether the device is functionally compatible or supports fingerprint capture cannot be established without reference protocol docs. | **HALT.** Record detected variant ID. |
| **FL-11** | **Wire CRC mismatch** | CRC received != computed. Driver sets state `UNKNOWN`, returns `-EBADMSG` (`errno == EBADMSG`). | CRC-16 CCITT integrity check failed. | Electrical noise, high trace capacitance, clock skew, transmission distortion. | Cannot conclude sensor is inactive (data bytes WERE clocked in). | **HALT.** Halt testing. Check signal integrity. |
| **FL-12** | **Device removal during userspace access** | `fte3600_remove()` runs while fd open. Offline asserted; pointers set to NULL. | Teardown protocol executed under `data->lock`. | Sysfs unbind invoked while userspace tool open. | Sensor internal state retains last write; bus isolated. | **HALT.** Active ioctl finishes; future ioctls return `-ENODEV`. |
| **FL-13** | **Driver state is `UNKNOWN`** | State equals `7`. Any `INIT` attempt returns `-EPERM` (`errno == EPERM`). | State machine gate in `hw_identify` failed. | Any FL-07 through FL-11 error previously occurred. | Silicon state uncertain until reset and re-identification. | **HALT.** Next `INIT` blocked. Requires `RESET` recovery attempt. |

---

## 6. Standalone Userspace Verification Tool Specification

To eliminate shell-script dependencies or hidden layers during hardware testing, a minimal C utility `tools/fte3600-tool.c` is specified to issue transactional ioctls and format telemetry.

```c
/* SPDX-License-Identifier: GPL-2.0-only */
/* tools/fte3600-tool.c - Minimal Hardware Validation Utility */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../kernel/fte3600.h"

static void print_state(const struct fte3600_state_snapshot *snap)
{
	const char *state_names[] = {
		"UNINITIALIZED", "RESETTING", "RESET_SEQUENCE_ISSUED",
		"INITIALIZING", "READY", "CAPTURING", "ERROR", "UNKNOWN",
		"IDENTIFIED"
	};
	const char *name = (snap->state <= 8) ? state_names[snap->state] : "INVALID";

	printf(">> FTE3600 State Snapshot:\n");
	printf("   Version:    %u\n", snap->version);
	printf("   State:      %u (%s)\n", snap->state, name);
	printf("   Flags:      0x%08X", snap->flags);
	if (snap->flags & FTE3600_FLAG_OFFLINE) printf(" [OFFLINE]");
	if (snap->flags & FTE3600_FLAG_RESET_SEQUENCE_ISSUED) printf(" [RESET_ISSUED]");
	printf("\n");
	printf("   Last Error: %u (%s)\n", snap->last_error,
	       snap->last_error ? strerror(snap->last_error) : "None");
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <get-state|reset|init|capture>\n", argv[0]);
		return 1;
	}

	int fd = open("/dev/fte3600", O_RDWR);
	if (fd < 0) {
		perror("open(/dev/fte3600)");
		return 1;
	}

	if (strcmp(argv[1], "get-state") == 0) {
		struct fte3600_state_snapshot snap;
		if (ioctl(fd, FTE3600_IOC_GET_STATE, &snap) < 0) {
			perror("ioctl(FTE3600_IOC_GET_STATE)");
			close(fd);
			return 1;
		}
		print_state(&snap);
	} else if (strcmp(argv[1], "reset") == 0) {
		printf(">> Issuing FTE3600_IOC_RESET...\n");
		if (ioctl(fd, FTE3600_IOC_RESET) < 0) {
			/* libc ioctl() returns -1 on error and sets errno */
			perror("ioctl(FTE3600_IOC_RESET)");
			close(fd);
			return 1;
		}
		printf(">> Reset ioctl returned 0 (Success).\n");
	} else if (strcmp(argv[1], "init") == 0) {
		struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
		printf(">> Issuing FTE3600_IOC_INIT...\n");
		if (ioctl(fd, FTE3600_IOC_INIT, &params) < 0) {
			/* libc ioctl() returns -1 on error and sets errno */
			perror("ioctl(FTE3600_IOC_INIT)");
			close(fd);
			return 1;
		}
		printf(">> Init ioctl returned 0 (Identification Success).\n");
	} else if (strcmp(argv[1], "capture") == 0) {
		struct fte3600_frame_buffer fb = {
			.version = FTE3600_UAPI_VERSION,
			.length = FTE3600_IMAGE_BYTES
		};
		printf(">> Issuing FTE3600_IOC_CAPTURE (Expect failure with errno == EOPNOTSUPP)...\n");
		if (ioctl(fd, FTE3600_IOC_CAPTURE, &fb) < 0) {
			if (errno == EOPNOTSUPP) {
				printf(">> Capture rejected as expected: %s (errno=%d, EOPNOTSUPP)\n",
				       strerror(errno), errno);
			} else {
				fprintf(stderr, ">> Capture failed with unexpected errno: %s (errno=%d)\n",
				        strerror(errno), errno);
				close(fd);
				return 1;
			}
		} else {
			fprintf(stderr, "ERROR: Capture unexpectedly succeeded!\n");
			close(fd);
			return 1;
		}
	} else {
		fprintf(stderr, "Unknown command: %s\n", argv[1]);
		close(fd);
		return 1;
	}

	close(fd);
	return 0;
}
```

---

## 7. Safety Restrictions & Preconditions

### Absolute Prohibitions:
1. **No Live Execution:** No command in this plan may be executed during Stage 2E.5, 2E.5.1, 2E.5.2, or 2E.5.3.
2. **No Module Loading:** `fte3600.ko` must remain un-inserted.
3. **No Device Binding:** `spi-FTE3600:00` must remain unbound.
4. **No Gate Modification:** `bin/fw9369-hw-tool` must remain sealed (exit code 1).
5. **No Script Execution:** Do not run `scripts/stage2a-test.sh` or any hardware script.
6. **No Automatic Chaining:** Future execution of Stage A, Stage B, and Stage C must be individually reviewed and separated by manual operator confirmation.

---

## 8. Authoritative Stage Verdict

```
DOCUMENTATION FINALIZED — READY FOR USER AUTHORIZATION DECISION
```

### Next Steps:
1. User reviews this finalized plan, the Stage 2E.5.1 review, the Stage 2E.5.2 audit, and the Stage 2E.5.3 boundary review.
2. Hardware testing will proceed **only** upon explicit, written user instruction specifying which stage to authorize.
