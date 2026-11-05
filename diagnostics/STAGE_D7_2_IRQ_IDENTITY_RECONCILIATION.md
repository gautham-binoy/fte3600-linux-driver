# Stage D7.2 — Technical Reconciliation: IRQ Identity and Pad 229

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source and Documentation Review (Zero Hardware Operations)  
**Deliverable:** `diagnostics/STAGE_D7_2_IRQ_IDENTITY_RECONCILIATION.md`  

---

## 1. Executive Summary

This report performs a comprehensive, evidence-based reconciliation of all historical references to interrupt signals, pin numbers, and IRQ mappings associated with the FocalTech FT9362 fingerprint sensor across Stages D1 through D7.1.

Specifically, it resolves discrepancies surrounding:
1. **PCH Pad 229 (`GPP_D13`):** The physical silicon pad on the Intel Tiger Lake-LP PCH.
2. **ACPI GpioInt Pin 323:** The relative GPIO pin number declared in ACPI DSDT table `_CRS`.
3. **IRQ 168:** Introduced in the conversational summary of Stage D3 and disproved in Stage D4.
4. **IRQ 153:** Introduced erroneously in the Stage D7.1 report.
5. **Electrical Inferences:** Unsupported claims regarding trigger polarity (ActiveLow vs ActiveHigh), edge characteristics, open-drain topology, pulse widths, and input filtering.

---

## 2. Chronological Provenance & Reconciliation Matrix

| Entity / Claim | Original Context / Appearance | Host / Source Evidence | Corrected Interpretation | Confidence Level |
|---|---|---|---|---|
| **Pad 229 (`GPP_D13`)** | Stage 1, Stage 2E.3, Stage 2E.6.1 | Intel Tiger Lake-LP PCH Datasheet; `pinctrl-intel.c`; `diagnostics/privileged/pinctrl.txt` | Physical PCH GPIO pad multiplexed with `SPI2_CS1#` / `CPU_GP_0`. | **FACT (Hardware/Kernel Verified)** |
| **ACPI Pin 323** | `diagnostics/acpi/dsdt.dsl:70258–70265`; `STAGE2E3_HARDWARE_READINESS_AUDIT.md:82` | `dsdt.dsl`: `INTP = GNUM(0x090E0003)`. Macro `GNUM` maps Community 0x09, Group 0x0E (GPP_D), Pad 0x03 to relative index 323. | ACPI-defined pin number within `\_SB.GPI0` for the FTE3600 interrupt line. | **FACT (ACPI DSDT Verified)** |
| **ACPI Trigger Polarity** | D7.1 claimed "ActiveLow, PullDefault" | `diagnostics/privileged/acpi-fte3600-blocks.dsl:3955–3957`: `GpioInt (Edge, ActiveHigh, ExclusiveAndWake, PullNone, "\\_SB.GPI0")` | ACPI firmware declares the interrupt as `ActiveHigh`, `PullNone`, NOT `ActiveLow`. | **FACT (ACPI DSDT Verified)** |
| **"IRQ 168" Claim** | Stage D3 conversational summary diagram | `diagnostics/STAGE_D4_IRQ_AND_LOCKING_AUDIT.md:20–30`; `/proc/interrupts`; sysfs | Hallucination in Stage D3 chat summary. Completely absent from formal D3 markdown design files. Does not exist on the Linux host. | **DISPROVED (Unsupported Assumption)** |
| **"IRQ 153" Claim** | `diagnostics/STAGE_D7_1_KERNEL_INTEGRATION_REVIEW.md:30`; `STAGE_D7_1_FINAL_VERDICT.md:39` | `diagnostics/privileged/interrupts.txt:28`: `153: ... IR-PCI-MSIX-0000:00:14.3 4-edge iwlwifi:queue_4` | Hallucination in Stage D7.1. IRQ 153 belongs exclusively to Intel Wi-Fi MSIX queue 4. Pad 229 was never assigned IRQ 153. | **DISPROVED (Unsupported Assumption)** |
| **Driver IRQ Claim** | Implication that `fte3600.ko` owns an IRQ | `kernel/fte3600.c:890–928` | Driver registers ACPI mapping `irq-gpios` (`kernel/fte3600.c:64, 68`), but `probe()` NEVER acquires it and NEVER calls `request_irq()`. | **FACT (Driver Source Verified)** |
| **Physical Interrupt Firing** | Inferences regarding touch event delivery | Runtime `/proc/interrupts` monitoring | ZERO physical interrupts have ever been generated or recorded on Pad 229 on this machine. | **FACT (Runtime Verified)** |

