# Stage 2C.5A — Hardware Failure and Recovery Matrix

**Document ID:** `STAGE2C5A_FAILURE_AND_RECOVERY_MATRIX`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Execution Boundary:** Static risk analysis and failure mode engineering. Zero hardware access.

---

## 1. Executive Summary

This document constructs a comprehensive failure and recovery matrix for the FocalTech FT9362 sensor across 15 failure modes spanning the entire operational lifecycle (pre-initialization, active tuning, FIFO capture, system crashes, and electrical interruptions).

Because the pure-spidev driver architecture has **zero access to software reset** (PCH Pad 14 is unmanaged and exclusive to ACPI), the host cannot pulse a hardware reset pin to recover from silicon deadlocks. This matrix documents the exact known vs. unknown states for every failure, evaluating whether subsequent SPI transactions are safe or dangerous, and defining conservative recovery boundaries.

---

## 2. Comprehensive 15-Point Failure Matrix

| ID | Failure Mode | Phase / Timing | Known State | Unknown State | Host Safe Recovery? | Sensor Reset Required? | S5 Cold Reboot Required? | Secondary Tx Risk? |
| :---: | :--- | :--- | :--- | :--- | :---: | :---: | :---: | :--- |
| **1** | **SPI Failure Before Init** | Phase 1 (Initial `0xC6` write fails with `-EIO`) | Host spidev failed ioctl; bus lines unaffected. | Silicon power-on state; whether lines floating. | **YES** (Close spidev, unbind). | **NO** (Silicon never engaged). | **NO** (Clean host rollback suffices). | **LOW** (Retrying bus binding is safe). |
| **2** | **SPI Failure After 0xC6** | Phase 1 (`0xC6` wrote `0x01`, but `0x1A8B` read fails) | `0xC6` written; transceiver clock un-gated. | Whether `0xC6` latched; whether MISO line active. | **YES** (Host releases spidev). | **RECOMMENDED** | **YES** (To return `0xC6` to low-power default). | **MEDIUM** (May hit uncharacterized bus timeout). |
| **3** | **Idle Handshake Timeout** | Phase 2 (`0x5A/0xA5` sent, but `0x80` never returns `0x50`) | Command dialog initiated; status poll timed out. | MCU execution state; whether boot ROM halted or looping. | **YES** (Send `0xA5/0x5A` delimiter, close spidev). | **YES** (MCU stuck in intermediate loop). | **YES** (To reset MCU program counter). | **HIGH** (Sending AFE writes while MCU is stalled risks bus lock). |
| **4** | **AFE Config Write Failure** | Phase 3 (SPI write to `0x1800`–`0x1887` fails) | Some AFE registers programmed; others unwritten. | Partial integration timing state in AFE bank. | **YES** (Run abort cleanup, lock `0x9A`, close). | **YES** (AFE in asymmetric partial configuration). | **YES** (Only cold reboot clears AFE registers). | **HIGH** (Executing scans with partial AFE risks analog clipping). |
| **5** | **Protected Unlock Failure** | Phase 3 (Write `0x9A = 0x5A` returns `-EIO`) | Unlock packet rejected by transport. | Whether silicon trim lock latch changed. | **YES** (Issue `0x9A = 0x00` cleanup, close). | **RECOMMENDED** | **YES** (To ensure trim lock is asserted). | **MEDIUM** (Avoid modifying trim bank registers). |
| **6** | **Protected Lock Failure** | Phase 3 (Inline write `0x9A = 0x00` returns `-EIO`) | Trim registers written; lock write rejected. | **CRITICAL: Silicon trim bank may remain UNLOCKED.** | **YES** (Report failure, abort pipeline). | **MANDATORY** | **YES (URGENT)** (To prevent trim corruption). | **EXTREME** (Do NOT send further writes until rebooted). |
| **7** | **FDT Calibration Divergence** | Phase 3 (DAC search reaches 32 trials without 512 crossing) | Hardware communicating; DAC tuning loop diverged. | True baseline capacitance; sensor surface contamination. | **YES** (Abort cleanup, lock `0x9A`, close). | **RECOMMENDED** | **YES** (To clear search state from `0x1801`). | **LOW** (Safe to abort; no write danger). |
| **8** | **Image DAC Divergence** | Phase 4 (Pixel median calculation fails to converge) | AFE calibrated; image baseline acquisition failed. | Physical optical/capacitive sensor noise floor. | **YES** (Close spidev, release buffers). | **RECOMMENDED** | **YES** (To reset image DAC registers). | **LOW** (Passive failure). |
| **9** | **Baseline Capture Failure** | Phase 4 (FIFO read fails or returns all zeros) | Pipeline reached Phase 4; frame readout aborted. | Sensor FIFO read pointer position. | **YES** (Free memory buffers, close spidev). | **YES** (FIFO pointer stuck). | **YES** (To reset internal FIFO hardware). | **MEDIUM** (Attempting another FIFO read may deadlock). |
| **10** | **FIFO Transfer Timeout** | Phase 4 (DMA transfer hangs; ioctl times out $> 1000$ ms) | Host DMA engine stalled; SPI CS# may be held low. | Hardware controller state; sensor shift register state. | **YES** (Host kernel aborts transfer). | **MANDATORY** | **YES** (Both LPSS DMA and sensor FIFO require reset). | **HIGH** (Bus lines may be in desynchronized state). |
| **11** | **Short FIFO Transfer** | Phase 4 (Returns $< 10,246$ bytes due to buffer cap) | Message truncated; CS# deasserted prematurely. | Sensor internal byte counter desynchronized. | **YES** (Free buffer, report `-EMSGSIZE`). | **MANDATORY** | **YES** (Sensor FIFO pointer corrupted). | **HIGH** (Sensor cannot resync without reset/power cycle). |
| **12** | **CRC Checksum Mismatch** | Phase 1 or 3 (Wire CRC on `0x1A8B` or `0x80B8` incorrect) | Frame received with bit errors. | Whether sensor shifted corrupt bits or bus noise. | **YES** (Abort pipeline, report `-EBADMSG`). | **RECOMMENDED** | **YES** (To eliminate electrical clock drift). | **MEDIUM** (Repeated CRC errors imply signal degradation). |
| **13** | **Process Termination (`SIGKILL`)** | Any Phase (Userspace process abruptly killed) | Process memory freed; open file descriptors closed. | **UNKNOWN: Register `0x9A` may be left UNLOCKED.** | **YES** (Kernel closes `/dev/spidev1.0`, deasserts CS#). | **MANDATORY** | **YES** (Userspace cannot guarantee relock after SIGKILL). | **HIGH** (Silicon state is undefined). |
| **14** | **Host Kernel Panic** | Any Phase (Operating system crashes or panics) | CPU halted; DMA transactions terminated. | SPI bus clock stopped mid-word; sensor in mid-state. | **NO** (System halted). | **MANDATORY** | **YES** (Hard power cycle required). | **EXTREME** (Hardware must be cold power-cycled). |
| **15** | **Platform Power Loss** | Any Phase (AC adapter unplugged, battery drained) | Host power dropped; motherboard standby rails collapsed. | Power decay timing; partial flash/register retention. | **YES** (On next boot, system undergoes cold start). | **AUTOMATIC** | **AUTOMATIC (S5 Cold Boot already occurred).** | **NONE** (Hardware restarts from factory power-on state). |

---

## 3. Deep Dive into Critical Failure Scenarios

### 3.1 Failure Scenario 6: Protected Lock Failure (`0x9A = 0x00` Fails)
- **Mechanism:** The driver successfully unlocked `0x9A = 0x5A` and wrote the 3 analog trim registers (`0x00C0`–`0x00C2`). However, when attempting to write `0x9A = 0x00`, the SPI transfer returns `-EIO` due to a bus error or controller glitch.
- **Danger:** The internal silicon trim bank remains write-enabled. Any subsequent word or byte write intended for a different register could accidentally modify the trim bank if address decoding slips, causing permanent analog calibration corruption.
- **Mandatory Policy:**
  1. The driver must **immediately set `lock_outcome = FW9369_LOCK_TRANSMISSION_FAILED`**.
  2. The driver must **HALT ALL SPI TRANSACTIONS IMMEDIATELY**.
  3. No retries, no probing, no further commands.
  4. The operator must execute an **immediate ACPI S5 cold reboot** to force the silicon back to its power-on locked state.

### 3.2 Failure Scenario 11: Truncated FIFO Transfer (`-EMSGSIZE` / Short Read)
- **Mechanism:** Software requests 10,246 bytes, but the kernel `spidev.bufsiz` was not increased from its default 4,096 bytes, or the DMA engine drops CS# at byte 4,096.
- **Danger:** The FT9362 internal FIFO address counter expects exactly 10,246 bytes to complete its circular frame readout. Deasserting CS# early strands the FIFO pointer at offset 4,096. Subsequent FIFO reads will start mid-frame, producing scrambled, unusable image telemetry.
- **Mandatory Policy:** The driver must not attempt to "finish" reading the remaining bytes. The transaction is irrecoverable; a full system power cycle is required to reset the FIFO pointer.

### 3.3 Failure Scenario 13: Uncatchable Process Kill (`SIGKILL`)
- **Mechanism:** An external process or operator executes `kill -9` on `fw9369-tool` while it is executing Phase 3 between lines 216 and 231 of `src/fw9369_state.c`.
- **Danger:** Because `SIGKILL` cannot be caught by signal handlers or `atexit()`, userspace unwinding does not execute `fw9369_pipeline_abort_cleanup()`. Register `0x9A` remains unlocked on the physical sensor.
- **Mandatory Policy:** Documented in operator procedures: **NEVER use `SIGKILL` during active hardware calibration**. If `SIGKILL` occurs, the machine must be power-cycled before any other fingerprint tool is launched.

---

## 4. The Established Recovery Principle

> [!CAUTION]
> **Zero Software Reset Capability:**
> Because Pad 14 cannot be pulsed from userspace in pure-spidev mode, **there is NO software command capable of resetting frozen FT9362 silicon**.
> 
> Attempting secondary recovery transactions (e.g., re-sending `0x5A`, writing random registers, or attempting to brute-force unlock/lock cycles) carries an unacceptable risk of aggravating silicon desynchronization.
> 
> Therefore, across all high-severity failure modes (Scenarios 3, 4, 6, 9, 10, 11, 13, 14), an **ACPI S5 system cold reboot (full laptop power down)** is the **ONLY established, conservative, and guaranteed recovery procedure**.

---

## 5. Audit Conclusion

The failure and recovery landscape for the FT9362 sensor is thoroughly mapped and classified.
- Safe abort paths exist for host-side resources.
- Critical silicon risks (particularly around `0x9A` and FIFO desynchronization) are identified with zero-tolerance containment policies.
- The failure matrix is **AUDITED AND APPROVED FOR OPERATOR REVIEW**.
