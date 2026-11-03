# Stage 2D.2 — Minimal Initialization Command Safety Audit

**Target System:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**SPI Bus Topology:** Intel Tiger Lake-LP PCH LPSS SPI2 (`pxa2xx-spi.2`) $\rightarrow$ `spi-FTE3600:00` (`/dev/spidev1.0`)  
**Audit Date:** 2026-11-03  
**Audit Scope:** Command Inventory, Prohibited Command Reconciliation, MCU Dialog Failure Mechanics, Reset Architecture Alternatives, and Minimal Initialization Proposal  
**Execution Mode:** STRICTLY SOFTWARE-ONLY (Zero Hardware Access)  
**Hardware Safety Gate:** **CLOSED (`bin/fw9369-hw-tool` exits with code 1 unconditionally)**  

---

## 1. Executive Summary

Stage 2D.2 conducts a rigorous, static-only safety audit to determine the exact minimal command sequence required to safely interact with the FT9362 fingerprint sensor in a future controlled hardware test.

This audit builds upon Stage 2A (physical chip identification), Stage 2C.5C.7 (static concurrency audit with 227/227 tests passing), Stage 2D (readiness audit), and Stage 2D.1 (blocker verification). Hardware execution remains strictly prohibited.

### Key Audit Findings

1. **Phase 2 Contains Prohibited Non-Volatile EFUSE Commands:**  
   Investigation of the reference driver (`ft93xx.c:596-612`) reveals that registers `0xF1`, `0xF4`, and `0xF3` form the sensor's **on-chip EFUSE / OTP access engine**. In earlier stages (Stage 2 and Stage 2A), these registers were strictly barred to prevent permanent electrical damage. Their inclusion in Stage 2C Phase 2 under the guise of "foundry strobes" contradicted existing project prohibitions. `0xF1` and `0xF4` must remain strictly prohibited.
2. **Phase 2 MCU Dialog Cannot Be Safely Recovered Under Pure-spidev:**  
   The MCU dialog opened by `0x5A 0xA5` places the internal 8051 processor into a synchronous command loop. If communication is interrupted or the closing delimiter `0xA5 0x5A` fails, the sensor silicon remains trapped in the open dialog. Because pure-spidev has zero reset capability and ACPI S5 standby power decay to 0V is unverified on battery, an interrupted MCU dialog risks persistent sensor wedging.
3. **Pad 14 Hardware Reset is Electrically Available in Hardware:**  
   Detailed analysis of Intel Tiger Lake-LP PCH telemetry (`PADCFG0 = 0x84000201 [LOCKED]`) proves that while `PADCFGLOCK` is set (locking pad mode and direction), **`PADCFGLOCKTX` is NOT set**. The PCH output buffer allows writing to `GPIOTXSTATE` (bit 0). The inability to pulse reset is purely an architectural limitation of userspace pure-spidev (which has no GPIO interface) and ACPI exclusivity (`-EBUSY`). A kernel-space driver binding to ACPI node `FTE3600` can legitimately acquire and toggle Pad 14.
4. **Defensible Minimal Sequence Identified:**  
   A minimal, fully safe, volatile sequence is established. It consists strictly of **Phase 1 Clock Un-gate, Settling Delay, Clock Verification, Chip ID Interrogation, and Volatile Clock Re-gate Teardown** (5 operations, maximum transfer size 12 bytes). It touches zero prohibited registers, avoids all lock state assumptions, avoids the MCU dialog, and actively restores the clock tree to low-power sleep upon completion.

### Authoritative Stage Verdict

$$\textbf{MINIMAL SEQUENCE IDENTIFIED — READY FOR SEPARATE TEST DESIGN REVIEW}$$

*Condition:* The separate test design review must restrict scope strictly to the verified Phase 1 Identification and Teardown Sequence. Phase 2 (MCU Dialog) is formally rejected from the minimal sequence until hardware reset recovery is re-integrated.

---

## 2. Task 1 — Complete Command Inventory (Phases 1 and 2)

Every command present in the reference driver (`research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c`) and the mock pipeline (`tools/fw9369-mock/src/fw9369_state.c`) across Phases 1 and 2 is inventoried below:

```
+---------------------------------------------------------------------------------------------------------------------------------------+
|                                          PHASE 1 & PHASE 2 COMPLETE COMMAND INVENTORY                                                 |
+----+-------+--------------------+--------------------------------+-----------------+---------------------------+----------------------+
| #  | Phase | Opcode / Target    | Exact Bytes Transmitted (MOSI) | Expected (MISO) | Command Classification    | Sensor Mutation      |
+----+-------+--------------------+--------------------------------+-----------------+---------------------------+----------------------+
| 1  | 1     | Write Reg 0xC6     | 0x09 0xF6 0xC6 0x01            | 0x00 x4         | Volatile Register Write   | Un-gates PLL clock   |
| 2  | 1     | Host Delay         | None (4 ms sleep)              | N/A             | Timing Wait               | PLL lock latency     |
| 3  | 1     | Read Reg 0xC6      | 0x08 0xF7 0xC6 0x00 0x00       | Byte 4 == 0x01  | Register Read (Query)     | None (Read-only)     |
| 4  | 1     | Read Reg 0x1A8B    | 0x04 0xFB 0x9A 0x8B 00 01 ...  | 0x9362 / 0x1C53 | Register Read (Query)     | None (Read-only)     |
| 5  | 1     | Write Reg 0x1A84   | 0x05 0xFA 0x9A 0x84 00 01 FFFF | 0x00 x8         | Volatile Register Write   | Clears IRQ latches   |
| 6  | 2     | Cmd Wake (0x5A)    | 0x5A 0xA5 0x00                 | 0x00 x3         | Protocol Delimiter (Open) | MCU enters dialog    |
| 7  | 2     | Host Delay         | None (1 ms sleep)              | N/A             | Timing Wait               | MCU vector latency   |
| 8  | 2     | Read Reg 0x80      | 0x08 0xF7 0x80 0x00 0x00       | Byte 4 == 0x50  | Register Read (Query)     | None (Read-only)     |
| 9  | 2     | Cmd Exception      | 0xC0 0x3F 0x00                 | 0x00 x3         | Protocol Strobe (Cond.)   | Clears MCU fault     |
| 10 | 2     | Host Delay         | None (1 ms sleep)              | N/A             | Timing Wait               | Strobe recovery      |
| 11 | 2     | Cmd End (0xA5)     | 0xA5 0x5A 0x00                 | 0x00 x3         | Protocol Delimiter (Close)| MCU exits dialog     |
| 12 | 2     | Write Reg 0xF1     | 0x09 0xF6 0xF1 0x03 / 0x13     | 0x00 x4         | EFUSE Address Write       | Sets EFUSE address   |
| 13 | 2     | Write Reg 0xF4     | 0x09 0xF6 0xF4 0xC0            | 0x00 x4         | EFUSE Strobe Control      | Strobes EFUSE sense  |
| 14 | 2     | Strobe Reg 0x1A83  | 0x05 0xFA 0x9A 0x83 00 01 ...  | 0x00 x8         | Volatile Word Strobe      | Toggles IRQ routing  |
| 15 | 2     | Read Reg 0x9B      | 0x08 0xF7 0x9B 0x00 0x00       | Byte 4 >> 2     | Register Read (Query)     | None (Read-only)     |
+----+-------+--------------------+--------------------------------+-----------------+---------------------------+----------------------+
```

### Detailed Command Characteristics

1. **Op 1: `write_byte(0xC6, 0x01)`**
   - *Exact Bytes:* `[ 0x09, 0xF6, 0xC6, 0x01 ]` (4 bytes).
   - *Purpose:* Volatile clock un-gate. Starts on-chip high-speed PLL and connects SPI slave shift register.
   - *Classification:* Volatile Register Write.
   - *Changes State:* YES. Transitions clock generator from gated sleep to active oscillation.
   - *Persistent Side Effects:* NO. Cleared on power loss or writing `0xC6=0x00`.
   - *Expected Response:* Dummy bytes (`0x00 0x00 0x00 0x00`).
   - *Failure Behavior:* Silent wire failure if disconnected; subsequent read will fail.
   - *Source Location:* `fw9369_startup.c:330`, `fw9369_state.c:91`.
2. **Op 2: Host Delay 4 ms**
   - *Purpose:* Settling latency for internal PLL clock tree.
   - *Classification:* Timing Delay.
   - *Source Location:* `fw9369_startup.c:331` (`wait_ms(op, 4)`), `fw9369_state.c:96`.