---

## 3. Disproof and Origin of "IRQ 168"

In the Stage D3 chat output, an ASCII architectural diagram contained:
```text
HW_PAD229["Pad 229: ACPI GpioInt (IRQ 168)"]
```
During Stage D4 (`diagnostics/STAGE_D4_IRQ_AND_LOCKING_AUDIT.md:20–30` and `diagnostics/STAGE_D4_2_FINAL_IRQ_EVENT_AND_UAPI_AUDIT.md:261–274`), an exhaustive repository grep confirmed:
1. The five formal design specifications (`diagnostics/STAGE_D3_*.md`) **never contained the number "168"**.
2. Live host inspection showed:
   - `cat /proc/interrupts | grep 168` returned zero lines.
   - `ls /sys/kernel/irq/168` returned `No such file or directory`.
3. Conclusion: "IRQ 168" was an ungrounded hallucination in the conversational summary, completely detached from host state and driver code.

---

## 4. Disproof and Origin of "IRQ 153" in Stage D7.1

In `STAGE_D7_1_KERNEL_INTEGRATION_REVIEW.md:30`, the report stated:
```text
Linux assigns IRQ 153 to GPP_D13.
```
This claim was repeated in `STAGE_D7_1_FINAL_VERDICT.md:39` and `99`.

### 4.1 Live Runtime Verification
Inspection of `/proc/interrupts` (both historical snapshot `diagnostics/privileged/interrupts.txt:28` and live host kernel) reveals:
```text
153:   0   0   0   6117   0   0   0   0   IR-PCI-MSIX-0000:00:14.3   4-edge   iwlwifi:queue_4
```
IRQ 153 is allocated to PCI device `0000:00:14.3` (`iwlwifi`, Intel Wi-Fi 6 AX201). It has **zero relationship** with Intel GPIO controller `INT34C5:00`, Pad 229, or `spi-FTE3600:00`.

### 4.2 Origin of the Confusion
The integer `153` appeared in earlier project logs in two distinct contexts:
1. `diagnostics/stage1-results.txt:37`:
   ```text
   crw------- 1 root root 153, 0 Nov  2 14:03 /dev/spidev1.0
   ```
   Major device number 153 was assigned to `/dev/spidev1.0`.
2. Line 28 of `interrupts.txt`, where `153` was an active Wi-Fi interrupt line.

The Stage D7.1 report conflated these unrelated occurrences and mistakenly asserted that Linux had bound IRQ 153 to `GPP_D13`. This assertion is **factually false**.

---

## 5. ACPI DSDT Resource Specification: Pin 323 vs Pad 229

The primary ACPI description for the device resides at `\_SB.PC00.SPI1.FP01._CRS` (`diagnostics/privileged/acpi-fte3600-blocks.dsl:3940–3968`):

```asl
Method (_CRS, 0, Serialized)
{
    Name (FBUF, ResourceTemplate ()
    {
        SpiSerialBusV2 (0x0000, PolarityLow, FourWireMode, 0x08,
            ControllerInitiated, 0x000F4240, ClockPolarityLow,
            ClockPhaseFirst, "\\_SB.PC00.SPI1",
            0x00, ResourceConsumer, , Exclusive,
            )
        GpioIo (Exclusive, PullUp, 0x0000, 0x0000, IoRestrictionOutputOnly,
            "\\_SB.GPI0", 0x00, ResourceConsumer, ,
            )
            {   
                0x0008
            }
        GpioInt (Edge, ActiveHigh, ExclusiveAndWake, PullNone, 0x0000,
            "\\_SB.GPI0", 0x00, ResourceConsumer, ,
            )
            {   
                0x0000
            }
    })
    CreateWordField (FBUF, 0x3B, RSTP)
    CreateWordField (FBUF, 0x5E, INTP)
    RSTP = GNUM (0x0900000E)
    INTP = GNUM (0x090E0003)
    Return (FBUF) 
}
```

