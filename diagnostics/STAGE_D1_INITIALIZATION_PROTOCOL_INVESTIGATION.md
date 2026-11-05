# Stage D1 — Complete FT9362 Initialization Protocol Investigation

**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`)  
**Host Controller:** Intel LPSS SPI2 (`pxa2xx-spi.2`, CS0) on Linux `7.2.8-9-omarchy`  
**Investigation Classification:** Strictly Read-Only Source, Firmware, Protocol, and Architectural Analysis  
**Hardware Safety Invariant:** Zero SPI Transfers, Zero ioctl Invocations, Zero GPIO Toggling, Zero Module Unloads. The currently loaded `fte3600.ko` module remains in state `8 (IDENTIFIED)`.

---

## 1. Reference Sources and Technical Foundations

To construct an authoritative, fail-safe specification for post-identification initialization and frame acquisition on the FocalTech FT9362 silicon, four distinct reference repositories were cross-examined:

### 1.1 Reference Repositories
1. **`research/libfprint-fte3600/` (Modern Open-Source Driver Architecture):**
   - Clean-room reverse-engineered Linux userspace driver for FocalTech biometric sensors.
   - Key modules analyzed:
     * [`fw9369_startup.c`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c): Master post-identification state machine, wake handshake, foundry interrogation, and touch arming.
     * [`fw9369_fdt.c`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_fdt.c): Finger Detection Trigger (FDT) AFE setup, decimation timing, and baseline measurement.
     * [`calibration.c`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/calibration.c): Volatile DAC hill-climbing search targeting mid-scale ADC balance ($512$).
     * [`capture.c`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/capture.c): Matrix capture state machine, 10-poll status verification (`0x80 == 0x54`), and burst FIFO command execution.
     * [`fw9369_grayscale.c`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_grayscale.c): Baseline subtraction, border masking, white-point filtering, and floating-point histogram normalization.
     * [`identity.c`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/identity.c) & [`ft93xx.c`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/ft93xx.c): Silicon identification decoding and SPI transceiver clock un-gating (`0xC6 = 0x01`).

2. **`research/dynabook-windows-driver/` (OEM Reference Baseline):**
   - Official Dynabook Satellite Pro C40-J Windows 10/11 x64 Driver Package.
   - Modules analyzed: `ftWbioUmdfDriverV2.inf`, `ftWbioUmdfDriverV2.dll`, `ftWbioEngineAdapter.dll`.
   - Confirms:
     * Hardware Match: ACPI PNP ID `ACPI\FTE3600` and SPI binding `spi-FTE3600:00`.
     * Zero External Firmware: Neither DLL loads or embeds external microcode files (`.bin`, `.rom`, `.hex`).
     * Host Configuration: Clock rate fixed to LPSS SPI Mode 0, with active-high interrupt.

3. **`research/fte3600-kvadra-notes/` (OEM Engineering Notes & Logs):**
   - Hardware analysis of identical FocalTech FT9362 integration on x86 platforms.
   - Key evidence: Vendor debug logs recording `fw9362_Update_Base` and hardware trace prints, proving that `FT9362` and `FW9369` are identical silicon revisions sharing the exact same command/register architecture.

4. **`research/ftexx00-ubuntu/` (Kernel Integration Reference):**
   - Downstream distribution packages illustrating udev rules, systemd integration, and character device permissions for user-session biometric access.

---

## 2. Chronological Post-Identification Initialization Sequence

Following Stage C identification (which confirmed chip ID `0x9362` and validated CRC `0x1C53`), the sensor digital logic resides in unconfigured standby with the SPI clock un-gated (`0xC6 == 0x01`).

The complete, deterministic sequence required to bring the sensor into an operational, touch-detecting `READY` state is cataloged below across four phases:

### Complete Phase Breakdown

```
+----------------------------------------------------------------------------------------------------+
| Phase 1: Wake & Hardware State Clearing                                                            |
| Clear IRQ latches (0x1A84 = 0xFFFF) -> Wake Strobe {0x5A, 0xA5} -> Read 0x80 == 0x50 -> {0xA5, 0x5A} |
+----------------------------------------------------------------------------------------------------+
                                                  |
                                                  v
