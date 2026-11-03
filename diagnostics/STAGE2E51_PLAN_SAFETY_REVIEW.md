# Stage 2E.5.1 Hardware Test Plan Safety Review and Corrections

**Project:** FocalTech FT9362 / FW9369 Unified Linux Driver (`fte3600.ko`)  
**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Host Controller:** Intel Tiger Lake-LP PCH LPSS SPI2 Controller (`pxa2xx-spi.2`, ACPI ID `FTE3600:00` at `spi1.0`)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (SMP preempt mod_unload x86_64)  
**Safety Gate Status:** CLOSED (`bin/fw9369-hw-tool` exit code 1; zero hardware execution authorized)  
**Execution Policy:** DOCUMENTATION CORRECTION AND STATIC SOURCE AUDIT ONLY. Zero hardware operations authorized.  
**Authoritative Verdict:** `PLAN CORRECTED — READY FOR SEPARATE USER REVIEW`

---

## 1. Executive Summary & Purpose

This safety review performs a comprehensive static source inspection of `kernel/fte3600.c`, `kernel/fte3600.h`, platform ACPI descriptions, and hardware investigation reports to correct all safety oversights, unwarranted assumptions, and terminology errors present in the initial Stage 2E.5 Hardware Test Plan (`STAGE2E5_HARDWARE_TEST_PLAN.md`).

### Key Corrective Outcomes:
1. **Redefined Probe Passivity:** Demonstrated that while probe is **software-passive** (zero SPI transfers, zero reset pulses), it is **NOT electrically passive** because acquiring the reset descriptor via `devm_gpiod_get(..., GPIOD_OUT_LOW)` actively issues MMIO register writes to the Intel Tiger Lake GPIO controller (`INT34C5:00`) configuring Pad 14 as an output and asserting physical HIGH.
2. **Clarified Reset Recovery Semantics:** Proven that `RESET_SEQUENCE_ISSUED` (2) certifies only that host controller MMIO writes were dispatched; it does not measure physical pin voltage or verify sensor silicon reset. RESET from `UNKNOWN` is an **attempted recovery**, not a verified recovery.
3. **Eliminated Unsupported Power-Off Claims:** Removed the unverified 15-second figure and claims that AC disconnection or S5 shutdown removes sensor power. Classified sensor power rail behavior as officially **UNKNOWN**.
4. **Corrected Userspace IOCTL Semantics:** Differentiated internal kernel negative errnos from POSIX userspace `ioctl()` returning `-1` with `errno` populated (e.g., `errno == EOPNOTSUPP`).
5. **Audited Failure Matrix:** Corrected FL-06 (failed reset does not set `UNKNOWN`), FL-10 (unexpected chip ID does not mean sensor is unresponsive; it indicates an active, transmitting sensor of an unexpected variant), and FL-11 (CRC mismatch indicates transmission corruption, not sensor inactivity).
6. **Detailed Teardown & Device Lifetime:** Exhaustively documented open file descriptor persistence, mutex serialization during removal, devres cleanup timing, and operator post-rollback verification. Strictly prohibited forced unloading (`rmmod -f`).

---

## 2. Meaning of Passive Probe & Electrical Side Effects

### 2.1 Source Inspection of `fte3600_probe()`
Inspecting [`fte3600_probe()`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L557-L634):

```c
557: static int fte3600_probe(struct spi_device *spi)
558: {
...
601: 	/* Acquire reset GPIO descriptor with initial logical 0 (physical HIGH / inactive) */
602: 	data->reset_gpiod = devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_LOW);
603: 	if (IS_ERR(data->reset_gpiod)) {
...
610: 	/* Configure SPI bus parameters: Mode 0, 8-bit, 1 MHz */
611: 	spi->mode = SPI_MODE_0;
612: 	spi->bits_per_word = 8;
613: 	spi->max_speed_hz = 1000000;
614: 	ret = spi_setup(spi);
...
630: 	/* Initial state remains strictly UNINITIALIZED; zero physical pulses/transactions issued */
631: 	dev_info(&spi->dev, "FTE3600 driver bound (passive probe, state UNINITIALIZED)\n");
632: 	return 0;
633: }
```

### 2.2 Detailed Operation Breakdown: Software vs. Electrical Reality

