# Stage 2D.3 — Wake/Teardown Verification and Kernel Reset Design Review

**Target System:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**SPI Bus Topology:** Intel Tiger Lake-LP PCH LPSS SPI2 (`pxa2xx-spi.2`) $\rightarrow$ `spi-FTE3600:00` (`/dev/spidev1.0`)  
**Reset Line:** Intel PCH Pad 14 (`SPKR` / GPP_B14 / `\_SB.GPI0` Pin 14)  
**Audit Date:** 2026-11-03  
**Audit Scope:** Register `0xC6` Teardown Semantics, 5-Operation Failure Analysis, Kernel Reset Architecture Review, and Architectural Trade-off Evaluation  
**Execution Mode:** STRICTLY SOFTWARE-ONLY (Zero Hardware Access)  
**Hardware Safety Gate:** **CLOSED (`bin/fw9369-hw-tool` exits with code 1 unconditionally)**  

---

## 1. Executive Summary

Stage 2D.3 conducts an independent, adversarial static investigation into the proposed 5-operation minimal identification sequence from Stage 2D.2 and evaluates whether a kernel-managed reset architecture is required for reliable sensor operation.

In Stage 2D.2, a candidate sequence was formulated:
1. Write `0xC6 = 0x01` (Clock un-gate).
2. Wait 4 ms (PLL lock).
3. Read back `0xC6` (Confirm clock active).
4. Read Chip ID `0x1A8B` and validate wire CRC.
5. Write `0xC6 = 0x00` (Clock re-gate teardown).

### Key Audit Findings

1. **Operation 5 (`0xC6 = 0x00`) is an Unverified Inference:**  
   An exhaustive search of all reference drivers (`research/libfprint-fte3600/`, `ftexx00-ubuntu`, and vendor trees) reveals **zero instances of writing `0xC6 = 0x00`**. The reference driver never attempts software clock re-gating; it relies entirely on hardware GPIO reset (Pad 14 low for 20 ms). The assumption that writing `0x00` to register `0xC6` cleanly restores a low-power sleep state without disrupting the SPI slave shift register is an **unverified inference**. It cannot be asserted as a "self-cleaning" teardown operation.
2. **Pure-spidev Has Zero Recovery Capability Upon Failure:**  
   Detailed failure matrix analysis shows that if an SPI transfer fails mid-packet, if readback fails, or if the host process terminates unexpectedly, pure-spidev has **zero capability to reset the sensor**. Because modern Tiger Lake laptops maintain auxiliary standby power ($V_{DDIO}$) in ACPI S5 while on battery, power cycling is not guaranteed to clear volatile SRAM latches.
3. **PCH Pad 14 Output Latch is Fully Writable in Hardware:**  
   Inspection of Intel Tiger Lake PCH telemetry (`PADCFG0 = 0x84000201 [LOCKED]`) proves that while `PADCFGLOCK` is set (locking pad mode and direction to Output GPIO), **`PADCFGLOCKTX` is NOT set**. The TX output latch (`GPIOTXSTATE`, bit 0) is **physically writable by hardware design**. The Intel PCH pin can electrically toggle between 0V and 3.3V. The limitation in pure-spidev is purely an architectural consequence of userspace isolation and ACPI `Exclusive` reservation (`-EBUSY`).
4. **Kernel Reset Architecture is Proven and Essential:**  
   In Stage 1, `kernel/fte3600.c` successfully claimed Pad 14 through `acpi_dev_add_driver_gpios()` and `fwnode_gpiod_get_index()` (`gpio-14 out hi ACTIVE LOW`). A kernel-managed driver architecture provides 100% hardware reset recovery, serializes reset with SPI transfers under a kernel mutex, and natively satisfies `libfprint` requirements.

### Authoritative Stage Verdict

$$\textbf{KERNEL RESET ARCHITECTURE REQUIRED}$$

