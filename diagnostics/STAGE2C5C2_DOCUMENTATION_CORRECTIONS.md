# Stage 2C.5C.2 — Technical Rigor and Documentation Inconsistency Corrections

**Document ID:** `STAGE2C5C2_DOCUMENTATION_CORRECTIONS`  
**Date:** 2026-11-02  
**Target Hardware:** FocalTech FT9362 / FW9369  
**Execution Boundary:** Strictly static documentation reconciliation and audit. Zero hardware operations authorized.

---

## 1. Executive Summary

A core objective of Stage 2C.5C.2 is to conduct an uncompromising audit of all historical documentation, technical claims, and safety assessments produced across Stages 2B.1 through 2C.5C.1.

In complex embedded reverse-engineering projects, engineering shorthand and imprecise phrasing can inadvertently harden into "accepted facts" that mislead future implementers.

This document formally reviews, clarifies, and corrects six specific technical claims identified in Stage 2C.5C.1, establishing an unshakeable boundary between **Verified Empirical Facts**, **Engineering Assumptions**, and **Unresolvable Physical Unknowns**.

---

## 2. Exhaustive Correction of the Six Critical Claims

### Claim 1: "Pad 229 Has Zero Electrical Risk"
- **Original Wording in Prior Reports:** *"Pad 229 has zero electrical risk because transmitter is disabled."*
- **Technical Critique:** Claiming "zero electrical risk" as an absolute physical fact is technically false.
  1. *Physical Connectivity:* Pad 229 (`CPU_GP_0`) is physically connected via PCB traces to the FT9362 interrupt pin. Any physical trace carries potential ESD vulnerability and capacitive coupling.
  2. *Firmware/Pinctrl Alterations:* While `PADCFG0` currently has `GPIOTXDIS = 1` (transmitter disabled) and is transmitter locked (`[LOCKED tx]`), that lock applies to runtime GPIO direction registers in Linux. If OEM BIOS firmware SMM code or an ACPI AML method were invoked during power state transitions, pinctrl registers could be reprogrammed to output mode (`GPIOTXDIS = 0`), creating bus contention if the FT9362 simultaneously drives the line high.
- **Authoritative Reconciled Formulation:**
  > **Correction:** Pad 229 is currently configured in a **passive high-impedance input state** (`GPIORXDIS=0, GPIOTXDIS=1`) with edge-triggering enabled. Under current Linux kernel lifecycle conditions, it presents **low operational risk** and does not actively drive current into the sensor. However, it is an active physical line and does not possess "zero electrical risk."

---

### Claim 2: "S5 for Exactly 15 Seconds is a Proven Universal Reset"
- **Original Wording in Prior Reports:** *"The only certified, non-intrusive recovery method is an ACPI S5 Cold Reboot (power off for 15 seconds)."*
- **Technical Critique:**
  1. *Empirical Rule of Thumb vs. Universal Proof:* A 15-second shutdown is an empirical engineering rule of thumb, not a mathematically or physically proven constant for all motherboard revisions.
  2. *Capacitive Discharge Rates:* Laptop power delivery subsystems employ extensive RC decoupling capacitor networks across $V_{DDIO}$ and $3.3\text{ V}$ standby rails. Depending on component aging, ambient temperature, and residual battery trickle charge, complete discharge to $0.0\text{ V}$ may take anywhere from 5 seconds to over 30 seconds unless a residual charge drain (holding the power button while disconnected) is performed.
  3. *Internal Sensor Micro-Power States:* Certain low-power fingerprint controllers can maintain SRAM retention at voltages as low as $0.6\text{ V}$.
- **Authoritative Reconciled Formulation:**
  > **Correction:** An ACPI S5 shutdown with AC power disconnected is the **conservative recommended recovery procedure** supported by hardware evidence. 15 seconds is an engineering estimate for rail decay, not a universal guarantee. Complete capacitor discharge can be enhanced by holding the power button for 10 seconds while disconnected from AC power.

---

### Claim 3: "A Task in State 'D' Always Requires a Hard Reboot"
- **Original Wording in Prior Reports:** *"If a task enters state D, the system permanently freezes and always requires a hard reboot."*
- **Technical Critique:**
  1. *The Nature of TASK_UNINTERRUPTIBLE:* State `D` means the task is sleeping in kernel space awaiting a hardware completion or waitqueue (`wait_for_completion(&done)`). Userspace signals like `SIGKILL` cannot awaken it *while it is waiting*.
  2. *Conditional Recovery:* The task does **not** permanently require a hard reboot if the completion eventually fires. For example, if an SPI transfer was stalled due to high bus latency or DMA queue starvation, and the hardware controller eventually shifts the bytes or generates a late interrupt, the completion will wake the task, allowing it to exit state `D` normally.
  3. *When Reboot is Required:* A hard reboot is required **if and only if** the hardware controller has suffered a permanent silicon lockup or the interrupt completion was dropped.
- **Authoritative Reconciled Formulation:**
  > **Correction:** A task blocked in `spidev_sync()` enters `TASK_UNINTERRUPTIBLE` (state `D`). In this state, it **cannot be terminated by userspace signals (`SIGKILL`)**. If the underlying SPI controller hardware permanently freezes or fails to signal completion, the task cannot recover and will require a hard system reboot. If the hardware eventually finishes the transaction, the task will resume.

---