| Parameter | State During Probe | Source / Subsystem Reality |
| :--- | :--- | :--- |
| **SPI Transfers** | **NONE (Zero)** | `fte3600_spi_sync_transfer()` is **never called** in probe. CS remains HIGH; SCK and MOSI are idle. |
| **Reset Timing Pulses** | **NONE (Zero)** | `fte3600_hw_reset_pulse()` is **never called** in probe. No 10ms-20ms-10ms pulse sequence is triggered. |
| **GPIO Direction Configuration** | **ACTIVE OUTPUT CONFIGURATION** | `devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_LOW)` calls Linux `gpiolib` (`gpiod_configure_flags`). `gpiolib` calls the controller driver's `direction_output` callback (`intel_gpio_direction_output` in `pinctrl-intel.c`). This executes active MMIO writes to Pad 14 `PADCFG0` on Intel Tiger Lake GPIO controller `INT34C5:00`. |
| **Initial Logical Value** | **Logical 0** | Requested flag is `GPIOD_OUT_LOW`, representing logical 0. |
| **Physical Value Implied** | **Physical HIGH** | Platform ACPI driver mapping declares `active_low = true` (`fte3600_reset_gpios = { 0, 0, true }`). Inverting logical 0 yields physical HIGH (`PADCFG0_GPIOTXSTATE = 1`), matching inactive reset voltage. |
| **Pin State Alteration** | **YES (Potential Electrical Change)** | If Pad 14 was previously configured as an input (tri-stated / high-Z) or driving physical LOW, `devm_gpiod_get(..., GPIOD_OUT_LOW)` actively configures the direction to output and forces physical HIGH. |
| **Prior State Preservation** | **NO** | Linux `gpiolib` does **not** read, cache, or preserve the existing pad configuration. It unconditionally enforces the requested output configuration. |
| **Initial Driver State** | **`UNINITIALIZED` (0)** | Set explicitly in `data->state = FTE3600_STATE_UNINITIALIZED;`. |

### 2.3 Safety Disclosures & Plan Correction
- **Old Claim:** The Stage 2E.5 plan described probe as "genuinely passive."
- **Corrected Disclosure:** Probe is **software-passive** (does not issue SPI transfers or reset timing pulses), but **NOT electrically passive**. Acquiring the descriptor actively programs the host controller via MMIO register writes and drives physical HIGH on Pad 14. This is an active electrical side effect that is now explicitly documented in Section 3.1.2 of the corrected test plan.

---

## 3. Reset Recovery Claims & State Semantics

### 3.1 Source Inspection of Reset Implementation
Inspecting [`fte3600_ioctl_common()`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L436-L457) and [`fte3600_hw_reset_pulse()`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L131-L158):

```c
131: static int fte3600_hw_reset_pulse(struct fte3600_data *data)
132: {
133: 	lockdep_assert_held(&data->lock);
134: 
135: 	if (atomic_read(&data->offline) || !data->reset_gpiod)
136: 		return -ENODEV;
137: 
138: 	data->state = FTE3600_STATE_RESETTING;
139: 
140: 	/* Pre-reset settle interval (Logical 0 = physical HIGH / inactive) */
141: 	gpiod_set_value_cansleep(data->reset_gpiod, 0);
142: 	msleep(10);
143: 
144: 	/* Assert reset: Logical 1 = physical LOW / asserted */
145: 	gpiod_set_value_cansleep(data->reset_gpiod, 1);
146: 	msleep(20); /* Reference hold duration (recovery.c:19) */
147: 
148: 	/* Deassert reset: Logical 0 = physical HIGH / inactive */
149: 	gpiod_set_value_cansleep(data->reset_gpiod, 0);
150: 	msleep(10); /* Reference settle latency (factory_startup.c:341) */
151: 
152: 	data->state = FTE3600_STATE_RESET_SEQUENCE_ISSUED;
153: 	return 0;
154: }
```

### 3.2 Analysis of Execution Conditions and State Transitions

1. **Acceptance Conditions:**
   - Accepted from **any** state (`UNINITIALIZED`, `RESET_SEQUENCE_ISSUED`, `IDENTIFIED`, `ERROR`, or `UNKNOWN`).
   - Requires `atomic_read(&data->offline) == 0`.
   - Requires `data->reset_gpiod != NULL`.
   - Requires successfully acquiring `mutex_lock_interruptible(&data->lock)`.

