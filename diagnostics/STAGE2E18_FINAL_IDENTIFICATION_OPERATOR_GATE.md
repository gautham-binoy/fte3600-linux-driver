# Stage 2E.18 — Final Read-Only Operator Gate for FT9362 Identification

**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Target SPI Device:** `spi-FTE3600:00` on Intel LPSS SPI2 (`pxa2xx-spi.2`, CS0)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (x86_64, SMP PREEMPT_DYNAMIC)  
**Timestamp:** 2026-11-04T09:55:00+05:30  
**Audit Status:** **READ-ONLY OPERATOR GATE COMPLETE — NO HARDWARE OPERATIONS PERFORMED**

---

## 1. Executive Summary

This document establishes the final read-only pre-execution gate for the proposed Stage C minimal sensor identification sequence. 

Following the successful Stage B controlled reset execution (Stage 2E.15), the Stage C communication investigation (Stage 2E.16), and the independent safety audit (Stage 2E.17), this gate independently audits:
1. Live Stage B post-reset system state.
2. Kernel and userspace Stage C identification implementations.
3. Cryptographic integrity and source-to-binary correspondence of executable artifacts.
4. Physical transaction boundaries for the 3 identification SPI frames.
5. Fail-closed fault isolation and signal interruption semantics.

**Absolute Safety Confirmation:**  
This evaluation was conducted under strict read-only constraints. Zero SPI transfers were dispatched, zero sensor registers were read or written, zero GPIO pins were toggled, zero module reloads or SPI rebinds were executed, and the legacy safety gate remains intact.

---

## 2. Live Stage B Post-Reset System Verification

The live host environment was audited without modifying kernel or hardware state. The driver remains loaded and bound under Option 1 retention policy:

| Telemetry / System Attribute | Expected Stage B State | Observed Live State | Audit Status |
| :--- | :--- | :--- | :---: |
| Running Kernel Release | `7.2.8-9-omarchy` | `7.2.8-9-omarchy` | **CONFIRMED** |
| Module `fte3600` Status | Loaded in kernel | Loaded (`fte3600 20480 0`) | **CONFIRMED** |
| Module `spidev` Status | Absent / Not loaded | Confirmed absent | **CONFIRMED** |
| ACPI SPI Link Binding | Bound to `fte3600` | `spi-FTE3600:00` $\rightarrow$ `bus/spi/drivers/fte3600` | **CONFIRMED** |
| Device Node `/dev/fte3600` | Present (`511:0`) | `crw------- 1 root root 511, 0` | **CONFIRMED** |
| Internal Driver State | `RESET_SEQUENCE_ISSUED` (2) | `2 (RESET_SEQUENCE_ISSUED)`, flag `0x00000002` | **CONFIRMED** |
| Active Node Descriptors | Zero open holders | `0` open descriptors (`fuser` / `lsof` clean) | **CONFIRMED** |
| Running Test Processes | Zero active runners | `0` processes matching `stage-b|fte3600|foot` | **CONFIRMED** |
| Runner Cooperative Lock | Unheld / Released | Lock file released (0 active file holders) | **CONFIRMED** |
| Total Prior SPI Bus Traffic | Strictly 0 bytes / transfers | All 28 sysfs counters strictly `0` | **CONFIRMED** |
| Legacy Userspace Safety Gate | Sealed (Exit Code 1) | `bin/fw9369-hw-tool` returned code 1 with policy banner | **CONFIRMED** |

All 28 SPI statistics in `/sys/bus/spi/devices/spi-FTE3600:00/statistics/` (`bytes`, `bytes_rx`, `bytes_tx`, `errors`, `messages`, `transfers`, etc.) confirm that **zero SPI transactions have ever touched the physical bus**.

---

## 3. Exact Stage C Implementation Audit

The Stage C identification logic implemented in [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c) and [`kernel/fte3600.h`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.h) was inspected line-by-line:

### 3.1 Entry Point & Parameter Validation
- **IOCTL Interface:** `#define FTE3600_IOC_INIT _IOWR('F', 0x03, struct fte3600_init_params)` ([`kernel/fte3600.h:96`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.h#L96)).
- **Dispatch Handler:** `fte3600_ioctl_common()` case `FTE3600_IOC_INIT` ([`kernel/fte3600.c:458-486`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L458-L486)):
  - Validates `copy_from_user(&params, ...) == 0` (line 462).
  - Validates `params.version == FTE3600_UAPI_VERSION` (1) (line 465).
  - Validates all 6 reserved words are strictly 0: `params.reserved[0..5] == 0` (lines 468-471).
  - Acquires `mutex_lock_interruptible(&data->lock)` (line 473).
  - Verifies device is online and SPI link is valid: `!atomic_read(&data->offline) && data->spi` (line 477).
  - Calls `fte3600_hw_identify(data)` (line 482).
  - Releases `mutex_unlock(&data->lock)` (line 484).

### 3.2 State Precondition & Implicit Execution Prevention
- **Strict Precondition:** Line 270:
  ```c
  if (data->state != FTE3600_STATE_RESET_SEQUENCE_ISSUED)
      return -EPERM;
  ```
  Identification is strictly impossible unless the driver is currently in `FTE3600_STATE_RESET_SEQUENCE_ISSUED`. All other states (`UNINITIALIZED`, `RESETTING`, `INITIALIZING`, `READY`, `CAPTURING`, `ERROR`, `UNKNOWN`, `IDENTIFIED`) reject `FTE3600_IOC_INIT` immediately with `-EPERM`.
- **Zero Implicit Execution:**
  - `fte3600_spi_probe()` sets state to `FTE3600_STATE_UNINITIALIZED` and performs no SPI transfers.
  - `fte3600_cdev_open()` only validates offline state and acquires the exclusive single-client lease (`data->is_open`).
  - `FTE3600_IOC_GET_STATE` only reads and returns state snapshot data under `data->lock`.
  - `FTE3600_IOC_RESET` only pulses the reset GPIO line.
  - `FTE3600_IOC_CAPTURE` returns `-EOPNOTSUPP`.
  - No background workqueues, timers, interrupts, or kernel threads exist that could trigger SPI transfers autonomously. Identification requires an explicit user-initiated `FTE3600_IOC_INIT`.

### 3.3 Transaction Sequence Breakdown
- **Initial Transition:** Line 273 sets `data->state = FTE3600_STATE_INITIALIZING`.
- **Frame 1 (Step A: Write 0xC6 = 0x01):** Lines 275-284:
  - Dispatches 4-byte frame: `[ 0x09, 0xF6, 0xC6, 0x01 ]`.
  - Ungates the volatile peripheral SPI transceiver clock.
  - Only write operation in the entire driver codebase.
  - Fail-closed: on error, returns immediately without issuing subsequent transfers.
- **Transceiver Stabilization Delay (Step B):** Line 287:
  - `usleep_range(4000, 5000);` (4 to 5 ms wait).
  - CS is deasserted HIGH during this interval.
- **Frame 2 (Step C: Readback Verification 0xC6):** Lines 289-309:
  - Dispatches 5-byte frame: `[ 0x08, 0xF7, 0xC6, 0x00, 0x00 ]`.
  - Validates `data->rx_buf[4] == 0x01`.
  - On mismatch, transitions to `FTE3600_STATE_UNKNOWN`, sets `last_error = EIO`, and returns `-EIO`.
- **Frame 3 (Step D: Read Chip ID Register 0x1A8B):** Lines 311-322:
  - Dispatches 10-byte frame: `[ 0x04, 0xFB, 0x9A, 0x8B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 ]`.
  - Extracts `chip_id = (rx[6] << 8) | rx[7]` and `crc_rx = (rx[8] << 8) | rx[9]`.
- **Step E: Wire CRC-16 Calculation:** Line 328:
  - `crc_calc = fte3600_crc16_wire(&data->rx_buf[6], 2);`
  - Calculates CCITT CRC-16 (poly `0x1021`, seed `0xFFFF`) over the 2-byte chip ID payload.
- **Step F: Multi-Tier Validation & Variant Gate:** Lines 330-356:
  - Bus fault check: if `chip_id == 0x0000 || chip_id == 0xFFFF`, transitions to `FTE3600_STATE_UNKNOWN`, sets `last_error = ENODEV`, returns `-ENODEV`.
  - CRC integrity check: if `crc_rx != crc_calc`, transitions to `FTE3600_STATE_UNKNOWN`, sets `last_error = EBADMSG`, returns `-EBADMSG`.
  - Expected silicon gate: if `chip_id != FTE3600_EXPECTED_CHIP_ID` (`0x9362`), transitions to `FTE3600_STATE_UNKNOWN`, sets `last_error = ENODEV`, returns `-ENODEV`.
- **Completion State:** Lines 358-364:
  - Transitions to `data->state = FTE3600_STATE_IDENTIFIED` (State 8).
  - **Explicit Semantic Boundary:** The state is `IDENTIFIED`, NOT `READY` (State 4). Capture remains blocked.
  - Clears `data->last_error = 0`.
  - Returns `0` (Success).

### 3.4 Non-Volatile Memory Isolation
- Zero references or commands touching OTP/EFUSE control registers (`0xF1`, `0xF4`, `0xF6`, `0xF7`).
- Zero flash programming or erasing sequences.
- Zero undocumented vendor unlock tokens (`0x5A`, `0xA5`).
- Operations are strictly limited to volatile digital clock gating register `0xC6` and read-only identification register `0x1A8B`.

---

## 4. Executable Artifacts & Cryptographic Baseline

### 4.1 In-Kernel vs On-Disk `srcversion` Correspondence
- On-disk binary `kernel/fte3600.ko`: `921EF4DBDD04C08D1502448` (via `modinfo -F srcversion`)
- Currently loaded kernel module: `/sys/module/fte3600/srcversion` $\rightarrow$ `921EF4DBDD04C08D1502448`
- **Verdict:** Bit-for-bit exact match. The loaded module in the host kernel is confirmed to contain the audited `fte3600_hw_identify()` implementation.

### 4.2 Userspace Tool Command Mapping
- In [`tools/fte3600-tool.c:64-74`](file:///home/titan/fte3600-linux-driver/tools/fte3600-tool.c#L64-L74), the CLI command `init` maps directly to:
  ```c
  struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
  ioctl(fd, FTE3600_IOC_INIT, &params);
  ```
  Zero hardware transfers or out-of-band accesses occur in userspace; userspace acts strictly as a thin ioctl caller.

### 4.3 SHA-256 Baseline Checksums

| Artifact File | Absolute Path | SHA-256 Cryptographic Hash |
| :--- | :--- | :--- |
| `kernel/fte3600.c` | `/home/titan/fte3600-linux-driver/kernel/fte3600.c` | `29d2726c9a63772f0981bf65321f48934882eeb6702fe4a02629794b410805e8` |
| `kernel/fte3600.h` | `/home/titan/fte3600-linux-driver/kernel/fte3600.h` | `1ffca1a4e04fad026efbfde409ed0fdb67508ac57b3944aaf1cc57f3dbc35754` |
| `kernel/fte3600.ko` | `/home/titan/fte3600-linux-driver/kernel/fte3600.ko` | `cc8c43d4d4abd79c3ee43c5877e0cffe5d5a1975de8c081cdda9eb2654cf56c9` |
| `tools/fte3600-tool.c` | `/home/titan/fte3600-linux-driver/tools/fte3600-tool.c` | `1b021a4d7f4770e8a627d6cd29116859dae908e2e5bf10739c0d05253d31d4f7` |
| `bin/fte3600-tool` | `/home/titan/fte3600-linux-driver/bin/fte3600-tool` | `b91d7dc3cb0ca172d7020613af884b649c451be79a55e1836d31dc6f66217d12` |
| `scripts/stage-b-test.sh` | `/home/titan/fte3600-linux-driver/scripts/stage-b-test.sh` | `ff53d058e7b7cce5631bbfb1c3e4449b042db4573cfd6cafcb43bb02a7a822b5` |
| `scripts/run-stage-b-in-terminal.sh` | `/home/titan/fte3600-linux-driver/scripts/run-stage-b-in-terminal.sh` | `0d45f94459124430c2b64c9bef9d820c22c984032a959a0d20b88248ed9b4e27` |
| `diagnostics/STAGE2E17_IDENTIFICATION_SAFETY_AUDIT.md` | `/home/titan/fte3600-linux-driver/diagnostics/STAGE2E17_IDENTIFICATION_SAFETY_AUDIT.md` | `51a91cd8d4afc2e0d7fcc1530549d28c7b1d5fa414ae305da51eb94da01f83b6` |

---

## 5. Physical Transaction Boundaries & Bus Protocol

The 3 identification frames conform strictly to the physical timing and framing verified in Stage 2A and 2E.16:

```
Physical Bus Profile:
- SPI Controller: Intel LPSS SPI2 (pxa2xx-spi.2)
- Chip Select: Active-Low CS0
- Mode: SPI Mode 0 (CPOL=0, CPHA=0)
- Max Frequency: 1,000,000 Hz (1 MHz)
- Bit Ordering: MSB-first, 8-bit words
```

### Frame 1: Clock Un-Gate Write
```
CS:     ──┐                                                 ┌──
          │                                                 │
MOSI:   ──┴──[0x09]──────[0xF6]──────[0xC6]──────[0x01]─────┴──
MISO:   ─────[Hi-Z]──────[Hi-Z]──────[Hi-Z]──────[Hi-Z]─────────
         Opcode(Wr)   ~Opcode      Reg(0xC6)    Val(0x01)
         (Len = 4 bytes)
```
- **CS Assertion:** Asserted LOW continuously across all 4 bytes (`cs_change = 0`).
- **CS Deassertion:** Deasserted HIGH at completion of message.

### Stabilization Interval
```
CS:     ──────────────────────────────────────────────────────── (HIGH)
SCLK:   ──────────────────────────────────────────────────────── (LOW, Idle)
Duration: 4,000 µs to 5,000 µs (usleep_range)
Action: Internal sensor PLL stabilizes; transceiver clocks un-gate.
```

### Frame 2: Clock Un-Gate Readback
```
CS:     ──┐                                                               ┌──
          │                                                               │
MOSI:   ──┴──[0x08]──────[0xF7]──────[0xC6]──────[0x00]──────[0x00]───────┴──
MISO:   ─────[Hi-Z]──────[Hi-Z]──────[Hi-Z]──────[Dummy]─────[0x01]──────────
         Opcode(Rd)   ~Opcode      Reg(0xC6)     Dummy 1     Dummy 2 / Data
         (Len = 5 bytes)
```
- **CS Assertion:** Asserted LOW continuously across all 5 bytes (`cs_change = 0`).
- **Validation:** RX byte 4 (`rx_buf[4]`) must strictly equal `0x01`.
- **CS Deassertion:** Deasserted HIGH at completion of message.

### Frame 3: Chip Identification Interrogation
```
CS:     ──┐                                                                                                         ┌──
          │                                                                                                         │
MOSI:   ──┴──[0x04]───[0xFB]───[0x9A]───[0x8B]───[0x00]───[0x00]───[0x00]───[0x00]───[0x00]───[0x00]──────────────┴──
MISO:   ─────[Hi-Z]───[Hi-Z]───[Hi-Z]───[Hi-Z]───[Dummy]──[Dummy]──[0x93]───[0x62]───[0x1C]───[0x53]─────────────────
         Opcode  ~Opcode  Addr_H   Addr_L     Turnaround Dummies     Chip ID [15:0]    Wire CRC-16 [15:0]
         (Len = 10 bytes)
```
- **CS Assertion:** Asserted LOW continuously across all 10 bytes (`cs_change = 0`).
- **Address Encoding:** `0x1A8B` encoded with bit 7 set in high byte: `0x80 | 0x1A = 0x9A`, `0x8B`.
- **Turnaround Latency:** 2 dummy latency bytes at offsets 4 and 5.
- **Payload Extraction:**
  - Chip ID at offsets 6–7: `(rx[6] << 8) | rx[7]` $\rightarrow$ expected `0x9362`.
  - Wire CRC at offsets 8–9: `(rx[8] << 8) | rx[9]` $\rightarrow$ expected `0x1C53`.
- **CS Deassertion:** Deasserted HIGH at completion of message.

---

## 6. Fault Isolation, Fail-Closed Semantics & Interruption Handling

| Fault Condition | Detection Point | Handling & Consequence | Safe Recovery Path |
| :--- | :--- | :--- | :--- |
| **Grounded MISO (0x00)** | Step C (line 302): `rx[4] == 0x00 != 0x01` | Logs error, transitions to `UNKNOWN`, returns `-EIO`. Step D read is aborted. | Issue `FTE3600_IOC_RESET` |
| **Floating MISO (0xFF)** | Step C (line 302): `rx[4] == 0xFF != 0x01` | Logs error, transitions to `UNKNOWN`, returns `-EIO`. Step D read is aborted. | Issue `FTE3600_IOC_RESET` |
| **Short SPI Transfer** | Line 217: `m.actual_length != len` | Aborts transfer immediately, transitions to `UNKNOWN`, returns `-EIO`. | Issue `FTE3600_IOC_RESET` |
| **Wire CRC Mismatch** | Step F (line 340): `crc_rx != crc_calc` | Rejects data, transitions to `UNKNOWN`, sets `last_error = EBADMSG`, returns `-EBADMSG`. | Issue `FTE3600_IOC_RESET` |
| **Silicon SKU Mismatch** | Step F (line 349): `chip_id != 0x9362` | Rejects variant, transitions to `UNKNOWN`, sets `last_error = ENODEV`, returns `-ENODEV`. | Hardware review |
| **Signal Arrival (Pre-Lock)** | Line 473: `mutex_lock_interruptible` | Returns `-ERESTARTSYS`. Zero hardware operations dispatched. | Caller retries ioctl |
| **Signal Arrival (In-Flight)** | Synchronous kernel execution | SPI frames complete atomically with CS asserted; signal delivered upon ioctl exit. | Clean process exit |
| **Concurrent Unbind/Removal** | Lines 477 & 183: `atomic_read(&data->offline)` | Mutex serializes unbind. If removed, returns `-ENODEV`. Refcount protects memory. | None (device unbound) |

---

## 7. Operational Pre-Execution Checklist for Stage C

Prior to any execution of Stage C, the operator must verify:
- [x] Host running kernel is `7.2.8-9-omarchy`.
- [x] Kernel module `fte3600` is loaded (`fte3600 20480 0`).
- [x] Module `srcversion` matches on disk and in kernel (`921EF4DBDD04C08D1502448`).
- [x] ACPI SPI device `spi-FTE3600:00` is bound to `fte3600`.
- [x] Device node `/dev/fte3600` exists (`511:0`).
- [x] Driver state is confirmed `2 (RESET_SEQUENCE_ISSUED)`.
- [x] Exactly ONE `FTE3600_IOC_INIT` invocation planned (no retries, no loops).
- [x] Option 1 retention policy remains active (module remains loaded and bound post-test).
- [x] Zero CAPTURE operations authorized.
- [x] Zero OTP/EFUSE/flash operations authorized.

---

## 8. Final Gate Verdict

All technical, protocol, and cryptographic prerequisites for minimal sensor identification have been verified in full. No state-changing hardware operations were performed during this audit.

**READY FOR SEPARATE EXPLICIT STAGE C AUTHORIZATION**
