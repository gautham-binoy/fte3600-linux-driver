# Stage 2C.5C.3 — SPI Completion Semantics and Kernel D-State Audit

## Executive Summary

This audit establishes the rigorous technical semantics of SPI completion in Linux userspace, analyzes the distinction between host transport completion and physical silicon verification, audits all naming and comments in the FW9369 codebase for epistemic precision, and conducts a factual review of Linux kernel `TASK_UNINTERRUPTIBLE` (D-state) behavior during SPI transfers.

---

## 1. The Five Levels of SPI Completion

When userspace executes `ioctl(fd, SPI_IOC_MESSAGE(1), &tr)`, successful completion does NOT mean the sensor executed the command. There are five distinct physical and architectural layers:

```
[Level 1: Syscall Return (ret == len)]
                 │
                 ▼
[Level 2: Host SPI Controller FIFO Shift & CS Deassertion]
                 │
                 ▼
[Level 3: Electrical Signal Propagation across PCB Traces to FT9362 Pads]
                 │
                 ▼
[Level 4: FT9362 SPI Slave Receiver Opcode & Complement Decoding]
                 │
                 ▼
[Level 5: Silicon Latch of Analog Trim Protection Gate]
```

### Detailed Breakdown of Levels

1. **Level 1 — Syscall Return (`ret == len`):**
   - **What is proven:** The Linux kernel `spidev` driver queued the transfer, the Intel LPSS SPI controller driver (`pxa2xx-spi.2`) pushed the bytes into the Tx FIFO, and the kernel transfer completion interrupt or polling finished without returning a system error.
   - **What is NOT proven:** Any behavior beyond the host controller's Tx/Rx shift register.
2. **Level 2 — Host Controller Completion:**
   - **What is proven:** SCLK clock edges were generated, MOSI shifted 32 bits, and Chip Select (CS) was asserted and subsequently deasserted.
   - **What is NOT proven:** Whether any slave was connected, powered, or listening.
3. **Level 3 — Physical Wire Propagation:**
   - **What is proven:** Voltage transitions traversed the motherboard traces and reached the physical pads of the FT9362 package.
   - **Failure modes at Level 3:** Cold solder joints, trace fracture, ESD diode clamp short, or floating lines.
4. **Level 4 — Slave Opcode Decoding:**
   - **What is proven:** The FT9362 SPI peripheral sampled MOSI on valid SCLK edges, matched the opcode (`0x09`) and bitwise complement (`0xF6`), and routed the register address (`0x9A`).
   - **Failure modes at Level 4:** Sensor powered down (`0xC6 == 0`), core clock stopped, framing misalignment (bit-shift from bus noise), or slave state machine lockup.
5. **Level 5 — Internal Silicon Latching:**
   - **What is proven:** The internal logic latched the data byte (`0x5A` or `0x00`) into the internal write-enable register gating the analog trim bank (`0x00C0..0x00C2`).
   - **Physical limitation:** Register `0x9A` is **strictly write-only**. There is **zero hardware readback** capability to confirm Level 5.

---

## 2. Epistemic Precision in Code and Documentation

### Codebase Audit
All identifiers, comments, and diagnostics in `tools/fw9369-mock/` were audited to verify they do not claim physical verification:

1. **Enum Naming:**
   [fw9369_transport_hw.h:37-41](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/include/fw9369_transport_hw.h#L37-L41):
   - Correctly named: `FW9369_LOCK_STATE_ASSUMED_LOCKED` and `FW9369_LOCK_STATE_ASSUMED_UNLOCKED`.
   - The prefix `ASSUMED_` explicitly reflects that the state is an epistemic hypothesis of userspace based on Level 1 completion, not confirmed Level 5 silicon latching.
2. **Header Documentation:**
   [fw9369_transport_hw.h:32-35](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/include/fw9369_transport_hw.h#L32-L35):
   ```c
   * NOTE: These states represent userspace software knowledge based on transmitted
   * commands and kernel ioctl return values. They DO NOT represent physical silicon
   * readback confirmations (the FT9362 hardware provides no readable lock status bit).
   ```
3. **Audit Finding:**
   No comments or identifiers in the hardened Stage 2C.5C codebase claim that the sensor is physically confirmed locked or unlocked. All references correctly identify these states as operational assumptions.

---

## 3. Kernel D-State (`TASK_UNINTERRUPTIBLE`) Fact-Checking

Previous reports in early stages occasionally contained loose generalizations regarding Linux kernel `TASK_UNINTERRUPTIBLE` (D-state) execution during SPI transfers. This section provides an authoritative, fact-checked clarification.

### The Kernel Execution Flow in `spidev`
1. Userspace calls `ioctl(fd, SPI_IOC_MESSAGE(1), &tr)`.
2. Inside the kernel (`drivers/spi/spidev.c`):
   ```c
   static ssize_t spidev_sync(struct spidev_data *spidev, struct spi_message *message)
   {
       ...
       status = spi_sync(spidev->spi, message);
       ...
   }
   ```
3. Inside `drivers/spi/spi.c`:
   ```c
   int spi_sync(struct spi_device *spi, struct spi_message *message)
   {
       ...
       wait_for_completion(&done);
       ...
   }
   ```
4. `wait_for_completion(&done)` puts the calling task into `TASK_UNINTERRUPTIBLE` (State `D`).

### Mechanics of Signals and State `D`
- **Signal Pending vs Signal Delivery:**
  If a process in state `D` receives `SIGKILL` or `SIGINT`:
  1. The kernel marks the signal as *pending* in `current->pending.signal`.
  2. The scheduler does NOT wake the task immediately, because the task is in `TASK_UNINTERRUPTIBLE`.
  3. The task remains blocked until `complete(&done)` is invoked by the SPI interrupt handler, DMA completion, or controller timeout.
  4. Once `complete()` wakes the task, the task transitions to `TASK_RUNNING`.
  5. Upon returning to userspace (or immediately before returning from kernel mode), the kernel checks pending signals, delivers `SIGKILL`, and terminates the process immediately.

### When Does State `D` Require a Hard Reboot?
- **Normal Operation:** Every standard synchronous SPI transaction briefly enters State `D` for the microsecond or millisecond duration of the physical transfer. This is normal kernel behavior and resolves automatically upon transfer completion.
- **Controller Stall without Timeout:** A task becomes permanently stuck in State `D` (an "unkillable process") **ONLY IF**:
  1. The host hardware controller (Intel LPSS SPI / `pxa2xx-spi.2`) completely locks up (e.g. FIFO stall, clock gate collapse, or lost interrupt).
  2. AND the controller driver lacks a watchdog timer or times out with an infinite completion wait.
  3. In this rare catastrophic failure mode, `complete(&done)` is never called, the task hangs forever, and a hard reboot is necessary to recover system shutdown.
- **Clarification:** It is technically incorrect to claim that "every D-state requires a hard reboot" or that "SIGKILL cannot kill an SPI process once the transfer finishes". The process terminates cleanly as soon as the kernel completion finishes.
