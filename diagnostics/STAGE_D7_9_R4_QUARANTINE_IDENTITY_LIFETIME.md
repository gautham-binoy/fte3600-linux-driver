# STAGE D7.9-R4: Deliverable 2 — Quarantine Identity, Domain Scope and Lifetime Specification

**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Peripheral Device:** FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Host Controller:** Intel LPSS SPI (`pxa2xx-spi.2` / `idma64`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Scope:** Topological Domain Scoping, Identity Persistence, and ABA-Immune Hardware Keying  

---

## 1. Executive Summary

This specification establishes the topological scope and hardware identity criteria for quarantined SPI transaction domains.

**Core Design Decisions:**
1. **Quarantine Domain Scope:** Quarantine applies to the **entire physical SPI controller**, not merely an individual chip select. In the Intel LPSS SPI architecture, the IDMA64 channels, interrupt line, and hardware FIFOs are controller-global shared resources. If a transaction hangs or times out, the entire controller engine is compromised.
2. **Immutable Physical Identity Key:** Storing raw kernel pointers (`struct spi_controller *`) or bare integer bus IDs (`ctlr->bus_num`) introduces ABA pointer-reuse vulnerabilities upon controller unregistration and re-registration. The driver identifies domains using the immutable parent PCI hardware device name:
   $$\mathbf{ID}_{\text{domain}} = \mathtt{dev\_name(spi->controller->dev.parent)} = \mathbf{"0000:00:1e.3"}$$
3. **Record Lifetime:** Quarantine records are statically allocated in the module's `.data` section. They remain valid indefinitely until the host system is cold-rebooted.

---

## 2. Evaluation of Identity Candidates

| Identity Candidate | Evaluation Mechanism | Vulnerability / Failure Mode | Safety Assessment |
| :--- | :--- | :--- | :--- |
| **1. Pointer `spi_controller *`** | Memory address of controller object | **FATAL ABA BUG:** If controller is unregistered (`spi_unregister_controller`) and reallocated, `kzalloc` can return the exact same memory pointer! | **REJECTED** |
| **2. Pointer `spi_device *`** | Memory address of peripheral device | **FATAL ABA BUG:** If peripheral unbinds and rebinds, new device struct can share old memory address. | **REJECTED** |
| **3. Bare `bus_num` (integer)** | Integer assigned by SPI IDR (`spi_controller_idr`) | **IDR REUSE BUG:** On unregister, IDR frees the integer. A hot-plugged USB-SPI dongle could receive the reused integer. | **INSUFFICIENT ALONE** |
| **4. Tuple `(bus_num, chip_select)`** | Logical bus + CS index | **UNDER-QUARANTINING:** If CS=0 hangs, CS=1 is left open, but DMA engine on CS=1 is already corrupted. | **INSUFFICIENT** |
| **5. Controller Device Reference (`get_device`)** | Pinning `ctlr->dev` in memory | **CONTROLLER PINNING LEAK:** Prevents driver-core from completing controller teardown cleanly. | **REJECTED** |
| **6. Parent PCI BDF (`0000:00:1e.3`)** | Physical Bus/Device/Function string in sysfs | **IMMUTABLE TO DYNAMIC RE-REGISTRATION:** Bound to physical motherboard silicon trace wiring. Cannot be aliased. | **SELECTED PRIMARY KEY** |
| **7. ACPI Companion Path (`\_SB.PC00.SPI1`)** | Firmware ACPI namespace string | Immutable in firmware, but requires string parsing of ACPI handles. | **SECONDARY CONFIRMATION** |

---

## 3. Scope Justification: Controller-Wide vs. Chip-Select Quarantine

In earlier development phases, quarantining individual chip selects (`cs = 0`) was considered. This adversarial audit establishes that **controller-wide quarantine is mandatory**:

```
+-----------------------------------------------------------------------------------+
|                        INTEL LPSS SPI CONTROLLER (0000:00:1e.3)                   |
|                                                                                   |
|  +------------------------+   +------------------------------------------------+  |
|  |     TX FIFO (64B)      |   |  IDMA64 Engine (Shared RX / TX DMA Channels)   |  |
|  +------------------------+   +------------------------------------------------+  |
|  |     RX FIFO (64B)      |   |  Interrupt Line (ssp->irq 16)                  |  |
|  +------------------------+   +------------------------------------------------+  |
|                                                                                   |
|         |                                      |                                  |
|         v                                      v                                  |
|  +------------------------+             +------------------------+                |
|  |     Chip Select 0      |             |     Chip Select 1      |                |
|  |  (FocalTech FT9362)    |             |       (Unused)         |                |
|  +------------------------+             +------------------------+                |
+-----------------------------------------------------------------------------------+
```

### Hardware Coupling Proof
1. **Shared DMA Engine:** The Intel LPSS SPI host controller has a single pair of DMA channels (`dma_rx` and `dma_tx`) managed by `idma64.c`. If a transfer hangs on `cs=0`, `idma64_chan_deactivate()` drains and halts the hardware channel. Admitting a transfer on `cs=1` would attempt to start a transfer on a crippled or de-synchronized DMA engine.
2. **Shared Hardware FIFOs:** The SSP core has one physical TX FIFO and one physical RX FIFO. A stalled SPI transaction leaves corrupted or partial bytes in the FIFO.
3. **Queue Serialization:** In `drivers/spi/spi.c:2210-2260`, `spi_stop_queue()` stops message processing for the **entire controller**.

**Architectural Rule:** When a transaction fails, the physical controller domain `parent_name` (`0000:00:1e.3`) must be completely quarantined.

---

## 4. Quarantine Record Lifetime and Storage Specification

```c
struct fte3600_domain_entry {
    char parent_name[32];          /* Permanent hardware key: "0000:00:1e.3" */
    int bus_num;                   /* Active logical bus number */
    enum fte3600_domain_state state; /* IDLE, BUSY, QUARANTINED */
    bool module_pinned;            /* Tracks whether __module_get() was called */
    pid_t active_pid;              /* Diagnostic PID */
};
```

### Lifetime Invariants
1. **Static Storage:** `fte3600_domains[]` is allocated statically in the driver's `.data` section. It requires **zero dynamic memory allocations** during runtime, ensuring quarantine publication cannot fail due to `ENOMEM`.
2. **Survives Device Unbind:** When the peripheral driver is unbound via `sysfs` (`echo ... > unbind`), `fte3600_spi_remove()` runs. The module remains loaded in RAM. The `.data` section remains intact. When rebind is attempted, `fte3600_probe()` inspects `fte3600_domains[]`, finds the parent name marked `QUARANTINED`, and rejects probe with `-EBUSY`.
3. **Survives Host Controller Rebind:** If the host controller driver (`pxa2xx-spi`) is unbound and rebound, it receives the exact same PCI address `0000:00:1e.3`. The driver matches the parent name and maintains the quarantine.
4. **Cleared Only on Cold Reboot:** The only mechanism that clears the quarantine table is a system reboot (power cycling the machine). This is mathematically sound because cold-rebooting the platform power-cycles the Intel LPSS host controller and the FT9362 sensor, resetting all DMA engines and registers to power-on defaults.

**Conclusion:** Identifying domains by physical PCI parent name (`0000:00:1e.3`) provides absolute immunity to pointer reuse, IDR integer reallocation, and dynamic rebind bypasses.