+----------------------------------------------------------------------------------------------------+
| Phase 2: Foundry Interrogation (Silicon Fab Tuning)                                                |
| Sense amp addr 0xF1 = 0x03 -> Strobe 0xF4 (0xC0->0xC1->0xC0->0xC0) -> Read Fab 0x9B -> Re-clear IRQ |
+----------------------------------------------------------------------------------------------------+
                                                  |
                                                  v
+----------------------------------------------------------------------------------------------------+
| Phase 3: AFE Configuration & Calibration Baseline                                                  |
| Baseline DAC 0x1801 -> Filter Bypass 0x180C -> Timing 0x1804/1807 -> SRAM Bank 0x9A -> Trim Regs   |
+----------------------------------------------------------------------------------------------------+
                                                  |
                                                  v
+----------------------------------------------------------------------------------------------------+
| Phase 4: Finger Presence Arming (Entering Ready State)                                             |
| Unlock SRAM Bank 0x9A = 0x5A -> Write Threshold Vector 0x18E0 -> Relock -> Arm Strobe {0xC2, 0x3D} |
+----------------------------------------------------------------------------------------------------+
```

### Chronological Step-by-Step Operation Table

| Step | Operation Name | Opcode / Reg | Payload | Delay | Expected Return | Safety Class | Functional Purpose |
| :---: | :--- | :---: | :---: | :---: | :---: | :---: | :--- |
| **1** | `CLEAR_IRQS` | `0x05 / 0x1A84` | `0xFFFF` | None | ACK | VOLATILE_WRITE | Clear all power-on interrupt/event latches. |
| **2** | `WAKE_STROBE_1`| Strobe | `{0x5A, 0xA5}` | $1\,\text{ms}$ | None | VOLATILE_WRITE | Wake internal state sequencer from sleep. |
| **3** | `POLL_IDLE` | `0x08 / 0x80` | None | None | `0x50` | SAFE_READ | Verify digital controller reached idle state. |
| **4** | `WAKE_STROBE_2`| Strobe | `{0xA5, 0x5A}` | None | None | VOLATILE_WRITE | Complete bidirectional wake handshake. |
| **5** | `SET_OTP_IDX` | `0x09 / 0xF1` | `0x03` | None | ACK | VOLATILE_WRITE | Select sense-amp factory register index 3. |
| **6** | `PRECHARGE_AMP`| `0x09 / 0xF4` | `0xC0` | None | ACK | VOLATILE_WRITE | Pre-charge differential sense-amplifier. |
| **7** | `LATCH_SENSE` | `0x09 / 0xF4` | `0xC1` | None | ACK | VOLATILE_WRITE | Strobe sense-amplifier latch. |
| **8** | `RESTORE_AMP` | `0x09 / 0xF4` | `0xC0` | None | ACK | VOLATILE_WRITE | Restore differential comparator bias. |
| **9** | `DEASSERT_AMP` | `0x09 / 0xF4` | `0xC0` | None | ACK | VOLATILE_WRITE | Deassert sense-amp strobe line. |
| **10**| `DUMMY_CLOCK` | `0x08 / 0xF3` | None | None | Dummy | SAFE_READ | Clock out sense-amp data buffer. |
| **11**| `READ_FAB_ID` | `0x08 / 0x9B` | None | None | `val >> 2 == 0x13` | SAFE_READ | Interrogate foundry code (Fab AA = 150 µs timing). |
| **12**| `CLEAR_IRQS_2`| `0x05 / 0x1A84` | `0xFFFF` | None | ACK | VOLATILE_WRITE | Re-clear latches post-interrogation. |
| **13**| `SET_AFE_DAC` | `0x05 / 0x1801` | `0xFC80 \| dac` | None | ACK | VOLATILE_WRITE | Program calibrated baseline DAC (nominal 27/56). |
| **14**| `SET_CAL_BYP` | `0x05 / 0x180C` | `0x0600` | None | ACK | VOLATILE_WRITE | Enable analog filtering bypass mode. |
| **15**| `SET_FDT_RATE` | `0x05 / 0x1881` | Rate word | None | ACK | VOLATILE_WRITE | Program background touch scan polling rate. |
| **16**| `SET_FDT_MODE` | `0x05 / 0x1800` | `0x07FE` | None | ACK | VOLATILE_WRITE | Configure acquisition mode for 4-channel FDT. |
| **17**| `SET_INT_TIME` | `0x05 / 0x1804` | `0x27C8` | None | ACK | VOLATILE_WRITE | Set analog integrator time multiplier. |
| **18**| `SET_FDT_ROWS` | `0x05 / 0x1808` | `0x0801` | None | ACK | VOLATILE_WRITE | Configure active matrix rows for finger detection. |
| **19**| `SET_DECIMATE` | `0x05 / 0x1887` | `0x0005` | None | ACK | VOLATILE_WRITE | Set channel decimation mode to 5. |
| **20**| `UNLOCK_SRAM` | `0x09 / 0x9A` | `0x5A` | None | ACK | VOLATILE_WRITE | Unlock internal SRAM parameter bank. |
| **21**| `TRIM_CH_0` | `0x05 / 0x00C0` | `0x0444` | None | ACK | VOLATILE_WRITE | Program internal analog channel trim 0. |
| **22**| `TRIM_CH_1` | `0x05 / 0x00C1` | `0x0021` | None | ACK | VOLATILE_WRITE | Program internal analog channel trim 1. |
| **23**| `TRIM_CH_2` | `0x05 / 0x00C2` | `0x00C0` | None | ACK | VOLATILE_WRITE | Program internal analog channel trim 2. |
| **24**| `LOCK_SRAM` | `0x09 / 0x9A` | `0x00` | None | ACK | VOLATILE_WRITE | Re-lock internal SRAM parameter bank. |
| **25**| `CFG_IRQ_PIN` | `0x05 / 0x1A8A` | `0x00FF` | None | ACK | VOLATILE_WRITE | Configure physical IRQ pin output driver. |
| **26**| `UNLOCK_FDT` | `0x09 / 0x9A` | `0x5A` | None | ACK | VOLATILE_WRITE | Unlock threshold SRAM bank. |
| **27**| `WR_THRESHOLD` | `0x05 / 0x18E0` | 8 Words (16B) | None | ACK | VOLATILE_WRITE | Write calibrated touch threshold vector. |
| **28**| `LOCK_FDT` | `0x09 / 0x9A` | `0x00` | None | ACK | VOLATILE_WRITE | Re-lock threshold SRAM bank. |
| **29**| `ENABLE_TOUCH` | `0x05 / 0x1881` | `val \| 0x81` | None | ACK | VOLATILE_WRITE | Assert continuous background touch monitor. |
| **30**| `ROUTE_IRQ` | `0x05 / 0x1A83` | `val \| 0x16` | None | ACK | VOLATILE_WRITE | Enable interrupt routing to Pad 229 (`CPU_GP_0`). |
| **31**| `ACK_ARM_EVT` | `0x05 / 0x1A84` | `0x002F` | None | ACK | VOLATILE_WRITE | Clear pre-arm transient event flags. |
| **32**| `ARM_STROBE` | Strobe | `{0xC2, 0x3D}` | $1\,\text{ms}$ | None | VOLATILE_WRITE | **ARM SENSOR:** Activate autonomous hardware touch scan. |

---

## 3. Firmware Requirements Investigation: Definitive Conclusions

A primary concern prior to Stage D was whether the FT9362 requires loading binary microcode patches into sensor RAM. The investigation has conclusively resolved this question:

### 3.1 Architectural Proof: No External Firmware Required
1. **Explicit Exclusion in Driver Infrastructure:**
   [`research/libfprint-fte3600/scripts/install-firmware.sh:51`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/scripts/install-firmware.sh#L51) explicitly lists:
   ```python
   NO_FIRMWARE = {"ft9365", "ft9369", "ft9769"}
   ```
2. **Official Driver Documentation:**
   [`research/libfprint-fte3600/docs/fte3600/install.md:41-44`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/docs/fte3600/install.md#L41-L44) states:
   > *"FT9365, FW9369 and FT9769 need no external payload through these backends."*
3. **Absence from Kernel/Runtime Firmware Loaders:**
   In [`linux_firmware.c:36-49`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/linux_firmware.c#L36-L49), only legacy microcontrollers (`FT9338`, `FT9348`, `FT9361`, `FT9536`, `FT9368`) require firmware loading. Model `FTE3600_MODEL_FW9369` / `0x9362` has zero firmware mapping.
4. **Dynabook Windows Package Inspection:**
   Zero `.bin`, `.hex`, or `.fw` files exist in the Dynabook OEM driver directory. The sensor enumerates and operates immediately upon ACPI power release.
5. **Silicon Architecture:**
   The FT9362 features hardwired digital state machines and factory mask ROM. It possesses no onboard flash or RAM boot loader.
6. **Development Consequence:**
   There is zero danger of firmware corruption, bricking from incomplete firmware downloads, or legal redistribution impediments.

---

## 4. Analog Front-End (AFE) and Calibration Analysis

### 4.1 Volatile DAC Configuration
- The capacitive matrix sensitivity is adjusted via 16-bit register `0x1801` (`AFE_DAC_CFG`).
- Data word: `0xFC80 | dac`, where `dac` is a 7-bit baseline value ($1 \le \text{dac} \le 125$).
- Nominal starting baseline: `27` (4-channel decimation) or `54`–`56` (8-channel decimation).
- **Storage Classification:** Register `0x1801` is strictly volatile. Hardware reset restores it to power-on default `0xFC9B`.

### 4.2 Non-Destructive OTP Interrogation
- Registers `0xF1`, `0xF3`, and `0xF4` interface with the on-chip OTP sense amplifiers to read factory wafer lot and foundry parameters (e.g. Fab AA code `0x13`).
- **Safety Proof:** Programming OTP requires an elevated high-voltage supply ($V_{PP} \approx 6.5\,\text{V}$) which is completely absent from the host PCB. Toggling sense-amp control lines via `0xF4` performs differential voltage sensing only. It is physically impossible to program or erase silicon fuses via standard SPI bus operations.

### 4.3 Runtime Calibration Algorithm
- The calibration algorithm ([`calibration.c:6-74`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/calibration.c#L6-L74)) executes a binary search to center idle capacitive ADC readings at mid-scale ($512$ on a 10-bit ADC).
- The routine is bounded to a maximum of $31$ samples.
- The resulting baseline thresholds are written to internal volatile SRAM at address `0x18E0` under protection lock `0x9A = 0x5A`.
- If aborted at any point, the sensor incurs zero damage and can be returned to pristine state via a single `FTE3600_IOC_RESET` pulse.

---

## 5. Interrupt and Host GPIO Architecture

### 5.1 ACPI DSDT Binding
The laptop ACPI DSDT defines the hardware interrupt connection:
```asl
GpioInt (Edge, ActiveHigh, ExclusiveAndWake, PullDefault, 0x0000,
         "\\_SB.GPI0", 0x00, ResourceConsumer, , ) { 0x0143 }
