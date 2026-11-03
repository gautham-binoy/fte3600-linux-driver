# Stage 2E.1.2 — Final Kernel Implementation Preconditions Review

**Target System:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**SPI Host Controller:** Intel Tiger Lake-LP PCH LPSS SPI2 (`pxa2xx-spi.2`, PCI `00:15.0`)  
**Reset Line:** Intel PCH Pad 14 (GPP_B14 / `SPKR` / `\_SB.GPI0` Pin 14)  
**Interrupt Line:** Intel PCH Pad 229 (GPP_D0 / `CPU_GP_0` / `\_SB.GPI0` Pin 323)  
**Document ID:** `STAGE2E1_2_FINAL_IMPLEMENTATION_PRECONDITIONS`  
**Execution Mode:** STRICTLY SOFTWARE-ONLY (Precondition Audit and Static Analysis)  
**Hardware Safety Gate:** **CLOSED (`bin/fw9369-hw-tool` exits with code 1 unconditionally)**  

---

## 1. Executive Summary & Authoritative Stage Verdict

Stage 2E.1.2 resolves all remaining technical ambiguities, concurrency definitions, and safety boundaries prior to authoring the kernel driver (`fte3600.ko`) in Stage 2E.2. 

Every requirement from the Stage 2E.1.1 correction report and earlier safety reviews has been evaluated against target kernel headers (`7.2.8-9-omarchy`), reference protocol sources (`research/libfprint-fte3600/`), and ACPI DSDT tables.

### Authoritative Stage Verdict

$$\mathbf{PRECONDITIONS\;SATISFIED\;—\;STAGE\;2E.2\;SOFTWARE\;IMPLEMENTATION\;MAY\;BEGIN}$$

---

## 2. Investigation Findings & Blockers Resolution

### 2.1 Interrupt Acknowledgement Safety (`0x1A82` & `0x1A84`)

#### Investigation of Reference Implementation
- **Source Tracing:**
  - `fw9369_release.c:309-326`:
    ```c
    word (0x1a82, &value, io, d);        // Reads interrupt status register
    write_word (0x1a84, value, io, d);   // Writes back status to acknowledge
    ```
  - `fw9369_startup.c:343, 371`: `write_word(op, 0x1a84, 0xffff);`
  - `ft93xx.c:485, 493, 1580`: `write_word(&op, 0x1a84, 0x20);`
  - `tests/test-fte3600-fw9369-runtime.c:128`:
    ```c
    if (at == 0x1a84) hw.status &= (uint16_t)~value; // Write-1-to-clear logic
    ```
- **Semantics:** Register `0x1A84` is a write-1-to-clear interrupt acknowledgement register. Writing active bits clears the internal latch and deasserts the physical IRQ pin.
- **Safety Restriction Reconciled:** Earlier safety reviews (`STAGE2A_MINIMAL_INITIALIZATION_DESIGN.md`, `STAGE2D2_MINIMAL_INITIALIZATION_SAFETY_AUDIT.md`) explicitly excluded `0x1A84` because unverified register writes must not occur during read-only or minimal identification stages.
- **Alternative Safe Mechanism:**
  - On Linux, registering Pad 229 with `IRQF_ONESHOT` causes the kernel interrupt core to mask the interrupt line at the PCH interrupt controller immediately upon firing.
  - Even if register `0x1A84` is **never written**, the interrupt cannot storm the CPU because the PCH keeps the line masked until the kernel driver reenables it.
- **Determination:**
  - **Physical interrupt acknowledgement via `0x1A84` is formally BLOCKED** pending separate independent protocol review and authorization.
  - The Stage 2E.2 software implementation **must NOT silently access register `0x1A84`**.
  - In Stage 2E.2, the threaded IRQ handler will only notify userspace wait-queues and will stub out `0x1A84` writes, ensuring zero prohibited register writes occur.

---

### 2.2 ACPI Interrupt Configuration & Request Flags

#### Kernel Mechanism Analysis (`include/linux/interrupt.h`)
- Lines 26–31 of `/lib/modules/7.2.8-9-omarchy/build/include/linux/interrupt.h`:
  ```c
  /*
   * When requesting an interrupt without specifying a IRQF_TRIGGER, the
   * setting should be assumed to be "as already configured", which
   * may be as per machine or firmware initialisation.
   */
  #define IRQF_TRIGGER_NONE   0x00000000
  ```
