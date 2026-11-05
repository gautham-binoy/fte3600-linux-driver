# STAGE D4: SPI DMA, CONTROLLER DRIVER, AND TRANSFER LIMIT AUDIT

## Document Information
- **Project:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`) Native Linux Fingerprint Driver
- **Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`)
- **Host Controller:** Intel LPSS SPI2 (`pxa2xx-spi.2`, CS0) on Linux `7.2.8-9-omarchy` (x86_64)
- **Author:** Independent Adversarial Architecture Reviewer (Antigravity Agent)
- **Status:** COMPLETE ADVERSARIAL AUDIT (Critical Issue A)
- **Safety Level:** STRICTLY READ-ONLY VERIFICATION (Zero Hardware Operations)

---

## 1. Executive Summary & Objective

Stage D3 proposed that the kernel driver read the FT9362 fingerprint image frame in a single, continuous 10,246-byte SPI burst transaction:
- **Command Prefix:** 6 bytes (`{0x06, 0xF9, 0x9A, 0x05, 0x14, 0x00}`)
- **Pixel Payload:** 10,240 bytes ($64 \times 80 \times 2$ bytes, 16-bit big-endian words)
- **Continuity Requirement:** Chip Select (`CS0`) must remain continuously asserted (LOW) throughout the entire 10,246-byte transfer without deassertion or mid-transfer toggle glitches.

The objective of this adversarial audit is to independently verify whether the actual Linux host controller stack, platform driver (`pxa2xx-spi`), PCI glue (`intel-lpss-pci`), core MFD driver (`intel-lpss`), and DMA engine (`idma64`) natively support a single 10,246-byte transfer with continuous CS0 assertion, or whether hardware/driver limitations would force transfer splitting, PIO fallback, or CS toggling.

---

## 2. Host Controller Topology & Driver Hierarchy

Inspection of sysfs and running kernel modules reveals the exact hardware and driver stack:

```
[ Intel Tiger Lake-UP3 PCH PCI Subsystem ]
                     │
                     ▼ PCI Device 0000:00:1e.3 (Vendor 8086, Device A0AA / LPSS SPI2)
       [ intel-lpss-pci.ko / intel-lpss.ko ]
                     │
                     ▼ Instantiates platform devices:
          ┌──────────┴──────────┐
          ▼                     ▼
[ idma64.2 (DMA Engine) ]  [ pxa2xx-spi.2 (SPI Host Controller) ]
  Driver: idma64.ko          Driver: spi-pxa2xx-platform.ko / spi-pxa2xx-core.ko
          │                     │
          └──────────┬──────────┘
                     ▼
       [ SPI Master Bus: spi1 ]
                     │
                     ▼ CS0 (Hardware Controlled via LPSS CS Register)
       [ SPI Client Device: spi-FTE3600:00 ]
         Driver: fte3600.ko
```

### Verified Driver Modules:
1. `intel-lpss-pci.ko.zst` (`drivers/mfd/intel-lpss-pci.ko`): Binds to PCI `0000:00:1e.3`.
2. `intel-lpss.ko.zst` (`drivers/mfd/intel-lpss.ko`): Registers platform device `pxa2xx-spi.2` and declares soft dependency `softdep: pre: platform:idma64`.
3. `idma64.ko.zst` (`drivers/dma/idma64.ko`): Intel Integrated DMA 64-bit engine (`idma64.0`, `idma64.1`, `idma64.2` confirmed in `journalctl -k`).
4. `spi-pxa2xx-platform.ko.zst` & `spi-pxa2xx-core.ko.zst`: PXA2xx SSP SPI controller platform and core driver.

---

## 3. Disassembly & Static Analysis of Controller Limits

To establish the exact transfer limits without guessing, binary object inspection and disassembly were performed on `spi-pxa2xx-core.ko` and `idma64.ko` extracted from `/lib/modules/7.2.8-9-omarchy/`.

