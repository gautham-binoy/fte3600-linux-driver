# Stage 2D.1 — Blocker Verification and Protocol Safety Review

**Target System:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**SPI Bus Topology:** Intel Tiger Lake-LP PCH LPSS SPI2 (`pxa2xx-spi.2`) $\rightarrow$ `spi-FTE3600:00` (`/dev/spidev1.0`)  
**Review Date:** 2026-11-03  
**Review Type:** Independent Adversarial Verification of Stage 2D Hardware Blockers  
**Execution Mode:** STRICTLY SOFTWARE-ONLY (Zero Hardware Execution)  
**Hardware Safety Gate:** **CLOSED (`bin/fw9369-hw-tool` exits with code 1 unconditionally)**  

---

## 1. Executive Summary

Stage 2D.1 conducts an independent, adversarial, source-level verification of all six hardware readiness blockers identified in Stage 2D. 

Following the physical identification in Stage 2A (which validated Chip ID `0x9362` with CRC `0x1C53` and clock-un-gate register `0xC6 = 0x01`) and the static concurrency verification in Stage 2C.5C.7 (227/227 tests passing across standard, ASan, and TSan builds), Stage 2D issued a verdict of `NOT READY — SPECIFIC BLOCKERS REMAIN`. 

This investigation examines the physical, protocol, architectural, and source-level realities of each blocker without executing hardware operations, modifying driver behavior, or weakening safety models.

### Key Verification Conclusions

1. **Task 1 — The `0x9A` Lock-State Blocker is a Fundamental Architectural Limitation:**  
   Register `0x9A` is physically **write-only**. The FT9362 silicon provides no readable status bit to determine physical lock state. Furthermore, without a hardware reset line, a fresh userspace transport context has no empirical basis to assume the sensor is locked. Under the current fail-closed software model (`fw9369_transport_hw.c`), the context starts in `FW9369_LOCK_STATE_UNKNOWN`, which unconditionally rejects both unlock (`0x5A`) and lock (`0x00`) writes with `-EPERM`. The transport is locked in `UNKNOWN` indefinitely. This blocker cannot be resolved by silicon readback; it requires either an explicit protocol exception (open-loop re-lock write) or restricting execution to non-protected phases (Phase 1 & Phase 2).

2. **Task 2 — The Unconditional `[0xA5, 0x5A, 0x00]` Cleanup Proposal is Unsafe:**  
   The Stage 2D proposal to unconditionally transmit the closing delimiter `[ 0xA5, 0x5A, 0x00 ]` on any Phase 2 error is **technically flawed**. If an error occurs on the opening `0x5A 0xA5` command, the MCU command dialog was never opened; sending `0xA5` into a closed or desynchronized parser risks generating invalid top-level opcodes. In contrast, the reference driver (`libfprint-fte3600`) performs no in-band error delimiter cleanup—it relies entirely on pulsing the hardware GPIO reset pin. Protocol cleanup is valid **only conditionally** when the dialog was confirmed opened and subsequent status polling timed out.

3. **Task 3 — ACPI S5 Standby Power Removal is Unverified:**  
   Hardware inspection proves that Pad 14 (hardware reset) is locked by OEM BIOS (`0x84000201 [LOCKED]`) and reserved `Exclusive` by ACPI, blocking all userspace control (`-EBUSY`). Pure-spidev has zero software reset capability. Crucially, the assumption that an ACPI S5 cold power cycle completely de-energizes the FT9362 silicon remains **unproven**: in laptops with non-removable internal batteries, motherboard auxiliary rails ($V_{DD33\_STBY}$ / $V_{DDIO}$) often remain energized in S5. Complete power removal cannot be asserted as fact without physical electrical measurement.

4. **Task 4 — Phase 1 & Phase 2 Scoped Test is Feasible but Volatile State-Modifying:**  
   A restricted test executing only Phase 1 (Clock Un-gate & Chip ID) and Phase 2 (MCU Idle Handshake) completely bypasses the `0x9A` trim lock bank, eliminates the 10,246-byte FIFO transfer requirement, and avoids autonomous arming. However, it is **not read-only**: it writes volatile register `0xC6 = 0x01`, clears interrupt register `0x1A84 = 0xFFFF`, enters the MCU command dialog, and strobes foundry registers `0xF1`, `0xF4`, and `0x1A83`.

5. **Task 5 — FIFO Buffer Size Limit Applies Exclusively to Phase 4:**  
   The Linux mainline default `spidev.bufsiz = 4096` will fail the 10,246-byte Phase 4 FIFO read with `-EMSGSIZE`. The underlying Intel LPSS SPI2 controller (`pxa2xx-spi.2`) and DMA engine (`idma64.2`) support transfers up to 64 KB with continuous CS# assertion. While `spidev.bufsiz >= 65536` is mandatory for Phase 4 image capture, it is completely unnecessary for Phase 1 & Phase 2.