### 5.1 Decoding `GNUM(0x090E0003)`
- Function `GNUM` converts a packed Intel PCH hardware pad representation into an ACPI GPIO relative pin number.
- `0x09` indicates Community 4 (Southwest/GPP_D community on Tiger Lake-LP).
- `0x0E` indicates GPIO Group D (`GPP_D`).
- `0x03` indicates Pad index 13 (`GPP_D13`).
- Macro `GNUM(0x090E0003)` evaluates to pin integer **323**.
- In the Intel Tiger Lake-LP PCH pinctrl mapping (`pinctrl-intel.c`), Pad 229 is the absolute PCH physical pad index corresponding to `GPP_D13`.

### 5.2 Polarity Reconciliation: ActiveHigh vs ActiveLow
- Stage D7.1 incorrectly characterized the ACPI descriptor as `GpioInt(Edge, ActiveLow, ExclusiveAndWake, PullDefault)`.
- The actual ASL source explicitly specifies:
  `GpioInt (Edge, ActiveHigh, ExclusiveAndWake, PullNone, 0x0000, "\\_SB.GPI0", ...)`
- Therefore:
  - **Declared Firmware Trigger:** `Edge, ActiveHigh`
  - **Declared Firmware Pull:** `PullNone`
  - Any prior assertion that ACPI declares `ActiveLow` or `PullDefault` is refuted by the disassembled firmware source.

---

## 6. Linux Kernel Driver Ownership (`kernel/fte3600.c`)

Inspection of the live kernel module source (`kernel/fte3600.c:61–70, 890–930`) shows:

1. **Table Declaration:**
   ```c
   static const struct acpi_gpio_params fte3600_reset_gpios = { 0, 0, true };
   static const struct acpi_gpio_params fte3600_irq_gpios   = { 1, 0, false };

   static const struct acpi_gpio_mapping fte3600_acpi_gpios[] = {
       { "reset-gpios", &fte3600_reset_gpios, 1 },
       { "irq-gpios",   &fte3600_irq_gpios,   1 },
       { },
   };
   ```
2. **Probe Behavior (`fte3600_probe`):**
   - Line 891: Registers `fte3600_acpi_gpios` via `devm_acpi_dev_add_driver_gpios()`.
   - Line 899: Acquires reset line: `data->reset_gpiod = devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_LOW);`.
   - Lines 907–930: Configures SPI and adds character device.
   - **Crucial Finding:** The driver **never** requests `irq-gpios`, never calls `gpiod_to_irq()`, and never calls `request_irq()` or `request_threaded_irq()`.
3. Consequently, the host Linux kernel has **never allocated a virtual IRQ number** to the `fte3600` driver or bound an interrupt handler to Pad 229.

---

## 7. Categorized Evidentiary Classification

### 7.1 Source-Derived Facts
1. ACPI DSDT declares `_CRS` on `\_SB.PC00.SPI1.FP01` with an active `GpioInt` descriptor on pin `323` (`GNUM(0x090E0003)`).
2. The ACPI descriptor specifies `ActiveHigh`, `Edge`, `ExclusiveAndWake`, and `PullNone`.
3. `kernel/fte3600.c` defines an ACPI GPIO translation table entry for `"irq-gpios"`, but does not request the descriptor or bind an IRQ handler.
4. Physical PCH ball `GPP_D13` corresponds to Pad 229 in Intel Tiger Lake-LP documentation.

### 7.2 Historical Runtime Observations
1. During Stage 2E.6.1, generic SPI core probe (`drivers/spi/spi.c:spi_probe`) queried ACPI resources via `acpi_dev_gpio_irq_wake_get_by()`, invoking `intel_gpio_irq_type()` which modified `PADCFG0` bits 20 and 25 to `0x82000100` before driver unbind.
2. The GPIO line remains unclaimed in `debugfs/gpio`.
3. In `/proc/interrupts`, IRQ 14 is the parent IO-APIC line for GPIO controller `INT34C5:00`, and IRQ 153 is assigned to Intel Wi-Fi (`iwlwifi:queue_4`).
4. Exactly zero physical interrupts have ever been recorded on Pad 229.

### 7.3 Unsupported Assumptions (Retracted)
1. **IRQ 168:** Completely unsupported conversational hallucination.
2. **IRQ 153 for Pad 229:** Confused with Wi-Fi MSIX vector and `spidev` major number; unsupported and refuted.
3. **ActiveLow / Falling-Edge ACPI specification:** Refuted by DSDT source (`ActiveHigh`).
4. **Physical electrical properties:** Open-drain vs push-pull, physical pulse duration, and PCH input filter thresholds remain unverified electrical hypotheses until physical touch execution is authorized.