### 3.1 Maximum DMA Transfer Size (`pxa2xx_spi_max_dma_transfer_size`)
Disassembly of `pxa2xx_spi_max_dma_transfer_size()` in `spi-pxa2xx-core.ko`:
```asm
0000000000000550 <pxa2xx_spi_max_dma_transfer_size>:
     550: endbr64
     554: call   559 <pxa2xx_spi_max_dma_transfer_size+0x9>
     559: mov    $0x10000,%eax
     55e: jmp    563 <pxa2xx_spi_max_dma_transfer_size+0x13>
```
- **Finding:** The controller driver explicitly advertises a maximum DMA transfer size of `0x10000` bytes ($65,536$ bytes / $64\,\text{KB}$).
- **Comparison:** Proposed transfer length is $10,246$ bytes.
- **Verdict:** $10,246 \le 65,536$. Well within the controller's maximum advertised DMA transfer limit.

### 3.2 DMA Qualification Check (`pxa2xx_spi_can_dma`)
Disassembly of `pxa2xx_spi_can_dma()` in `spi-pxa2xx-core.ko`:
```asm
0000000000000490 <pxa2xx_spi_can_dma>:
     490: endbr64
     494: call   499 <pxa2xx_spi_can_dma+0x9>
     499: mov    0x78(%rdi),%rax
     49d: mov    0x18(%rax),%rax
     4a1: cmpb   $0x0,0x1(%rax)
     4a5: je     4c0 <pxa2xx_spi_can_dma+0x30>
     4a7: mov    0x10(%rdx),%edx          ; %edx = t->len
     4aa: cmp    $0x10000,%edx            ; Compare t->len with 65536
     4b0: ja     4c0 <pxa2xx_spi_can_dma+0x30> ; If > 64KB, reject DMA
     4b2: movzbl 0x2(%rax),%eax           ; %eax = dma_burst_size (16 or 32)
     4b6: cmp    %eax,%edx                ; Compare t->len with burst_size
     4b8: setae  %al                      ; Return 1 if t->len >= burst_size
```
- **Finding:** A transfer qualifies for DMA execution if and only if:
  1. DMA is enabled on the controller (`0x1(%rax) != 0`).
  2. Transfer length `t->len <= 65536`.
  3. Transfer length `t->len >= burst_size` (typically 16 bytes).
- **Comparison:** For the 10,246-byte frame transfer, $16 \le 10,246 \le 65,536$.
- **Verdict:** `pxa2xx_spi_can_dma` unconditionally returns **TRUE** (1) for $10,246$ bytes. The transfer is routed to the DMA path (`pxa2xx_spi_dma_transfer`).

### 3.3 Core Transfer Entry (`pxa2xx_spi_transfer_one`)
Disassembly of `pxa2xx_spi_transfer_one()` in `spi-pxa2xx-core.ko`:
```asm
0000000000001700 <pxa2xx_spi_transfer_one>:
     ...
    175b: cmpl   $0x10000,0x10(%r12)      ; Compare t->len with 65536
    1764: jbe    1774                     ; If <= 64KB, proceed normally
    1766: mov    0x18(%rbp),%rax
    176a: cmpb   $0x0,0x1(%rax)
    176e: jne    1ea4                     ; Reject if oversized
    1774: ...
```
- **Finding:** The controller driver does not split or fragment transfers below 65,536 bytes. A single `spi_transfer` of 10,246 bytes is executed in its entirety as a single DMA transaction.

### 3.4 Intel LPSS Integrated DMA Engine (`idma64`) Limits
Disassembly of `idma64_prep_slave_sg()` in `idma64.ko`:
```asm
0000000000000c40 <idma64_prep_slave_sg>:
     ...
     e47: and    $0x1ffff,%r9d            ; Mask block length with 0x1FFFF (131,071)
```
- **Finding:** The hardware descriptor block size field (`CTL_LO.BLOCK_TS`) in the Intel `idma64` IP core is 17 bits wide (`0x1FFFF`), supporting up to $131,071$ items ($128\,\text{KB}$).
- **Verdict:** A 10,246-byte buffer requires exactly **one** hardware DMA descriptor. No scatter-gather chaining or multi-block splitting occurs.