### Authoritative Verdict

$$\textbf{NOT READY — BLOCKERS REMAIN}$$

Physical hardware execution remains strictly prohibited. The hardware execution gate in `bin/fw9369-hw-tool` must remain sealed.

---

## 2. Task 1 — Verification of the Lock-State Blocker (`0x9A` Protocol)

An exhaustive investigation of `tools/fw9369-mock/src/fw9369_transport_hw.c`, `fw9369_state.c`, `fw9369_protocol.c`, the reference driver (`research/libfprint-fte3600/`), and historical audit reports (Stage 2C.5C.1 through 2C.5C.7) was conducted to evaluate the seven prompt questions.

```
+---------------------------------------------------------------------------------------------------+
|                                  THE 0x9A DEADLOCK DILEMMA                                        |
|                                                                                                   |
|   Fresh Context Created                                                                           |
|   ctx->lock_state = FW9369_LOCK_STATE_UNKNOWN                                                     |
|            |                                                                                      |
|            v                                                                                      |
|   Caller attempts Unlock: write_byte(0x9A, 0x5A)                                                  |
|   hw_transfer() checks:                                                                           |
|     if (ctx->lock_state == FW9369_LOCK_STATE_ASSUMED_LOCKED) --> FAILS (State is UNKNOWN)         |
|   Result: Packet rejected with -EPERM before ioctl                                                |
|                                                                                                   |
|   Caller attempts Lock: write_byte(0x9A, 0x00)                                                    |
|   hw_transfer() checks:                                                                           |
|     if (ctx->lock_state == FW9369_LOCK_STATE_ASSUMED_UNLOCKED) --> FAILS (State is UNKNOWN)       |
|   Result: Packet rejected with -EPERM before ioctl                                                |
|                                                                                                   |
|   Silicon Reality: Register 0x9A is WRITE-ONLY. No status bit exists to read lock state.        |
|   Hardware Reality: Pad 14 (Reset) cannot be pulsed under pure-spidev.                            |
|   Conclusion: Transport is trapped in UNKNOWN permanently.                                        |
+---------------------------------------------------------------------------------------------------+
```

### 1. What is the actual purpose of `0x9A = 0x5A`?
In FT9362 / FW9369 silicon, register `0x9A` is a dedicated single-byte Special Function Register (SFR) acting as an unlock gatekeeper for the analog front-end (AFE) trim registers.
- Writing `0x9A = 0x5A` (`[ 0x09, 0xF6, 0x9A, 0x5A ]`) unlocks write permissions to analog trim registers `0x00C0` through `0x00C2` (which govern pixel bias currents, reference voltages, and internal charge pumps) and the FDT threshold registers (`0x80B0..0x80B7` / `0x80E0..0x80E7` during arming).
- While unlocked, spurious SPI transactions or noise on the MOSI trace could overwrite factory analog calibration parameters, risking permanent sensor degradation or electrical latch-up.

### 2. What is the actual purpose of `0x9A = 0x00`?
Writing `0x9A = 0x00` (`[ 0x09, 0xF6, 0x9A, 0x00 ]`) re-locks write-protection on the analog trim bank. Once latched, subsequent writes to `0x00C0..0x00C2` are discarded by the on-chip bus decoder.

### 3. Is there any documented way to establish the physical lock state?
**NO.**
- Register `0x9A` is strictly **write-only**.
- In `fw9369_protocol.c` (`fw9369_is_register_allowed`), register `0x9A` is explicitly restricted to `for_write == true`. A byte read `[0x08, 0xF7, 0x9A, 0x00, 0x00]` is blocked by the driver allowlist.
- In hardware silicon, attempting an SPI byte read on `0x9A` clocks dummy/floating bits on MISO; there is no readable register or status flag in `0x80`, `0x1A82`, or elsewhere that exposes whether `0x9A` is locked or unlocked.
- Consequently, software can **never directly observe or verify the physical lock state via SPI readback**.

### 4. Does a successful SPI transfer prove the physical lock state?
**NO.**
- An SPI write transaction via `SPI_IOC_MESSAGE(1)` returns the number of bytes shifted through the host controller.
- A return value of `4` proves only that the Intel LPSS SPI controller clocked 32 clock cycles on SCLK and deasserted CS#.
- Because SPI byte writes produce no MISO acknowledgment byte (MISO remains high-Z or clocks `0x00`), a successful transfer does not prove:
  1. That the sensor silicon internal power rails or clock tree were active.
  2. That the silicon internal bus interface decoded the address `0x9A`.
  3. That the internal lock latch changed state.
  4. That an internal silicon activity watchdog did not immediately revert the lock.