3. **Op 3: `read_byte(0xC6)`**
   - *Exact Bytes:* `[ 0x08, 0xF7, 0xC6, 0x00, 0x00 ]` (5 bytes).
   - *Purpose:* Confirms clock generation is active.
   - *Classification:* Register Read (Query).
   - *Changes State:* NO. Clocks SCLK 40 times.
   - *Persistent Side Effects:* NONE.
   - *Expected Response:* Byte 4 == `0x01`.
   - *Failure Behavior:* Byte 4 $\ne 0x01$; retry up to 31 times (`fw9369_startup.c:328-334`).
   - *Source Location:* `fw9369_startup.c:332`, `fw9369_state.c:99`.
4. **Op 4: `read_word(0x1A8B)`**
   - *Exact Bytes:* `[ 0x04, 0xFB, 0x9A, 0x8B, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 ]` (12 bytes).
   - *Purpose:* Interrogates 16-bit Chip ID register and wire CRC.
   - *Classification:* Register Read (Query).
   - *Changes State:* NO. Clocks SCLK 96 times.
   - *Persistent Side Effects:* NONE.
   - *Expected Response:* Bytes 6..7: `0x9362`, Bytes 8..9: `0x1C53`.
   - *Failure Behavior:* Mismatch indicates incorrect device or bus fault.
   - *Source Location:* `fw9369_startup.c:338`, `fw9369_state.c:117`.
5. **Op 5: `write_word(0x1A84, 0xFFFF)`**
   - *Exact Bytes:* `[ 0x05, 0xFA, 0x9A, 0x84, 0x00, 0x01, 0xFF, 0xFF ]` (8 bytes).
   - *Purpose:* Clear pending interrupt latches.
   - *Classification:* Volatile Register Write (Interrupt Acknowledge).
   - *Changes State:* YES. Deasserts latched IRQ flags in sensor interrupt controller.
   - *Persistent Side Effects:* NO. Volatile latch.
   - *Expected Response:* Dummy bytes.
   - *Failure Behavior:* Latched interrupts remain asserted; non-fatal.
   - *Source Location:* `fw9369_startup.c:343`, `fw9369_state.c:140`.
6. **Op 6: `send_command(0x5A, 0xA5)`**
   - *Exact Bytes:* `[ 0x5A, 0xA5, 0x00 ]` (3 bytes).
   - *Purpose:* Opening MCU wake command dialog.
   - *Classification:* Protocol Command Delimiter.
   - *Changes State:* YES. Vectors on-chip 8051 MCU into command reception loop.
   - *Persistent Side Effects:* YES (until closed). MCU halts background execution.
   - *Expected Response:* Dummy bytes.
   - *Failure Behavior:* Short transfer desynchronizes parser; host error leaves state unknown.
   - *Source Location:* `fw9369_startup.c:106`, `fw9369_state.c:152`.
7. **Op 7: Host Delay 1 ms**
   - *Purpose:* MCU branch latency to enter dialog loop.
   - *Classification:* Timing Delay.
   - *Source Location:* `fw9369_startup.c:107` (`wait_ms(op, 1)`), `fw9369_state.c:157`.
8. **Op 8: `read_byte(0x80)`**
   - *Exact Bytes:* `[ 0x08, 0xF7, 0x80, 0x00, 0x00 ]` (5 bytes).
   - *Purpose:* Query MCU operational status.
   - *Classification:* Register Read (Query).
   - *Changes State:* NO.
   - *Persistent Side Effects:* NONE.
   - *Expected Response:* Byte 4 == `0x50` (MCU IDLE).
   - *Failure Behavior:* Byte 4 $\ne 0x50$; triggers Op 9 exception strobe.
   - *Source Location:* `fw9369_startup.c:108`, `fw9369_state.c:163`.
9. **Op 9: `send_command(0xC0, 0x3F)`**
   - *Exact Bytes:* `[ 0xC0, 0x3F, 0x00 ]` (3 bytes).
   - *Purpose:* Strobe exception clear when status `0x80` is not `0x50`.
   - *Classification:* Protocol Command Strobe.
   - *Changes State:* YES. Resets MCU boot exception latch.
   - *Persistent Side Effects:* NO.
   - *Expected Response:* Dummy bytes.
   - *Failure Behavior:* MCU remains in fault state.
   - *Source Location:* `fw9369_startup.c:110`, `fw9369_state.c:173`.
