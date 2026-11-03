# Stage 2D — Hardware Readiness and Recovery Audit

**Target System:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**SPI Bus Topology:** Intel Tiger Lake-LP PCH LPSS SPI2 (`pxa2xx-spi.2`) $\rightarrow$ `spi-FTE3600:00` (`/dev/spidev1.0`)  
**Audit Date:** 2026-11-03  
**Audit Scope:** Comprehensive Static Analysis of Sensor Recovery, Hardware Pin Control, Initialization Sequence, SPI Transport Readiness, and First Hardware Test Prerequisites  
**Execution Mode:** STRICTLY SOFTWARE-ONLY (Zero Hardware Execution)  
**Hardware Safety Gate:** **CLOSED (`bin/fw9369-hw-tool` exits with code 1 unconditionally)**

---

## 1. Executive Summary

Stage 2D performs an independent, adversarial hardware readiness and recovery audit for the Dynabook Satellite Pro C40-J fingerprint sensor driver project.

Following the successful Stage 2A physical identification (which clocked register `0xC6 = 0x01` and validated Chip ID `0x9362` with CRC `0x1C53`) and the Stage 2C.5C.7 final static concurrency audit (227/227 tests passing under standard, ASan, and TSan builds), this audit evaluates whether the software implementation, driver architecture, and platform environment are sufficiently safe to proceed toward the first post-Stage-2A controlled physical sensor test.

### Key Audit Findings
1. **The Software Implementation is Hardened Offline but Blocked at Runtime:**
   While the userspace mock transport and state machine are mathematically verified and memory-safe offline, the hardware transport implementation contains an **absolute software blocker**: `ctx->lock_state` starts in `UNKNOWN`, which unconditionally rejects protection register `0x9A` unlock commands with `-EPERM`. Under current code, the transport cannot execute Phase 3 of initialization.
2. **Zero Software Reset Capability Under Pure-spidev:**
   The hardware reset line (PCH Pad 14) is locked by BIOS firmware (`0x84000201 [LOCKED]`) and reserved `Exclusive` by ACPI, blocking all userspace GPIO control (`/dev/gpiochip*` returns `-EBUSY`). In pure-spidev mode, the operating system has **zero ability to pulse the reset line**. The sensor cannot be reset via software.
3. **Sensor Silicon Internal State is Physically UNKNOWN:**
   Because volatile register `0xC6 = 0x01` was written in Stage 2A and motherboard auxiliary standby power ($V_{DDIO}$) has been continuously maintained without power-on reset or hardware reset, the physical sensor silicon resides in an indeterminate state.
4. **Kernel Buffer Limit Hazard (`spidev.bufsiz`):**
   The mainline Linux default for `spidev.bufsiz` is 4,096 bytes. The 10,246-byte burst FIFO frame read required by Phase 4 will fail immediately at the syscall boundary with `-EMSGSIZE` unless `spidev` is explicitly reloaded with `bufsiz=65536`.
5. **Protocol Error Delimiter Defect:**
   In `fw9369_state.c`, if a bus error occurs during Phase 2 MCU status polling, the code aborts to `fail:` without transmitting the mandatory closing delimiter `[ 0xA5, 0x5A, 0x00 ]`, leaving the MCU command processor hanging in an active dialog.
6. **Full 5-Phase Pipeline is Irreversible in Software:**
   Once Phase 2 (MCU idle) and Phase 3 (AFE tuning) are executed, the sensor silicon cannot be restored to cold-boot defaults via software commands.

### Authoritative Stage Verdict
**`NOT READY — SPECIFIC BLOCKERS REMAIN`**

Hardware execution remains **STRICTLY PROHIBITED**. Six specific, concrete blockers must be resolved before any physical sensor interaction is authorized.

---

## 2. Current Software and Hardware Knowledge

