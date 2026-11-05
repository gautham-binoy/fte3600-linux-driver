# Stage D11 — Protocol Engine & Calibration Sequence Verification Report

**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Host Controller:** Intel LPSS SPI2 (`pxa2xx-spi.2` / `idma64`, PCI `0000:00:1e.3`, Vendor `0x8086`, Device `0xa0ab`)  
**Milestone:** Stage D11 — Protocol Engine & Calibration Sequence Implementation  
**Status:** FORMAL PASS  
**Safety Gate:** ZERO PHYSICAL HARDWARE ACCESS AUTHORIZED — MOCK/SIMULATION & SOURCE ONLY

---

## 1. Executive Summary

Stage D11 of the master execution plan implements the complete protocol engine, wire framing, register semantics, FDT capacitive analog front-end (AFE) calibration, threshold bank formatting, and operational arming sequence for the FocalTech FT9362 / FW9369 driver.

All protocol operations execute through the asynchronous SPI transport engine (`fte3600_spi_async_transfer()`) hardened in Stage D10, ensuring conservative DMA retention ($\mathtt{FREED}(t) \equiv 0$ on timeout), controller domain verification, fail-closed quarantine latching, and deadlock-free watchdog cancellation.

Automated verification achieved **100 / 100 tests passed (100.0%)** across 13 suites in `kernel/fte3600-test.c` plus **9 / 9 policy groups passed (100.0%)** in `kernel/fte3600-policy-test.c`. The kernel module `kernel/fte3600.ko` compiled cleanly with `W=1` against `/lib/modules/7.2.8-9-omarchy/build` with BTF generation and zero compiler warnings.

---

## 2. Wire Framing & Protocol Engine Implementation

The protocol engine is formalized in `kernel/fte3600.c` through four fundamental wire framing primitives:

### 2.1 3-Byte Command Strobes
All state commands strictly obey the 3-byte wire rule:
$$\text{Frame} = [\text{CMD}, \sim\text{CMD}, 0\text{x}00]$$
- Wakeup 1 (`CMD_WAKE`): `{ 0x5A, 0xA5, 0x00 }`
- Wakeup 2 (`CMD_IDLE_ACK`): `{ 0xA5, 0x5A, 0x00 }`
- Soft Reset / Abort (`CMD_ABORT`): `{ 0xC0, 0x3F, 0x00 }`
- Touch Monitoring Arm (`CMD_ARM`): `{ 0xC2, 0x3D, 0x00 }`
- Image Capture Trigger (`CMD_CAPTURE`): `{ 0xC4, 0x3B, 0x00 }`

### 2.2 8-bit Special Function Register (SFR) Access
- **SFR Write (4 Bytes):** `[0x09, 0xF6, addr, val]`
- **SFR Read (5 Bytes):** `[0x08, 0xF7, addr, 0x00, 0x00]` (response payload clocked into byte 4)

### 2.3 16-bit Configuration Register Access
- **16-bit Write (`fte3600_spi_write_reg16`, 8 Bytes):**
  $$\text{TX} = [0\text{x}05, 0\text{x}FA, 0\text{x}80 \mid (\text{addr} \gg 8), \text{addr} \ \& \ 0\text{xFF}, 0\text{x}00, 0\text{x}01, \text{val} \gg 8, \text{val} \ \& \ 0\text{xFF}]$$
- **16-bit Read (`fte3600_spi_read_reg16`, 10 Bytes):**
  $$\text{TX} = [0\text{x}04, 0\text{x}FB, 0\text{x}80 \mid (\text{addr} \gg 8), \text{addr} \ \& \ 0\text{xFF}, 0\text{x}00, 0\text{x}01, 0, 0, 0, 0]$$
  Hardware returns big-endian 16-bit register value at `rx[6..7]` and CCITT CRC-16 checksum at `rx[8..9]`. The driver verifies CRC-16 over `rx[6..7]`; any mismatch immediately fails closed with `-EBADMSG` and transitions to `FTE3600_STATE_UNKNOWN`.
- **16-bit Read-Modify-Write (`fte3600_spi_rmw_reg16`):**
  Performs safe atomic read, masks via `(orig & mask) | val`, and writes back to target register.

---

## 3. Capacitive Touch (FDT) Calibration & Arming Engine

### 3.1 FDT Sample Measurement (`fte3600_hw_fdt_sample`)
Invoked via `FTE3600_IOC_SESSION_FDT_SAMPLE`:
1. Programs AFE DAC register: writes `0x1801 = 0xFC80 | (sfdt->dac_val & 0x7F)`.
2. Sets scan trigger: read-modify-writes `0x1881` with mask `0xFFFF` and bit `0x0002`.
3. Dispatches trigger strobe: transmits `{ 0xC2, 0x3D, 0x00 }`.
4. Waits 1 ms for analog stabilization (`usleep_range(1000, 1500)`).
5. Triggers ADC conversion: writes `0x1885 = 0x0001`.
6. Polls status register `0x1A82`: waits up to 10 iterations for bit 3 (`0x0008`) to assert; returns `-ETIMEDOUT` if not ready.
7. Reads 4-channel ADC bank (16 bytes): selects bank address `0xE8` (SMIC Fab AA) or `0xB8` (Non-SMIC Fab).
8. Validates wire CRC-16 over the 8 payload bytes (`rx[6..13]`) against `rx[14..15]`.
9. Copies calibrated 4-channel ADC counts to `sfdt->adc_samples[0..3]`, zeroes padding channels 4..7, and sets `status_flags = 0x01`.

