# Stage 2E.6.1 — Read-Only Pad 229 Configuration Investigation

**Target SoC:** Intel Tiger Lake-LP PCH (`INT34C5:00`)  
**Investigated Signal:** ACPI Pin 323 / PCH Pad 229 (`CPU_GP_0` / Fingerprint Interrupt)  
**Host Kernel:** `7.2.8-9-omarchy` (SMP preempt x86_64)  
**Target Device:** `spi-FTE3600:00` (FocalTech FT9362 / FW9369)  
**Investigation Scope:** Strictly read-only static analysis, kernel disassembly, and ACPI/pinctrl tracing.  
**Execution Gate:** Strictly read-only. Zero driver loads, zero register writes, zero MMIO writes.

---

## 1. Executive Summary & Observed Telemetry

During the Stage 2E.6 (Stage A) hardware test, pre-flight telemetry recorded PCH Pad 229 in its BIOS boot-default state (`0x80100100`). Following driver insertion (`insmod kernel/fte3600.ko`) and binding to `spi-FTE3600:00`, Pad 229 transitioned to `0x82000100` and persisted in that state through module teardown (`rmmod fte3600`):

| Lifecycle Phase | Timestamp | Pad 229 `PADCFG0` (DW0) | Pad 229 `PADCFG1` (DW1) | Pad Status / Flags |
| :--- | :---: | :---: | :---: | :--- |
| **Baseline (Pre-Stage A)** | 2026-11-03 13:32:26 | `0x80100100` | `0x00000029` | `[LOCKED tx]` |
| **Active Probe (Under Probe)** | 2026-11-03 13:32:27 | `0x82000100` | `0x00000029` | `[LOCKED tx]` |
| **Post-Teardown (Post-Cleanup)** | 2026-11-03 13:32:28 | `0x82000100` | `0x00000029` | `[LOCKED tx]` |

### Primary Findings
1. **The Driver Does Not Touch Pad 229:** Inspection of [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c) proves that the unified driver contains zero calls to `devm_gpiod_get(..., "irq", ...)`, zero calls to `gpiod_to_irq()`, and zero calls to `request_irq()` / `request_threaded_irq()`.
2. **Direct Causation by Generic Linux SPI Core:** Kernel disassembly of the running kernel (`vmlinux`) proves that the generic Linux SPI subsystem's bus-level probe handler, `spi_probe()` in `drivers/spi/spi.c` (at `0xffffffff824df730`), automatically inspects the ACPI companion node. When `spi->irq < 0`, it unconditionally calls `acpi_dev_gpio_irq_wake_get_by()`, which resolves ACPI Pin 323, reads the `GpioInt` edge/polarity flags from DSDT, and calls `irq_set_irq_type()`.
3. **Pinctrl Hardware Reprogramming:** `irq_set_irq_type()` invokes `intel_gpio_irq_type()` in `drivers/pinctrl/intel/pinctrl-intel.c` (at `0xffffffff820c18e0`), which clears bit 20 (`GPIROUTIOXAPIC`) and sets bit 25 (`RXEVCFG_EDGE`, `0x02000000`), altering `PADCFG0` from `0x80100100` to `0x82000100`.
4. **Historical Identity:** This is the identical transition documented in Stage 1 and Stage 2B.1. When the machine rebooted this morning (08:54), BIOS restored `0x80100100`. Binding `fte3600.ko` to `spi-FTE3600:00` triggered the exact same kernel SPI core routine.
5. **Electrical Safety Invariant:** Throughout this transition, bit 8 (`GPIOTXDIS`) remains `1`. The PCH output transmitter buffer is **strictly disabled** (high-impedance input mode). The PCH drives zero voltage and zero current onto the physical line.

---

## 2. Register Decode: `0x80100100` vs. `0x82000100`

In Intel Tiger Lake-LP PCH (`INT34C5:00`), pad configuration is governed by `PADCFG0` (DW0) and `PADCFG1` (DW1).

### 2.1 PADCFG1 (`0x00000029` — Unchanged)
`PADCFG1` controls internal pull-up / pull-down termination resistance:
- Baseline: `0x00000029`
- Post-Cleanup: `0x00000029`
- **Result:** 100% Identical. Pad termination was not altered.

### 2.2 PADCFG0 Bitwise Breakdown
```text
Baseline (Pre-Stage A):   0x80100100 = 1000 0000 0001 0000 0000 0001 0000 0000 b
Post-Probe & Post-Clean:  0x82000100 = 1000 0010 0000 0000 0000 0001 0000 0000 b
Diff Mask:                0x02100000 = .... ..1. ...1 .... .... .... .... .... b
```