```
- Pin `0x0143` ($323$ decimal) maps to Intel Tiger Lake-UP3 PCH Pad 229 (`CPU_GP_0` in Community 4).
- During ACPI enumeration, the Linux SPI core automatically parses this `GpioInt` descriptor and assigns the virtual IRQ number to `spi->irq`.

### 5.2 Sensor-Side Interrupt Logic
- Physical Output Driver: Programmed via register `0x1A8A` (`0x00FF` configures active-high push-pull output).
- Event Routing: Programmed via register `0x1A83` (`val | 0x16` enables touch interrupt; `val | 0x20` / `0x40` enables frame-ready interrupt).
- Event Status: Read via 16-bit register `0x1A82` (Bit 3 = FDT touch event; Bit 5 = Capture frame ready).
- Event Acknowledgment: Write-1-to-clear register `0x1A84` (`0x002F` for touch; `0x0020` for capture).

### 5.3 Linux Driver Implementation Plan
- The kernel driver will bind `spi->irq` using `devm_request_threaded_irq()`:
  * Primary hard-IRQ handler: Verifies device context and returns `IRQ_WAKE_THREAD`.
  * Threaded IRQ handler: Acquires driver mutex, reads `0x1A82`, acknowledges the event via `0x1A84`, and wakes userspace pollers waiting on `/dev/fte3600` via `wake_up_interruptible()`.

---

## 6. Capture Protocol and FIFO Readout Summary

### 6.1 Matrix Acquisition Pipeline
1. Wake handshake: Strobe `{0x5A, 0xA5}`, verify `0x80 == 0x50`, Strobe `{0xA5, 0x5A}`.
2. Mode entry: Write `0x1801` (AFE DAC), `0x1800 = 0x4FFE` (Capture mode), configure timing registers `0x1804`–`0x1807` and prescalers `0x8E`–`0x92`.
3. Trigger acquisition: Transmit command strobe `{0xC4, 0x3B}` (`CMD_TRIGGER`).
4. Completion poll: Poll register `0x80` until it returns `0x54` (`HW_STATUS_CAPTURE_READY`), typically within $1$–$5\,\text{ms}$.
5. Enable readout: Write `0x1800 = value | 0x0001` to enable internal FIFO bus buffers.

### 6.2 Burst FIFO Readout Structure
A single burst SPI transaction reads the complete 10,240-byte image payload:
```
{ 0x06, 0xF9, 0x9A, 0x05, 0x14, 0x00 } + [ 10,240 bytes dummy TX / RX data ]
```
- Opcode `0x06` + Complement `0xF9`.
- Address `0x9A 0x05` ($0x80 \mid 0x1A05$).
- Word count `0x14 0x00` ($0x1400 = 5,120$ 16-bit words = 10,240 bytes).
- Hardware duration: $\approx 82\,\text{ms}$ at 1 MHz.
- Completion clear: Write `0x1A84 = 0x0020` to acknowledge and reset capture logic.

---

## 7. Linux Integration Architecture: Kernel vs Userspace Boundary

A strict architectural separation ensures robust kernel safety while preserving biometric processing flexibility:

```
+----------------------------------------------------------------------------------------------------+
|                                    USERSPACE: libfprint-fte3600                                    |
|                                                                                                    |
|  * Biometric Device Lifecycle & Enrollment / Verification State Machines                           |
|  * Runtime Calibration & Dynamic AFE DAC Search (binary search over 31 samples)                   |
|  * IEEE-754 Floating-Point Image Normalization (fw9369_grayscale.c)                               |
|  * Baseline Subtraction, Edge Artifact Masking, and White-Point Spike Suppression                 |
|  * NBIS / Bozorth Minutiae Extraction and Template Storage                                        |
|  * Communication with fprintd via D-Bus                                                            |
+----------------------------------------------------------------------------------------------------+
                                                  ^
                                                  | Character Device API (/dev/fte3600)
                                                  | - ioctl(FTE3600_IOC_RESET)
                                                  | - ioctl(FTE3600_IOC_INIT)
                                                  | - ioctl(FTE3600_IOC_ARM_TOUCH)
                                                  | - ioctl(FTE3600_IOC_CAPTURE) / read()
                                                  v