### 2.1 Software Repository & Commit Baseline
- **Git Commit:** `b9bb1e9` (*Milestone1*, 2026-11-02T22:28:17+05:30).
- **Working Tree State:** Completely clean (`git status` reports 0 unstaged/untracked changes).
- **Driver Source Code:**
  - `kernel/fte3600.c`: Out-of-tree legacy kernel driver (Stage 1). Retained for historical reference; deliberately unused in the pure-spidev architecture.
  - `tools/fw9369-mock/src/fw9369_transport_hw.c`: Hardened Linux SPI transport backend using `PTHREAD_MUTEX_NORMAL`, safe copy-out telemetry getters, and fail-closed allowlists.
  - `tools/fw9369-mock/src/fw9369_protocol.c`: Protocol framing, complement encoding, and CRC verification.
  - `tools/fw9369-mock/src/fw9369_state.c`: 5-phase initialization and arming pipeline engine.
  - `tools/fw9369-mock/src/fw9369_calibration.c`: Capacitive AFE centering, FDT threshold derivation, and image baseline acquisition.
  - `tools/fw9369-mock/src/fw9369_main_hw.c`: Hardware transport tool entry point with fail-closed safety gate (exit code 1).
- **Test Infrastructure:**
  - `bin/fw9369-tests`: 14 test suites, 227 deterministic unit tests.
  - Test suites 1 through 7: Mock silicon mechanics, CRC vectors, calibration algorithms.
  - Test suites 8 through 14: Hardware transport identity, parameter validation, adversarial framing, mutex invariants, and safe copy-out.
- **Hardware Scripts:**
  - `scripts/stage1-test.sh`: Automated kernel module test harness with 12-point static safety verification.
  - `scripts/stage2a-test.sh`: Pure-spidev Stage 2A identification harness with automated rollback.

### 2.2 Historical Physical Execution Record (Stage 2A)
On 2026-11-02T16:04:13+05:30, Stage 2A successfully executed four physical SPI transactions via `/dev/spidev1.0`:
1. **Write `0xC6 = 0x01` (`[ 0x09, 0xF6, 0xC6, 0x01 ]`):** Volatile transceiver un-gate command. CS# asserted, 4 bytes clocked at 1 MHz, CS# deasserted.
2. **Settle Delay:** 4 ms sleep (`nanosleep`).
3. **Read `0xC6` (`[ 0x08, 0xF7, 0xC6, 0x00, 0x00 ]`):** Returned `[ 0x00, 0x00, 0x00, 0x00, 0x01 ]`. Verified that transceiver clock generation engaged.
4. **Read Chip ID `0x1A8B` (`[ 0x04, 0xFB, 0x9A, 0x8B, ... ]`):** Returned raw word `0x9362` and hardware wire CRC `0x1C53`. Mathematically verified against `fw9369_crc_wire([0x93, 0x62])`.
5. **Rollback Execution:** `spidev` unbound from `spi-FTE3600:00`, `driver_override` cleared, `spidev` module unloaded. Verified: `/dev/spidev*` absent, driver unbound.

### 2.3 Categorical Distinction of Current System State

| State Layer | Current Physical Reality | Method of Proof |
| :--- | :--- | :--- |
| **Host Kernel State** | **Pristine, Unbound, Safe:** No `spidev` or `fte3600` modules loaded; `spi-FTE3600:00` completely unbound; no `/dev/spidev*` nodes. | `lsmod`, sysfs inspection |
| **PCH Pin 14 (Reset)** | **Output-High / Firmware-Locked:** `0x84000201 [LOCKED]`. Sensor held continuously out of reset. | Intel debugfs pinctrl |
| **PCH Pin 229 (IRQ)** | **Input High-Z / Edge-Triggered:** `0x82000100 [LOCKED tx]`. Floating `PullNone` trace. | Intel debugfs pinctrl |
| **Sensor Silicon State** | **PHYSICALLY UNKNOWN:** Standby power remained active; `0xC6=0x01` was written; no reset occurred. | Historical telemetry reconciliation |

---

## 3. Sensor Recovery Analysis

### 3.1 Post-Stage-2A Sensor State
During Stage 2A, volatile register `0xC6` was written with `0x01`. No inverse write (`0xC6 = 0x00`) was submitted prior to host unbinding.
- **Standby Power Persistence:** In modern laptops (Dynabook Satellite Pro C40-J), auxiliary standby power rails ($V_{DDIO}$ / $V_{DD33}$) remain energized as long as the battery is connected, even when the system is suspended or warm-rebooted.
- **Absence of Reset:** Because Pad 14 was never pulsed low, the sensor did not experience an electrical power-on reset (POR).
- **Physical Possibilities:**
  1. Sensor retained `0xC6 = 0x01` and remains in high-frequency transceiver standby.
  2. Sensor autonomously powered down the transceiver via an undocumented internal activity watchdog.
  3. Floating SPI chip-select lines caused internal state machine drift.
