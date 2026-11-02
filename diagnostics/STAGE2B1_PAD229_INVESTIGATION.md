# Stage 2B.1 — Pad 229 Residual Configuration Investigation

**Document ID:** `STAGE2B1_PAD229_INVESTIGATION`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target SoC:** Intel Tiger Lake-LP PCH (`INT34C5:00`)  
**Investigated Signal:** ACPI Pin 323 / PCH Pad 229 (`CPU_GP_0` / Fingerprint Interrupt)  
**Investigation Scope:** Deep register decode, bit difference attribution, kernel lifecycle invariants, and restoration feasibility.  
**Execution Gate:** Strictly static analysis. No register writes, no GPIO toggling.

---

## 1. Observed Telemetry Comparison

Across the hardware milestones of this project, PCH Pad 229 (`CPU_GP_0`) recorded the following register values in `/sys/kernel/debug/pinctrl/INT34C5:00/pins`:

| Milestone | Timestamp | Pad 229 `PADCFG0` | Pad 229 `PADCFG1` | Context |
| :--- | :--- | :---: | :---: | :--- |
| **Historical Baseline (Pre-Stage 1)** | 2026-11-02T13:30 | `0x80100100` | `0x00000029` | Pristine boot state prior to module loads |
| **Active Stage 1 Test** | 2026-11-02T14:05 | `0x82000100` | `0x00000029` | `fte3600.ko` bound, IRQ requested |
| **Post-Stage 1 Rollback** | 2026-11-02T14:15 | `0x82000102` | `0x00000029` | `fte3600.ko` unloaded |
| **Stage 2A Pre-Flight & Execution** | 2026-11-02T16:04 | `0x82000100` | `0x00000029` | Pure-spidev Stage 2A test |

Pad 229 is currently in state: **`0x82000100 0x00000029`**.

---

## 2. Bitwise Breakdown & Field Analysis

In the Intel Tiger Lake-LP PCH architecture (`drivers/pinctrl/intel/pinctrl-intel.h`), each pin's configuration is stored across two 32-bit registers: `PADCFG0` (DW0) and `PADCFG1` (DW1).

### 2.1 PADCFG1 (`0x00000029` — Unchanged)
`PADCFG1` controls internal pull-up and pull-down termination.
- Baseline: `0x00000029`
- Current: `0x00000029`
- **Result:** **100% Identical.** Termination did not change.

### 2.2 PADCFG0 (`0x80100100` vs `0x82000100` / `0x82000102`)

```text
Historical Baseline: 0x80100100 = 1000 0000 0001 0000 0000 0001 0000 0000 b
Current Observed:    0x82000100 = 1000 0010 0000 0000 0000 0001 0000 0000 b
Diff Mask:           0x02100000 = .... ..1. ...1 .... .... .... .... .... b
```

| Bit Position | Macro Name | Historical (`0x80100100`) | Current (`0x82000100`) | Nature / Classification | Function & Meaning |
| :---: | :--- | :---: | :---: | :--- | :--- |
| **[31]** | `PADCFG0_RST_CONF` | `1` | `1` | Configuration | Reset source: resets on Deep Sleep / Host Reset. (Unchanged) |
| **[26:25]** | `PADCFG0_RXEVCFG` | `0b00` (`LEVEL`) | `0b01` (`EDGE`) | **Configuration** | **Receive Event Trigger:** `00` = Level sensitive, `01` = Edge sensitive. **CHANGED.** |
| **[23]** | `PADCFG0_RXINV` | `0` | `0` | Configuration | Input inversion: non-inverted (Active High/Rising). (Unchanged) |
| **[20]** | `PADCFG0_GPIROUTIOXAPIC` | `1` | `0` | **Configuration** | **Route to IOxAPIC:** `1` = Route directly to IOxAPIC; `0` = Route via GPIO Community IRQ. **CHANGED.** |
| **[13:10]** | `PADCFG0_PMODE` | `0b0000` | `0b0000` | Configuration | Pad Mode: `0` = GPIO function. (Unchanged) |
| **[9]** | `PADCFG0_GPIORXDIS` | `0` | `0` | Configuration | Receiver disable: `0` = Input receiver buffer enabled. (Unchanged) |
| **[8]** | `PADCFG0_GPIOTXDIS` | `1` | `1` | Configuration | Transmitter disable: `1` = Output driver disabled (High-Z input). (Unchanged) |
| **[1]** | `PADCFG0_GPIORXSTATE` | `0` | `0` *(or `1`)* | **Read-Only Status** | **Live Input Pad Level:** Reflects instantaneous digital voltage on physical pin. Fluctuates (`0x00` vs `0x02`) due to floating high-impedance state. |
| **[0]** | `PADCFG0_GPIOTXSTATE` | `0` | `0` | Configuration | Output TX latch (inactive because `GPIOTXDIS=1`). (Unchanged) |