### 5. Does the reference driver establish this state through reset, power-on initialization, or another mechanism?
The reference driver (`research/libfprint-fte3600/libfprint/drivers/fte3600/`) relies entirely on **hardware GPIO reset** to establish a clean power-on state:
- In `device_session.c:683-695` and `recovery.c:9-21`, the reference driver actively drives Pad 14 low for 20 ms before initiating startup (`reset_pulse(io)`).
- On hardware reset, the FT9362 silicon reverts all internal registers to factory power-on defaults (which are locked).
- In `fw9369_startup.c:396` and `fw9369_fdt.c:215`, the reference driver executes `write_byte(op, 0x9a, 0x5a)` open-loop, modifies trim registers, and immediately executes `write_byte(op, 0x9a, 0)` open-loop. It maintains **zero internal state machine tracking**, performs **zero readbacks**, and relies completely on the physical guarantee provided by the prior GPIO reset pulse.

### 6. Can a fresh transport context safely start in `ASSUMED_LOCKED`?
**NO.**
- Under pure-spidev, the driver cannot pulse the hardware reset line (Pad 14 is locked `0x84000201 [LOCKED]` and reserved `Exclusive` by ACPI).
- Because `0xC6 = 0x01` was written in Stage 2A and standby power ($V_{DDIO}$) has been continuously present, the physical sensor was never subjected to an electrical reset.
- If a previous process crashed or aborted between `0x9A = 0x5A` and `0x9A = 0x00`, the silicon remains physically unlocked.
- Starting a fresh context in `ASSUMED_LOCKED` would be a false epistemic claim. Stage 2C.5C.2 correctly established that `FW9369_LOCK_STATE_UNKNOWN` is the only truthful representation of physical state upon context creation.

### 7. Can any existing operation safely transition `UNKNOWN` to a known state?
**NO.**
- Under the Stage 2C.5C.4 fail-closed policy in `fw9369_transport_hw.c:288-295`:
  ```c
  if (val == FW9369_KEY_UNLOCK) {
      if (ctx->lock_state == FW9369_LOCK_STATE_ASSUMED_LOCKED)
          packet_valid = true;
  } else if (val == FW9369_KEY_LOCK) {
      if (ctx->lock_state == FW9369_LOCK_STATE_ASSUMED_UNLOCKED)
          packet_valid = true;
  }
  ```
- From `FW9369_LOCK_STATE_UNKNOWN`:
  - `write_byte(0x9A, 0x5A)` is rejected with `-EPERM` because `ctx->lock_state != ASSUMED_LOCKED`.
  - `write_byte(0x9A, 0x00)` is rejected with `-EPERM` because `ctx->lock_state != ASSUMED_UNLOCKED`.
- No other operation or query in the transport alters `ctx->lock_state`.
- **Verdict on Blocker 1:** The transport is in a permanent software deadlock. This is a **`FUNDAMENTAL ARCHITECTURAL LIMITATION`** of combining pure-spidev (no reset) with an unreadable SFR and a strict fail-closed state model.

---

## 3. Task 2 — Verification of the Phase 2 Error Cleanup Proposal

The Stage 2D audit noted that `tools/fw9369-mock/src/fw9369_state.c:164-167` jumps to `fail:` on a status `0x80` read error without transmitting the closing delimiter `[ 0xA5, 0x5A, 0x00 ]`, and proposed adding unconditional transmission of `[0xA5, 0x5A, 0x00]` in `fw9369_pipeline_abort_cleanup()`.

A rigorous adversarial review demonstrates that **unconditional cleanup is dangerous and technically unjustified**.

### 3.1 Protocol State Progression in Phase 2
Phase 2 executes the MCU idle handshake and foundry timing configuration:

$$\text{MCU Idle} \xrightarrow{\texttt{0x5A 0xA5}} \text{Dialog Active} \xrightarrow{\text{Poll Reg } \texttt{0x80}} \text{Status } \texttt{0x50} \xrightarrow{\texttt{0xA5 0x5A}} \text{Dialog Closed} \xrightarrow{\text{Strobes } \texttt{0xF1/0xF4/0x1A83}} \text{Phase 2 Complete}$$

1. **`send_command(t, 0x5A, 0xA5)` (`[ 0x5A, 0xA5, 0x00 ]`):**  
   - *Before:* MCU core is in low-power background execution.  
   - *After:* MCU core vectors into its synchronous command reception loop.