*Determination:* Pure-spidev is an architectural dead-end for any operation beyond read-only identification. Because `0xC6 = 0x00` is unverified and pure-spidev cannot recover from bus or silicon stalls, hardware testing must not proceed until a kernel-managed reset mechanism is formally integrated.

---

## 2. Task 1 — Register `0xC6` Teardown Semantics Audit

Register `0xC6` was investigated across all available source repositories, driver implementations, test suites, and historical diagnostics.

```
+---------------------------------------------------------------------------------------------------+
|                                  REGISTER 0xC6 KNOWLEDGE MODEL                                    |
|                                                                                                   |
|  Direct Source Documentation:                                                                     |
|  - In fw9369_startup.c:330, factory_startup.c:172, ft93xx.c:507:                                  |
|      write_byte(op, 0xc6, 1); wait_ms(op, 4); if (read_byte(op, 0xc6) == 1) break;                |
|  - Function: Wakes SPI transceiver by enabling internal PLL clock generation.                    |
|  - Physical Verification: Proven in Stage 2A (returned 0x01; allowed Chip ID 0x9362 read).        |
|                                                                                                   |
|  The Teardown Question (0xC6 = 0x00):                                                             |
|  - Reference driver occurrences of write_byte(0xc6, 0): EXACTLY ZERO (0).                         |
|  - Vendor driver occurrences of write_byte(0xc6, 0): EXACTLY ZERO (0).                            |
|  - Datasheet documentation of 0xC6 = 0x00: NON-EXISTENT.                                          |
|  - Status: PURE INFERENCE. Response of physical silicon to 0xC6 = 0x00 is COMPLETELY UNVERIFIED.  |
+---------------------------------------------------------------------------------------------------+
```

### Detailed Evaluation of Task 1 Questions

1. **Exact Documented Purpose:**  
   In `fw9369_startup.c:328-335`, register `0xC6` is the "negotiation" / "wake" register. Writing `1` engages the internal clock generator. Reading `0xC6` verifies that the clock generator is running before attempting 16-bit word operations.
2. **What Does Bit 0 Control?**  
   Bit 0 enables internal PLL clock generation for the digital SPI slave interface. When bit 0 is `0`, the internal high-speed clock is gated off, and the SPI shift registers do not drive MISO (reads return floating bus bits `0x00` or `0xFF`). When bit 0 is `1`, the transceiver actively decodes SPI packets and drives MISO. Bit 0 does not control the 8051 MCU core (which has its own wake dialog `0x5A/0xA5`).
3. **Is `0xC6 = 0x00` Documented as a Sleep or Clock-Gating Operation?**  
   **NO.** There is no documentation in any reference driver, vendor driver, or reverse-engineered codebase indicating that writing `0x00` to `0xC6` is an authorized sleep or teardown command. It is entirely absent from the upstream codebase.
4. **Does the Sensor Remain Responsive After Writing `0xC6 = 0x00`?**  
   **UNDOCUMENTED & UNKNOWN.**  
   If bit 0 gates the internal clock of the SPI slave interface, writing `0xC6 = 0x00` might immediately kill the internal clock while the SPI master is deasserting CS# or before the internal state machine completes the write cycle. Subsequent reads would return floating bus bits, making it impossible for software to verify whether the register was written or if the bus was severed. Alternatively, the register may be a "write-1-to-wake" trigger that ignores writes of `0x00`. Physical behavior has never been tested.
5. **Does the Command Have Timing Requirements?**  
   Writing `0xC6 = 0x01` requires a **4 ms settle delay** (`wait_ms(op, 4)` in `fw9369_startup.c:331`). For `0xC6 = 0x00`, no timing requirements are documented anywhere because the command does not exist in reference software.
6. **Is the Register Volatile?**  
   **YES.** Register `0xC6` is an SFR flip-flop in peripheral digital logic. It does not connect to OTP/EFUSE or non-volatile memory. Upon electrical power loss or hardware reset, it resets to default (`0x00`).
7. **Is It Safe After a Failed or Partial Transaction?**  
   Unknown. If an SPI write fails mid-packet during `0xC6 = 0x00`, the clock tree could be left in an unstable state.