- **Deductive Conclusion:** The sensor silicon cannot be assumed to be in its pristine reset state. It must be treated as an **indeterminate state machine**.

### 3.2 Persistent Effects of Register `0xC6 = 0x01`
- **Volatile vs. Non-Volatile:** Register `0xC6` is a volatile SRAM control register. It does **not** write to on-chip OTP ROM or EFUSE.
- **Electrical Effect:** Enables the internal high-speed PLL and connects the SPI shift registers. Quiescent current consumption increases slightly (~several mA). It does not activate analog charge pumps or pixel bias generators.
- **Idempotency:** In FocalTech protocol specifications, writing `0xC6 = 0x01` when the transceiver is already active is a safe, idempotent operation. Re-asserting `0xC6 = 0x01` at the beginning of a new session is safe.

### 3.3 Cold Boot vs. Warm Reboot
- **Warm Reboot / OS Restart:** Does **not** de-energize motherboard auxiliary rails. PCH GPIO configurations (including Pad 229) and peripheral standby states persist across warm reboot cycles.
- **ACPI S5 Cold Power Cycle:** Shutting down the laptop completely, disconnecting the AC adapter, and allowing standby rails to decay to 0V (or power-cycling from S5) triggers a true hardware power-on reset (POR) across the entire platform.
- **Assessment:** A cold boot is not merely a conservative preference; **it is the ONLY verified physical recovery mechanism** available on this hardware.

### 3.4 Hardware Reset Line Availability
- **Pad 14 Status:** Physically wired to the FT9362 reset pin. Driven HIGH by Intel PCH hardware (`0x84000201`).
- **Software Access:** Blocked. ACPI DSDT declares `GpioIo (Exclusive, ...)` on Pin 14. Linux `gpiolib` denies userspace access (`/dev/gpiochip*` returns `-EBUSY`).
- **Pure-spidev Architecture:** `spidev` binds only to the SPI bus; it has no GPIO awareness.
- **Alternative Reset Mechanisms:**
  - ACPI `_RST` method: **Does not exist** on `FP01`.
  - ACPI Power Resource (`_PR0`/`_PS3`): **Does not exist** on `FP01`.
  - Linux Reset Controller API: **Not implemented** by `INT34C5:00` or `pxa2xx-spi.2`.
  - In-band SPI soft reset command: **Does not exist** in FT9362 / FW9369 silicon.
- **Conclusion:** Under pure-spidev, the driver has **zero software reset capability**.