2. **Failure Conditions & Exact State Behavior:**
   - **Interrupted Mutex Wait:** If interrupted by a signal, `mutex_lock_interruptible` returns `-ERESTARTSYS`. Kernel returns `-EINTR` to userspace. `data->state` is **NOT modified**.
   - **Offline / Null Descriptor:** If `data->offline` is 1 or `data->reset_gpiod` is NULL, lines 446–449 abort under lock:
     ```c
     if (atomic_read(&data->offline) || !data->reset_gpiod) {
         mutex_unlock(&data->lock);
         return -ENODEV;
     }
     ```
     Kernel returns `-ENODEV` (userspace receives `-1` with `errno == ENODEV`). `data->state` is **NOT modified** (it does **not** transition to `UNKNOWN`).
   - Note that once `fte3600_hw_reset_pulse()` begins execution, `gpiod_set_value_cansleep()` returns `void`; the sequence executes to completion and returns `0`.

3. **What `RESET_SEQUENCE_ISSUED` Proves:**
   - It proves **strictly** that the software driver issued the active-low timing sequence MMIO register writes to the host Intel LPSS GPIO controller.

4. **What `RESET_SEQUENCE_ISSUED` Does NOT Prove:**
   - Does **not** prove physical trace voltage on Pad 14 changed (voltage is unmeasured; motherboard trace integrity unverified).
   - Does **not** prove the sensor silicon accepted the reset.
   - Does **not** prove internal digital registers or state machines were re-initialized.
   - Does **not** prove the sensor silicon is alive, powered, or responsive.

5. **RESET from `UNKNOWN` is Strictly an Attempted Recovery:**
   - When the driver is in `FTE3600_STATE_UNKNOWN`, calling `FTE3600_IOC_RESET` transitions state from `UNKNOWN` to `RESETTING` and then `RESET_SEQUENCE_ISSUED`.
   - This operation is strictly an **attempted recovery**, **not a verified recovery**.
   - Silicon recovery and responsiveness remain completely unproven until a subsequent Stage C `FTE3600_IOC_INIT` succeeds in waking the sensor and validating Chip ID and wire CRC.
   - Stage B halts immediately after state verification without issuing any SPI transactions.

6. **Identification Proof Boundary:**
   - A subsequent successful identification (`FTE3600_IOC_INIT`) proves sensor responsiveness, clock gating via `0xC6`, and silicon identity (`0x9362`, CRC `0x1C53`).
   - It does **NOT** prove operational capture readiness (AFE calibration, DAC offset tuning, gain trim, baseline subtraction, FIFO arming remain unperformed).

---

## 4. Elimination of Unsupported Power-Off Claims

### 4.1 Critique of Initial Test Plan Claims
The initial Stage 2E.5 plan stated:
> "To ensure a true silicon power-on reset (POR), the system must have AC disconnected and power held off for at least 15 seconds."

### 4.2 Technical Reality on the Target Host
1. **Internal Laptop Battery:** The Dynabook Satellite Pro C40-J is a portable laptop with an integrated internal rechargeable lithium battery. Disconnecting the AC adapter simply switches system power to the internal battery; it does not remove power from motherboard rails.
2. **Auxiliary Rails in S5 / Modern Standby:** On modern Intel Tiger Lake-LP SoCs, auxiliary power rails (`VREG_FP`, `VCC_AUX`, `+3VALW`) are frequently maintained during ACPI S5 (shutdown) or warm reboot to support Modern Standby or wake-on-fingerprint.
3. **No Empirical Rail Discharge Data:** There are no hardware schematics, oscilloscope traces, or rail measurement logs documenting the capacitor bleed-down rate for the fingerprint sensor power rail on this motherboard. The 15-second figure was an arbitrary guess with zero empirical support.

### 4.3 Corrective Action Taken
- The 15-second figure has been **completely removed**.
- The plan explicitly refrains from claiming that AC disconnection removes sensor power.
- The plan explicitly refrains from claiming that S5 shutdown or warm reboot guarantees sensor power removal.
- The sensor power-removal behavior is officially classified as **UNKNOWN**.
- The plan discloses that a proven, verified sensor power-cycle method has **not yet been established** on this host platform.

---

## 5. Correction of IOCTL Error Descriptions

### 5.1 Kernel vs. Userspace Error Mechanics
In Linux driver development and POSIX programming:
- **Kernel Space:** Driver handlers (`fte3600_ioctl_common()`) return a negative errno value (e.g., `-EOPNOTSUPP`, `-ENODEV`, `-EINVAL`, `-EBADMSG`, `-EIO`, `-ERESTARTSYS`).
- **Userspace (libc):** The C library `ioctl(int fd, unsigned long request, ...)` system call wrapper inspects the return value. On negative returns, libc sets `errno = -ret` and returns `-1`.

