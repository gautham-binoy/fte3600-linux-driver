# Stage 2B.1 — Sensor Register Safety & Protocol Verification Audit

**Document ID:** `STAGE2B1_REGISTER_SAFETY_AUDIT`  
**Date:** 2026-11-02  
**Target Hardware:** FocalTech FT9362 (FW9369 Architecture)  
**Host Controller:** Intel LPSS SPI (`pxa2xx-spi.2`) / Tiger Lake-UP3 PCH-LP GPIO  
**Audit Scope:** Deep static code audit of registers `0xF1`, `0xF3`, `0xF4`, `0xF6`, `0xF7`, protected register unlock `0x9A`, and bank writes.  
**Execution Gate:** Strictly static analysis. Zero hardware commands executed.

---

## 1. Audit of High-Index Registers (0xF1, 0xF3, 0xF4, 0xF6, 0xF7)

In previous project phases, registers in the range `0xF0`–`0xFF` were placed under a strict precautionary quarantine due to potential association with non-volatile OTP (One-Time Programmable) memory or EFUSE burning logic. This audit traces every occurrence of these registers across all available reference drivers.

### 1.1 Summary Table of Operations

| Register | Read/Write | Exact SPI Framing | Source File & Line | Documented Purpose | Target Memory Type | Persistence / Safety Impact | Classification |
| :---: | :---: | :--- | :--- | :--- | :--- | :--- | :---: |
| **`0xF1`** | Write | `[ 09 F6 F1 03 ]`<br>`[ 09 F6 F1 13 ]` | `fw9369_startup.c:349`<br>`ft93xx.c:606,616`<br>`discovery.c:74` | EFUSE / OTP address pointer | OTP Address Latch | Volatile address register. Does not modify persistent memory. | **VERIFIED WITH CONDITIONS** |
| **`0xF4`** | Write | `[ 09 F6 F4 C0 ]`<br>`[ 09 F6 F4 C1 ]` | `fw9369_startup.c:350-353`<br>`ft93xx.c:609,617-618`<br>`discovery.c:76,78` | EFUSE read clock / strobe sequencer | OTP Controller Control Latch | Volatile control strobe. Clocks internal OTP sense amps into shadow registers. | **VERIFIED WITH CONDITIONS** |
| **`0xF3`** | Read | `[ 08 F7 F3 00 00 ]` | `fw9369_startup.c:354`<br>`ft93xx.c:611,618`<br>`discovery.c:77` | EFUSE / OTP sensed data output buffer | OTP Sense Output Buffer | Read-only buffer. Discarded in `fw9369_startup.c`. Zero persistence. | **VERIFIED** |
| **`0xF6`** | Write | `[ 09 F6 F6 VAL ]` | `ft93xx.c:604,621`<br>*(NOT present in `fw9369_*.c`)* | Legacy EFUSE address write on FT9398/FT9769 | Legacy OTP Address Latch | Unused in FW9369 pathway. | **PROHIBITED** (in FT9362 scope) |
| **`0xF7`** | Read | `[ 08 F7 F7 00 00 ]` | `ft93xx.c:605,623`<br>*(NOT present in `fw9369_*.c`)* | Legacy EFUSE data read on FT9398/FT9769 | Legacy OTP Data Buffer | Unused in FW9369 pathway. | **PROHIBITED** (in FT9362 scope) |

---

### 1.2 Deep Trace: Registers 0xF1, 0xF4, and 0xF3

#### 1. Source Evidence in FW9369 Startup
In `research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c:348-356`:
```c
348:  for (unsigned int i = 0; i < 2; ++i)
349:    {
350:      write_byte (op, 0xf1, i ? 0x13 : 3);
351:      write_byte (op, 0xf4, 0xc0);
352:      write_byte (op, 0xf4, 0xc1);
353:      write_byte (op, 0xf4, 0xc0);
354:      write_byte (op, 0xf4, 0xc0);
355:      (void) read_byte (op, 0xf3);
356:    }
```

#### 2. Corroboration in FT93xx Core Driver
In `research/libfprint-fte3600/libfprint/drivers/fte3600/ft93xx.c:595-613`:
```c
static int
efuse_byte (const struct operation *op, uint16_t chip_id,
            uint8_t foundry, uint8_t address, uint8_t *value)
{
  ...
  if (foundry == 2)
    {
      static const uint8_t control[] = {0xc0, 0xc1, 0xc0, 0x80};
      if (!write_byte (op, 0xf1, address))
        return 0;
      for (size_t i = 0; i < sizeof (control); ++i)
        if (!write_byte (op, 0xf4, control[i]))
          return 0;
      return read_byte (op, 0xf3, value);
    }
  ...
}
```