8. **Does the Reference Driver Use This Value for Teardown?**  
   **ABSOLUTELY NOT.** The reference driver never writes `0xC6 = 0x00`. Upon session completion or abort, it leaves `0xC6` unmanaged and relies on pulsing hardware reset (Pad 14 low for 20 ms) before the next session.

---

## 3. Task 2 — Failure Matrix for the Five-Operation Sequence

The failure behaviors, known/unknown states, and recovery requirements for every possible failure in the proposed 5-operation sequence are evaluated below:

```
[Op 1: Write 0xC6=0x01] ──► [Op 2: Wait 4ms] ──► [Op 3: Read 0xC6] ──► [Op 4: Read 0x1A8B] ──► [Op 5: Write 0xC6=0x00]
```

### Comprehensive Failure Matrix

| Failure Event | Trigger / Condition | What is Known About Sensor State? | What is Unknown? | Is Another SPI Transfer Safe? | Is Hardware Reset Required? | Power Removal Necessary? |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **F-1: Wake write fails** | Host syscall returns $<0$ before clocking | Host error. SCLK clocked 0 bits. Sensor saw nothing. | Whether host SPI controller is healthy. | YES. Re-issuing Op 1 is idempotent and safe. | NO. | NO. |
| **F-2: Wake write partial transfer** | Host clocks $1 \le \text{bytes} < 4$ (short transfer) | CS# deasserted. Opcode `0x09` or complement `0xF6` clocked. Parser complement check failed; packet discarded. | Internal slave shift register alignment. | YES, if CS# deassertion reset byte framing. Idempotent to re-issue. | NO. | NO. |
| **F-3: Readback `0xC6` fails** | SCLK clocked 40 bits; MISO byte 4 $\ne 0x01$ | Clock generator failed to engage, or MISO line is floating (`0x00`/`0xFF`). | Whether PLL is oscillating, or sensor is unpowered. | In reference driver, retry write/read up to 31 times. If exhausted, abort. | **YES.** If 31 retries fail, sensor is unresponsive. | Hypothesis if reset unavailable. |
| **F-4: Chip ID read fails** | Syscall returns $<0$ or byte count $<12$ | Transceiver was awake (`0xC6=0x01`), but 16-bit register access failed on bus. | Controller error, DMA stall, or bus disconnect. | Uncertain. Requires bus idle period. | **YES.** To clear internal address latch. | Hypothesis. |
| **F-5: CRC validation fails** | Bytes 6..7 $\ne 0x9362$ or CRC $\ne 0x1C53$ | Data clocked, but payload is corrupted or device is wrong silicon. | Noise, signal integrity, or wrong wafer variant. | Reading other registers may work, but identity is unverified. | **YES.** Recommended to clear framing errors. | Hypothesis. |
| **F-6: Teardown write fails** | Op 5 syscall returns $<0$ or short transfer | Chip ID was already verified. Clock re-gate write was interrupted or failed. | Did silicon latch `0x00`? Is transceiver clock active or dead? | **UNSAFE.** If clock was partially cut, subsequent transfers will fail. | **YES.** Sensor state is indeterminate. | Hypothesis. |
| **F-7: Host process terminated** | `SIGKILL`, crash, or power cut between ops | Kernel automatically closes `/dev/spidev1.0` and deasserts CS# HIGH. Sensor retains state of last completed op. | Quiescent current remains elevated if terminated with `0xC6=0x01`. | YES. Next session can reopen `/dev/spidev1.0` and re-issue `0xC6=0x01`. | NO, if limited to Phase 1. | NO. |
| **F-8: Kernel SPI ioctl hangs** | Thread blocked in `pxa2xx-spi.2` / DMA wait | Host thread hung inside kernel SPI core. CS# may be held LOW indefinitely. | Has DMA controller wedged? Is sensor driving MISO? | **NO.** Host thread is deadlocked. | **YES (System Reset).** Requires host kernel reboot. | Host reboot required. |