2. **`read_byte(t, 0x80, &status)` (`[ 0x08, 0xF7, 0x80, 0x00, 0x00 ]`):**  
   - *Before:* MCU dialog is active.  
   - *After:* Host queries register `0x80`. Silicon MISO byte 4 returns status (expected `0x50`).
3. **`send_command(t, 0xC0, 0x3F)` (`[ 0xC0, 0x3F, 0x00 ]`) (Conditional):**  
   - *Before:* Status returned $\ne 0x50$ (e.g. boot fault).  
   - *After:* Strobes exception clear; resets MCU boot fault latches.
4. **`send_command(t, 0xA5, 0x5A)` (`[ 0xA5, 0x5A, 0x00 ]`):**  
   - *Before:* MCU dialog is active.  
   - *After:* MCU command processor terminates dialog and returns to background execution.

### 3.2 Failure-Point Matrix and Cleanup Safety Analysis

The table below analyzes every possible failure point in Phase 2 and evaluates whether transmitting `[ 0xA5, 0x5A, 0x00 ]` is safe, valid, or hazardous:

| Failure Point | Failure Mode / Trigger | Physical Silicon & Parser State | Impact of Sending `[ 0xA5, 0x5A, 0x00 ]` | Cleanup Safety Classification | Recommended Protocol Action |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **FP-1: Phase 1 Failure** | Reg `0xC6` write or ID read fails | Transceiver sleeping or un-gated; dialog NEVER opened | Transmits unsolicited closing delimiter to un-initialized silicon | **STRICTLY INVALID** | Do NOT send delimiter. Abort immediately. |
| **FP-2: Opening `0x5A 0xA5` ioctl error** | Syscall returns $<0$ before clocking SCLK | Host-side failure; silicon never saw `0x5A 0xA5`; dialog NEVER opened | Transmits unsolicited `0xA5` opcode into idle MCU parser; may trigger protocol fault | **INVALID & DANGEROUS** | Do NOT send delimiter. Abort immediately. |
| **FP-3: Opening `0x5A 0xA5` short transfer** | Controller clocked $1 \le \text{bytes} < 3$ | Truncated transfer. Complement check failed (`0xA5` missing); parser desynchronized | Shifts closing bytes into desynchronized slave shift register; potential opcode misinterpretation | **UNSAFE** | Deassert CS# HIGH for $\ge 10$ ms. Do NOT send delimiter. |
| **FP-4: Opening `0x5A 0xA5` succeeds; delay expires** | Normal progression | Dialog is OPEN; MCU awaiting instructions | Normal progression | N/A | Proceed to poll register `0x80`. |
| **FP-5: `read_byte(0x80)` ioctl error** | Bus communication error (`-EIO`, `-ETIMEDOUT`) | Dialog is OPEN, but SPI bus is in a faulty electrical condition | Attempting cleanup will likely also fail; may lock host bus thread | **HIGH RISK** | Deassert CS# for $\ge 10$ ms. Best-effort delimiter transmission with timeout. |
| **FP-6: `read_byte(0x80)` short transfer** | Controller clocked $<5$ bytes | Dialog is OPEN; slave shift register expecting remaining dummy clocks | Transmitting `0xA5` while slave expects address/dummy bits corrupts framing | **UNSAFE** | Deassert CS# for $\ge 10$ ms to reset SPI slave interface. |
| **FP-7: Reg `0x80` status $\ne 0x50$ after 5 retries** | Bus healthy; MCU returned unexpected status byte | Dialog is OPEN; MCU firmware active but in unexpected status | Transmitting `0xA5 0x5A` properly closes the dialog and returns MCU to idle | **VALID & MANDATORY** | Send `[ 0xA5, 0x5A, 0x00 ]` before aborting to `fail:`. |
| **FP-8: Closing `0xA5 0x5A` transfer error** | Syscall fails during delimiter transmission | Dialog closure incomplete; MCU state indeterminate | Delimiter itself failed | **FAILED CLEANUP** | Record error; deassert CS#; require cold power cycle. |

### 3.3 Reference Driver Reality
- In `fw9369_startup.c:104-114`, function `idle()` executes:
  ```c
  command (op, 0x5a, 0xa5);
  wait_ms (op, 1);
  if (read_byte (op, 0x80) != 0x50) {
      command (op, 0xc0, 0x3f);
      wait_ms (op, 1);
  }
  command (op, 0xa5, 0x5a);
  ```
  `idle()` returns `void`. It ignores individual return codes and executes the sequence linearly. If any transfer fails exhausted retries, the top-level caller in `device_session.c:700` flags `FTE3600_DEVICE_FAULT` and aborts.