---

## 3. Attribution to Stage 1 GPIO IRQ Setup

The difference between `0x80100100` and `0x82000100` is **100% attributable to Stage 1 driver execution** (`kernel/fte3600.c`):

1. **How the change occurred:**
   - During Stage 1, `kernel/fte3600.c` executed:
     ```c
     f->irq = gpiod_to_irq(f->lines[FTE3600_IRQ]);
     request_threaded_irq(f->irq, NULL, fte3600_irq_handler, ...);
     ```
   - The ACPI DSDT declares this pin under `FTE3600` as:
     `GpioInt(Edge, ActiveHigh, ExclusiveAndWake, PullNone, 0, "\\_SB.GPI0", ...) { 323 }`.
   - In response to `request_irq()`, the Linux kernel interrupt framework called `intel_gpio_irq_type()` in `drivers/pinctrl/intel/pinctrl-intel.c`:
     - It set `PADCFG0_RXEVCFG` to `0b01` (bit 25 = 1, configuring edge detection).
     - It cleared `PADCFG0_GPIROUTIOXAPIC` (bit 20 = 0, routing the interrupt through the Intel PCH GPIO community interrupt tree rather than directly to the APIC).
   - This transformed `0x80100100` directly into `0x82000100`.

2. **Why it was not restored when `fte3600.ko` was removed:**
   - Linux `gpiolib` and `pinctrl-intel` manage *software resource leases*.
   - When `fte3600_remove()` called `gpiod_put(f->lines[FTE3600_IRQ])`, the kernel invoked `.irq_release_resources = gpiochip_irq_relres`.
   - `gpiochip_irq_relres` decrements internal reference counts and marks the GPIO line as free in debugfs (`debugfs gpio` confirmed 0 lines claimed).
   - **Architectural Fact:** Mainline Linux `pinctrl-intel` does **NOT** cache pre-boot register contents or reprogram PCH hardware registers back to firmware states on module unload.
   - Therefore, the hardware register remained in the edge-configured state (`0x82000100`).

3. **Did Stage 2A alter Pad 229?**
   - **NO.** Stage 2A used pure `spidev` and did not claim, lease, or touch Pad 229.
   - The Stage 2A execution log ([stage2a-results.txt:20](file:///home/titan/fte3600-linux-driver/diagnostics/stage2a-results.txt#L20)) shows `0x82000100` before, during, and after Stage 2A, confirming it was the residual state inherited from Stage 1.

---

## 4. Restoration Feasibility & Requirements

### 4.1 Does Firmware or Kernel Code Restore Pad 229?
- **Kernel Code:** **NO.** No mainline Linux driver or framework restores PCH pad registers upon driver unload.
- **Firmware Code:** **NO.** Once booted, UEFI BIOS firmware does not re-enter or rewrite PCH registers unless a system power-state transition occurs.

### 4.2 What Is Required to Restore the Historical Baseline?
To transition Pad 229 from `0x82000100` back to `0x80100100`:
1. **Cold Power Cycle / Full System Reboot (Recommended / Safe):**
   - In `PADCFG0`, bit 31 (`PADCFG0_RST_CONF`) is set to `1`.
   - On Intel Tiger Lake PCH, `RST_CONF = 1` specifies that pad configuration registers reset to hardware default / UEFI ROM defaults upon a **Host Reset / Cold Power Cycle** (transition through ACPI S5).
   - A cold reboot will restore `0x80100100` automatically with zero risk.
2. **Direct PCH MMIO Register Write (PROHIBITED):**
   - Software could theoretically map PCH memory-mapped I/O and write `0x80100100` to the Pad 229 `PADCFG0` register.
   - **Strictly Prohibited:** Bypassing kernel subsystem boundaries to write raw PCH chipset MMIO registers risks peripheral bus instability, kernel panic, or hardware collision.

### 4.3 Operational State Statement
Pad 229 is **NOT restored** to its pre-Stage-1 baseline. It remains in its post-Stage-1 residual edge-configured input state (`0x82000100`). Because the pad's output driver remains disabled (`PADCFG0_GPIOTXDIS = 1`, High-Z input mode) and software leases are completely free, there is zero electrical risk or bus contention.