### Claim 4: "The Initialization Sequence is Literally Atomic"
- **Original Wording in Prior Reports:** *"The FW9369 initialization sequence is an indivisible, atomic operational pipeline."*
- **Technical Critique:**
  1. *Bus-Level Reality:* The initialization sequence is composed of over 30 separate SPI transactions spanning five distinct phases, with explicit inter-packet delays (`nanosleep(1 ms)` and `nanosleep(4 ms)`). CS# toggles high between every single register transaction. It is **not** an atomic bus transaction in the electrical or hardware sense.
  2. *Architectural vs. Electrical Atomicity:* What was intended was *architectural pipeline dependency*: Phase 5 detection thresholds depend functionally on Phase 3 FDT calibration minima, which depend on Phase 2 MCU idle setup.
- **Authoritative Reconciled Formulation:**
  > **Correction:** The FW9369 initialization flow is an **architecturally dependent pipeline**, not an electrically atomic bus transaction. While each register operation is an independent SPI transfer, executing partial segments of the pipeline is functionally unsafe because downstream phases depend strictly on calibrated state established in earlier phases.

---

### Claim 5: "A Newly Allocated Context Proves the Sensor is Locked"
- **Original Wording in Prior Reports:** *"calloc() initializes ctx->is_unlocked to false, ensuring the transport starts locked."*
- **Technical Critique:**
  1. *Heap Zeroing vs. Peripheral State:* `calloc()` allocates memory in the virtual address space of a userspace Linux process. Zeroing a userspace struct has zero physical influence over the FT9362 silicon.
  2. *Prior History Ignorance:* If the previous process crashed while the sensor was unlocked, the silicon remains unlocked. A new context cannot claim to know the sensor is locked merely because its heap variable is zero.
- **Authoritative Reconciled Formulation:**
  > **Correction:** A newly allocated transport context proves **zero facts** regarding peripheral silicon state. The initial software knowledge state must be represented as **`FW9369_LOCK_STATE_UNKNOWN`**, and protected-register writes must be fail-closed until an explicit state-transition synchronization is executed.

---

### Claim 6: "A Failed Unlock Transfer Proves the Command Was Not Received"
- **Original Wording in Prior Reports:** *"If unlock ioctl fails, is_unlocked remains false because the unlock command failed."*
- **Technical Critique:**
  1. *Wire Shift Progress:* Once the kernel begins clocking MOSI, the unlock key `0x5A` may have already been shifted into the FT9362 shift register before an error (e.g. DMA fault, short transfer, or `EINTR`) occurred.
  2. *Deassertion Latching:* When the kernel terminates the aborted transfer, CS# rises high, which may cause the sensor silicon to latch `0x9A = 0x5A`, physically opening the write protection latch.
  3. *Uncertainty Rule:* A failed transfer does not prove non-reception; it proves that **transfer completion is uncertain**.
- **Authoritative Reconciled Formulation:**
  > **Correction:** A failed unlock transfer (`ioctl < 0`, short count, or error) **does not prove the command was unreceived**. Bits may have reached the sensor before the failure. Software must classify the physical state as **`FW9369_LOCK_STATE_UNKNOWN`** and treat the silicon as potentially unlocked and unsafe.

---

## 3. Epistemic Classification Matrix

All technical claims across the project are now formally categorized under one of three epistemic tiers:

| Knowledge Tier | Criteria & Standard of Evidence | Current Examples in Project |
| :--- | :--- | :--- |
| **Tier 1: Verified Empirical Fact** | Directly measured on physical hardware with reproducible telemetry or confirmed directly in compiled kernel binaries. | - Stage 2A Chip ID `0x9362`, CRC `0x1C53`.<br>- PCH Pad 14 locked at `0x84000201 [LOCKED]`.<br>- PCH Pad 229 at `0x82000100 [LOCKED tx]`.<br>- Kernel module `spi-pxa2xx-core` uses `wait_for_completion` in state `D`. |
| **Tier 2: Engineering Assumption** | Defensible deduction based on protocol disassembly or vendor source patterns, but unverified by physical instrumentation on this specific platform. | - Continuous CS# assertion during 10,246 B burst.<br>- 15-second S5 power-down duration for rail decay.<br>- FDT calibration convergence within 32 trials.<br>- MCU idle transition indicated by `0x80 == 0x50`. |
| **Tier 3: Physical Unknown** | Parameters that cannot be measured or proven in-band via software readback. | - Silicon latch state of register `0x9A`.<br>- Persistence or autonomous decay of volatile register `0xC6`.<br>- Internal watchdog reset behavior of FT9362 MCU. |

---

## 4. Preservation of Historical Stage 2A Evidence

This audit explicitly confirms that **zero historical telemetry from Stage 2A has been erased, altered, or deprecated**. The identification results in `stage2a-results.txt` remain the foundational physical baseline of this repository:
- Device: `spi-FTE3600:00` bound to `/dev/spidev1.0`.
- Operation 1: `write_byte(0xC6, 0x01)` completed.
- Operation 3: `read_byte(0xC6)` returned `0x01`.
- Operation 4: `read_word(0x1A8B)` returned Chip ID `0x9362` and hardware CRC `0x1C53`.
- Post-test rollback: `spidev` unbound and unloaded; host confirmed clean.

---

## 5. Audit Conclusion

All technical documentation inconsistencies identified in Stage 2C.5C.1 have been corrected. The project maintains strict precision, distinguishes verified facts from assumptions, and explicitly documents physical unknowns.
