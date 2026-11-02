# FTE3600 Linux Driver — Stage 2A Final Safety Gate: Pre-Execution Review

**Date/Time:** 2026-11-02T15:15:00+05:30  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Device:** `spi-FTE3600:00` on Intel GSPI #1 (`pxa2xx-spi.2`)  
**Audit Type:** Static Pre-Execution Safety Review (Zero Hardware Access)  
**Evaluated Documents:**
- `diagnostics/STAGE2A_MINIMAL_INITIALIZATION_DESIGN.md`
- `diagnostics/STAGE2_PROTOCOL_EVIDENCE_AUDIT.md`
- `diagnostics/STAGE1_GPIO_RESTORATION_INVESTIGATION.md`
- `PROGRESS.md`
- Reference Driver Source: `research/libfprint-fte3600/libfprint/drivers/fte3600/` (`ft93xx.c`, `factory_startup.c`, `fw9369_startup.c`, `fw9369_release.c`, `linux_board.c`)
- Mock Test Suite: `research/libfprint-fte3600/tests/` (`test-ft93xx-backend.c`, `test-fw9369-backend.c`, `test-fte3600-fw9369-release.c`)

---

## Executive Summary & Final Verdict

### Final Verdict: `READY FOR EXPLICIT USER AUTHORIZATION`

### Verdict Justification
1. **Definitive Sequence Boundaries:**  
   The proposed minimal identification sequence has been isolated to exactly **one** 4-byte write (`write_byte 0xC6, 0x01`), a 4 ms inter-transaction delay with Chip Select deasserted, a 5-byte read verification (`read_byte 0xC6`), a 10-byte Chip ID read (`read_word 0x1A8B`), and a conditional 10-byte sub-revision check (`read_word 0x1816` if ID is `0x9391`).
2. **Evidence-Qualified Register Assessment:**  
   In the reference driver codebase and test mocks, register `0xC6` is an SFR separate from the OTP/EFUSE programming engine (mapped to registers `0xF1`, `0xF4`, `0xF6`, `0xF7`). There is no public hardware documentation proving internal silicon circuitry; based on reference code, writing `0xC6` via opcode `0x09` is not observed to interact with non-volatile memory.
3. **Strict Deterministic Failure Handling:**  
   All failure conditions (transport errors, acknowledgment mismatch, all-zeros bus, all-ones bus, CRC mismatch, unknown ID, sub-revision failure) enforce a mandatory **STOP** rule. No loops, no retries, no GPIO reset toggling, and no recovery writes will be executed.
4. **Transparent Inferred Residual State Disclosure:**  
   Host-side rollback cleanly frees all kernel modules, device nodes, and sysfs leases. It is explicitly disclosed that register `0xC6` is inferred to remain set to `0x01` inside the sensor hardware until the next system reboot or cold power cycle, as retention behavior is undocumented.
5. **Zero Execution Mandate:**  
   This safety review is static. **No hardware commands, module operations, or bus transactions have been executed.** Execution remains completely locked pending explicit user authorization.

---

## 1. Register Volatility Analysis (`0xC6`)

To ensure rigorous analysis, the evidential basis for register `0xC6` must be strictly categorized, distinguishing what reference code demonstrates from what hardware documentation proves:

### 1.1 Evidential Classification Matrix
| Category | Finding / Evidence | Evidential Status |
| :--- | :--- | :---: |
| **Explicit Hardware Datasheet** | **ABSENT.** No official FocalTech silicon datasheet, errata, or register specification is publicly available. | Unverified by Hardware Documentation |
| **Reference-Driver Comments** | **ABSENT.** Driver sources (`ft93xx.c`, `factory_startup.c`) do not contain docstrings or comments defining the physical silicon structure of `0xC6`. | Unverified by Documentation |
| **Structural & Architectural Context** | In [`test-ft93xx-backend.c:155`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/tests/test-ft93xx-backend.c#L155), `0xC6` is modeled in the `mock.sfr[]` (Special Function Register) array. In [`test-fw9369-backend.c:311-320`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/tests/test-fw9369-backend.c#L311-L320), sleeping hardware accepts writes to `0xC6` without awakening non-volatile subsystems. | Demonstrated in Reference Code / Tests |
| **Power/Reset Handling Inference** | Every reference driver startup implementation unconditionally writes `0xC6 = 1` on every initialization attempt. If `0xC6` were non-volatile and retained across power removal, re-asserting it on every cold boot would be redundant. | Inferred from Code Structure |
| **OTP/EFUSE Segregation** | Non-volatile fuse blowing in FocalTech architecture requires explicit sequencing via registers `0xF1`, `0xF4`, `0xF6`, and `0xF7` ([`ft93xx.c:596-610`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/ft93xx.c#L596-L610), [`discovery.c:63-81`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/discovery.c#L63-L81)). Register `0xC6` is outside this address range and uses standard opcode `0x09` (`write_byte`), which lacks programming charge-pump controls. | Demonstrated in Reference Code |

### 1.2 Qualified Volatility Assessment
Register `0xC6` is **inferred to be volatile** based on structural, architectural, and operational evidence across four independent reference implementations. It cannot be termed "proven volatile" by vendor datasheet text because no public datasheet exists. In available reference sources and test mocks, writing `0xC6 = 0x01` is an SFR access distinct from OTP registers (`0xF1/0xF4/0xF6/0xF7`); however, in the absence of authoritative vendor silicon errata or schematics, absolute claims of zero risk cannot be proven from hardware documentation alone.

---

## 2. Delay Requirement Analysis (4 ms Inter-Transaction Delay)

### 2.1 Origin of the 4 ms Delay
- **Datasheet Citation:** None available.
- **Reference-Driver Evidence:** The exact integer delay `4 ms` appears across four independent driver implementations in the codebase:
  1. [`ft93xx.c:509`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/ft93xx.c#L509): `wait_ms (op, 4);`
  2. [`factory_startup.c:174`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/factory_startup.c#L174): `wait_ms (op, 4);`
  3. [`fw9369_startup.c:331`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L331): `wait_ms (op, 4);`
  4. [`fw9369_release.c:433`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_release.c#L433): `io->wait_ms (io->context, 4);`
  5. Verified in unit test trace assertion: [`tests/test-fte3600-fw9369-release.c:388`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/tests/test-fte3600-fw9369-release.c#L388) (`CHECK(e[1].delay == 4 && e[2].length == 5 && e[2].data[2] == 0xc6);`).
- **Implementation Mechanism:** In Linux userspace, `wait_ms(4)` maps to `usleep(4000)` ([`linux_board.c:165`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/linux_board.c#L165)).
- **Confidence Level:** **Empirical Choice in Reference Driver.** The delay is an operational choice established by the reference driver implementation, presumably accommodating transceiver clock/PLL settling. It is not confirmed by hardware documentation.

---

## 3. SPI Transaction Integrity

### 3.1 Chip-Select (CS) Continuity and Transaction Independence
Each operation is submitted to `/dev/spidev1.0` via standard ioctl `SPI_IOC_MESSAGE(1)` ([`linux_board.c:427-440`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/linux_board.c#L427-L440)):
- **Transaction 1 (`write_byte 0xC6, 0x01`):**
  - Length: 4 bytes (`09 F6 C6 01`).
  - CS Line: Asserted low before byte 0; held low for all 4 bytes; **deasserted high** immediately after byte 3.
- **Inter-Transaction Delay (4 ms):**
  - **CS Line: DEASSERTED HIGH** throughout the entire 4 ms wait. No clock pulses are generated.
- **Transaction 2 (`read_byte 0xC6`):**
  - Length: 5 bytes (`08 F7 C6 00 00`).
  - CS Line: Asserted low before byte 0; held low for all 5 bytes; **deasserted high** after byte 4.
- **Transaction 3 (`read_word 0x1A8B`):**
  - Length: 10 bytes (`04 FB 9A 8B 00 00 00 00 00 00`).
  - CS Line: Asserted low before byte 0; held continuously low across all 10 bytes (header, turnaround, payload, CRC); **deasserted high** after byte 9.
- **Transaction 4 (`read_word 0x1816` — Conditional):**
  - Length: 10 bytes (`04 FB 98 16 00 00 00 00 00 00`).
  - CS Line: Separate transaction; asserted low for 10 bytes, then deasserted high.

### 3.2 Response Offsets & CRC Validation Rules
- `read_byte 0xC6`: Data returned in `buffer[4]`. Expected value: `0x01`.
- `read_word 0x1A8B`:
  - Bytes 0–5: Header and turnaround dummy cycles.
  - Bytes 6–7: Data word (`(buffer[6] << 8) | buffer[7]`).
  - Bytes 8–9: Big-endian CRC-CCITT (`(buffer[8] << 8) | buffer[9]`).
  - CRC Algorithm: Polynomial `0x1021`, seed `0xFFFF`, evaluated over bytes 6 and 7 only.
- **Zero-CRC Exception Clarification:**  
  In [`ft93xx.c:209`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/ft93xx.c#L209), the reference driver checks:
  ```c
  if (buffer[8] != 0 || buffer[9] != 0) { ... verify crc ... }
  ```
  **Critical Distinction:** Bypassing CRC verification when `buffer[8..9] == 0x0000` is strictly **reference-driver behavior**, not proof that the response is valid. Zero bytes could result from an unconfigured CRC generator, an unclocked output buffer, or an invalid bus state. A zero-CRC response must be recorded as an unverified response.

---

## 4. Deterministic Failure Handling Specification

Unlike the reference driver (which retries operations up to 4 or 31 times), this safety gate enforces a **ZERO-RETRY, STRICT STOP** policy on any anomaly:

| Failure Mode | Trigger Condition | Deterministic Action | Prohibited Actions |
| :--- | :--- | :--- | :--- |
| **Write Transport Error** | `ioctl(SPI_IOC_MESSAGE)` returns `< 0` or `!= 4` on `0xC6` write. | **STOP.** Log errno. Abort to rollback. | ❌ No retries. ❌ No reset toggle. ❌ No recovery writes. |
| **Verify Read Transport Error** | `ioctl` returns `< 0` or `!= 5` on `0xC6` read. | **STOP.** Log errno. Abort to rollback. | ❌ No retries. ❌ No reset toggle. |
| **Verification Value Mismatch** | `buffer[4] != 0x01` on `0xC6` read (e.g. returns `0x00` or `0xFF`). | **STOP.** Log received byte. Abort to rollback. | ❌ No retry loop. ❌ No recovery writes. ❌ No idle commands. |
| **Chip-ID All-Zeroes Bus** | `buffer[6..7] == 0x0000`. | **STOP.** Log bus grounded / inactive. Abort to rollback. | ❌ No retries. ❌ No GPIO toggling. |
| **Chip-ID All-Ones Bus** | `buffer[6..7] == 0xFFFF`. | **STOP.** Log bus floating / unpowered. Abort to rollback. | ❌ No retries. ❌ No GPIO toggling. |
| **CRC Mismatch** | `computed_crc != received_crc` (and received CRC `!= 0x0000`). | **STOP.** Flag packet corrupted. Abort to rollback. | ❌ Do not discard and re-read. ❌ No retries. |
| **Unknown Chip ID** | ID is valid non-zero/non-FFFF word, but not in supported table. | **STOP.** Log observed raw ID. Abort to rollback. | ❌ Do not attempt sub-revision read. ❌ No idle commands. |
| **Sub-Revision Read Failure** | Transport error, CRC mismatch, or timeout on `0x1816`. | **STOP.** Log partial identification (`0x9391`). Abort to rollback. | ❌ No retries. ❌ No GPIO toggling. |

**Universal Post-Failure Sequence:**  
On any STOP trigger, the test harness immediately invokes the proven Stage 1 cleanup routine:
1. Close `/dev/spidev1.0`.
2. Unbind `spi-FTE3600:00` from `spidev`.
3. Clear `driver_override`.
4. Unload `spidev`.
5. Remove `fte3600.ko`.
6. Unload `uio` (if loaded by script).

---

## 5. Reset and Teardown: Residual Hardware State

### 5.1 Host-Side Resources
The Stage 1 rollback harness is **100% sufficient** to release all host-side resources:
- All character devices (`/dev/spidev1.0`, `/dev/uio0`, `/dev/gpiochip1`) are destroyed.
- All kernel modules (`fte3600`, `spidev`, `uio`) are unloaded from memory.
- GSPI controller #1 (`pxa2xx-spi.2`) is idle with Chip Select held high.
- Host GPIO pins 14 (reset) and 323 (Pad 229, IRQ) are released and return to high-impedance input mode.

### 5.2 Sensor Hardware Residual State Disclosure
- **Inferred Residual State:**  
  When register `0xC6` is set to `0x01`, internal digital logic ungates the sensor's SPI transceiver clock. Because Stage 2A avoids pulsing hardware reset Pad 14 and does not power-cycle the host, **register `0xC6` is inferred to remain set to `0x01` inside the sensor**. Authoritative documentation confirming retention duration or autonomous timeout does not exist.
- **Established Physical State:**  
  Only host-side conditions are established: Chip Select is deasserted high and host-side driver/SPI resources have been fully released. The sensor's internal electrical state when idle with CS high is not defined by published hardware documentation.
- **Clearing Inferred Residual State:**  
  Returning the sensor silicon to its unclocked cold baseline is inferred to require a host system shutdown / cold power cycle.

---

## 6. Sensor Variant Safety & Evidence Reassessment

### 6.1 Supported Silicon Families (Supported by Direct Source Evidence)
The minimal sequence (`0xC6=1` -> 4ms -> read `0xC6` -> read `0x1A8B`) is supported by direct source code evidence for the following variants only:
1. **`FT9362` / `FW9369` (Chip ID `0x9362`):**  
   Directly evidenced in [`fw9369_startup.c:320-345`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c#L320-L345), [`fw9369_release.c:425-445`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_release.c#L425-L445), and test suite `test-fw9369-backend.c`.
2. **`FT9365` (Chip ID `0x9365`):**  
   Directly evidenced in [`factory_startup.c:161-224`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/factory_startup.c#L161-L224) and test suite `test-ft93xx-backend.c:212`.
3. **`FT9391` / `FT9395` (Base Chip ID `0x9391`):**  
   Directly evidenced in [`ft93xx.c:543-558`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/ft93xx.c#L543-L558). Base ID `0x9391` with sub-revision read of `0x1816` (`0x0FFF` identifying FT9395).

### 6.2 Variants Lacking Dedicated Sequence Evidence
Variants such as **`FT9349`**, `FT9363`, `FT9372`, `FT9396`, `FT9397`, and `FT9398` appear in general profile tables ([`ft93xx.c:12-23`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/ft93xx.c#L12-L23)), but lack dedicated initialization trace evidence or test mocks in the reference repository.  
**Strict Rule:** If register `0x1A8B` returns any ID other than `0x9362`, `0x9365`, or `0x9391`, the test harness must treat it as an **UNKNOWN / UNPROVEN VARIANT and immediately STOP**.

### 6.3 OEM Driver Strings vs Hardware Reality
Windows driver strings (`FtRansacEdage_32f_FT9391`) prove only OEM software bundle package contents. They do not prove what silicon is soldered to this laptop motherboard.

---

## 7. Pre-Execution Checklist & Sign-Off

- [x] Register `0xC6` claims qualified: SFR context demonstrated in code; lack of hardware documentation acknowledged.
- [x] 4 ms delay documented as empirical reference-driver choice.
- [x] SPI transactions verified as separate messages with CS deasserted during delays.
- [x] Frame length (10 bytes), turnaround cycles (2 bytes), and CRC math verified.
- [x] CRC zero-value bypass clarified as driver heuristic, not hardware integrity proof.
- [x] Zero-retry, deterministic STOP policy defined for all failure modes.
- [x] Inferred residual hardware state documented; unverified phrases removed.
- [x] Supported variants restricted to `FT9362`, `FT9365`, `FT9391`, `FT9395`; all others mandate STOP.
- [x] Strict zero-hardware-execution boundary maintained.

---

### Conclusion & Final Recommendation

The minimal identification sequence is thoroughly documented with evidence-qualified claims and deterministic failure boundaries.

**Recommendation:** Refer to [STAGE2A_EXECUTION_CHECKLIST.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2A_EXECUTION_CHECKLIST.md) for the complete operational checklist. Hardware access remains locked until the user explicitly reviews these findings and issues separate authorization.