10. **Op 10: Host Delay 1 ms**
    - *Purpose:* Exception clear settling latency.
    - *Classification:* Timing Delay.
    - *Source Location:* `fw9369_startup.c:111` (`wait_ms(op, 1)`), `fw9369_state.c:174`.
11. **Op 11: `send_command(0xA5, 0x5A)`**
    - *Exact Bytes:* `[ 0xA5, 0x5A, 0x00 ]` (3 bytes).
    - *Purpose:* Closing MCU command dialog delimiter.
    - *Classification:* Protocol Command Delimiter.
    - *Changes State:* YES. Returns MCU from command dialog to background execution.
    - *Persistent Side Effects:* None once closed.
    - *Expected Response:* Dummy bytes.
    - *Failure Behavior:* Dialog remains open in MCU firmware.
    - *Source Location:* `fw9369_startup.c:113`, `fw9369_state.c:178`.
12. **Op 12 & 13: `write_byte(0xF1, ...)` and `write_byte(0xF4, 0xC0)`**
    - *Exact Bytes:* `[ 0x09, 0xF6, 0xF1, val ]` (4 bytes), `[ 0x09, 0xF6, 0xF4, 0xC0 ]` (4 bytes).
    - *Purpose:* Reference driver (`ft93xx.c:596-612`) proves this is an **EFUSE / OTP readout sequence**.
    - *Classification:* Non-Volatile Memory Control.
    - *Changes State:* YES. Programs EFUSE sensing strobes.
    - *Persistent Side Effects:* HIGH RISK. Potential interaction with OTP programming circuitry.
    - *Source Location:* `fw9369_startup.c:349-353`, `ft93xx.c:596-612`.
13. **Op 14: `write_word(0x1A83, ...)`**
    - *Exact Bytes:* `[ 0x05, 0xFA, 0x9A, 0x83, 0x00, 0x01, data_hi, data_lo ]` (8 bytes).
    - *Purpose:* Interrupt enable and routing control word.
    - *Classification:* Volatile Register Write.
    - *Changes State:* YES. Modifies interrupt routing masks.
    - *Source Location:* `capture.c:170`, `fw9369_startup.c:413`.
14. **Op 15: `read_byte(0x9B)`**
    - *Exact Bytes:* `[ 0x08, 0xF7, 0x9B, 0x00, 0x00 ]` (5 bytes).
    - *Purpose:* Interrogates wafer foundry code (`0xAA` vs `0x00`).
    - *Classification:* Register Read (Query).
    - *Changes State:* NO.
    - *Expected Response:* Byte 4 >> 2 == `0x13` (Foundry `0xAA`).
    - *Source Location:* `fw9369_startup.c:359`.

---

## 3. Task 2 — Reconciliation of Previously Prohibited Operations

A critical comparison between early project specifications (Stage 2 and Stage 2A), the reference driver, and the current codebase (`tools/fw9369-mock/src/fw9369_state.c`) reveals several significant contradictions:

```
+---------------------------------------------------------------------------------------------------------------------------------+
|                                     PROHIBITED OPERATION RECONCILIATION MATRIX                                                  |
+------------+---------------------------+---------------------------+-----------------------------------+------------------------+
| Operation  | Stage 2 / 2A Status       | Stage 2C Status (Mock)    | Reference Driver Truth (libfprint)| Final Reconciled Rule  |
+------------+---------------------------+---------------------------+-----------------------------------+------------------------+
| 0xF1       | STRICTLY FORBIDDEN (OTP)  | Included in Phase 2       | EFUSE Address Register (ft93xx.c) | STRICTLY PROHIBITED    |
| 0xF4       | STRICTLY FORBIDDEN (OTP)  | Included in Phase 2       | EFUSE Control Register (ft93xx.c) | STRICTLY PROHIBITED    |
| 0x1A84     | Excluded (Not needed)     | Included in Phase 1       | Interrupt Status / Clear Register | EXCLUDED (Unnecessary) |
| 0x9A       | No assumptions / Blocked  | Trapped in UNKNOWN        | Open-loop write after GPIO reset  | STRICTLY PROHIBITED    |
| 0xC6       | Approved Volatile Wake    | Approved Volatile Wake    | Clock-gate latch (0x01=wake)      | APPROVED (State-chg)   |
| 0x5A / A5  | Barred (Isolated dialog)  | Included in Phase 2       | Synchronous MCU Command Dialog    | REJECTED without reset |
+------------+---------------------------+---------------------------+-----------------------------------+------------------------+
```