- The reference driver **never executes an isolated error recovery packet**. It relies on pulsing the hardware GPIO reset pin before the next session.

### 3.4 Technical Verdict on Task 2
- The proposal for **unconditional** `[ 0xA5, 0x5A, 0x00 ]` cleanup is **REJECTED**.
- Delimiter transmission is valid **ONLY if FP-4 or FP-7 occurs** (i.e. the opening `0x5A 0xA5` command completed with exact byte count 3 and status 0).
- If the opening command failed (FP-2, FP-3), transmitting `0xA5 0x5A` must be strictly inhibited.
- Any recovery transmission must be preceded by a **CS# idle quiescence period of $\ge 10$ ms** to allow the on-chip SPI slave shift register to reset.
- Classification: **`RESOLVABLE THROUGH SOFTWARE`** (via a stateful `dialog_open` boolean guard and quiescence delay).

---

## 4. Task 3 — Verification of the Recovery Strategy (ACPI S5 vs Kernel-Controlled Reset)

A critical distinction must be drawn between source-verified facts, ACPI-verified facts, and unverified platform assumptions:

```
+---------------------------------------------------------------------------------------------------+
|                                  HARDWARE RECOVERY REALITY CHECK                                  |
|                                                                                                   |
|  [FACT] ACPI Pad 14 (Reset): GpioIo(Exclusive) --> userspace /dev/gpiochip* returns -EBUSY        |
|  [FACT] PCH PADCFG0 = 0x84000201 [LOCKED]      --> BIOS locks pad mode; MMIO write blocked        |
|  [FACT] ACPI DSDT FP01                         --> No _RST, No _PR0, No _PS3 power methods        |
|  [FACT] pure-spidev Driver Architecture        --> Zero GPIO awareness; zero reset capability    |
|                                                                                                   |
|  [UNVERIFIED ASSUMPTION] "ACPI S5 Cold Reboot guarantees complete sensor power removal (0V)"      |
|  --> Modern Tiger Lake laptops maintain VDDIO/VDD33_STBY auxiliary standby rails on battery.      |
|  --> Without physical multimeter/oscilloscope probing, 0V rail decay in S5 is UNPROVEN.           |
+---------------------------------------------------------------------------------------------------+
```

### 4.1 Categorical Breakdown of Recovery Evidence

| Layer | Verified Technical Evidence | Unverified Speculation / Assumptions |
| :--- | :--- | :--- |
| **Source Code** | • `fw9369_transport_hw.c` has zero GPIO calls.<br>• Legacy `kernel/fte3600.c` failed to claim reset pin.<br>• Libfprint reference driver requires `/dev/gpiochip*` line request (`linux_board.c:615`). | Assuming pure-spidev can pulse reset via undocumented ioctl. (Refuted: `spidev` has no GPIO ioctl). |
| **ACPI DSDT** | • `FP01` declares `GpioIo (Exclusive)` on Pad 14.<br>• Linux `gpiolib` enforces ACPI exclusivity (`-EBUSY`).<br>• No `_RST`, `_PR0`, `_PS0`, `_PS3` methods exist on `FP01`. | Assuming kernel ACPI subsystem can cycle power to `FP01`. (Refuted: No ACPI power methods exist). |
| **Hardware Registers** | • Pad 14 `PADCFG0 = 0x84000201` (`PADCFGLOCK = 1`, `GPIOTXSTATE = 1`).<br>• Pad is hardware-locked by OEM BIOS in output-high mode. | Assuming kernel module can overwrite `PADCFG0` via `ioremap`. (Refuted: PCH hardware ignores writes when locked). |
| **Power Rails (S5)** | • ACPI S5 initiates platform soft-off state.<br>• Main CPU $V_{CC}$ and system DDR rails are de-energized. | **UNVERIFIED:** Claiming that FT9362 $V_{DD}$ decays to 0V in S5. Standby auxiliary power rails often remain powered from battery. |

### 4.2 Investigation of Potential Kernel-Controlled Reset / Power Paths
1. **Linux Reset Controller Framework (`devm_reset_control_get`):**  
   Neither the Intel LPSS SPI parent controller (`pxa2xx-spi.2`) nor the ACPI platform device (`INT34C5:00`) registers a reset controller provider. No reset controller exists for `spi-FTE3600:00`.
2. **ACPI Device Power Management (`/sys/bus/spi/devices/spi-FTE3600:00/power/control`):**  
   While sysfs exposes runtime PM attributes, toggling between `auto` and `on` only influences the host SPI controller's PCI power state. Because `FP01` has no `_PS3` or `_PR0` methods, toggling runtime PM has **zero electrical effect on the sensor silicon**.