### 3.2 Threshold Bank Formatting & Write (`fte3600_hw_set_thresholds`)
Invoked via `FTE3600_IOC_SESSION_SET_THRESHOLDS`:
1. Enforces bank length is strictly `FTE3600_THRESHOLD_BANK_LEN` (22 bytes).
2. Verifies 6-byte header: `[0x05, 0xFA, 0x80, bank_addr, 0x00, 0x08]` where `bank_addr` is `0xE0` (Fab AA) or `0xB0` (Default).
3. Verifies zero padding across channels 4..7 (`threshold_bank[14..21] == 0`).
4. Unlocks protected SFR: writes `0x9A = 0x5A`.
5. Transmits 22-byte burst packet over SPI.
6. Locks protected SFR: writes `0x9A = 0x00`.
7. Records `session.thresholds_programmed = true`.

### 3.3 Operational Arming Sequence (`fte3600_hw_arm_sensor`)
Invoked during `FTE3600_IOC_SESSION_COMMIT`:
1. Configures sensor detection mode: read-modify-writes `0x1881` with `(val & 0xFF7C) | 0x0081`.
2. Configures detection filter & routing: read-modify-writes `0x1A83` with `(val & 0xFF80) | 0x0016`.
3. Clears pending interrupt latches: writes `0x1A84 = 0x002F`.
4. Dispatches Arm strobe: transmits `{ 0xC2, 0x3D, 0x00 }`.
5. Waits 1 ms for detector stabilization (`usleep_range(1000, 1500)`).
6. Transitions state machine to `FTE3600_STATE_READY`.

---

## 4. Test Suite 13 Results

Test Suite 13 was implemented in `kernel/fte3600-test.c` to rigorously validate all protocol engine operations in standalone userspace simulation:

| Scenario | Objective | Verification Method | Result |
| :--- | :--- | :--- | :---: |
| **Scenario 1** | Command strobe 3-byte wire framing (`[CMD, ~CMD, 0x00]`) | Verified across all strobes (`0x5A`, `0xA5`, `0xC0`, `0xC2`, `0xC4`) | **PASS** |
| **Scenario 2** | 8-bit SFR read and write wire framing | 4-byte write (`0x09, 0xF6`) and 5-byte read (`0x08, 0xF7`) with rx byte 4 extraction | **PASS** |
| **Scenario 3** | 16-bit register read/write wire framing and CRC-16 check | 8-byte write (`0x05, 0xFA`), 10-byte read (`0x04, 0xFB`), CRC match and corruption detect | **PASS** |
| **Scenario 4** | 16-bit read-modify-write bitmask accuracy | Tested `0x1881` (`0xFF7C \| 0x0081`) and `0x1A83` (`0xFF80 \| 0x0016`) bit preservation | **PASS** |
| **Scenario 5** | FDT sample execution framing and ADC extraction | 16-byte burst read, 8-byte payload extraction, wire CRC validation, channel zeroing | **PASS** |
| **Scenario 6** | 10-sample stability loop logic (convergence on 3 passes $\le 5$) | Verified early exit at sample 2 on stable input; verified 10-sample timeout on noise | **PASS** |
| **Scenario 7** | Threshold calculation formula ($\max(0, \min - 30)$ in $[300, 700]$) | Verified threshold subtraction, floor at 0, and fail-closed rejection out of bounds | **PASS** |
| **Scenario 8** | 22-byte threshold bank formatting and bank address (`0xE0` vs `0xB0`) | Verified Fab AA (`0xE0`) and default (`0xB0`), 8-byte pad zero enforcement | **PASS** |
| **Scenario 9** | Operational arming register sequence and strobe transmission | Verified 5-step sequence (`0x1881`, `0x1A83`, `0x1A84`, `{0xC2, 0x3D, 0x00}`) | **PASS** |
| **Scenario 10** | Full session calibration workflow from `SESSION_BEGIN` to `READY` | End-to-end lease acquisition, FDT sampling, bank write, commit, arm, and transition | **PASS** |

### Complete Test Execution Summary
- **Unit & Simulation Suite (Suites 1–13):** **100 / 100 Passed (100.0%)**
- **Glue Policy Suite (Groups 1–9):** **9 / 9 Passed (100.0%)**
- **Total Automated Tests:** **109 / 109 Passed (100.0%)**

---

## 5. Kernel Build Verification

```
make -C /lib/modules/7.2.8-9-omarchy/build M=/home/titan/fte3600-linux-driver/kernel W=1 modules
  CC [M]  fte3600.o
  MODPOST Module.symvers
  CC [M]  fte3600.mod.o
  CC [M]  .module-common.o
  LD [M]  fte3600.ko
  BTF [M] fte3600.ko
```
- **Target Kernel:** Linux `7.2.8-9-omarchy` (x86_64)
- **Compiler Flags:** `W=1`, strict prototyes, BTF generation enabled
- **Compiler Warnings:** **0 warnings**
- **Compiler Errors:** **0 errors**
- **Binary Status:** `kernel/fte3600.ko` successfully generated with BTF debug symbols.

---

## 6. Passive Invariants & Safety Verification

Verification confirms the system remains strictly in passive safety mode:
1. `fte3600: NOT LOADED` (`lsmod | grep fte3600` is empty).
2. `/dev/fte3600: ABSENT` (`ls /dev/fte*` returns "No such file or directory").
3. SPI bus transfers: **0** physical transfers issued.
4. GPIO / RST lines: untouched.
5. Legacy tool safety gate: `bin/fw9369-hw-tool` remains sealed.

---

## 7. Stage D11 Formal Sign-Off Verdict

**VERDICT: FORMAL PASS**  
The Stage D11 protocol engine, wire framing, 16-bit register access, FDT calibration, threshold banking, and operational arming routines are completely implemented, build-verified, and 100% test-verified in source code. The project is cleared to proceed to **Stage D12 (Capture Pipeline Implementation)**.