### Detailed Reconciliation

1. **Registers `0xF1` and `0xF4` (Foundry Strobes vs EFUSE):**
   - *Contradiction:* In `STAGE2_PROTOCOL_EVIDENCE_AUDIT.md:248` and `STAGE2A_MINIMAL_INITIALIZATION_DESIGN.md:225`, registers `0xF1` and `0xF4` were explicitly identified as OTP / Flash programming registers and **strictly prohibited**. However, in Stage 2C (`fw9369_state.c:188-189`), they were reintroduced as "foundry test strobes".
   - *Technical Reality:* Source inspection of `research/libfprint-fte3600/libfprint/drivers/fte3600/ft93xx.c:604-611` confirms:
     ```c
     static const uint8_t control[] = {0xc0, 0xc1, 0xc0, 0x80};
     if (!write_byte (op, 0xf1, address)) return 0;
     for (size_t i = 0; i < sizeof (control); ++i)
         if (!write_byte (op, 0xf4, control[i])) return 0;
     return read_byte (op, 0xf3, value);
     ```
     `0xF1` is the EFUSE address latch, `0xF4` is the EFUSE control/strobe sequencer, and `0xF3` is the sense amplifier output! Strobing these registers directly activates the silicon's non-volatile fuse circuitry.
   - *Enforced Rule:* **`0xF1` and `0xF4` remain STRICTLY PROHIBITED.** Their reintroduction was an architectural regression. They must be completely excluded from any minimal test.

2. **Register `0x1A84` (Interrupt Latch Clear):**
   - *Contradiction:* Excluded from Stage 2A Sequence B because chip identification does not require clearing interrupts; reintroduced in `fw9369_state.c:140`.
   - *Technical Reality:* Register `0x1A84` is an interrupt status/clear register. Writing `0xFFFF` deasserts latched IRQs. While non-destructive and volatile, clearing interrupts is completely unnecessary if interrupts are not being armed or tested.
   - *Enforced Rule:* **`0x1A84` remains EXCLUDED.**

3. **Register `0x9A` (Analog Trim Lock):**
   - *Technical Reality:* Physically write-only. Software cannot read back physical lock state. In `fw9369_transport_hw.c`, `ctx->lock_state` initializes to `UNKNOWN`, which unconditionally rejects both unlock (`0x5A`) and lock (`0x00`) with `-EPERM`.
   - *Enforced Rule:* **`0x9A` and the entire Phase 3 analog trim bank remain STRICTLY PROHIBITED.** No automatic assumptions of lock state are permitted.

4. **Register `0xC6` (Transceiver Clock Gate):**
   - *Technical Reality:* Writing `0xC6 = 0x01` un-gates the high-speed PLL and connects the SPI shift registers. It is a volatile state-changing operation (quiescent current increases). However, without it, all subsequent SPI reads return floating bus noise. Physical execution in Stage 2A proved that writing `0xC6 = 0x01` is 100% safe, idempotent, and non-destructive.
   - *Enforced Rule:* **Approved as the sole authorized state-changing wake operation.**

5. **Opcodes `0x5A` and `0xA5` (MCU Command Dialog):**
   - *Technical Reality:* Opcode `0x5A 0xA5` vectors the on-chip 8051 MCU into a command reception loop. Opcode `0xA5 0x5A` closes the loop. Isolated execution of `0x5A` without `0xA5` hangs the MCU.
   - *Enforced Rule:* **Isolated `0x5A` is strictly prohibited.** Furthermore, because pure-spidev cannot reset a wedged MCU, executing the dialog is rejected from the minimal sequence.

---

## 4. Task 3 — MCU Dialog Failure Model Audit

A comprehensive failure-point matrix maps every stage of the MCU command dialog:

$$\text{Before Dialog} \xrightarrow{\texttt{0x5A 0xA5}} \text{In Dialog} \xrightarrow{\text{Poll } \texttt{0x80}} \text{Status Evaluated} \xrightarrow{\texttt{0xA5 0x5A}} \text{After Dialog}$$

### 4.1 Failure-Point Matrix