### 3.5 SPI Interruption and Packet Parser Recovery
What happens if an SPI transfer fails or is interrupted mid-packet?
- **Electrical Framing:** FT9362 SPI communication uses SPI Mode 0 (CPOL=0, CPHA=0). Every transaction is strictly framed by Chip Select (CS# active-low).
- **CS# Deassertion Mechanics:**
  - Raising CS# to 3.3V (deasserted) terminates the active SPI transfer.
  - In reference driver implementations, CS# deassertion explicitly resets the internal FIFO burst read pointer.
- **Opcode Complement Guard:**
  - FT9362 enforces an inverted complement byte check on every opcode (`0x09`/`0xF6`, `0x08`/`0xF7`, `0x05`/`0xFA`, `0x04`/`0xFB`, `0x06`/`0xF9`).
  - If a packet is interrupted before the complement and address bytes are shifted, the command is discarded by the on-chip parser.
- **Bit-Level Framing Invariant:**
  - The Intel PXA2xx SPI controller always clocks data in exact multiples of 8 bits (`bits_per_word = 8`). Fractional byte shifts (e.g. 7 bits) cannot occur on the physical bus.
- **High-Level Protocol State Hazards:**
  1. **Protection Register 0x9A:** If `0x9A = 0x5A` (unlock) succeeds but subsequent operations fail before `0x9A = 0x00` (lock) is sent, the analog trim bank remains writable.
  2. **MCU Command Dialog:** If `0x5A 0xA5` (wake) is issued but `0xA5 0x5A` (end) is not sent, the MCU command processor hangs awaiting further opcodes.

### 3.6 Re-establishing a Known Protocol State
To safely recover from an interrupted transaction or uncertain silicon state without a hardware reset pin:
1. **Host Bus Quiescence:** Deassert CS# HIGH and hold the SPI bus idle for at least 10 ms.
2. **Clock Tree Resynchronization:** Write `0xC6 = 0x01` and verify readback `0x01`.
3. **Identity Verification:** Read register `0x1A8B` and verify word `0x9362` and wire CRC `0x1C53`.
4. **Mandatory Trim Lock:** Unconditionally transmit `write_byte(0x9A, 0x00)`.
5. **Mandatory Dialog Termination:** Unconditionally transmit `send_command(0xA5, 0x5A)`.
6. **Interrupt Latch Clear:** Write `0x1A84 = 0xFFFF`.

---

## 4. GPIO and Pin Configuration Audit

```
Intel Tiger Lake PCH (INT34C5:00)
├── Pin 14 (GPP_B14 / SPKR) ──[Exclusive ACPI]──> FT9362 Reset Pin (Active-Low)
│   └── PADCFG0 = 0x84000201 [LOCKED] (Output-High, Tx Enabled, Mode Locked)
└── Pin 323 (GPP_D0 / CPU_GP_0) ──[ACPI GpioInt]──< FT9362 IRQ Pin (Active-High)
    └── PADCFG0 = 0x82000100 [LOCKED tx] (Input High-Z, Rx Enabled, Edge Trig)
```

### 4.1 Historical Pin Observations and Current Capabilities

| Pin / Pad | ACPI Signal | Hardware Register State | Software Controllability | Hardware Lock Status | Functional Role |
|---|---|---|---|---|---|
| **Pin 14 (GPP_B14)** | `RSTP` (Reset) | `PADCFG0 = 0x84000201`<br>`PADCFG1 = 0x0003c026` | **UNCONTROLLABLE** by userspace (`-EBUSY`). Cannot be toggled by `spidev`. | `[LOCKED]` (Configuration locked by BIOS; Tx latch unlocked but unreachable) | Physical active-low reset. Held continuously HIGH (3.3V). |
| **Pin 323 (Pad 229)** | `INTP` (IRQ) | `PADCFG0 = 0x82000100`<br>`PADCFG1 = 0x00000029` | **OBSERVABLE** via kernel IRQ subsystem. Passive input only. | `[LOCKED tx]` (Transmitter locked off; cannot drive output) | Physical interrupt line from sensor. Configured for edge trigger. |

### 4.2 Pad 229 Delta Characterization
- **Pre-Stage 1:** `0x80100100` (`RXEVCFG = 00` Level, `GPIROUTIOXAPIC = 1`).
- **Current State:** `0x82000100` (`RXEVCFG = 01` Edge, `GPIROUTIOXAPIC = 0`).
- **Explanation:** During Stage 1, `kernel/fte3600.c` requested the interrupt line via `gpiod_to_irq()`. Linux `pinctrl-intel` configured the pad to match the ACPI DSDT declaration (`GpioInt(Edge, ActiveHigh)`).
- **Line State Bit (Bit 1):** In `PADCFG0`, bit 1 (`GPIORXSTATE`) is a live read-only status bit. Its occasional toggle to `0x82000102` reflects electrostatic charge on an un-driven, `PullNone` trace when the sensor's interrupt output is in high-impedance mode.
- **Safety Verdict:** Pad 229 remains safely in high-impedance input mode (`GPIOTXDIS = 1`). It cannot drive voltage or cause bus contention. It is electrically safe and compatible with future interrupt operations.
- **Restoration Boundary:** Linux `gpiolib` does not revert hardware registers on driver unload. Pad 229 will retain `0x82000100` until an ACPI S5 cold reboot.

---

## 5. Complete Initialization Sequence Audit

The FW9369 reference sequence consists of five discrete phases. Every command is audited below:

```
[Phase 1: Wakeup] ──► [Phase 2: MCU Idle] ──► [Phase 3: FDT Calibration] ──► [Phase 4: Image Baseline] ──► [Phase 5: Arm Finger Detect]
```

### 5.1 Command-by-Command Technical Matrix

| # | Phase | Register / Opcode | Exact Transmitted Bytes (MOSI) | Expected Response (MISO) | Timing | State Mutation | Persistent Side Effect? | Retry Safe? |
|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| **1** | 1 | Reg `0xC6` Write | `0x09 0xF6 0xC6 0x01` | Don't care | 4 ms wait | Un-gates SPI clock | Volatile register; higher standby power | Yes |
| **2** | 1 | Reg `0xC6` Read | `0x08 0xF7 0xC6 0x00 0x00` | Byte 4 == `0x01` | Poll $\le 31 \times$ | Verifies clock | None | Yes |
| **3** | 1 | Reg `0x1A8B` Read | `0x04 0xFB 0x9A 0x8B 00 00 00 00 00 00` | Bytes 6..7: `0x9362`<br>Bytes 8..9: `0x1C53` | Immediate | None (Read-only) | None | Yes |
| **4** | 1 | Reg `0x1A84` Write | `0x05 0xFA 0x9A 0x84 00 01 FF FF` | Don't care | Immediate | Clears pending IRQs | Clears interrupt latches | Yes |
| **5** | 2 | Cmd `Wake` (0x5A) | `0x5A 0xA5 0x00` | Don't care | 1 ms wait | Opens MCU command dialog | MCU enters command reception mode | **NO (Must send 0xA5)** |
| **6** | 2 | Reg `0x80` Read | `0x08 0xF7 0x80 0x00 0x00` | Byte 4 == `0x50` | Poll $\le 5 \times$ | None (Read-only) | None | Yes |
| **7** | 2 | Cmd `Exception` | `0xC0 0x3F 0x00` | Don't care | 1 ms wait | Strobes exception reset | Clears MCU boot ROM fault | Yes |
| **8** | 2 | Cmd `End` (0xA5) | `0xA5 0x5A 0x00` | Don't care | Immediate | Closes MCU command dialog | Returns MCU to idle execution | Yes |
| **9** | 2 | Foundry Strobes | `0xF1=0x03/0x13`, `0xF4=0xC0`, strobe `0x1A83` | Don't care | Immediate | Internal foundry clock setup | Factory timing parameters latched | Yes |
| **10** | 3 | AFE Timing Setup | Regs `0x1800`, `0x1804`, `0x1807`, `0x1808`, `0x1887` | Don't care | Immediate | Biases capacitive analog front-end | Sets charge pump & integration timing | Yes |
| **11** | 3 | Reg `0x9A` Unlock | `0x09 0xF6 0x9A 0x5A` | Don't care | Immediate | **UNLOCKS analog trim bank** | **Exposes trims 0x00C0..0x00C2 to write** | **NO (High Risk)** |
| **12** | 3 | Trim Bank Writes | `0x00C0=0x0444`, `0x00C1=0x0021`, `0x00C2=0x00C0` | Don't care | Immediate | Calibrates analog bias voltage | Latches analog front-end trims | Yes |
| **13** | 3 | Reg `0x9A` Lock | `0x09 0xF6 0x9A 0x00` | Don't care | Immediate | **RELOCKS analog trim bank** | Write-protects trim bank | Yes |
| **14** | 3 | FDT Calibration | Iterative `0x1801` DAC tune, poll `0x1A82` bit 3, read `0x80B8` | Minima in `[300, 700]` | $\le 32$ DAC steps | Tunes capacitive baseline | Stores tuned DAC setting in host RAM | Yes |
| **15** | 4 | Frame Control | Regs `0x1804`, `0x1807`, `0x8E`, `0x90`, `0x91`, `0x92` | Don't care | Immediate | Prepares pixel readout matrix | Configures pixel integration window | Yes |
| **16** | 4 | FIFO Burst Read | `0x06 0xF9 0x9A 0x05 0x14 0x00` + 10,240 dummy bytes | 10,240 pixel bytes (5,120 words) | $\approx 82$ ms | Reads out raw frame buffer | Resets FIFO read pointer to 0 | Yes |
| **17** | 5 | Threshold Writes | Regs `0x80B0`..`0x80B3` write 4 words | Don't care | Immediate | Programs finger detect levels | Stores capacitive touch thresholds | Yes |
| **18** | 5 | Arm Interrupts | Regs `0x1A83 = 0x0016`, `0x1A84 = 0x002F` | Don't care | Immediate | Unmasks touch interrupt | Routes touch comparator to IRQ pin | Yes |
| **19** | 5 | Cmd `Arm` (0xC2) | `0xC2 0x3D 0x00` | Don't care | 1 ms wait | **Arms autonomous scanning** | **Sensor enters continuous scan loop** | **NO (Irreversible)** |

### 5.2 Critical Safety Analysis of High-Risk Operations

#### 1. The `0x5A` / `0xA5` MCU Command Dialog
- **Mechanism:** Command `0x5A 0xA5` places the on-chip 8051-compatible MCU into a synchronous command loop. Command `0xA5 0x5A` terminates the loop.
- **Isolated Execution Hazard:** Submitting `0x5A 0xA5` without `0xA5 0x5A` leaves the MCU command processor waiting indefinitely for subsequent instructions. Normal register reads may return corrupt data or time out.
- **Implementation Defect in `fw9369_state.c`:**
  In lines 163-167 of `tools/fw9369-mock/src/fw9369_state.c`:
  ```c
  ret = read_byte(t, FW9369_REG_STATUS_80, &status);
  if (ret < 0) {
      ctx->last_error = ret;
      goto fail;  // HAZARD: Jumps to fail without sending 0xA5 0x5A!
  }
  ```
  If `read_byte(0x80)` fails on the bus, the code jumps straight to `fail:` without executing line 178 (`send_command(t, 0xA5, 0x5A)`). The sensor is abandoned with its command dialog active!
  **Remediation:** The abort handler `fw9369_pipeline_abort_cleanup()` MUST transmit `0xA5 0x5A` if Phase 2 was entered.

#### 2. Register `0x9A` (Analog Trim Protection Key)
- **Mechanism:** Register `0x9A` write-protects registers `0x00C0` through `0x00C2`. These registers control the internal analog bias currents, reference voltages, and charge pump voltages.
- **Hazard:** Leaving `0x9A == 0x5A` exposes trims to accidental corruption by spurious SPI bus writes, risking permanent analog drift.
- **Architectural Blocker in `fw9369_transport_hw.c`:**
  Under the Stage 2C.5C.4 fail-closed policy, `hw_transfer()` evaluates:
  ```c
  if (val == FW9369_KEY_UNLOCK) {
      if (ctx->lock_state == FW9369_LOCK_STATE_ASSUMED_LOCKED)
          packet_valid = true;
  }
  ```
  Because a freshly created context starts with `ctx->lock_state == FW9369_LOCK_STATE_UNKNOWN`, `hw_transfer()` **rejects the unlock command with `-EPERM`**.
  In production, the transport has no mechanism to transition from `UNKNOWN` to `ASSUMED_LOCKED`. Therefore, **Phase 3 cannot execute on physical hardware**.

#### 3. Command `0xC2 0x3D` (Autonomous Arming)
- **Mechanism:** Transitions the sensor into its autonomous finger detection state machine. The sensor continuously cycles through capacitive FDT channels and drives the physical IRQ pin when a finger touch is detected.
- **Irreversibility:** Once armed, there is no documented SPI software command to disarm the sensor back to idle. The sensor remains in this scanning loop until an interrupt touch event occurs or an electrical reset is applied.

---

## 6. SPI Transport Readiness Audit

### 6.1 Seven-Layer Transport Matrix

| Layer | Component | Specification & Operational Limits | Status | Impact / Finding |
|:---:|:---:|:---:|:---:|:---:|
| **1** | Kernel `spidev.bufsiz` | Mainline Linux default: `4096` bytes. Phase 4 FIFO read: `10246` bytes. | **BLOCKER** | Submitting Phase 4 without `spidev bufsiz=65536` fails with `-EMSGSIZE`. |
| **2** | Syscall Message Framing | `SPI_IOC_MESSAGE(1)`, `cs_change = 0`, `len = 10246`. | **READY** | Guarantees single uninterrupted CS# assertion. |
| **3** | Controller Driver | Intel `pxa2xx-spi.2`: transfers $> 16$ bytes delegate to DMA. | **READY** | Transfers $\le 64$ KB accepted without segmentation. |
| **4** | DMA Controller | Intel LPSS `idma64.2`: single-block scatter-gather $\le 64$ KB. | **READY** | 10,246 bytes fit in a single DMA block; no bus pauses. |
| **5** | Continuous CS# Timing | At 1 MHz: $10,246 \times 8\ \mu\text{s} \approx 82.0$ ms. Kernel timeout: 1,000 ms. | **READY** | CS# held low for 82 ms; well within 1,000 ms timeout. |
| **6** | Mutex & Concurrency | `PTHREAD_MUTEX_NORMAL`: strict serialization of transfers & getters. | **READY** | 227/227 tests passing; zero data races under TSan. |
| **7** | Context Teardown | Single-owner contract: owner joins worker threads before destroy. | **READY** | Destructor drains in-flight transfers; clean heap free. |

### 6.2 The `UNKNOWN` State Architectural Blocker
In `tools/fw9369-mock/src/fw9369_transport_hw.c`:
```c
struct fw9369_transport *fw9369_hw_transport_create_with_ops(...) {
    struct hw_context *ctx = calloc(1, sizeof(*ctx));
    // ctx->lock_state is zero (FW9369_LOCK_STATE_UNKNOWN)
}
```
When `write_byte(0x9A, 0x5A)` is executed:
- `ctx->lock_state` is `UNKNOWN`.
- Line 289 checks: `if (ctx->lock_state == FW9369_LOCK_STATE_ASSUMED_LOCKED) packet_valid = true;`
- Check fails $\rightarrow$ `packet_valid` remains `false`.
- Line 353 returns `-EPERM` before any ioctl is issued.
- **Operational Reality:** The hardware transport currently fails closed on the first protected command. A physical initialization run will fail at Phase 3 with `-EPERM`.

---

## 7. Known Unknowns

1. **Physical Silicon Register 0xC6 State:** Whether the FT9362 silicon has retained `0xC6 = 0x01` since Stage 2A or autonomously reverted to idle is unknown without in-band readback.
2. **Silicon Latency After FDT Scan Trigger:** The reference driver inserts a 5 ms poll interval on register `0x1A82` bit 3. The exact number of milliseconds required by physical silicon on this specific wafer lot has never been measured empirically.
3. **Hardware Watchdog on `0x9A`:** Whether the FT9362 silicon has an internal hardware watchdog that relocks register `0x9A` if no trim writes occur within a specific timeout is unverified.
4. **Behavior on Corrupted MISO:** If the SPI MISO line floats during a hardware fault, whether the host controller receives all `0xFF` or all `0x00` depends on internal pull resistors in the PCH pad configuration.
5. **Pad 14 Hardware Unlocking:** Whether OEM BIOS updates or ACPI DSDT overrides can safely unlock `PADCFGLOCK` on Pad 14 without bricking motherboard audio (`SPKR`) is unknown.

---

## 8. Hardware Test Prerequisites

Before any future controlled physical sensor test is authorized, the following prerequisites must be satisfied:

```
[Prerequisite 1: Scoped Test Scope] ────► Only Phase 1 & 2 (No 0x9A unlock, No FIFO read)
[Prerequisite 2: Protocol Fix] ─────────► Abort cleanup must send 0xA5 0x5A
[Prerequisite 3: Buffer Parameter] ──────► spidev.bufsiz >= 65536 in kernel parameters
[Prerequisite 4: Host Verification] ────► spidev absent, spi-FTE3600:00 unbound, pins clean
[Prerequisite 5: Cold Reboot Fallback] ─► Pre-authorized S5 power cycle recovery procedure
[Prerequisite 6: Explicit Sign-off] ────► Manual operator review and execution gate unsealing
```

### 1. Scoped Test Architecture (Phase 1 & Phase 2 Only)
- **Criterion:** The first post-Stage-2A test must **NOT** attempt the full 5-phase pipeline.
- **Justification:** Full pipeline execution attempts `0x9A` unlock (which fails closed with `-EPERM`), attempts a 10,246-byte FIFO read (which risks `-EMSGSIZE`), and permanently arms autonomous scanning (Phase 5).
- **Scope Limit:** The first controlled test must execute **strictly Phase 1 (Bus Wakeup & Chip ID)** and **Phase 2 (MCU Idle Handshake `0x5A/0xA5` and status `0x80` check)**, followed by immediate host rollback.

### 2. Protocol Cleanup Fix in `fw9369_state.c`
- **Criterion:** In `tools/fw9369-mock/src/fw9369_state.c`, `fw9369_pipeline_abort_cleanup()` must be updated to guarantee that `send_command(t, 0xA5, 0x5A)` is executed whenever Phase 2 has been initiated, preventing an orphaned MCU command dialog.
- **Evidence:** Verified by static unit test simulating `-EIO` on register `0x80` read.

### 3. Kernel Buffer Size Configuration
- **Criterion:** Prior to executing any test involving Phase 4, `spidev` must be loaded with parameter `bufsiz=65536`.
- **Verification:** Read `/sys/module/spidev/parameters/bufsiz` and assert value $\ge 65536$.

### 4. Pre-Execution Host Verification
- **Criterion:** Standard pre-flight check confirming:
  - Kernel is `7.2.8-9-omarchy`.
  - `spidev` is confirmed ABSENT.
  - `spi-FTE3600:00` is confirmed UNBOUND with clean `driver_override`.
  - Pad 14 is confirmed `0x84000201`.
  - Pad 229 is confirmed `0x82000100` (or `0x82000102`).

### 5. Documented Cold Reboot Recovery Plan
- **Criterion:** Because software reset is impossible under pure-spidev, the test procedure must document an explicit, pre-authorized manual fallback:
  - If the sensor silicon enters an unresponsive state, the operator must execute a full ACPI S5 cold reboot (shutdown, disconnect AC, wait 30 seconds, power on).

### 6. Explicit Operator Authorization
- **Criterion:** The hardware execution gate in `bin/fw9369-hw-tool` must remain sealed until an independent operator review explicitly approves the scoped test plan.

---

## 9. Risk Register

| Risk ID | Failure Event | Probability | Severity | Unmitigated Impact | Implemented / Proposed Mitigation |
|:---:|:---:|:---:|:---:|:---:|:---:|
| **R1** | Trim Bank Corruption via `0x9A` | Low | **CRITICAL** | Overwriting `0x00C0..0x00C2` with invalid values causes analog drift. | Trim writes are constrained to fixed vetted constants; `0x9A` unlock window is bounded to $<1$ ms; fail-closed allowlist blocks writes from `UNKNOWN`. |
| **R2** | `spidev.bufsiz` Overflow (`-EMSGSIZE`) | High | **HIGH** | Phase 4 FIFO read fails at syscall boundary; frame capture impossible. | Must load `spidev bufsiz=65536`. Verified in Layer 1 audit. |
| **R3** | Orphaned MCU Command Dialog | Medium | **HIGH** | Failing to send `0xA5 0x5A` leaves MCU command processor hung. | Update `abort_cleanup()` to unconditionally transmit `0xA5 0x5A` upon Phase 2 abort. |
| **R4** | Phase 3 Execution Failure (`-EPERM`) | **CERTAIN (100%)** | **HIGH** | `ctx->lock_state` starting in `UNKNOWN` causes `hw_transfer()` to reject `0x9A` writes. | Restrict first hardware test strictly to Phase 1 and Phase 2; re-evaluate lock state bootstrap model. |
| **R5** | Irreversible Autonomous Arming | High | **MEDIUM** | Phase 5 arm command `0xC2 0x3D` cannot be disarmed in software; requires S5 reboot. | Do not execute Phase 5 on the first hardware run; verify baseline capture first. |
| **R6** | SPI Bus Desynchronization | Low | **MEDIUM** | Incomplete packet shift leaves parser out of phase. | PXA2xx hardware controller guarantees 8-bit byte framing; CS# deassertion high resets FIFO pointer. |
| **R7** | Absence of Hardware Reset | **CERTAIN (100%)** | **HIGH** | If sensor silicon deadlocks, software cannot pulse reset. | Acknowledge reality; establish ACPI S5 cold reboot as the sole certified recovery mechanism. |

---

## 10. Final Verdict

### **NOT READY — SPECIFIC BLOCKERS REMAIN**

Physical hardware execution is **STRICTLY PROHIBITED**. The hardware gate in `bin/fw9369-hw-tool` remains **CLOSED**.

### Required Action Items Before Hardware Execution
1. **Resolve the Epistemic `UNKNOWN` State Blocker:** Reconcile how a physical driver context safely transitions from `UNKNOWN` to `ASSUMED_LOCKED` after power-on / wakeup, or explicitly restrict the next stage to Phase 1 / Phase 2 read-only operations.
2. **Correct the Abort Cleanup Delimiter:** Patch `fw9369_state.c` so that `fw9369_pipeline_abort_cleanup()` unconditionally sends `0xA5 0x5A` if Phase 2 was entered.
3. **Enforce `spidev.bufsiz` Configuration:** Ensure test harnesses verify `bufsiz >= 65536` before any FIFO read is attempted.
4. **Scope the Next Hardware Stage:** Author a dedicated, highly constrained Stage 2E test specification limited strictly to read-only Phase 1 (Wakeup & Chip ID) and Phase 2 (MCU Idle Handshake).
5. **Maintain Standby Status:** Await explicit user authorization before modifying code or unsealing the execution gate.