### 5.2 Specific Corrections in Plan & Tool Specification
- `FTE3600_IOC_CAPTURE`:
  - Kernel handler returns `-EOPNOTSUPP`.
  - Userspace `ioctl()` returns `-1` with `errno == EOPNOTSUPP` (`Operation not supported`).
- `tools/fte3600-tool.c` and Section 3.3.6 have been updated to test:
  ```c
  if (ioctl(fd, FTE3600_IOC_CAPTURE, &fb) < 0) {
      if (errno == EOPNOTSUPP) {
          printf(">> Capture rejected as expected: %s (errno=%d, EOPNOTSUPP)\n",
                 strerror(errno), errno);
      }
  }
  ```

---

## 6. Comprehensive Failure Matrix Audit & Corrections

Every row of the failure matrix was re-audited against the exact source implementation in `kernel/fte3600.c`:

| ID | Failure Mode | Source Verification & Invariant | Plan Correction Applied |
| :---: | :--- | :--- | :--- |
| **FL-01** | ACPI match failure | `fte3600_probe()` lines 562–563: `if (!ACPI_COMPANION(&spi->dev)) return -ENODEV;` | Prohibits binding. insmod returns error. |
| **FL-02** | GPIO acquisition failure | `fte3600_probe()` lines 602–608: `IS_ERR(data->reset_gpiod)` returns PTR_ERR. | Unwinds cleanly via `put_device(&data->dev)`. |
| **FL-03** | SPI configuration failure | `fte3600_probe()` lines 614–619: `spi_setup()` failure unwinds probe. | Unwinds cleanly via `put_device(&data->dev)`. |
| **FL-04** | cdev creation failure | `fte3600_probe()` lines 624–629: `cdev_device_add()` failure unwinds probe. | Unwinds cleanly via `put_device(&data->dev)`. |
| **FL-05** | Probe failure overall | Kernel core handles probe failure; `/dev/fte3600` is never created. | Clean halt; no cleanup required. |
| **FL-06** | Reset ioctl fails | Lines 446–449: `if (atomic_read(&data->offline) \|\| !data->reset_gpiod) { mutex_unlock(&data->lock); return -ENODEV; }` | **CORRECTED:** State does **NOT** transition to `UNKNOWN`; state remains unchanged. Returns `-ENODEV` (or `-ERESTARTSYS` if interrupted). |
| **FL-07** | SPI sync error | Lines 204–208: `if (ret) { data->state = UNKNOWN; data->last_error = -ret; return ret; }` | Returns negative error. State becomes `UNKNOWN`. Next `INIT` rejected with `-EPERM`. |
| **FL-08** | Short transfer | Lines 217–221: `if (m.actual_length != len) { data->state = UNKNOWN; data->last_error = EIO; return -EIO; }` | Returns `-EIO`. State becomes `UNKNOWN`. Truncated RX data is discarded. |
| **FL-09** | Wake readback mismatch | Lines 302–309: `if (data->rx_buf[4] != 0x01) { data->state = UNKNOWN; data->last_error = EIO; return -EIO; }` | Observed error: readback != 0x01. Inferred condition: Transceiver clock gate inactive or unverified. |
| **FL-10a** | Chip ID bus fault | Lines 331–338: `if (chip_id == 0x0000 \|\| chip_id == 0xFFFF)` | Observed error: `0x0000` or `0xFFFF`. Inferred condition: Bus grounded or floating; sensor unpowered or trace fault. |
| **FL-10b** | Chip ID variant mismatch | Lines 349–356: `if (chip_id != FTE3600_EXPECTED_CHIP_ID)` | **CORRECTED:** If ID is non-zero (e.g. `0x9368`), the sensor **IS RESPONSIVE and actively transmitting** over SPI! It does NOT prove sensor is unresponsive; it detects an unsupported variant. |
| **FL-11** | Wire CRC mismatch | Lines 340–347: `if (crc_rx != crc_calc)` | **CORRECTED:** CRC mismatch does NOT prove sensor is inactive. Bytes WERE received from silicon; mismatch proves wire corruption, noise, or timing skew. |
| **FL-12** | Removal during access | Lines 637–687: `fte3600_remove()` locks `data->lock`, sets `offline = 1`, nullifies pointers. | **CORRECTED:** Detailed locking and devres lifetime documented. Active ioctl finishes safely; subsequent ioctls return `-ENODEV`. |
| **FL-13** | State is `UNKNOWN` | Driver state equals `7`. Lines 270–271: `if (data->state != RESET_SEQUENCE_ISSUED) return -EPERM;` | Subsequent `INIT` blocked with `-EPERM`. Requires `RESET` as an attempted recovery operation. |

