# Stage 2E.5.3 — Final Evidence-Boundary Review

**Project:** FocalTech FT9362 / FW9369 Unified Linux Driver (`fte3600.ko`)  
**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Host Controller:** Intel Tiger Lake-LP PCH LPSS SPI2 Controller (`pxa2xx-spi.2`, ACPI ID `FTE3600:00` at `spi1.0`)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (SMP preempt mod_unload x86_64)  
**Safety Gate Status:** CLOSED (`bin/fw9369-hw-tool` exit code 1; zero hardware execution authorized)  
**Execution Policy:** STRICTLY READ-ONLY DOCUMENTATION AND EVIDENCE-BOUNDARY CORRECTION. Zero hardware execution authorized.  
**Authoritative Verdict:** `DOCUMENTATION FINALIZED — READY FOR USER AUTHORIZATION DECISION`

---

## 1. Executive Summary & Authoritative Stage Verdict

This narrowly scoped review establishes strict evidence boundaries across the Stage 2E.5 Hardware Test Plan (`STAGE2E5_HARDWARE_TEST_PLAN.md`). It ensures that every technical description strictly differentiates verified code behavior and platform evidence from hypotheses, unverified hardware claims, and unmeasured electrical states.

### Authoritative Verdict:
```
DOCUMENTATION FINALIZED — READY FOR USER AUTHORIZATION DECISION
```

> [!IMPORTANT]
> **Safety Notice:** A finalized documentation verdict does **NOT** authorize hardware execution. The hardware safety gate remains sealed (`bin/fw9369-hw-tool` exit code 1). No kernel module has been loaded, no device bound, no GPIO toggled, and no SPI transaction executed. Hardware testing remains strictly blocked pending separate, explicit user authorization.

---

## 2. Inspected Source Locations

The review inspected the following source files and exact line ranges:

| Component / Subsystem | Source File | Exact Line Range | Inspected Content |
| :--- | :--- | :--- | :--- |
| **ACPI GPIO Mapping** | [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c) | Lines 60–68 | `fte3600_acpi_gpios`, `active_low = true` |
| **Reset Sequence Helper** | [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c) | Lines 131–158 | `fte3600_hw_reset_pulse()`, mutex lock requirement |
| **SPI Transport Engine** | [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c) | Lines 173–224 | `fte3600_spi_sync_transfer()`, length and status checks |
| **Identification Sequence** | [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c) | Lines 260–365 | `fte3600_hw_identify()`, Step F CRC and ID gates |
| **File Operations & Open** | [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c) | Lines 367–399, 544–554 | `fte3600_fops`, `cdev_open/release`, `.owner = THIS_MODULE` |
| **IOCTL Dispatcher** | [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c) | Lines 402–513 | `fte3600_ioctl_common()`, `mutex_lock_interruptible` |
| **Driver Probe & Remove** | [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c) | Lines 557–687 | `fte3600_probe()`, `cdev_device_add/del`, `fte3600_remove()` |
| **UAPI Header** | [`kernel/fte3600.h`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.h) | Lines 14–98 | ABI version, state machine, IOCTL declarations |
| **Hardware Test Plan** | [`STAGE2E5_HARDWARE_TEST_PLAN.md`](file:///home/titan/fte3600-linux-driver/STAGE2E5_HARDWARE_TEST_PLAN.md) | Lines 1–552 | Decoupled test stages, rollback, and failure matrix |

---

## 3. Required Corrections & Evidence Boundaries

### 3.1 Correction 1 — Qualified `ERESTARTSYS` Signal Interruption
- **Problem in Earlier Drafts:** Unconditional claims regarding userspace receiving `errno == EINTR` or experiencing guaranteed `SA_RESTART` system call restarts.
- **Source Grounding (`kernel/fte3600.c:437–439`):**
  ```c
  case FTE3600_IOC_RESET: {
      if (mutex_lock_interruptible(&data->lock))
          return -ERESTARTSYS;
  ```
- **Verified Code Invariant:** If a signal arrives while a thread waits on `mutex_lock_interruptible(&data->lock)`, the handler aborts immediately and returns `-ERESTARTSYS`. The return occurs **before `data->lock` is acquired** and **before `fte3600_hw_reset_pulse()` executes**. No GPIO MMIO writes occur, and `data->state` remains **completely unchanged**.
- **Qualified Syscall Boundary Explanation:** The exact outcome in userspace cannot be unconditionally promised because it depends on external process state:
  - If the calling process configured its signal handler with `SA_RESTART`, the kernel may automatically rewind and restart the `ioctl()` syscall transparently.
  - If `SA_RESTART` is not configured, or if the signal is unhandled/fatal, the kernel translates `-ERESTARTSYS` to `-EINTR`, causing libc `ioctl()` to return `-1` with `errno == EINTR`.
- **Plan Update:** Section 3.2.2 and Section 5 (FL-06) have been updated to document only the confirmed kernel-level invariant without promising a particular userspace outcome.

### 3.2 Correction 2 — CRC-Valid Nonmatching Chip IDs
- **Problem in Earlier Drafts:** Speculative attribution of non-`0x9362` chip IDs to specific unverified variant model numbers (e.g. "FT9368", "FT9361").
- **Source Grounding (`kernel/fte3600.c:340–356`):**
  ```c
  if (crc_rx != crc_calc) {
      ...
      return -EBADMSG;
  }
  if (chip_id != FTE3600_EXPECTED_CHIP_ID) {
      ...
      return -ENODEV;
  }
  ```
- **Verified Code Invariant:** In `fte3600_hw_identify()`, wire CRC validation (`crc_rx != crc_calc`) is evaluated **strictly before** the Chip ID equality check (`chip_id != FTE3600_EXPECTED_CHIP_ID`).
- **Evidence-Based Description:** Reaching `chip_id != FTE3600_EXPECTED_CHIP_ID` guarantees that the 2-byte wire CRC received from the sensor **matched** the CCITT polynomial over the received Chip ID bytes.
- **Strict Evidence Boundary:**
  - This condition is described strictly as a **CRC-valid, nonmatching chip ID**.
  - It constitutes evidence of a **protocol-level response** from silicon implementing this read command sequence and CCITT CRC algorithm.
  - References to specific part numbers (e.g. FT9368, FT9361) have been **completely removed**, as no official protocol specification or datasheet in this repository establishes their register IDs.
  - Whether such a device supports fingerprint image capture cannot be established without reference protocol documentation.

### 3.3 Correction 3 — Hypotheses for `0x0000` and `0xFFFF`
- **Problem in Earlier Drafts:** Conflating observed bus readings with definitive diagnoses of sensor power rail status or physical line disconnection.
- **Source Grounding (`kernel/fte3600.c:331–338`):**
  ```c
  if (chip_id == 0x0000 || chip_id == 0xFFFF) {
      ...
      return -ENODEV;
  }
  ```
- **Strict Evidence Boundary:**
  - Plausible causes are retained **strictly as hypotheses**:
    - *Hypothesis A:* Physical MISO line held low / shorted to ground (`0x0000`) or pulled high / floating (`0xFFFF`).
    - *Hypothesis B:* Sensor power rail unpowered or decayed.
    - *Hypothesis C:* Active hardware reset clamp on the sensor.
    - *Hypothesis D:* Sensor SPI transceiver output driver in high-impedance state.
  - **Explicit Invariant:** **These values do not diagnose the underlying electrical or power condition.** Static audit and bus reads cannot distinguish between unpowered silicon, a broken trace, an asserted reset line, or internal chip fault.

### 3.4 Correction 4 — Module Unload vs. Device Unbind & Reference Tracking
- **Problem in Earlier Drafts:** Overstating module reference counting as an absolute runtime guarantee while obscuring the critical distinction between module unloading and device unbinding.
- **Source Grounding (`kernel/fte3600.c:545, 623, 637`):**
  - `fte3600_fops.owner = THIS_MODULE` and `cdev.owner = THIS_MODULE`.
  - Open file descriptors cause VFS `chrdev_open()` to call `try_module_get(THIS_MODULE)`.
- **Qualified Explanation:**
  - While `.owner = THIS_MODULE` enables VFS reference tracking so ordinary `rmmod fte3600` returns `-EBUSY` when descriptors are open, reference tracking across varying container namespaces and kernel environments should never be relied upon as an infallible substitute for disciplined operation.
- **Preserved Practical Safety Rules:**
  1. **Always close all userspace file descriptors** before attempting ordinary module unload (`rmmod fte3600`), and independently verify with `lsof /dev/fte3600`.
  2. **Never attempt forced module unload (`rmmod -f`)**, which bypasses refcounts and causes kernel panics.
  3. **Do not confuse device unbind with module unload:** Sysfs device unbind (`echo ... > unbind`) unbinds the driver from the SPI device immediately regardless of open file descriptors, invoking `fte3600_remove()`, whereas module unload (`rmmod`) operates at the module binary level.

### 3.5 Correction 5 — GPIO Voltage & Physical Pad 14 Boundaries
- **Problem in Earlier Drafts:** Blurring the line between software requests, expected active-low polarities, and actual physical pin voltages.
- **Source Grounding (`kernel/fte3600.c:602`):**
  ```c
  data->reset_gpiod = devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_LOW);
  ```
- **Strict Evidence Boundary:**
  - *Requested Logical Value:* Logical 0 (inactive reset in driver logic).
  - *Expected Physical Polarity:* Physical HIGH (due to `active_low = true` in ACPI mapping).
  - *Independently Measured Voltage:* **NEVER MEASURED**.
  - **Explicit Invariant:** **No static audit may claim that the physical pin or sensor reset trace actually reached HIGH or any particular voltage level.** The physical motherboard trace has never been measured with an oscilloscope or digital multimeter.

---

## 4. Synthesis: Verified Facts vs. Hypotheses vs. Unknowns

| Category | Item | Classification & Evidence Boundary |
| :--- | :--- | :--- |
| **Verified Fact** | In-Kernel Synchronous SPI Engine | Enforces `m.actual_length == len`, checks status, returns negative errno, fails closed to `UNKNOWN`. |
| **Verified Fact** | Reset Pulse Dispatch | `fte3600_hw_reset_pulse()` issues 10ms-20ms-10ms active-low GPIO MMIO writes. |
| **Verified Fact** | `RESET_SEQUENCE_ISSUED` (2) | Certifies strictly host controller MMIO register dispatch; does not verify physical voltage. |
| **Verified Fact** | Identification Protocol | Evaluates CRC before Chip ID; transitions to `IDENTIFIED` (8), never `READY` (4). |
| **Verified Fact** | Capture IOCTL | Unconditionally returns `-EOPNOTSUPP` in kernel space; userspace receives `-1` (`errno == EOPNOTSUPP`). |
| **Verified Fact** | Interrupted Mutex Lock | Returns `-ERESTARTSYS` before acquiring lock and before reset execution; state unchanged. |
| **Verified Fact** | Teardown Serialization | `fte3600_remove()` acquires `data->lock`, sets `offline = 1`, and clears hardware pointers. |
| **Verified Fact** | Lifespan Protection | Embedded `struct device dev` kobject refcounts preserve struct memory until final userspace `close()`. |
| **Hypothesis** | Bus Fault `0x0000` / `0xFFFF` | Hypothesized as trace ground/float, unpowered sensor, active clamp, or tri-state. Unproven. |
| **Hypothesis** | CRC-Valid Nonmatching ID | Hypothesized as an active protocol-level response from an unverified silicon variant. Unproven. |
| **Unknown** | Pad 14 Physical Voltage | Actual pin voltage on the motherboard trace has **never been measured**. |
| **Unknown** | Sensor Silicon Reset State | Whether sensor silicon accepted reset or initialized internal registers is **completely unknown**. |
| **Unknown** | Sensor Power-Cycle Behavior | Whether AC disconnect or S5 shutdown removes power to sensor rails is officially **UNKNOWN**. |

---

## 5. Hardware Safety Gate & Compliance Verification

- The hardware execution gate remains strictly **CLOSED**:
  ```bash
  /home/titan/fte3600-linux-driver/bin/fw9369-hw-tool
  # Exit code: 1 (Physical device access is blocked by policy)
  ```
- **Zero hardware operations were authorized or performed.**
- **Zero kernel modules were loaded or unloaded.**
- **Zero devices were bound or unbound.**
- **Zero SPI transactions or GPIO toggles were issued.**
- **Zero driver source code files (`kernel/fte3600.c` or `kernel/fte3600.h`) were modified.**
- **Zero git commits were created.**

---

## 6. Deliverables Summary

| Deliverable Path | Description | Status |
| :--- | :--- | :--- |
| [`STAGE2E5_HARDWARE_TEST_PLAN.md`](file:///home/titan/fte3600-linux-driver/STAGE2E5_HARDWARE_TEST_PLAN.md) | Finalized Multi-Stage Hardware Test Plan | **Reconciled & Finalized** |
| [`diagnostics/STAGE2E5_HARDWARE_TEST_PLAN.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2E5_HARDWARE_TEST_PLAN.md) | Synchronized diagnostics copy | **Reconciled & Finalized** |
| [`STAGE2E53_FINAL_EVIDENCE_BOUNDARY_REVIEW.md`](file:///home/titan/fte3600-linux-driver/STAGE2E53_FINAL_EVIDENCE_BOUNDARY_REVIEW.md) | Comprehensive Evidence-Boundary Review | **Authored & Persisted** |
| [`diagnostics/STAGE2E53_FINAL_EVIDENCE_BOUNDARY_REVIEW.md`](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2E53_FINAL_EVIDENCE_BOUNDARY_REVIEW.md) | Synchronized diagnostics copy | **Authored & Persisted** |
| [`PROGRESS.md`](file:///home/titan/fte3600-linux-driver/PROGRESS.md#L2265-L2310) | Master project milestone log | **Updated** |

---

## 7. Authoritative Conclusion

The Stage 2E.5 Hardware Test Plan and all supporting documentation have been thoroughly audited, verified against kernel source, and stripped of unverified hypotheses and speculative claims.

The documentation is completely reconciled and finalized:

```
DOCUMENTATION FINALIZED — READY FOR USER AUTHORIZATION DECISION
```