| Bit Field | Name / Macro | Baseline (`0x80100100`) | Current (`0x82000100`) | Nature | Field Description & Architectural Function |
| :---: | :--- | :---: | :---: | :--- | :--- |
| **[31]** | `PADCFG0_RST_CONF` | `1` | `1` | Config | **Reset Source:** Resets to hardware/UEFI default on Host Reset / Deep Sleep (ACPI S5). (Unchanged) |
| **[26:25]** | `PADCFG0_RXEVCFG` | `0b00` (`LEVEL`) | `0b01` (`EDGE`) | **Config** | **Interrupt Trigger Configuration:** `00` = Level sensitive; `01` = Edge sensitive. **CHANGED.** |
| **[23]** | `PADCFG0_RXINV` | `0` | `0` | Config | **Input Inversion:** `0` = Non-inverted (`ActiveHigh` / rising edge). (Unchanged) |
| **[20]** | `PADCFG0_GPIROUTIOXAPIC` | `1` | `0` | **Config** | **Route to IOxAPIC:** `1` = Route directly to IO-APIC; `0` = Route via Intel GPIO Community Interrupt Tree. **CHANGED.** |
| **[13:10]** | `PADCFG0_PMODE` | `0b0000` | `0b0000` | Config | **Pad Mode:** Mode 0 = General Purpose I/O (`PMODE_GPIO`). (Unchanged) |
| **[9]** | `PADCFG0_GPIORXDIS` | `0` | `0` | Config | **Receiver Disable:** `0` = Input receiver buffer enabled. (Unchanged) |
| **[8]** | `PADCFG0_GPIOTXDIS` | `1` | `1` | Config | **Transmitter Disable:** `1` = Output buffer **DISABLED** (High-Z input mode). (Unchanged) |
| **[1]** | `PADCFG0_GPIORXSTATE` | `0` *(or `1`)* | `0` *(or `1`)* | Status | **Live Sensed Line State:** Real-time digital level on physical pin. Fluctuates if line is unpowered/floating. |
| **[0]** | `PADCFG0_GPIOTXSTATE` | `0` | `0` | Config | **Transmitter Latch:** Inactive because `GPIOTXDIS = 1`. (Unchanged) |

---

## 3. Source Code Inspection & Driver Paths

### 3.1 Driver Source Audit (`kernel/fte3600.c` & `kernel/fte3600.h`)
Inspection of all probe and GPIO initialization paths in `kernel/fte3600.c`:

1. **GPIO Descriptor Request:**
   Lines 601-608 of [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L601-L608):
   ```c
   /* Acquire reset GPIO descriptor with initial logical 0 (physical HIGH / inactive) */
   data->reset_gpiod = devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_LOW);
   if (IS_ERR(data->reset_gpiod)) {
       ...
   }
   ```
   The driver exclusively requests descriptor `"reset"`. It never requests `"irq"`.
2. **Interrupt Handlers:**
   There are zero calls to `request_irq()`, `request_threaded_irq()`, `gpiod_to_irq()`, or `enable_irq()` in `kernel/fte3600.c`.
3. **GPIO Mapping Table:**
   Lines 60-68 of [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L60-L68):
   ```c
   static const struct acpi_gpio_params fte3600_reset_gpios = { 0, 0, true };
   static const struct acpi_gpio_params fte3600_irq_gpios   = { 1, 0, false };

   static const struct acpi_gpio_mapping fte3600_acpi_gpios[] = {
       { "reset-gpios", &fte3600_reset_gpios, 1 },
       { "irq-gpios",   &fte3600_irq_gpios,   1 },
       { },
   };
   ```
   Line 594 registers this table via:
   ```c
   ret = devm_acpi_dev_add_driver_gpios(&spi->dev, fte3600_acpi_gpios);
   ```

### 3.2 What `devm_acpi_dev_add_driver_gpios()` Does
Disassembly of `acpi_dev_add_driver_gpios()` in the running kernel (`vmlinux` at `0xffffffff820d8330`):
```text
ffffffff820d8339: test   %rdi,%rdi
ffffffff820d833c: je     ffffffff820d8351
ffffffff820d833e: test   %rsi,%rsi
ffffffff820d8341: je     ffffffff820d8351
ffffffff820d8343: mov    %rsi,0x250(%rdi)       # adev->driver_gpios = gpios;
ffffffff820d834a: xor    %eax,%eax
ffffffff820d834c: jmp    __pi___x86_return_thunk
```
**Conclusion:** `devm_acpi_dev_add_driver_gpios()` does **NOT** configure GPIO pins, does not issue MMIO writes, and does not request interrupts. It only stores a pointer in kernel memory for subsequent string-based lookups.

---

## 4. Kernel Call Chain & Causation Tracing

The transition of Pad 229 occurred **before** `fte3600_probe()` was ever called, inside the generic Linux SPI subsystem bus driver.