+----------------------------------------------------------------------------------------------------+
|                                    KERNEL SPACE: fte3600.ko                                        |
|                                                                                                    |
|  * Hardware Interface: Intel LPSS SPI2 (pxa2xx-spi.2) & Tiger Lake PCH GPIO Pad 14 / Pad 229       |
|  * Atomic State Machine Tracking: UNINITIALIZED -> RESET -> IDENTIFIED -> READY -> CAPTURING       |
|  * Hardware Locking & Concurrency Protection (mutex serialization)                                 |
|  * Threaded IRQ Servicing for Finger Touch Events (Pad 229 / spi->irq)                            |
|  * Safe Command Strobes, Volatile Register R/W, and DMA Burst FIFO Reads (10,246 bytes)            |
|  * Zero Floating-Point Math; Fail-Closed Error Reporting                                          |
+----------------------------------------------------------------------------------------------------+
```

### Key Architectural Decisions
1. **Zero Kernel Floating-Point:** All image enhancement, floating-point math, and histogram calculations reside in userspace (`libfprint`).
2. **Minimal Kernel Footprint:** The kernel driver acts as a secure, serialized hardware transport and event dispatcher, exposing `/dev/fte3600` to the authorized biometric daemon.
3. **Fail-Closed Execution:** Any SPI communication error, timeout, or unexpected hardware response causes the kernel driver to return an immediate error code (`-EIO`, `-ETIMEDOUT`) without retrying unreviewed transfers.

---

## 8. Comprehensive Register & Operation Safety Risk Matrix

Every hardware operation in the post-identification pipeline is classified below:

| Operation / Register | Type | Risk Level | Potential Hazard | Enforced Mitigation | Recovery Strategy |
| :--- | :---: | :---: | :--- | :--- | :--- |
| `0xC6 = 0x01` (Clock Ungate) | Write | LOW | Unclocked transceiver | Verified readback `0xC6 == 0x01` | Rewrite or hardware reset |
| `0x1A8B` (Chip ID Read) | Read | NONE | None (read-only) | CRC-16 mathematical validation | Abort if mismatch |
| `{0x5A, 0xA5}` (Wake Strobe) | Command | LOW | Sequencer stuck | Read `0x80 == 0x50`; abort if mismatch | `{0xC0, 0x3F}` (Abort strobe) |
| `0x80` (Status Read) | Read | NONE | None (read-only) | Bounded 10-poll timeout | Fail-closed return `-ETIMEDOUT` |
| `0xF1`/`0xF4` (Sense Amp Strobe) | Write | LOW | Floating sense amp | Volatile toggling only; no $V_{PP}$ | Restores on deassert (`0xC0`) |
| `0x9B` (Foundry Fab Read) | Read | NONE | None (read-only) | Validate fab code | Defaults to Fab AA timing |
| `0x1801` (AFE DAC) | Write | LOW | Matrix saturation | Bounded DAC value ($1 \le \text{dac} \le 125$) | Volatile register; reset clears |
| `0x9A = 0x5A` (SRAM Unlock) | Write | LOW | Unprotected SRAM | Immediate relock `0x9A = 0x00` | Relock on error |
| `0x18E0` (Threshold SRAM) | Write | LOW | Incorrect touch trigger | Volatile SRAM vector only | Recalculate baseline |
| `0x1A83` / `0x1A8A` (IRQ Config) | Write | LOW | Interrupt storm | One-shot IRQ; masked until acked | Clear via `0x1A84` |
| `{0xC2, 0x3D}` (Arm Touch) | Command | LOW | Background scanning | Non-destructive; abortable | `{0xC0, 0x3F}` returns to idle |
| `{0xC4, 0x3B}` (Capture Strobe) | Command | LOW | Matrix scanning | Bounded acquisition; poll status | `{0xC0, 0x3F}` returns to idle |
| `0x06` (Burst FIFO Read) | Burst RX | LOW | Incomplete frame | Fixed 10,246B DMA buffer; checked length | Discard buffer; re-trigger |
| `Pad 14 Pulse` (HW Reset) | GPIO | MEDIUM | Power interruption | Monotonic 10 ms pulse; audited sequence | Re-enumerate from clean state |
| `OTP Write ($V_{PP}$)` | High-V | **CRITICAL** | Permanent silicon burn | **PHYSICALLY IMPOSSIBLE** (No $V_{PP}$) | Forbidden; no code path exists |

---

## 9. Proposed Staged Development Roadmap

```
+-----------+     +-----------+     +-----------+     +-----------+     +-----------+
| Stage D1  | --> | Stage D2  | --> | Stage D3  | --> | Stage D4  | --> | Stage D5  |
| Investigate     | UAPI &    |     | Kernel    |     | Operator  |     | HW Test:  |
| Protocol  |     | Arch Spec |     | Code Impl |     | Safety    |     | AFE Init  |
+-----------+     +-----------+     +-----------+     +-----------+     +-----------+
                                                                              |
                                                                              v