#### 3. Corroboration in Windows OEM Driver
In `research/dynabook-windows-driver/strings/ftWbioUmdfDriverV2.dll.ascii.txt:2389-2396`:
- `fw9369_check_communication`
- `fw9369_get_SMIC_IC_flag`
- `fw9369_Get_OTP_Info`

#### 4. Hardware Mechanism & Safety Analysis
- **What this sequence does:** Registers `0xF1`, `0xF4`, and `0xF3` form the internal FocalTech EFUSE / OTP read-access interface.
  - Writing `0xF1` sets the OTP row/byte address pointer (`3` on first iteration, `0x13` on second).
  - Writing `0xF4` with `0xC0 -> 0xC1 -> 0xC0 -> 0xC0` toggles the internal OTP word-line read clock and pre-charge/sense strobe.
  - Reading `0xF3` latches the output from the OTP sense amplifier.
- **Why `fw9369_startup.c` runs this loop:** In `fw9369_startup.c:355`, the read byte from `0xF3` is cast to `void` and discarded. The true purpose of toggling this OTP clocking loop is to load internal OTP bits into the digital shadow register `0x9B` (`fw9369_startup.c:357-368`), which allows reading `read_byte(0x9B) >> 2` to determine whether the silicon was fabricated by SMIC (`0xAA`) or standard foundry (`0x00`).
- **Can it modify persistent state?**
  - **Electrical reality of OTP/EFUSE:** Reading an EFUSE involves low-current sensing of the resistive state of an anti-fuse or fuse link. Burning/programming an anti-fuse link requires high programming voltage ($V_{PP}$ typically 5V–12V), a sustained high-current programming pulse, and specific write command opcodes. None of these write conditions exist in this read sequence.
  - **Verdict:** This sequence is a read-only sensing sequence. It does not write to non-volatile flash or burn OTP links.
  - **Safety Classification:** **VERIFIED WITH CONDITIONS**. The condition is that `0xF4` must strictly be written only with the read strobe patterns (`0xC0`, `0xC1`) and never with unverified programming patterns.

---

### 1.3 Disambiguation of 0xF6 and 0xF7

A critical distinction must be maintained between **protocol framing bytes** and **sensor register addresses**:

1. **Protocol Framing Bytes (SPI Transport Layer):**
   - In standard FocalTech SPI framing:
     - 8-bit Register Write: MOSI is `[ 0x09, 0xF6, address, value ]`. The second byte `0xF6` is the bitwise one's complement of the write opcode `0x09` (`~0x09 = 0xF6`). It is NOT a register address.
     - 8-bit Register Read: MOSI is `[ 0x08, 0xF7, address, 0x00, 0x00 ]`. The second byte `0xF7` is the bitwise one's complement of the read opcode `0x08` (`~0x08 = 0xF7`). It is NOT a register address.
2. **Register Addresses 0xF6 and 0xF7 (Sensor Address Space):**
   - In older chip families (FT9398/FT9769 foundry 1/3, as seen in `ft93xx.c:604-623`), registers `0xF6` and `0xF7` exist as physical register addresses used for OTP read access (`[ 0x09, 0xF6, 0xF6, ... ]`).
   - In the FT9362 / FW9369 pathway (`fw9369_*.c`), **registers `0xF6` and `0xF7` are NEVER accessed as register addresses.**
   - **Verdict:** Framing bytes `0xF6` and `0xF7` are **VERIFIED** protocol transport bytes. Addressing registers `0xF6` or `0xF7` in FT9362 is **PROHIBITED**.

---

## 2. Protected Register Unlock Mechanism (Register 0x9A)

Register `0x9A` functions as a hardware write-protect gate for sensitive analog calibration and threshold register banks.

### 2.1 Trace of All 0x9A Operations in Reference Code

There are exactly three locations in the FW9369 pathway where `0x9A` is accessed:

#### Location 1: FDT Mode Configuration (`fw9369_fdt.c:215-224`)
```c
215:  write_byte (op, 0x9a, 0x5a);
216:  for (unsigned int group = 0; group < (c->channel_mode == 1 ? 1u : 2u); ++group)
217:    {
218:      uint16_t base = c->channel_mode == 1 ? 0xc0 : (uint16_t) (0xc9 + group * 9);
219:      if (!modify (op, base, 0xe000, c->channel_mode == 1 ? 0x0444 : 0x06cd, 1) ||
220:          !modify (op, (uint16_t) (base + 1), 0xffc0, 0x0021, 1) ||
221:          !modify (op, (uint16_t) (base + 2), 0xffff, 0x00c0, 1))
222:        return 0;
223:    }
224:  write_byte (op, 0x9a, 0);
```

