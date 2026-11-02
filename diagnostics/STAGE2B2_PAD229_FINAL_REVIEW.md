# Stage 2B.2 — PCH Pad 229 Configuration & Restoration Audit

**Document ID:** `STAGE2B2_PAD229_FINAL_REVIEW`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target SoC:** Intel Tiger Lake-LP PCH (`INT34C5:00`)  
**Investigated Signal:** ACPI Pin 323 / PCH Pad 229 (`CPU_GP_0` / Fingerprint Interrupt)  
**Scope:** Confirmation of baseline vs. current fields, Intel GPIO driver capabilities, and restoration mechanisms.  
**Execution Gate:** Strictly static analysis. No MMIO writes, no GPIO toggling.

---

## 1. Register Telemetry Confirmation

Confirmed register measurements from `/sys/kernel/debug/pinctrl/INT34C5:00/pins`:

- **Historical Baseline (Pre-Stage 1):**
  - `PADCFG0 = 0x80100100`
  - `PADCFG1 = 0x00000029`
- **Current Observed (Post-Stage 1, Pre/Post-Stage 2A):**
  - `PADCFG0 = 0x82000100` *(or `0x82000102` when input floats high)*
  - `PADCFG1 = 0x00000029`

---

## 2. Detailed Field Classification & Difference Analysis

In `drivers/pinctrl/intel/pinctrl-intel.h` and Intel Tiger Lake PCH technical specifications:

### 2.1 PADCFG1 (`0x00000029` — Identical)
- Bits `[13:10]` (`TERM`): `0b0000` = `PullNone` (no internal pull-up / pull-down termination).
- Bits `[7:0]`: `0x29` = Pad tolerance and buffer configuration.
- **Confirmation:** Identical across all tests.

### 2.2 PADCFG0 (`0x80100100` vs `0x82000100`)

```text
Baseline: 0x80100100 = [31]=1, [26:25]=00, [20]=1, [8]=1, [1]=0
Current:  0x82000100 = [31]=1, [26:25]=01, [20]=0, [8]=1, [1]=0
```

1. **Fields That Differ:**
   - **Bits `[26:25]` (`PADCFG0_RXEVCFG`):**
     - Baseline: `0b00` (`LEVEL` — Level-sensitive interrupt).
     - Current: `0b01` (`EDGE` — Edge-sensitive interrupt).
     - **Nature:** Configuration field.
   - **Bit `[20]` (`PADCFG0_GPIROUTIOXAPIC`):**
     - Baseline: `1` (Direct IOxAPIC routing).
     - Current: `0` (GPIO Community interrupt tree routing).
     - **Nature:** Configuration field.
2. **Read-Only Live Status Field:**
   - **Bit `[1]` (`PADCFG0_GPIORXSTATE`):**
     - Value: Fluctuates between `0` and `1` (`0x82000100` vs `0x82000102`).
     - **Nature: READ-ONLY LIVE STATUS BIT.** Driven directly by the physical digital receiver buffer. Because the sensor pin is high-impedance and ACPI declares `PullNone`, ambient charge causes the live read-only status bit to float.
3. **Unchanged Direction / Electrical Fields:**
   - **Bit `[8]` (`PADCFG0_GPIOTXDIS = 1`):** Transmitter is permanently disabled. Pad is strictly in high-impedance input mode.
   - **Bit `[9]` (`PADCFG0_GPIORXDIS = 0`):** Receiver buffer is enabled.
   - **Bits `[13:10]` (`PADCFG0_PMODE = 0`):** Configured as GPIO.

---

## 3. Intel GPIO Driver Capabilities & Restoration Mechanisms

### 3.1 Does the Intel GPIO Driver Provide a Supported Way to Restore Configuration?
- **NO.**
- In `drivers/pinctrl/intel/pinctrl-intel.c`:
  - When `gpiod_to_irq()` was called in Stage 1, `intel_gpio_irq_type()` reconfigured `RXEVCFG` and `GPIROUTIOXAPIC` to match the ACPI `GpioInt(Edge, ActiveHigh)` descriptor.
  - When `fte3600.ko` was removed, `gpiod_put()` executed `gpiochip_irq_relres()`.
  - `gpiochip_irq_relres()` only clears kernel software tracking flags (`FLAG_REQUESTED`, `FLAG_IS_OUT`).
  - **Source Fact:** Mainline Linux `pinctrl-intel` contains **zero code** to cache previous register values or write them back upon descriptor release. The debugfs interface (`/sys/kernel/debug/pinctrl/*/pins`) is read-only.
  - There is no supported Linux sysfs, debugfs, or kernel API to revert an individual pad's configuration register back to pre-probe values without writing raw MMIO.

### 3.2 What Mechanisms Can Restore the Original Configuration?
Authoritative source evidence establishes the following mechanisms:

1. **Host Reset / Cold Power Cycle (Proven Hardware Mechanism):**
   - In `PADCFG0`, Bit 31 is `PADCFG0_RST_CONF = 1`.
   - In Intel PCH specifications, `RST_CONF = 1` designates the **Host Reset / Deep Sleep reset domain**.
   - Upon a cold system reboot or power cycle (transition through ACPI S5), the PCH hardware logic resets the `PADCFG0` register back to the UEFI BIOS ROM default value (`0x80100100`).
2. **Deep Sleep State Transitions (S3 / S4 / S2idle Resume):**
   - During platform suspend-to-RAM (S3) or hibernate (S4), the Intel Fast Boot / FSP firmware re-initializes the motherboard's static GPIO tables upon resume. (On modern S2idle, whether FSP runs depends on platform power-gating depth).
3. **Unsupported / Prohibited Approaches:**
   - Writing directly to PCH memory-mapped I/O (`/dev/mem` or custom kernel MMIO poke) could force `0x80100100` into `PADCFG0`.
   - **Determination:** Strictly prohibited. Bypassing kernel driver ownership to write raw chipset registers risks peripheral instability.

---

## 4. Operational Conclusion

- Pad 229 was altered by Stage 1 GPIO IRQ setup and has **NOT been restored**.
- It remains safely configured as a high-impedance input receiver (`0x82000100`).
- Because software ownership is completely released (0 lines claimed in `debugfs gpio`) and the transmitter is disabled (`GPIOTXDIS=1`), there is zero electrical risk or bus contention.
- Restoration of the historical baseline `0x80100100` will occur naturally upon the next system reboot or cold power cycle via the PCH hardware reset logic.