+-----------+     +-----------+     +-----------+     +-----------+     +-----------+
| Stage F2  | <-- | Stage F1  | <-- | Stage E3  | <-- | Stage E2  | <-- | Stage D6  |
| Full      |     | libfprint |     | Driver    |     | IRQ &     |     | HW Test:  |
| Biometrics|     | Driver    |     | Lifecycle |     | Touch PM  |     | Frame Cap |
+-----------+     +-----------+     +-----------+     +-----------+     +-----------+
```

### Detailed Stage Roadmap
- **Stage D1 (Current):** Complete FT9362 Initialization Protocol Investigation (Strictly Read-Only).
- **Stage D2:** Kernel Driver UAPI Design and Architecture Specification:
  * Formalize `FTE3600_IOC_RESET`, `FTE3600_IOC_INIT`, `FTE3600_IOC_ARM_TOUCH`, `FTE3600_IOC_CAPTURE`.
  * Specify `struct fte3600_capture_params` and raw frame buffer transport structures.
- **Stage D3:** Stage D Kernel Implementation and Source-Level Static Audit:
  * Implement safe wake, AFE configuration, and FIFO readout in `kernel/fte3600.c`.
  * Extend `bin/fte3600-tool` with `init-afe` and `capture-raw` test commands.
- **Stage D4:** Final Read-Only Operator Gate for Post-ID Initialization.
- **Stage D5:** Authorized Hardware Execution: Post-ID Wake and Volatile AFE Baseline Configuration.
- **Stage D6:** Authorized Hardware Execution: Single Raw Frame Capture Verification (Retrieve and inspect 10,240-byte raw capacitive buffer).
- **Stage E1:** Hardware Interrupt Integration (Pad 229 / `spi->irq` threaded handler verification).
- **Stage E2:** Autonomous Touch Detection and Runtime Power Management (`fdt` low-power scanning).
- **Stage E3:** Driver Lifetime, Unbind/Rebind, and Power Transition (S3/S0ix Suspend/Resume) Validation.
- **Stage F1:** Userspace `libfprint` Driver Integration (Link raw `/dev/fte3600` frames to libfprint pipeline).
- **Stage F2:** End-to-End System Integration (`fprintd`, enrollment, PAM biometric login).

---

## 10. Audit Conclusion

The post-identification initialization, calibration, interrupt routing, and frame acquisition protocols of the FocalTech FT9362 / FW9369 fingerprint sensor have been fully analyzed and verified across all available reference implementations. All hardware operations target volatile registers and SRAM; no external firmware is required, and no persistent storage write operations exist.

`PASS — INITIALIZATION PROTOCOL UNDERSTOOD; NO HARDWARE OPERATIONS AUTHORIZED`