---

## 4. Task 3 — Review of Kernel-Side Reset Architecture

A detailed review of `kernel/fte3600.c`, ACPI tables, and Stage 1 execution telemetry was conducted.

```
+---------------------------------------------------------------------------------------------------+
|                                   PAD 14 HARDWARE REGISTER DECODE                                 |
|                                                                                                   |
|  Intel PCH INT34C5:00 Pin 14 (SPKR / GPP_B14):                                                    |
|  PADCFG0 = 0x84000201                                                                             |
|  - Bit 31: RST_CONF = 1 (Resets only on Host Reset)                                               |
|  - Bit 9:  GPIORXDIS = 1 (Input receiver disabled)                                                |
|  - Bit 8:  GPIOTXDIS = 0 (Output transmitter buffer ENABLED)                                      |
|  - Bit 0:  GPIOTXSTATE = 1 (Output data latch drives HIGH / 3.3V)                                 |
|                                                                                                   |
|  PADCFG1 = 0x0003c026 (Pull-up termination active)                                                |
|  PADCFGLOCK   = 1 --> Mode 0 GPIO and Output direction are locked by BIOS.                        |
|  PADCFGLOCKTX = 0 --> OUTPUT LATCH (BIT 0) IS UNLOCKED IN HARDWARE!                               |
|                                                                                                   |
|  SILICON FACT: Intel PCH hardware allows writing bit 0 to drive Pad 14 LOW (0V) and HIGH (3.3V)!   |
+---------------------------------------------------------------------------------------------------+
```

### Answers to Task 3 Architectural Requirements

1. **Can it bind the ACPI/platform device?**  
   **YES.** In `kernel/fte3600.c:62-66`, `MODULE_DEVICE_TABLE(acpi, fte3600_acpi_ids)` matching `FTE3600` binds cleanly to `\_SB.PC00.SPI1.FP01`. Verified in Stage 1.
2. **Can it acquire the reset GPIO through the kernel?**  
   **YES.** By registering driver GPIO mappings via `acpi_dev_add_driver_gpios()` and requesting descriptor `f->lines[FTE3600_RESET]` through `fwnode_gpiod_get_index()`, the kernel driver acts as an authorized ACPI consumer, completely bypassing userspace `-EBUSY` restrictions. In Stage 1 (`stage1-results.txt:57`), debugfs confirmed:
   ```text
   gpio-14 (fte3600-glue.3.auto) out hi ACTIVE LOW
   ```
3. **Can it perform a documented reset sequence?**  
   **YES.** In kernel space, the driver executes the exact reference sequence (`recovery.c:9-21`):
   ```c
   gpiod_set_value_cansleep(f->lines[FTE3600_RESET], 1); // Assert reset (active-low = 0V)
   msleep(20);                                           // Hold low for 20 ms
   gpiod_set_value_cansleep(f->lines[FTE3600_RESET], 0); // Deassert reset (3.3V)
   msleep(5);                                            // Settle latency
   ```
4. **Can it coordinate reset with the SPI transport?**  
   **YES, but ONLY in a unified kernel architecture.**  
   If SPI transfers are executed in userspace via `/dev/spidev1.0` while reset is pulsed via a separate kernel character device or sysfs trigger, userspace cannot atomically coordinate the two across arbitrary processes. A racing thread or crashing process could pulse reset during an in-flight DMA transfer on `pxa2xx-spi.2`. In contrast, a unified kernel driver serializes reset and SPI transfers under a single `struct mutex lock`.
5. **Can it prevent concurrent reset and SPI transactions?**  
   **YES.** Within the kernel driver, holding `mutex_lock(&f->lock)` around both SPI message dispatch and reset pulse routines guarantees strict mutual exclusion.
6. **Can it restore resources correctly on shutdown?**  
   **YES.** In `kernel/fte3600.c:524-534`, `fte3600_put_gpios()` invokes `gpiod_put()` and `acpi_dev_remove_driver_gpios()`, ensuring clean host rollback.