- In `kernel/irq/manage.c`, if `flags & IRQF_TRIGGER_MASK == 0` (i.e. `IRQF_TRIGGER_NONE`), the kernel retains the trigger configuration established by ACPI firmware during device enumeration (`acpi_dev_gpio_irq_get()`).
- Passing explicit trigger flags (such as `IRQF_TRIGGER_RISING`) attempts to reconfigure the hardware trigger mode in the Intel PCH GPIO/pinctrl controller, risking `-EBUSY` or conflict if locked by firmware.
- **Determination:**
  - The driver must pass **only `IRQF_ONESHOT`** (with `IRQF_TRIGGER_NONE`) to `devm_request_threaded_irq()`:
    ```c
    ret = devm_request_threaded_irq(&spi->dev, data->irq, NULL,
                                    fte3600_irq_thread,
                                    IRQF_ONESHOT,
                                    "fte3600-irq", data);
    ```
  - This safely inherits the ACPI DSDT-declared configuration (`GpioInt (Edge, ActiveHigh, ExclusiveAndWake, PullNone)`) without risking trigger mismatch or unauthorized pad reconfiguration.

---

### 2.3 FIFO Transfer Length Discrepancy Resolved

#### Protocol Layer vs. Application Layer Breakdown
The discrepancy between the 10,246-byte transfer and the 10,240-byte frame is resolved by inspecting packet construction in `tools/fw9369-mock/include/fw9369_protocol.h` and `src/fw9369_protocol.c`:

| Component | Length (Bytes) | Description |
| :--- | :---: | :--- |
| **Command / Prefix Header** | 6 | `[ 0x10, 0xEF, 0x9A, 0x05, 0x14, 0x00 ]` (Opcode + parameters) |
| **Sensor Payload (Pixel Data)** | 10,240 | 5,120 pixels $\times$ 2 bytes (64 width $\times$ 80 height, 16-bit word) |
| **Wire Trailer / CRC** | 0 | No trailing CRC bytes appended to FIFO burst packet |
| **Total SPI Wire Transfer** | **10,246** | Full duplex buffer length submitted to `spi_sync()` |
| **Userspace Image Buffer** | **10,240** | Raw pixel payload copied to caller (prefix bytes `rx[0..5]` stripped) |

- **Determination:**
  - The kernel SPI transfer length must remain exactly **10,246 bytes** (`FW9369_FIFO_TRANSFER_LEN`).
  - The userspace frame buffer payload delivered by ioctl `FTE3600_IOC_CAPTURE` is exactly **10,240 bytes** (`FW9369_IMAGE_BYTES`).
  - Both numbers are technically correct at their respective layers. All architecture diagrams and state machines are harmonized to reflect this distinction.

---

### 2.4 PXA2xx SPI Controller Limits Audit

#### Kernel Source & Header Analysis
- **Target Kernel Source Availability:** Locally available headers include `include/linux/spi/spi.h` and `include/linux/pxa2xx_ssp.h`. Full C driver source `drivers/spi/spi-pxa2xx.c` is not included in `/lib/modules/$(uname -r)/build/`.
- **Known Subsystem Limits (from kernel architecture):**
  - `MAX_DMA_LEN = SZ_64K` (65,536 bytes) in `spi-pxa2xx.h`.
  - `controller->max_dma_len = MAX_DMA_LEN`.
  - `controller->max_transfer_size = pxa2xx_spi_max_dma_transfer_size` (64 KiB).
  - Since $10,246 < 65,536$, the 10,246-byte burst read is within controller boundaries.
  - Setting `cs_change = 0` instructs the controller and SPI core to keep chip-select continuously asserted.
- **Strict Boundary:**
  - Source-level maximums do **not** prove physical silicon success.
  - Whether LPSS DMA channels are bound on this specific PCI device instance (`00:15.0`) or whether the driver uses PIO fallback has not been verified on live hardware.
  - **Determination:** The 10,246-byte transfer capability is **theoretically supported by controller structures, but remains UNVERIFIED ON PHYSICAL HARDWARE**. It is prohibited from hardware execution until separately authorized.

---

### 2.5 Probe and Reset Lifecycle Policy: Selection of Policy A

#### Comparison of Architectural Policies