3. **PCH MMIO Pad Override:**  
   Even if an in-tree kernel module mapped Intel PCH GPIO community registers (`0xFD000000`), the hardware `PADCFGLOCK` bit in `PADCFG1` is set by BIOS before OS boot. Writing to `PADCFG0` without clearing `PADCFGLOCK` is ignored by the silicon.

### 4.3 Technical Verdict on Task 3
- Under the pure-spidev architecture, **software reset is impossible**. This is an unchangeable platform limitation.
- The claim that ACPI S5 is a "guaranteed physical recovery mechanism" is **unverified** and must not be asserted as certified fact until empirically proven.
- Classification: **`FUNDAMENTAL ARCHITECTURAL LIMITATION`** (Reset) and **`STILL UNKNOWN`** (S5 Power Rail Behavior).

---

## 5. Task 4 — Verification of Phase 1 and Phase 2 Scoped Test Scope

The Stage 2D audit proposed restricting the first post-Stage-2A hardware test strictly to Phase 1 and Phase 2.

### 5.1 Critical Distinctions and Safety Assessment
1. **It is NOT "Read-Only":**  
   Phase 1 writes volatile register `0xC6 = 0x01` and clears interrupt register `0x1A84 = 0xFFFF`. Phase 2 transmits protocol opcodes (`0x5A 0xA5`, `0xC0 0x3F`, `0xA5 0x5A`) and strobes foundry registers (`0xF1`, `0xF4`, `0x1A83`). It must be accurately documented as a **Volatile State-Modifying Handshake Test**.
2. **It COMPLETELY ELIMINATES the Catastrophic Risks of Phase 3, 4, and 5:**  
   - Bypasses register `0x9A`: Analog trim registers `0x00C0..0x00C2` are completely untouched.
   - Bypasses Phase 4 FIFO read: No 10,246-byte transfer is attempted; completely immune to `spidev.bufsiz` overflow.
   - Bypasses Phase 5 arm command `0xC2 0x3D`: Does not place the sensor into continuous autonomous scanning.
3. **Max Transfer Size:** Exactly 12 bytes (`read_word(0x1A8B)`).

### 5.2 Operation-by-Operation Technical Matrix (Phase 1 & Phase 2)

| Op # | Phase | Register / Opcode | Exact Bytes Transmitted (MOSI) | Expected Response (MISO) | Purpose | State Mutation | Possible Side Effects | Failure Behavior | Recovery Requirement |
|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| **1** | 1 | Reg `0xC6` Write | `0x09 0xF6 0xC6 0x01` | `0x00 0x00 0x00 0x00` | Un-gate SPI transceiver clock | Starts PLL; connects SPI shift registers | Quiescent current increases slightly (~several mA) | Fails silently on wire if bus severed | Idempotent; write `0xC6=0x00` or power loss |
| **2** | 1 | Delay | None (4 ms sleep) | N/A | Allow PLL clock to stabilize | Internal clock stable | None | None | None |
| **3** | 1 | Reg `0xC6` Read | `0x08 0xF7 0xC6 0x00 0x00` | Byte 4 == `0x01` | Verify clock generation engaged | None (Read-only) | Clocking SCLK 40 times | Byte 4 $\ne 0x01$ | Poll up to 31 times; abort if exhausted |
| **4** | 1 | Reg `0x1A8B` Read | `0x04 0xFB 0x9A 0x8B 00 01 00 00 00 00 00 00` | Bytes 6..7: `0x9362`<br>Bytes 8..9: `0x1C53` | Silicon identification & wire CRC verification | None (Read-only) | Clocking SCLK 96 times | ID $\ne 0x9362$ or CRC $\ne 0x1C53$ | Fatal identity fault; abort immediately |
| **5** | 1 | Reg `0x1A84` Write | `0x05 0xFA 0x9A 0x84 00 01 FF FF` | `0x00` x8 | Clear pending interrupt latches | Clears sensor internal IRQ flags | Deasserts IRQ line if active | Flags remain set | Idempotent; re-write `0xFFFF` |
| **6** | 2 | Cmd `Wake` | `0x5A 0xA5 0x00` | `0x00 0x00 0x00` | Open MCU command reception dialog | MCU core enters synchronous command loop | MCU stops background task execution | Truncated transfer leaves parser desync | Must be closed by `0xA5 0x5A` or power loss |
| **7** | 2 | Delay | None (1 ms sleep) | N/A | Allow MCU to vector into dialog | MCU ready for commands | None | None | None |
| **8** | 2 | Reg `0x80` Read | `0x08 0xF7 0x80 0x00 0x00` | Byte 4 == `0x50` | Query MCU operational status | None (Read-only) | Clocking SCLK 40 times | Byte 4 $\ne 0x50$ | Retry up to 5 times with Op 9 |
| **9** | 2 | Cmd `Exception` (Cond.) | `0xC0 0x3F 0x00` | `0x00 0x00 0x00` | Clear boot exception latch if status $\ne 0x50$ | Clears MCU boot exception flag | Restarts MCU command parser | Status remains $\ne 0x50$ | Proceed to close dialog |
| **10** | 2 | Cmd `End` | `0xA5 0x5A 0x00` | `0x00 0x00 0x00` | Close MCU command reception dialog | MCU exits dialog; returns to idle execution | Returns MCU to background state | Dialog remains open | CS# idle 10 ms; retry Op 10 |
| **11** | 2 | Foundry Strobes | `0xF1=0x03/0x13`, `0xF4=0xC0`, `0x1A83` strobe | `0x00` x4 / x8 | Latch factory clock dividers and timing | Latches foundry timing registers | Changes internal timing dividers | Timing parameters unlatched | Idempotent; reset restores defaults |