---

## 7. Stage A Rollback, Teardown & Device Lifetime Audit

### 7.1 Source Verification of Teardown Sequence
Inspecting [`fte3600_remove()`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L637-L687) and [`fte3600_device_release()`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L104-L110):

```c
637: static void fte3600_remove(struct spi_device *spi)
638: {
...
649: 	cdev_device_del(&data->cdev, &data->dev);
...
655: 	mutex_lock(&data->lock);
661: 	atomic_set(&data->offline, 1);
662: 	data->reset_gpiod = NULL;
663: 	data->spi = NULL;
666: 	mutex_unlock(&data->lock);
...
676: 	wake_up_all(&data->event_wq);
678: 	spi_set_drvdata(spi, NULL);
685: 	put_device(&data->dev);
...
687: }
```

### 7.2 Detailed Lifecycle and Lifetime Protections

1. **Persistence of Open Descriptors:**
   - If a userspace process holds `/dev/fte3600` open when module unload or driver unbind occurs, the file descriptor remains open and valid in userspace.
   - `cdev_device_del()` deletes the cdev from character major/minor lookup and unlinks `/dev/fte3600` from sysfs, ensuring that **no new opens** can occur.
2. **In-Flight IOCTL Serialization:**
   - Any in-flight ioctl holds `data->lock`.
   - `fte3600_remove()` calls `mutex_lock(&data->lock)`, blocking until the in-flight ioctl releases the mutex.
3. **Rejection of Future Operations:**
   - Under `data->lock`, `fte3600_remove()` asserts `offline = 1` and clears `data->reset_gpiod = NULL` and `data->spi = NULL`.
   - Any subsequent ioctl on an open file descriptor hits the fast-path check:
     `if (atomic_read(&data->offline) && cmd != FTE3600_IOC_GET_STATE) return -ENODEV;`
     or acquires the mutex, observes `offline == 1`, and returns `-ENODEV`.
4. **Devres Cleanup Timing vs. Struct Lifetime:**
   - When `fte3600_remove()` completes, `device_release_driver()` invokes devres cleanup, freeing the reset GPIO descriptor and driver ACPI GPIO mappings.
   - Because `data->reset_gpiod` was set to `NULL` under mutex, no future ioctl can dereference the freed descriptor.
   - `struct fte3600_data` memory is protected by the embedded `struct device dev` kobject refcount.
   - In `fte3600_remove()`, `put_device(&data->dev)` drops the probe reference.
   - When userspace finally closes the open file descriptor, `fte3600_cdev_release()` calls `put_device(&data->dev)`, dropping the final reference and triggering `fte3600_device_release()` to execute `kfree(data)`.
5. **Operator Verification Checklist:**
   - Check open descriptors: `lsof /dev/fte3600 2>/dev/null || true`.
   - Graceful unload: `sudo rmmod fte3600`.
   - Verify devnode removal: `test ! -e /dev/fte3600 && echo "DEVNODE REMOVED: OK"`.
   - Verify sysfs unbind: `test ! -d /sys/bus/spi/devices/spi-FTE3600:00/driver && echo "DEVICE UNBOUND: OK"`.
   - Verify module unloaded: `lsmod | grep fte3600 || echo "MODULE UNLOADED: OK"`.
6. **Strict Prohibition of Forced Unload:**
   - Never run `rmmod -f`. Forced unloading forcibly severs kernel module code while kobjects and open file references are still active, resulting in kernel page faults or slab memory corruption.

---

## 8. Hardware Execution Gate & Safety Compliance

- The hardware execution gate remains strictly **CLOSED**:
  ```bash
  /home/titan/fte3600-linux-driver/bin/fw9369-hw-tool
  # Exit code: 1 (Hardware execution prohibited)
  ```
- **Zero hardware operations were authorized or performed.**
- **Zero module loads or unloads were executed.**
- **Zero SPI transactions or GPIO toggles were issued.**
- **Zero driver source code modifications were made.**
- **Zero git commits were created.**

---

## 9. Authoritative Stage Verdict

```
PLAN CORRECTED — READY FOR SEPARATE USER REVIEW
```

### Next Steps:
1. The user reviews the corrected `STAGE2E5_HARDWARE_TEST_PLAN.md` alongside this review report.
2. The user independently evaluates whether the 3-stage validation sequence, failure matrix, and rollback protocols are acceptable.
3. Hardware execution will **only** occur if the user provides explicit, separate authorization.