---

## 4. Chip Select (CS0) Continuity Invariant

The FT9362 silicon internal FIFO sequencer requires that Chip Select remain continuously asserted throughout the 6 command bytes and 10,240 data bytes. If CS deasserts prematurely, the FT9362 resets its internal burst read pointer.

### 4.1 Linux SPI Core CS Handling (`drivers/spi/spi.c`)
In the standard Linux SPI master queue engine:
- `spi_transfer_one_message()` iterates through `msg->transfers`.
- Before the first transfer, `spi_set_cs(msg->spi, true)` is called (CS asserts LOW).
- During the transfer, if `t->cs_change == 0`, CS remains continuously asserted.
- Only after all transfers in the message complete is `spi_set_cs(msg->spi, false)` called (CS deasserts HIGH).

### 4.2 LPSS Hardware CS Control (`lpss_ssp_cs_control`)
Disassembly of `lpss_ssp_cs_control()` in `spi-pxa2xx-core.ko`:
- CS for Intel LPSS is controlled via dedicated MMIO register bit manipulation in the LPSS private configuration space (`CS_CONTROL` register).
- `lpss_ssp_cs_control` is invoked exclusively by `pxa2xx_spi_set_cs()` at transfer/message boundaries.
- **Finding:** The LPSS hardware does **not** auto-toggle CS during DMA bursting. CS remains driven LOW by the LPSS hardware sequencer until explicitly toggled by `pxa2xx_spi_set_cs`.

---

## 5. PIO Fallback Analysis

What occurs if DMA allocation fails or is unavailable?
- If `pxa2xx_spi_can_dma` fails or DMA channels cannot be acquired, `pxa2xx_spi_transfer_one()` falls back to `interrupt_transfer()` (PIO interrupt-driven FIFO pumping).
- In PIO mode, the CPU services the 32-entry hardware FIFO in `ssp_int()` interrupts.
- **Timing Impact:** At 1 MHz SPI frequency, transferring 10,246 bytes takes approximately $82\,\text{ms}$. In PIO mode, this generates ~320 interrupts, consuming CPU cycles.
- **CS Impact:** In PIO mode, CS0 is **still** held continuously asserted by `pxa2xx_spi_set_cs()` because `t->cs_change == 0`. The transfer timing does not deassert CS.
- **Operational Reality:** On the target Dynabook C40-J, `idma64` is fully operational and active (`idma64.0`, `idma64.1`, `idma64.2` initialized at boot), so DMA is the active operational mode.

---

## 6. Audit Verdict

| Property | Requirement | Host Controller Evidence | Adversarial Verdict |
|:---|:---:|:---:|:---:|
| **Transfer Size** | 10,246 bytes | `max_dma_transfer_size = 0x10000` ($65,536$ B) | **CONFIRMED** |
| **DMA Qualification** | `can_dma == true` | $16 \le 10,246 \le 65,536 \implies$ DMA enabled | **CONFIRMED** |
| **Descriptor Count** | Single DMA block | `idma64` block size mask `0x1FFFF` ($131,071$ B) | **CONFIRMED** |
| **No Mid-Transfer Split** | Single `spi_transfer` | Driver processes `t->len <= 65536` in 1 pass | **CONFIRMED** |
| **CS0 Continuity** | Continuous LOW | LPSS `CS_CONTROL` asserted across message | **CONFIRMED** |
| **Active DMA Engine** | `idma64` loaded | `lsmod` & `journalctl` confirm 3 channels active | **CONFIRMED** |

### Final Finding for Critical Issue A:
The proposed single 10,246-byte SPI transfer is **100% natively supported** by the Intel LPSS SPI2 controller (`pxa2xx-spi`), the Intel `idma64` DMA engine, and the Linux SPI core subsystem without transfer splitting, buffer truncation, or chip-select deassertion glitches.