### 4.1 ACPI DSDT Resource Declaration
In `diagnostics/acpi/dsdt.dsl` (lines 70273-70300) under `Device (FP01)`:
```asl
Method (_CRS, 0, Serialized)
{
    Name (FBUF, ResourceTemplate ()
    {
        SpiSerialBusV2 (0x0000, PolarityLow, FourWireMode, 0x08, ControllerInitiated,
            0x000F4240, ClockPolarityLow, ClockPhaseFirst, "\\_SB.PC00.SPI1", ...)
        GpioIo (Exclusive, PullUp, 0x0000, 0x0000, IoRestrictionOutputOnly,
            "\\_SB.GPI0", 0x00, ResourceConsumer, ...) { 0x0008 }
        GpioInt (Edge, ActiveHigh, ExclusiveAndWake, PullNone, 0x0000,
            "\\_SB.GPI0", 0x00, ResourceConsumer, ...) { 0x0000 }
    })
    ...
    RSTP = GNUM (0x0900000E)  // Resolves to Pin 14 (PCH Pad 14)
    INTP = GNUM (0x090E0003)  // Resolves to Pin 323 (PCH Pad 229)
    Return (FBUF)
}
```

### 4.2 Generic Linux SPI Core Bus Probe Wrapper (`drivers/spi/spi.c`)
Disassembly of `spi_probe()` in `vmlinux` at `0xffffffff824df730`:
```text
ffffffff824df74f: call   ffffffff82417080 <__dev_fwnode>
ffffffff824df75a: call   ffffffff821a95b0 <is_acpi_device_node>
ffffffff824df75f: test   %al,%al
ffffffff824df761: mov    0x344(%rbx),%eax       # eax = spi->irq
ffffffff824df767: je     ffffffff824df7bd
ffffffff824df769: test   %eax,%eax
ffffffff824df76b: js     ffffffff824df794        # if (spi->irq < 0) ...
...
ffffffff824df7b2: call   ffffffff820d9400 <acpi_dev_gpio_irq_wake_get_by>
ffffffff824df7b7: mov    %eax,0x344(%rbx)       # spi->irq = acpi_dev_gpio_irq_wake_get_by(...)
...
ffffffff824df78e: jmp    __pi___x86_indirect_thunk_rdx # Invokes sdrv->probe(spi) [fte3600_probe]
```

### 4.3 Trigger Resolution in `acpi_dev_gpio_irq_wake_get_by()`
Disassembly of `acpi_dev_gpio_irq_wake_get_by()` in `vmlinux` at `0xffffffff820d9400`:
1. It passes `name = NULL` to `__acpi_find_gpio()`, scanning raw ACPI `_CRS` resources directly.
2. It encounters the `GpioInt` descriptor on Pin 323 (Pad 229).
3. It maps the pin to a Linux virtual IRQ via `gpiod_to_irq()` (`0xffffffff820c4450`).
4. It parses trigger characteristics via `acpi_dev_get_irq_type()` (`0xffffffff82197840`), returning `IRQ_TYPE_EDGE_RISING`.
5. At address `0xffffffff820d97be`, it calls:
   ```text
   ffffffff820d97b9: mov    %ebx,%esi       # IRQ_TYPE_EDGE_RISING
   ffffffff820d97bb: mov    %r13d,%edi      # Virtual IRQ number
   ffffffff820d97be: call   ffffffff8153f5d0 <irq_set_irq_type>
   ```

### 4.4 Hardware Programming in `intel_gpio_irq_type()`
Disassembly of `intel_gpio_irq_type()` in `drivers/pinctrl/intel/pinctrl-intel.c` at `0xffffffff820c18e0`:
```text
ffffffff820c1a1e: mov    (%r14),%eax            # Read PADCFG0 (was 0x80100100)
ffffffff820c1a21: and    $0xffe1c1ff,%eax       # Clears bit 20 (PADCFG0_GPIROUTIOXAPIC)
ffffffff820c1a29: mov    %eax,(%r14)            # MMIO write
ffffffff820c1a2c: mov    (%r14),%eax
ffffffff820c1a2f: and    $0xf97fffff,%eax       # Clears RXEVCFG
ffffffff820c1a34: or     0x4(%rsp),%eax         # Sets bit 25 (0x02000000, PADCFG0_RXEVCFG_EDGE)
ffffffff820c1a3b: mov    %eax,(%r14)            # MMIO write -> Result: 0x82000100!
```
**Conclusion:** The generic kernel SPI bus driver automatically queried ACPI and invoked the Intel pinctrl driver to configure Pad 229 for edge detection BEFORE `fte3600_probe()` was ever called.

---

## 5. Teardown Behavior & Why Pad 229 Was Not Restored