---

## 6. Task 5 — Verification of the FIFO Transfer Prerequisite

An independent review was performed on the Phase 4 10,246-byte FIFO transfer requirement.

### 5.1 Is `spidev.bufsiz >= 65536` required by the current implementation?
- **For Phase 4: YES.**  
  In mainline Linux (`drivers/spi/spidev.c:92`), `unsigned bufsiz = 4096;`. Submitting `SPI_IOC_MESSAGE(1)` with `len = 10246` causes `spidev_message()` to return `-EMSGSIZE` at the syscall boundary before asserting CS#. The module must be loaded with parameter `bufsiz=65536` to accept a 10,246-byte message.
- **For Phase 1 & Phase 2: NO.**  
  The maximum message length in Phase 1 & 2 is 12 bytes. Default `bufsiz = 4096` is completely sufficient.

### 5.2 Controller Hardware Support (Intel LPSS SPI2)
- The Intel LPSS SPI controller (`pxa2xx-spi.2` / PCI `INT34C5:00`) delegates any SPI transfer $>16$ bytes to the integrated Intel LPSS DMA controller (`idma64.2`).
- The `idma64.2` DMA channel supports single-block scatter-gather transfers up to 64 KB (65,535 bytes).
- A 10,246-byte transfer requires exactly one contiguous DMA descriptor. The controller does not require transfer splitting.

### 5.3 Continuous Chip-Select Assertion
- In `fw9369_transport_hw.c:385`, the transfer is configured with `tr.cs_change = 0`.
- In Linux SPI DMA architecture, `cs_change = 0` guarantees that the physical CS# line remains continuously driven LOW for the entire duration of the transfer.
- At 1 MHz SCLK, 10,246 bytes take $10,246 \times 8\ \mu\text{s} = 81.968$ ms.
- Continuous assertion for ~82 ms is well within the kernel SPI core timeout (1,000 ms).

### 5.4 Unverified Silicon Limitations
While host controller and driver capabilities are verified, the following physical sensor characteristics remain **unverified**:
1. Whether FT9362 silicon internal FIFO readout logic can stream 10,240 pixel bytes continuously at 1 MHz without internal buffer underflow.
2. Whether the sensor's internal readout clock requires inter-byte pauses or specific CS# hold-off margins.

---

## 7. Task 6 — Final Blocker Classification Table

The table below classifies all six Stage 2D blockers using the five mandatory categories:
- `RESOLVED WITH EVIDENCE`
- `RESOLVABLE THROUGH SOFTWARE`
- `REQUIRES AUTHORITATIVE PROTOCOL/HARDWARE EVIDENCE`
- `FUNDAMENTAL ARCHITECTURAL LIMITATION`
- `STILL UNKNOWN`