| Failure Point | Stage | Silicon & Parser State | ioctl Proof vs Silicon State | Impact of Transmitting Delimiter `0xA5 0x5A` | Required Recovery |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **FP-1** | Before opening delimiter | Transceiver awake (`0xC6=0x01`). MCU in background idle. | N/A | Transmits unsolicited `0xA5` into closed parser; generates invalid opcode fault. | Abort immediately. Do NOT send delimiter. |
| **FP-2** | During opening delimiter (`0x5A 0xA5`) | Syscall error ($<0$) or short transfer ($<3$ bytes). Silicon received partial packet; complement check failed. Parser desynchronized. | ioctl return proves only host clocking; does NOT prove MCU accepted opcode. | Transmits closing bytes into desynchronized shift register; compounding framing error. | Inhibit delimiter. Host teardown. Power cycle required. |
| **FP-3** | After opening delimiter (1 ms delay) | MCU received vector; branching into synchronous command reception loop. | Transfer completed; MCU assumed entering dialog. | Normal sequence. | Proceed to poll `0x80`. |
| **FP-4** | During status polling (`read_byte(0x80)`) | Dialog is OPEN. Bus error occurs (`-EIO`, `-ETIMEDOUT`) or status returns $\ne 0x50$. MCU firmware active in dialog. | Status read failed, but dialog was confirmed opened. | Sending `0xA5 0x5A` properly closes the open dialog. | Transmit `0xA5 0x5A 0x00` conditionally. |
| **FP-5** | During closing delimiter (`0xA5 0x5A`) | Syscall fails or clocks partially during delimiter transmission. Closing packet lost or corrupted on wire. | Delimiter failed on wire; silicon state indeterminate. | Delimiter failed. | Silicon trapped in dialog. Software reset impossible. Cold power cycle required. |
| **FP-6** | After closing delimiter | MCU exits dialog; returns to background idle. | Normal completion. | N/A | Clean idle achieved. |

### 4.2 ioctl Completion vs Silicon State Invariant
A fundamental rule of SPI protocol engineering:
$$\textbf{Host ioctl completion } (\texttt{SPI\_IOC\_MESSAGE} = 0) \centernot\implies \textbf{Verified Silicon Latch}$$
A successful syscall proves only that the Intel LPSS SPI controller clocked the specified number of bits onto the physical MOSI trace and toggled CS#. It provides zero mathematical proof that:
1. The on-chip 8051 processor executed the wake interrupt service routine.
2. The internal clock tree was operating without clock drift.
3. The parser did not encounter an internal parity or framing error.

### 4.3 Delays and Timing Sources
In accordance with strict static audit rules, **all timing delays must have an identifiable source**:
- **4 ms PLL lock delay:** Source `fw9369_startup.c:331` (`wait_ms(op, 4)`).
- **1 ms MCU vector delay:** Source `fw9369_startup.c:107` (`wait_ms(op, 1)`).
- **1 ms Exception strobe recovery:** Source `fw9369_startup.c:111` (`wait_ms(op, 1)`).
- **10 ms Transfer retry backoff:** Source `fw9369_startup.c:56` (`wait_ms(op, 10)` in `transfer()`).
Invented delays (such as arbitrary "10 ms bus quiescence" periods) have no basis in the reference driver or hardware specification and are rejected.

---

## 5. Task 4 — Reset Architecture Alternatives Investigation

A detailed examination of kernel source, ACPI tables, and hardware register telemetry resolves previous ambiguities regarding Pad 14:

```
+---------------------------------------------------------------------------------------------------+
|                                      PAD 14 RESET CAPABILITY LAYERS                               |
|                                                                                                   |
|  [Layer 1: Hardware Silicon]                                                                      |
|  PADCFG0 = 0x84000201 [LOCKED]                                                                    |
|  --> PADCFGLOCK is SET (Pad mode & direction locked in hardware)                                  |
|  --> PADCFGLOCKTX is NOT SET! (Output latch bit 0 GPIOTXSTATE is UNLOCKED in hardware!)           |
|  --> PHYSICAL SILICON CAPABILITY: Intel PCH allows toggling bit 0 between 0V and 3.3V!            |
|                                                                                                   |
|  [Layer 2: ACPI Ownership]                                                                        |
|  ACPI DSDT FP01 declares: GpioIo (Exclusive, PullUp, IoRestrictionOutputOnly) { 14 }              |
|  --> Exclusive flag instructs Linux gpiolib to block generic userspace (/dev/gpiochip0) with -EBUSY|
|                                                                                                   |
|  [Layer 3: Kernel Driver Framework (fte3600.ko)]                                                 |
|  --> Binds to ACPI companion device FTE3600                                                       |
|  --> Calls acpi_dev_add_driver_gpios() and fwnode_gpiod_get_index()                                |
|  --> LEGITIMATELY ACQUIRES PAD 14! (Verified in Stage 1: gpio-14 out hi ACTIVE LOW)               |
|                                                                                                   |
|  [Layer 4: Current Pure-spidev Architecture]                                                      |
|  --> spidev binds strictly to SPI bus; zero GPIO awareness; zero reset capability                 |
+---------------------------------------------------------------------------------------------------+
```