#### Location 2: Startup Finger Detection Arming (`fw9369_startup.c:396-405`)
```c
396:  write_byte (op, 0x9a, 0x5a);
397:  if (op->s->config.foundry == 0xaa)
398:    bytes[3] = 0xe0;
399:  for (size_t c = 0; c < 8; ++c)
400:    {
401:      bytes[6 + c * 2] = (uint8_t) (op->s->fdt_down[c] >> 8);
402:      bytes[7 + c * 2] = (uint8_t) op->s->fdt_down[c];
403:    }
404:  (void) transfer (op, bytes, sizeof (bytes));
405:  write_byte (op, 0x9a, 0);
```

#### Location 3: Release Finger Detection Arming (`fw9369_release.c:265-274`)
```c
265:  uint8_t unlock[4] = {9, 0xf6, 0x9a, 0x5a}, lock[4] = {9, 0xf6, 0x9a, 0};
266:  uint8_t bank[22] = {5, 0xfa, 0x80, s->config.foundry == 0xaa ? 0xe0 : 0xb0, 0, 8};
...
273:  if (!transfer (unlock, sizeof (unlock), io, d) ||
274:      !transfer (bank, sizeof (bank), io, d) ||
275:      !transfer (lock, sizeof (lock), io, d))
276:    return FTE3600_FW9369_RELEASE_TRANSPORT_ERROR;
```

---

### 2.2 Safety & Error-Path Audit Findings

1. **Exact Write Ordering:**
   - Write `0x5A` to register `0x9A` (unlocks protected write gates).
   - Write target configuration data to protected registers (`0x00C0`..`0x00C2` or bank `0x80B0` / `0x80E0`).
   - Write `0x00` to register `0x9A` (locks protected write gates).
2. **Defect Identified: Lock Is NOT Guaranteed on Error Paths in Upstream Code:**
   - In `fw9369_fdt.c:222`, if `modify()` fails mid-loop, `return 0;` exits immediately **without calling `write_byte(op, 0x9a, 0)`**. The sensor is left in an unlocked state (`0x9A = 0x5A`).
   - In `fw9369_release.c:273-275`, C logical OR (`||`) short-circuit evaluation means that if `transfer(bank, ...)` fails, `transfer(lock, ...)` is **never invoked**. The sensor is left in an unlocked state.
3. **Absence of Readback Verification:**
   - None of the reference drivers perform a readback verification of register `0x9A` to ensure it returned to `0x00`.
4. **Interruption Vulnerability:**
   - Because `0x9A = 0x5A` and `0x9A = 0x00` are separate SPI transactions, an asynchronous process termination, timeout, or signal between the two writes leaves the physical sensor unlocked until power cycle or reset.
5. **Persistence of Register 0x9A:**
   - Register `0x9A` is a volatile control register. It resets to `0x00` upon hardware reset or system cold boot. It does not persist in flash.

### 2.3 Mandatory Safety Requirements for Any Future Implementation
To correct the upstream reference defects:
1. **Guaranteed Cleanup / Finally Block:** The lock write (`write_byte(0x9A, 0x00)`) must be executed unconditionally on ALL exit paths (including allocation, transfer, or timeout errors).
2. **Readback Verification:** After writing `0x00` to `0x9A`, the driver must read back `0x9A` to confirm `0x00` is latched.
3. **Signal Masking:** Host signals (SIGINT, SIGTERM) must be blocked/deferred during the critical section between unlock and lock.

---

## 3. Final Classification of Studied Operations

| Operation | Target / Address | Value / Opcode | Classification | Required Conditions |
| :--- | :--- | :--- | :---: | :--- |
| Clock Ungate | `0xC6` | `0x01` | **VERIFIED** | 4 ms delay + readback verification |
| ID Read | `0x1A8B` | Word read | **VERIFIED** | Must match `0x9362` and CRC-CCITT |
| Interrupt Flag Clear | `0x1A84` | `0xFFFF` | **VERIFIED** | Standard volatile register write |
| Idle Handshake | `0x5A 0xA5` / `0xA5 0x5A` | Commands | **VERIFIED** | Verified non-destructive idle state |
| OTP Address Latch | `0xF1` | `0x03`, `0x13` | **VERIFIED WITH CONDITIONS** | Must only be written with documented read pointers |
| OTP Clock Strobe | `0xF4` | `0xC0`, `0xC1` | **VERIFIED WITH CONDITIONS** | Must strictly use read pulse patterns; no write strobes |
| OTP Buffer Read | `0xF3` | Byte read | **VERIFIED** | Pure read operation; discarded |
| Protected Unlock | `0x9A` | `0x5A` | **VERIFIED WITH CONDITIONS** | Mandatory signal blocking, guaranteed lock in `finally`, readback verification |
| Protected Lock | `0x9A` | `0x00` | **VERIFIED** | Must be verified by readback |
| Legacy OTP Address | `0xF6` (as register) | Any | **PROHIBITED** | Unused in FT9362; prohibited |
| Legacy OTP Read | `0xF7` (as register) | Any | **PROHIBITED** | Unused in FT9362; prohibited |