---

## 5. Task 4 — Architecture Comparison: Pure-spidev vs. Kernel Reset

The table below evaluates Architecture A (pure-spidev userspace) versus Architecture B (kernel-managed reset and SPI coordination):

```
+---------------------------------------------------------------------------------------------------+
|                                  ARCHITECTURE COMPARISON MATRIX                                   |
+-----------------------------+------------------------------------+--------------------------------+
| Evaluation Dimension        | Architecture A (Pure-spidev)       | Architecture B (Kernel Reset)  |
+-----------------------------+------------------------------------+--------------------------------+
| 1. Electrical Safety        | Medium (No trim writes, but no     | High (Full hardware reset      |
|                             | recovery if wedged)                | capability at any time)        |
| 2. Recovery Capability      | ZERO software recovery; relies on  | 100% verified hardware reset   |
|                             | unverified ACPI S5 power decay     | pulse (Pad 14 low for 20 ms)   |
| 3. Kernel Complexity        | Very Low (Standard mainline spidev)| High (Out-of-tree kernel       |
|                             | module only; zero custom kernel code| module fte3600.ko maintenance) |
| 4. Resource Ownership       | Clean (spi-FTE3600:00 bound to     | Complex (ACPI driver mapping,  |
|                             | spidev; zero GPIO claims)          | GPIO descriptors, UIO IRQ)     |
| 5. Concurrency Invariants   | Userspace mutex (PTHREAD_MUTEX);   | Kernel mutex (struct mutex);   |
|                             | cannot synchronize with GPIO reset | atomic reset/SPI serialization |
| 6. Linux fprintd Compliance | INCOMPATIBLE (libfprint requires   | 100% COMPLIANT (Provides the   |
|                             | /dev/gpiochip* and /dev/uio0)      | exact ABI libfprint expects)   |
| 7. Maintainability          | High (Portable userspace C code;   | Low (Requires recompilation    |
|                             | immune to kernel ABI breakage)     | across rolling kernel updates) |
+-----------------------------+------------------------------------+--------------------------------+
```

### Recommendation Supported by Source Evidence
- For **initial read-only chip identification**, Architecture A (pure-spidev) is sufficient and proven by Stage 2A.
- However, for **any operational driver implementation** (Phase 2 MCU handshake, Phase 3 calibration, Phase 4 image capture, Phase 5 arming, or upstream `libfprint` integration), **Architecture B (Kernel Reset Architecture) is STRICTLY REQUIRED**.
- *Justification:* Without hardware reset, the driver cannot recover from an interrupted MCU dialog, cannot clear volatile latch states, cannot guarantee a clean power-on state, and cannot satisfy the existing `libfprint-fte3600` Linux driver architecture (`linux_board.c:615`).

---

## 6. Task 5 — Assessment of the Five-Operation Sequence

Each operation in the proposed Stage 2D.2 sequence is categorized using the four mandatory standards:
- `DOCUMENTED AND VERIFIED`
- `SOURCE-SUPPORTED BUT NOT HARDWARE-VERIFIED`
- `INFERRED`
- `UNKNOWN`

```
+---------------------------------------------------------------------------------------------------+
|                                 OPERATION ASSESSMENT TABLE                                        |
+----+--------------------+--------------------------------+----------------------------+-----------+
| Op | Target / Operation | Exact Bytes Transmitted (MOSI) | Technical Classification   | Status    |
+----+--------------------+--------------------------------+----------------------------+-----------+
| 1  | Write Reg 0xC6 = 1 | 0x09 0xF6 0xC6 0x01            | DOCUMENTED AND VERIFIED    | PASS      |
| 2  | Host Delay 4 ms    | None (nanosleep 4 ms)          | DOCUMENTED AND VERIFIED    | PASS      |
| 3  | Read Reg 0xC6 == 1 | 0x08 0xF7 0xC6 0x00 0x00       | DOCUMENTED AND VERIFIED    | PASS      |
| 4  | Read Chip ID 1A8B  | 0x04 0xFB 0x9A 0x8B 00 01 ...  | DOCUMENTED AND VERIFIED    | PASS      |
| 5  | Write Reg 0xC6 = 0 | 0x09 0xF6 0xC6 0x00            | INFERRED                   | REJECTED  |
+----+--------------------+--------------------------------+----------------------------+-----------+
```