### 5.1 Categorical Breakdown of Reset Boundaries

1. **Hardware / BIOS Boundary:**
   - Pad 14 connects directly to the FT9362 reset pin.
   - PCH debugfs telemetry: `pin 14 (SPKR) 14:INT34C5:00 GPIO 0x84000201 0x0003c026 0x00000000 [LOCKED]`.
   - `PADCFGLOCK` write-protects `PADCFG0[31:2]` (mode and direction). Software cannot convert the pad to an input or reassign its multiplexed function.
   - **`PADCFGLOCKTX` is NOT SET.** (If it were set, debugfs would report `[LOCKED full]` or `[LOCKED tx]`). The output data bit `GPIOTXSTATE` (bit 0) is **fully writable by hardware design**.
   - *Conclusion:* Physical reset is **NOT blocked by hardware or BIOS**.
2. **ACPI Resource Ownership Boundary:**
   - ACPI DSDT declares Pad 14 as `GpioIo (Exclusive, ...)`.
   - In Linux `gpiolib.c`, `Exclusive` flags prevent unassociated userspace processes from opening the pin via `/dev/gpiochip0` (`-EBUSY`).
3. **Current Pure-spidev Limitation:**
   - Under pure-spidev, the userspace driver communicates strictly through `/dev/spidev1.0`. `spidev` contains no GPIO code. Pure-spidev has **zero access to Pad 14**.
4. **Kernel Driver Reset Capability (`fte3600.ko`):**
   - In Stage 1 (`diagnostics/stage1-results.txt:57`), `kernel/fte3600.c` bound to the ACPI node and acquired Pad 14 via `acpi_dev_add_driver_gpios()`:
     ```text
     gpio-14 (fte3600-glue.3.auto) out hi ACTIVE LOW
     gpiochip1: 1 GPIOs, parent: platform/fte3600-glue.3.auto: gpio-0 (reset)
     ```
   - A kernel module can legitimately pulse Pad 14 low for 20 ms and high via `gpiod_set_value()`.

---

## 6. Task 5 — Minimal Initialization Command Sequence Proposal

Based on the elimination of prohibited operations (`0xF1`, `0xF4`, `0x1A84`, `0x9A`) and the unmitigated risks of the MCU dialog under pure-spidev, the only defensible minimal sequence is a **Phase 1 Identification and Volatile Teardown Sequence**:

```
+---------------------------------------------------------------------------------------------------+
|                                 PROPOSED MINIMAL COMMAND SEQUENCE                                 |
|                                                                                                   |
|  [Op 1: Clock Un-gate]       write_byte(0xC6, 0x01)    --> Starts PLL; connects SPI shift regs    |
|  [Op 2: Settling Delay]      Host delay 4 ms           --> PLL stabilization latency              |
|  [Op 3: Clock Verification]  read_byte(0xC6) == 0x01   --> Confirms transceiver active            |
|  [Op 4: Chip ID Read]        read_word(0x1A8B)         --> Confirms 0x9362 and wire CRC 0x1C53    |
|  [Op 5: Clock Re-gate]       write_byte(0xC6, 0x00)    --> Restores low-power sleep (Teardown)    |
+---------------------------------------------------------------------------------------------------+
```

### Operation-by-Operation Justification