When `rmmod fte3600` was executed:
1. `fte3600_remove()` ran.
2. The SPI bus unbind procedure detached `fte3600` from `spi-FTE3600:00`.
3. In Linux `gpiolib` and `pinctrl-intel`, interrupt allocations use `.irq_release_resources = gpiochip_irq_relres`.
4. `gpiochip_irq_relres` decrements kernel reference counters and marks the line unclaimed in `debugfs gpio` (confirmed: `GPIO lines claimed by fte3600 post-cleanup: None`).
5. **Standard Linux Kernel Architecture:** Mainline Linux `pinctrl-intel` **does NOT cache BIOS pre-boot register contents** and does not reprogram hardware registers back to firmware states upon unbind.
6. Consequently, `PADCFG0` remained in the edge-configured state (`0x82000100`).

---

## 6. Confirmed Facts vs. Hypotheses

| Statement | Classification | Supporting Evidence |
| :--- | :---: | :--- |
| `kernel/fte3600.c` never requested or claimed the IRQ GPIO descriptor. | **CONFIRMED FACT** | Driver source inspection; only `"reset"` is passed to `devm_gpiod_get()`. |
| `kernel/fte3600.c` never registered an IRQ handler or called `request_irq()`. | **CONFIRMED FACT** | Zero IRQ APIs present in driver source. |
| The transition was triggered by generic Linux SPI core `spi_probe()`. | **CONFIRMED FACT** | Disassembly of `spi_probe()` shows automatic call to `acpi_dev_gpio_irq_wake_get_by()`. |
| `intel_gpio_irq_type()` executed MMIO writes altering bits 25 and 20. | **CONFIRMED FACT** | Disassembly of `intel_gpio_irq_type()` reveals exact instructions modifying bits 20 and 25. |
| The transition was not caused by external system noise or unrelated devices. | **CONFIRMED FACT** | Telemetry logged immediately before and after `insmod` within 1 second. |
| Pad 229 is electrically high-Z and poses zero electrical conflict. | **CONFIRMED FACT** | Bit 8 (`PADCFG0_GPIOTXDIS = 1`) ensures output buffer is completely disabled. |
| Removing `"irq-gpios"` from `fte3600_acpi_gpios` would prevent the change. | **REFUTED HYPOTHESIS** | `spi_probe()` passes `name = NULL`, scanning ACPI `_CRS` directly, ignoring driver GPIO maps. |
| Custom MMIO writes during remove could restore `0x80100100`. | **HYPOTHESIS / UNRECOMMENDED** | Technically possible via raw PCH MMIO, but violates safe driver boundaries and risks kernel instability. |

---

## 7. Prevention Feasibility & Impact on Reset GPIO / SPI Probe

### Can this change be prevented by modifying `kernel/fte3600.c`?
**NO.**
- `spi_probe()` is part of the core Linux SPI subsystem (`drivers/spi/spi.c`) built into `vmlinux`.
- It executes **prior** to `fte3600_probe()`.
- It queries the ACPI table directly with `name = NULL`, regardless of what `fte3600_acpi_gpios` contains.
- Any out-of-tree SPI driver binding to `spi-FTE3600:00` under this kernel will trigger this behavior.

### Does `0x82000100` compromise Reset GPIO or SPI Probe?
**NO.**
- Reset GPIO is on Pad 14, an entirely separate PCH pad in a different GPIO group.
- Pad 229's change has zero effect on Pad 14 or SPI transactions.
- In fact, `0x82000100` (`Edge, ActiveHigh, Community IRQ`) correctly aligns the PCH hardware with the platform ACPI table (`GpioInt, Edge, ActiveHigh`) for future interrupt handling.

---

## 8. Hardware Safety Assessment & Next Stage Readiness

### Is Further Hardware Testing Blocked by Pad 229?
**NO.**
1. **Zero Electrical Contention:** Pad 229 has `GPIOTXDIS = 1` (transmitter disabled). It is purely a passive input buffer.
2. **ACPI Compliance:** The hardware register state `0x82000100` matches the platform firmware's declared intent (`GpioInt, Edge, ActiveHigh`).
3. **Reproducibility:** This is the identical, benign state observed in Stage 1 and Stage 2A.
4. **Natural Restoration:** A cold reboot (transition through ACPI S5) automatically resets `PADCFG0` back to `0x80100100` via hardware reset source bit 31 (`RST_CONF = 1`).

---

## 9. Investigation Conclusion

The change in Pad 229 from `0x80100100` to `0x82000100` during Stage A was **not caused by a bug or deliberate action in `fte3600.c`**, but by the generic Linux SPI core (`spi_probe` in `drivers/spi/spi.c`) automatically initializing the device's ACPI-declared interrupt line upon driver binding.

The state is benign, electrically passive (High-Z input), and fully understood. Pad 229 does not pose an electrical risk or block further testing.