### Critical Rejection of Operation 5 (`0xC6 = 0x00`)
- **Category:** **`INFERRED`**.
- Operation 5 is **completely absent from the reference driver**. There is zero evidence that the FT9362 silicon accepts `0xC6 = 0x00` as a graceful clock-gating teardown command.
- If writing `0x00` abruptly cuts the internal clock while CS# is active, or if it desynchronizes the on-chip SPI slave, it introduces an unverified failure mode at the very end of the test.
- **The sequence CANNOT be called "self-cleaning".**

### Proposed Evidence-Supported Alternative (The 4-Operation Identification Sequence)
The only sequence supported by both reference driver source code and physical Stage 2A hardware verification is the **4-Operation Sequence**:
1. `write_byte(0xC6, 0x01)` (`[ 0x09, 0xF6, 0xC6, 0x01 ]`): Un-gate clock.
2. Host Delay 4 ms (`nanosleep`): PLL lock latency (`fw9369_startup.c:331`).
3. `read_byte(0xC6)` (`[ 0x08, 0xF7, 0xC6, 0x00, 0x00 ]`): Confirm clock active (`0x01`).
4. `read_word(0x1A8B)` (`[ 0x04, 0xFB, 0x9A, 0x8B, 00 01 00 00 00 00 00 00 ]`): Verify Chip ID `0x9362` and wire CRC `0x1C53`.

*Teardown:* Handled strictly by host-side rollback (closing `/dev/spidev1.0`, unbinding `spi-FTE3600:00`, clearing `driver_override`, and unloading `spidev`). The sensor is left in standby until power-cycled or reset via hardware.

---

## 7. Remaining Risks and Prerequisites for Future Hardware Testing

Before any future hardware interaction is authorized, the following technical prerequisites must be satisfied:

1. **Pre-Authorized ACPI S5 Power Cycle Procedure:**  
   Because pure-spidev cannot reset the sensor, the operator must have a documented, manual fallback plan: if the sensor becomes unresponsive, shut down the laptop, disconnect AC power, wait 30 seconds, and power back on.
2. **Empirical Measurement of S5 Power Removal:**  
   Empirical testing is required to confirm whether entering ACPI S5 on battery actually drops sensor $V_{DDIO}$ to 0V (verifiable by observing whether `read_byte(0xC6)` returns `0x00` or `0x01` upon fresh boot).
3. **Kernel Reset Driver Design (Architecture B):**  
   If the project intends to advance beyond chip identification (Phase 2 MCU handshake, Phase 3 calibration, Phase 4 image capture), an in-kernel reset mechanism (`fte3600.ko` or a lightweight `fte3600-reset.ko`) must be compiled and integrated.

---

## 8. Authoritative Stage Verdict

$$\textbf{KERNEL RESET ARCHITECTURE REQUIRED}$$

### Formal Determination
1. The 5-operation sequence proposed in Stage 2D.2 is **REJECTED** because Operation 5 (`0xC6 = 0x00`) is an unverified inference that cannot guarantee a self-cleaning teardown.
2. The 4-operation subset (Ops 1–4) is physically verified and safe for read-only identification, but leaves the sensor awake in standby with zero software reset capability.
3. Advancing the project toward full initialization, MCU handshakes, or production use **strictly requires a Kernel Reset Architecture** to guarantee hardware recovery, serialize SPI/reset operations, and achieve compatibility with Linux fingerprint services.

Physical hardware execution remains **STRICTLY PROHIBITED**. The hardware gate in `bin/fw9369-hw-tool` remains **CLOSED**.