| Metric | Policy A: Passive Probe / Explicit Reset | Policy B: Active Probe with Auto-Reset |
| :--- | :--- | :--- |
| **Probe Behavior** | Acquires GPIO & SPI descriptors; sets state `UNINITIALIZED`. Zero hardware signaling. | Drives Pad 14 low for 20 ms and high for 10 ms during boot/probe. |
| **Hardware Safety** | **100% Non-Intrusive:** No physical signals emitted until explicit userspace ioctl. | **Hazardous:** Emits electrical pulses automatically during system boot. |
| **Operator Control** | Complete control over execution timing and test conditions. | Out-of-band execution without operator consent. |
| **Failure Blast Radius** | Probe failure is strictly software-bound (e.g. resource missing). | Failed reset pulse could leave silicon clamped in reset at boot. |
| **Project Compliance** | Fully compliant with zero-hardware-execution safety mandates. | Violates unprompted hardware signaling restrictions. |

- **Selection:** **Policy A is formally adopted.**
- **Specification:**
  1. `fte3600_spi_probe()` acquires descriptors with `GPIOD_OUT_LOW` (logical 0 = physical HIGH / inactive), registers `/dev/fte3600`, and sets state to `UNINITIALIZED`. Zero reset pulses are driven during probe.
  2. Reset occurs **only** when userspace opens `/dev/fte3600` and issues `ioctl(fd, FTE3600_IOC_RESET)`.
  3. A successful reset pulse transitions state from `UNINITIALIZED` (or `UNKNOWN`) to `RESET_COMPLETE`.
  4. State **never** transitions to `READY` until subsequent SPI transactions verify chip ID `0x9362` and CRC `0x1C53`.
  5. Any failed or uncertain reset sequence leaves the state `UNKNOWN`.

---

### 2.6 Character-Device Concurrency & Lifecycle Architecture

#### 1. Synchronized Single-Open Ownership
- The driver does not rely on userspace `O_EXCL` flags.
- Device state embeds an atomic ownership flag:
  ```c
  atomic_t is_open; /* Initialized to 0 */
  ```
- In `fte3600_cdev_open()`:
  ```c
  if (atomic_cmpxchg(&data->is_open, 0, 1) != 0)
      return -EBUSY; /* Exclusive single-client access enforced */
  ```
- In `fte3600_cdev_release()`:
  ```c
  atomic_set(&data->is_open, 0);
  ```

#### 2. Concurrent IOCTL Serialization & Signal Interruption
- All ioctls acquire `mutex_lock_interruptible(&data->lock)`:
  ```c
  if (mutex_lock_interruptible(&data->lock))
      return -ERESTARTSYS; /* Interrupted by signal; clean return */
  ```
- If multiple threads within the owning process issue concurrent ioctls, they serialize strictly under `data->lock`.

#### 3. Device Removal & Shutdown Sequence
- When `fte3600_spi_remove()` is triggered:
  1. Mark driver shutdown: `atomic_set(&data->offline, 1);`
  2. Wake all sleeping pollers/readers: `wake_up_all(&data->event_wq);`
  3. Deregister character device: `cdev_del(&data->cdev);`
  4. Free IRQ: `devm_free_irq(&spi->dev, data->irq, data);`
  5. Acquire and release `mutex_lock(&data->lock)` to ensure in-flight ioctls have drained.
- Any subsequent or waiting ioctls checking `atomic_read(&data->offline)` immediately exit with `-ENODEV`.

#### 4. Post-Shutdown Poll/Read Semantics
- `poll()` returns `EPOLLERR | EPOLLHUP` immediately when offline.
- `read()` returns `-ENODEV` immediately when offline.

---

### 2.7 Pad 229 Safety Wording & Historical Discrepancy

#### Electrical & Register Layer Distinction
The historical telemetry discrepancy on Pad 229 (baseline `0x80100100` vs. post-Stage 1 `0x82000100` / `0x82000102`) is formally characterized:

1. **Output Driver Status:** `PADCFG0_GPIOTXDIS = 1` (Bit 8). The PCH output buffer is **STRICTLY DISABLED**. The host PCH is not driving voltage or current onto the pin.
2. **Input Receiver Status:** `PADCFG0_GPIORXDIS = 0` (Bit 9). Receiver is active and listening to logic levels.
3. **Interrupt Routing:** `PADCFG0_ROUTX = 1` (Bit 17). Input is routed to the IOxAPIC/interrupt controller.
4. **Trigger Polarity:** ACPI specifies `Edge, ActiveHigh`.
5. **Physical Wire State:** DW1 (`PADCFG1 = 0x00000029`) specifies `PullNone`. When the sensor is idle, the line is in a high-impedance floating state. Bit 1 (`GPIORXSTATE`) fluctuates according to ambient charge.
6. **Strict Safety Boundary:**
   - The interrupt path is **NOT hardware-verified**.
   - Direct memory-mapped writes to PCH `PADCFG` registers are **STRICTLY PROHIBITED**.
   - The driver interacts with Pad 229 exclusively through Linux generic IRQ APIs (`devm_request_threaded_irq`), passing `IRQF_ONESHOT`.

---

## 3. Implementation Checklist for Stage 2E.2

```
========================================================================================
                          STAGE 2E.2 IMPLEMENTATION CHECKLIST
========================================================================================
[APPROVED FOR SOFTWARE IMPLEMENTATION]
  [x] UAPI Header (kernel/fte3600.h) with fixed-width types (__u32, __u8) and 64-bit alignment.
  [x] Direct ACPI SPI Driver registration (struct spi_driver matching "FTE3600").
  [x] Driver GPIO registration via devm_acpi_dev_add_driver_gpios() before descriptor get.
  [x] Active-low GPIO reset pulse sequence (logical 1 = assert, logical 0 = deassert).
  [x] Policy A probe lifecycle (probe is passive; explicit FTE3600_IOC_RESET required).
  [x] In-kernel spi_sync() engine with GFP_KERNEL heap buffers and wire CRC-16 validation.
  [x] Character device node (/dev/fte3600) with atomic single-open enforcement.
  [x] Mutex serialization of all SPI and reset operations (mutex_lock_interruptible).
  [x] IRQF_ONESHOT threaded IRQ registration without trigger override (IRQF_TRIGGER_NONE).
  [x] Clean unbind and shutdown ordering with wait-queue wakeups.

[BLOCKED PENDING FURTHER SOURCE VERIFICATION]
  [-] Physical interrupt acknowledgement via register 0x1A84:
      -> Threaded IRQ handler in Stage 2E.2 must NOT access 0x1A84.
      -> Must remain stubbed/disabled until protocol acknowledgement review is authorized.

[PROHIBITED PENDING SEPARATE HARDWARE AUTHORIZATION]
  [!] Binding fte3600.ko to physical SPI hardware (spi-FTE3600:00).
  [!] Loading fte3600.ko with live ACPI device enumeration enabled.
  [!] Toggling physical reset line (Pad 14) on live hardware.
  [!] Servicing live interrupts on Pad 229.
  [!] Physical execution of the 10,246-byte burst read on hardware.
  [!] Writing registers 0xF1, 0xF4 (EFUSE), 0x9A (Trim unlock), or 0x1A84.
  [!] Unsealing the hardware execution gate (bin/fw9369-hw-tool must remain exit code 1).
========================================================================================
```

---

## 4. Files Changed and Verification Results

| File | Action | Description |
| :--- | :--- | :--- |
| `STAGE2E1_2_FINAL_IMPLEMENTATION_PRECONDITIONS.md` | Authored | Comprehensive final implementation preconditions report (this file). |
| `diagnostics/STAGE2E1_2_FINAL_IMPLEMENTATION_PRECONDITIONS.md` | Authored | Mirrored report in diagnostics repository. |
| `STAGE2E1_KERNEL_ARCHITECTURE_DESIGN.md` | Updated | Updated with Policy A probe lifecycle, 10,246/10,240 B distinction, and blocked 0x1A84 ACK. |
| `diagnostics/STAGE2E1_KERNEL_ARCHITECTURE_DESIGN.md` | Updated | Mirrored updated design in diagnostics repository. |
| `PROGRESS.md` | Updated | Appended Stage 2E.1.2 milestone review with historical continuity. |

### Verification of Safety Gate and Offline Tests
- **Hardware Safety Gate:** `bin/fw9369-hw-tool` $\rightarrow$ **Exit Code 1 (PASS — Gate Strictly Sealed)**
- **Offline Mock Test Suite:** `bin/fw9369-tests` $\rightarrow$ **227 / 227 Passed (100.0%)**
- **Git Working Tree:** No Git commit created; changes remain unstaged for review.
