# Stage 2C.3 — Recovery & Hardware State Review

**Document ID:** `STAGE2C3_RECOVERY_AND_HARDWARE_STATE_REVIEW`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369  
**Investigation Scope:** Investigation 7 — Rigorous audit of recovery claims, hardware state truths, uncertainties, and platform restoration boundaries.  
**Execution Boundary:** Static analysis only. Zero hardware access.

---

## 1. Executive Summary & Hardware State Invariants

This audit formalizes the exact physical and software boundaries of the test platform. It enforces strict intellectual discipline, distinguishing what has been mathematically and empirically verified from what remains physically unobservable or unknown.

The essential state invariants are:
1. **Host-Side Isolation is Complete:** Kernel module `spidev` is absent, device `spi-FTE3600:00` is unbound, `driver_override` is cleared, and character devices `/dev/spidev*` do not exist.
2. **Zero Hardware Operations Since Stage 2A:** Throughout Stages 2B.1, 2B.2, 2B.3, 2B.4, 2B.5, 2C.2, and 2C.3, **zero SPI transactions, zero GPIO toggles, zero module loads, and zero device bindings** have occurred.
3. **Sensor-Internal State is Unconfirmed:** Stage 2A wrote `0xC6 = 0x01`. Whether the physical FT9362 silicon retains `0xC6 = 0x01`, autonomously decayed, or reset via internal watchdogs cannot be verified without initiating a bus transaction.
4. **Pad 229 Telemetry Difference:** PCH Pad 229 remains in its post-Stage-1 edge-configured state (`0x82000100`), differing from its pre-Stage-1 level-triggered baseline (`0x80100100`). Both states are electrically identical High-Z inputs. Direct MMIO manipulation is strictly prohibited.
5. **ACPI S5 Cold Reboot is the Sole Known Recovery Path:** Because Pad 14 cannot be pulsed from userspace, full platform power-down (ACPI S5) remains the only conservative, non-intrusive recovery path.

---

## 2. Distinction Ledger: Host-Side vs. Silicon-Internal Reality

| Dimension | Host OS State | Sensor Physical Silicon State | Verification Method |
| :--- | :--- | :--- | :--- |
| **Driver Binding** | `UNBOUND` (Confirmed) | N/A | `/sys/bus/spi/devices/spi-FTE3600:00/driver` absent |
| **Kernel Module** | `spidev` ABSENT (Confirmed) | N/A | `/proc/modules` |
| **SPI Controller** | Quiescent / Low-Power | Quiescent (SCLK=0V, CS#=3.3V) | Intel LPSS controller hardware state |
| **Register `0xC6`** | Not cached | **UNKNOWN** | Cannot be read without SPI transaction |
| **AFE Registers** | Not cached | Factory default / uncalibrated | Never written during Stage 2A |
| **Internal FIFO** | Zero bytes buffered | Empty / Reset | Last transaction deasserted CS# |
| **MCU State** | N/A | Idle or quiescent sensing | Internal silicon logic |

---

## 3. PCH Pad Invariants

### 3.1 Pad 14 (Hardware Reset Line `RST_N`)
- **Telemetry:** `pin 14 (SPKR) 14:INT34C5:00 GPIO 0x84000201 0x0003c026 0x00000000 [LOCKED]`
- **Status:** **100% Unchanged from Factory Boot Baseline.**
- **Hardware Mode:** Output buffer active (`GPIORXDIS=1, GPIOTXDIS=0`), driven HIGH (`TXSTATE=1`, 3.3V).
- **Physical Impact:** The sensor's active-low reset pin is continuously held in the inactive (running) state by the Intel PCH hardware output driver.
- **Operational Reality:** Software cannot toggle this pad from userspace in pure-spidev.

### 3.2 Pad 229 (Sensor Interrupt Line `IRQ`)
- **Telemetry:** `pin 229 (CPU_GP_0) 323:INT34C5:00 GPIO 0x82000100 0x00000029 0x00000000 [LOCKED tx]`
- **Comparison to Historical Baseline:**
  - *Baseline (Pre-Stage 1):* `0x80100100` (`RXEVCFG = 0b00` Level, `GPIROUTIOXAPIC = 1`)
  - *Current (Post-Stage 1):* `0x82000100` (`RXEVCFG = 0b01` Edge, `GPIROUTIOXAPIC = 0`)
- **Electrical Safety:**
  - In both baseline and current states, `GPIOTXDIS = 1` and `GPIORXDIS = 0`.
  - The pad remains strictly configured in **High-Z Input Mode**.
  - There is zero drive contention and zero electrical conflict with the sensor's IRQ output pin.
- **Strict Prohibition on MMIO Writes:**
  - Writing raw chipset MMIO registers via `/dev/mem` or kernel hacks to force `PADCFG0` back to `0x80100100` is **strictly prohibited**. It violates kernel subsystem isolation and risks platform instability.
  - The configuration will automatically revert upon the next cold reboot.

---

## 4. Recovery Protocol & Assumptions

1. **Defensive Programming over Hardware Reset:**
   Because upstream recovery code relies on toggling Pad 14 (which is impossible in pure-spidev), future userspace tooling must:
   - Impose bounded timeouts (maximum 20 iterations or 100 ms) on all polling loops.
   - Enforce structured RAII / `finally` blocks to guarantee that unlock key `write_byte(0x9A, 0x00)` is submitted even if an intermediate transaction fails.
2. **Cold Power Cycle (ACPI S5) Recovery:**
   If the FT9362 silicon enters an unexpected latch-up, deadlock, or communication hang:
   - Software cannot recover it via SPI.
   - Software cannot pulse reset via Pad 14.
   - The operator must execute a **full system shutdown and cold power cycle (ACPI S5)**. This cuts standby rails, forcing both Intel PCH pads and FT9362 silicon back to factory hardware power-on reset defaults.