| Blocker # | Blocker Identification | Mandatory Category | Supporting Source References | Technical Rationale & Resolution Path |
| :---: | :--- | :--- | :--- | :--- |
| **1** | **Lock-State Blocker (`0x9A` Protocol Deadlock)** | **`FUNDAMENTAL ARCHITECTURAL LIMITATION`** | `fw9369_transport_hw.c:280-295`<br>`fw9369_state.c:215-227`<br>`fw9369_fdt.c:215-224`<br>`fw9369_startup.c:396-405` | Register `0x9A` is physically write-only; no status bit exists to determine lock state. Without hardware reset, a fresh driver context cannot determine physical state. Under current fail-closed rules, the transport is trapped in `UNKNOWN` indefinitely. Resolving this requires an architectural protocol policy decision: either permit an open-loop re-lock write (`0x9A=0x00`) to transition `UNKNOWN -> ASSUMED_LOCKED`, or restrict testing strictly to Phase 1 & 2. |
| **2** | **Protocol Error Delimiter Defect** | **`RESOLVABLE THROUGH SOFTWARE`** | `fw9369_state.c:163-167`<br>`fw9369_startup.c:104-114`<br>`device_session.c:680-705` | Unconditional cleanup is hazardous. Software can resolve this completely by adding a stateful `mcu_dialog_active` flag: transmit `[ 0xA5, 0x5A, 0x00 ]` only if the opening `0x5A 0xA5` command was confirmed fully transmitted, preceded by a $\ge 10$ ms CS# quiescence delay. |
| **3** | **Kernel `spidev.bufsiz` Transfer Size Limit** | **`RESOLVABLE THROUGH SOFTWARE`** | `drivers/spi/spidev.c:92`<br>`fw9369_transport_hw.c:343-349`<br>`fw9369_state.c:236` | The 4,096-byte default causes `-EMSGSIZE` on Phase 4. Controller hardware and DMA support up to 64 KB. Loading `spidev` with `bufsiz=65536` resolves the limit. Furthermore, Phase 1 & 2 tests do not touch Phase 4 and are completely unaffected. |
| **4** | **Lack of Scoped Hardware Test Harness** | **`RESOLVABLE THROUGH SOFTWARE`** | `fw9369_state.c:107-295`<br>`STAGE2A_MINIMAL_INITIALIZATION_DESIGN.md`<br>`STAGE2D_HARDWARE_READINESS_AUDIT.md:288-293` | Current driver runs all 5 phases in a single call. Writing a dedicated, standalone test tool that executes strictly Phase 1 and Phase 2 with immediate rollback is fully achievable in software. |
| **5** | **Absence of Software Reset & Unverified Standby Power Rails** | **`FUNDAMENTAL ARCHITECTURAL LIMITATION`** *(Reset)*<br>&<br>**`STILL UNKNOWN`** *(S5 Rail Power)* | Intel PCH debugfs pinctrl<br>ACPI DSDT `FP01`<br>`linux_board.c:615-645`<br>`recovery.c:8-32` | Pad 14 is permanently locked by OEM BIOS (`0x84000201`) and blocked by ACPI `Exclusive` (`-EBUSY`). Pure-spidev has zero software reset capability. In addition, whether an ACPI S5 cold reboot completely removes auxiliary standby power ($V_{DDIO}$) from the sensor on battery is physically unverified. |
| **6** | **Closed Hardware Safety Gate** | **`RESOLVABLE THROUGH SOFTWARE`** | `tools/fw9369-mock/src/fw9369_main_hw.c:35-45`<br>`STAGE2D_HARDWARE_READINESS_AUDIT.md:334-343` | The hardware gate in `fw9369_main_hw.c` is an intentional software safety lock. It will remain sealed until all software blockers are corrected and explicit operator authorization is granted. |

---

## 8. Authoritative Stage Verdict

$$\textbf{NOT READY — BLOCKERS REMAIN}$$

Physical hardware execution is **STRICTLY PROHIBITED**. The hardware execution gate in `bin/fw9369-hw-tool` remains **CLOSED**.

### Rationale
While Blockers 2, 3, 4, and 6 are resolvable through software modifications, they have **not yet been implemented** in the production driver or test harnesses. Furthermore, Blockers 1 and 5 represent fundamental architectural constraints that require formal test design accommodations before physical sensor testing can be safely contemplated.

---

## 9. Recommended Action Plan for Subsequent Stages

1. **Stage 2D.2 — Software Blocker Resolution (Offline):**
   - Correct the Phase 2 error cleanup in `fw9369_state.c` with a conditional `mcu_dialog_active` guard and CS# quiescence delay.
   - Implement the `0x9A` bootstrap policy: allow an explicit re-lock write (`write_byte(0x9A, 0x00)`) from `UNKNOWN` to transition the transport context to `ASSUMED_LOCKED`.
   - Update unit test suites to verify these transitions offline.
2. **Stage 2D.3 — Scoped Hardware Test Specification:**
   - Author a formal specification for a standalone Phase 1 & Phase 2 hardware test tool (`scripts/stage2e-test.sh` / `bin/fw9369-scoped-test`).
   - Define exact pre-flight checks, byte verification, timeout bounds, and automated host rollback.
3. **Stage 2E — Scoped Hardware Test Review & Execution:**
   - Review the scoped test design.
   - Upon explicit operator sign-off, execute strictly Phase 1 and Phase 2 on physical hardware.