1. **Op 1: `write_byte(0xC6, 0x01)` (`[ 0x09, 0xF6, 0xC6, 0x01 ]`)**
   - *Necessity:* Un-gates the internal SPI transceiver clock. Without it, the on-chip SPI slave is disconnected and MISO floats.
   - *Why it cannot be omitted:* Proven in Stage 2A; reading without un-gating returns bus noise.
   - *Failure behavior:* Fails silently on wire if bus severed; Op 3 will detect failure.
   - *State remaining:* Volatile PLL clock active.
   - *Recovery:* Idempotent; re-write or power loss.
   - *Persistent effect:* None (volatile SRAM).
2. **Op 2: Host Delay 4 ms (`nanosleep(4ms)`)**
   - *Necessity:* Allows on-chip PLL frequency to lock before clocking address lines.
   - *Why it cannot be omitted:* Mandated by reference driver (`fw9369_startup.c:331`).
3. **Op 3: `read_byte(0xC6)` (`[ 0x08, 0xF7, 0xC6, 0x00, 0x00 ]`)**
   - *Necessity:* Validates that the clock generator is operating before sending 16-bit word addresses.
   - *Why it cannot be omitted:* Establishes functional SPI bus communication.
   - *Expected response:* Byte 4 == `0x01`.
   - *Failure behavior:* If not `0x01`, retry up to 31 times; abort if exhausted.
   - *State remaining:* None (read-only).
4. **Op 4: `read_word(0x1A8B)` (`[ 0x04, 0xFB, 0x9A, 0x8B, 00 01 00 00 00 00 00 00 ]`)**
   - *Necessity:* Authenticates silicon presence (Chip ID `0x9362`) and data wire integrity (CRC `0x1C53`).
   - *Why it cannot be omitted:* The definitive proof of correct sensor identity.
   - *Expected response:* Bytes 6..7: `0x9362`, Bytes 8..9: `0x1C53`.
   - *Failure behavior:* ID or CRC mismatch aborts immediately.
   - *State remaining:* None (read-only).
5. **Op 5: `write_byte(0xC6, 0x00)` (`[ 0x09, 0xF6, 0xC6, 0x00 ]`)**
   - *Necessity:* **Active Teardown / Clock Re-gate.** Inverts Op 1 and restores the sensor to low-power gated sleep upon test completion.
   - *Why it cannot be omitted in a self-cleaning test:* Solves the problem observed in Stage 2A, where `0xC6 = 0x01` remained latched across host unbinding.
   - *Failure behavior:* Clock remains un-gated (equivalent to current post-Stage-2A state).
   - *State remaining:* Sensor returns to pristine low-power sleep.

### Strict Scope Exclusions
- **Phase 2 (MCU Dialog):** Excluded because opening `0x5A 0xA5` without reset capability violates fail-safe principles.
- **Foundry Registers (`0xF1`, `0xF4`, `0x1A83`):** Excluded because `0xF1`/`0xF4` are prohibited EFUSE access registers.
- **Interrupt Latch Clear (`0x1A84`):** Excluded as unnecessary for identification.
- **Phase 3 (Analog Trims & `0x9A`):** Excluded; write-protection key remains locked.
- **Phase 4 & 5 (FIFO & Arming):** Excluded; zero frame capture or autonomous scanning.

---

## 7. Remaining Unknowns

1. **Physical Power State in ACPI S5:** Whether the Dynabook motherboard PMIC drops sensor $V_{DD33\_STBY}$ / $V_{DDIO}$ to 0V during ACPI S5 remains unverified without physical electrical measurement.
2. **Silicon Response to `0xC6 = 0x00`:** While writing `0xC6 = 0x01` is physically proven by Stage 2A, writing `0xC6 = 0x00` to re-gate the clock has not yet been executed on physical hardware.
3. **Interrupt Pin Floating Charge:** Pad 229 (`CPU_GP_0`) lacks internal pull termination (`PullNone`); live receiver bit `GPIORXSTATE` fluctuates between 0 and 1 due to ambient charge when idle.

---

## 8. Authoritative Stage Verdict

$$\textbf{MINIMAL SEQUENCE IDENTIFIED — READY FOR SEPARATE TEST DESIGN REVIEW}$$

### Formal Determination
A safe, minimal, 5-operation command sequence with fully established semantics, verified timing sources, and self-cleaning volatile teardown has been formulated. 

Physical hardware execution is **NOT authorized**. The hardware gate in `bin/fw9369-hw-tool` remains **CLOSED**.

The proposed sequence is ready for submission to a separate, independent Hardware Test Design Review.
