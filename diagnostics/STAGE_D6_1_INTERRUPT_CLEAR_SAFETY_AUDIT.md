# Stage D6.1 — Audit of the 0x1A84 Interrupt-Clear Operation

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Hardware:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`, Intel LPSS SPI2)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Document Ref:** `diagnostics/STAGE_D6_1_INTERRUPT_CLEAR_SAFETY_AUDIT.md`  
**Audit Classification:** `SOURCE-OBSERVED BUT HARDWARE SEMANTICS UNVERIFIED`

---

## 1. Executive Summary

In Stage D6, the volatile initialization sequence implemented in `kernel/fte3600.c` executes the following SPI transaction twice:
```text
05 FA 9A 84 00 01 FF FF
```
This transaction writes the 16-bit word `0xFFFF` to register `0x1A84` at Step 1 (pre-wake) and Step 8 (post-interrogation).

This audit investigates the origin, wire framing, hardware semantics, risk profile, and documentation basis of this operation. It confirms that while the operation is directly observed in factory source code (`fw9369_startup.c:343, 371`), its claimed Write-1-to-Clear (W1C) behavior originates exclusively from a userspace mock test fixture (`test-fw9369-backend.c:473`) and is **unsupported by authoritative silicon documentation**.

---

## 2. Technical Investigation of Register 0x1A84

### 2.1 Exact Register Address and Wire Encoding
- **Register Address:** 16-bit register address `0x1A84`.
- **Wire Framing:**
  * Opcode: `0x05` (Write 16-bit configuration register).
  * Subcode: `0xFA` (16-bit register write marker).
  * Address High Byte: Encoded as `0x80 | (Address >> 8) = 0x80 | 0x1A = 0x9A`.
  * Address Low Byte: `Address & 0xFF = 0x84`.
  * Word Count: High byte `0x00`, low byte `0x01` (1 word = 2 bytes).
  * Data Payload: High byte `0xFF`, low byte `0xFF` (`0xFFFF` in big-endian wire order).
  * Transmitted Bytes (8 bytes total): `05 FA 9A 84 00 01 FF FF`.
- **Finding:** The wire encoding implemented in `fte3600_spi_write_reg16(data, 0x1A84, 0xFFFF)` matches the vendor framing helper `write_word(op, 0x1a84, 0xffff)` in `research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c:85-91`.

### 2.2 Original Factory-Driver Usage
In the vendor reference implementation (`libfprint-fte3600`), register `0x1A84` appears across several operational phases:
1. `fw9369_startup.c:343`: Executed immediately after reading chip ID `0x1A8B`:
   ```c
   reset_cached (op->s);
   write_word (op, 0x1a84, 0xffff);
   ```
2. `fw9369_startup.c:371`: Executed immediately after completing the foundry interrogation loop on `0xF1`/`0xF4` and reading `0x9B`:
   ```c
   reset_cached (op->s);
   write_word (op, 0x1a84, 0xffff);
   ```
3. `ft93xx.c:547`: Executed during `probe_inner()` after reading `0x1A8B`.
4. `ft93xx.c:797`: Executed during `prepare_startup()`.
5. `fw9369_startup.c:416`: Executed as `write_word(op, 0x1a84, 0x002F)` prior to arming the sensor for finger detection.
6. `capture.c:280`: Executed as `write_word(&op, 0x1a84, 0x0020)` after completing pixel FIFO frame readout.
7. `fw9369_release.c:320, 554, 597`: Executed dynamically by writing back status values previously read from register `0x1A82`.

### 2.3 Documentation of W1C Behavior: Mock vs Silicon Reality
- **Datasheet Search:** Complete inspection of all project files, ACPI DSDT tables, Windows INF/DLL strings, and reference driver documentation confirms that **zero authoritative FocalTech register manuals or silicon datasheets exist**.
- **Mock Implementation:** In `research/libfprint-fte3600/tests/test-fw9369-backend.c:471-479`:
  ```c
  if (address == 0x1a84)
    {
      mock.words[0x1a82] &= ~be16 (tx + 6);
      ...
    }
  ```
- **Finding:** The model where writing a '1' to a bit in `0x1A84` clears that bit in `0x1A82` without affecting other bits was written by the author of `libfprint-fte3600` to simulate register behavior in unit tests. Promoting this mock simulation logic to an established silicon hardware invariant is scientifically invalid.

### 2.4 Individual Bits Cleared by Writing 0xFFFF
Writing `0xFFFF` asserts all 16 bits simultaneously. Driver source code implies the following bit mapping for status register `0x1A82`:
- **Bit 0:** Touch / baseline contact indicator (part of `0x2F` mask).
- **Bit 1:** Touch detected event flag.
- **Bit 2:** Finger release event flag.
- **Bit 3:** Navigation / movement event flag.
- **Bit 4:** Fault / comparator overflow event flag.
- **Bit 5:** Frame Ready (pixel capture complete in FIFO).
- **Bits 6–15:** **Undocumented and unassigned** in all operational code.

Writing `0xFFFF` indiscriminately attempts to clear every single latched flag in the register, including all undocumented upper bits.

### 2.5 Existence of Reserved Bits
- Bits 6 through 15 are never individually evaluated or masked in any operational driver logic.
- Whether Bits 6–15 represent reserved test-mode latches, internal sequencer control flags, or undefined silicon nodes is unknown.
- Writing '1' to reserved bits in poorly documented mixed-signal ASICs can trigger undefined analog comparator states, reset internal timing counters, or induce high-impedance floating bus conditions.

### 2.6 Potential Event-Loss Impact
- Because writing `0xFFFF` asserts all bits, it clears touch, release, and frame-ready event latches.
- However, during Steps 1 and 8 of `fte3600_hw_startup()`:
  * The sensor has just emerged from electrical reset.
  * Finger detection (`CMD_ARM_TOUCH: {0xC2, 0x3D}`) has not been armed.
  * Pixel frame acquisition has not been triggered.
  * No user interaction is being monitored.
- Therefore, any latches cleared at Step 1 or Step 8 are power-on electrical artifacts or transient noise induced by sense-amp toggling, not valid biometric user events.

### 2.7 Safety Prior to Touch Arming
- Clearing interrupt latches *before* enabling host IRQ line monitoring or arming touch scanning is standard practice to prevent interrupt storms caused by power-on floating levels.
- However, performing a blind write of `0xFFFF` to an unverified register without reading the status register first is an aggressive implementation choice.

### 2.8 Necessity of the Two Writes
- **First Write (Step 1):** Clears any power-on reset state before issuing wake strobes.
- **Second Write (Step 8):** Purges any spurious interrupt flags generated by the high-frequency sense-amp cycling on `0xF1`/`0xF4` during foundry interrogation.
- In `fw9369_startup.c`, both writes are performed. However, if the sensor has not been armed, the second write is redundant unless the sense-amp interrogation loop actually asserts hardware interrupt latches.

### 2.9 Observed-Status-Only Acknowledgement vs Blanket Write
- An observed-status-only acknowledgement:
  1. Reads `0x1A82`.
  2. If `status != 0`, writes `write_reg16(0x1A84, status)`.
- **Advantages of Observed-Status-Only:**
  * Avoids writing '1' to reserved bits 6–15.
  * Avoids unnecessary SPI bus transactions if `0x1A82 == 0`.
  * Logs the exact hardware event flags that were asserted.
- **Drawback:** Factory code in `fw9369_startup.c:343, 371` unconditionally writes `0xFFFF` without reading `0x1A82`.

### 2.10 Authoritative Silicon Documentation Status
- **Status:** **NON-EXISTENT**. No manufacturer datasheet supports the claimed W1C behavior or bit definitions.
- The behavior is inferred exclusively from reverse-engineered userspace source code and mock unit test fixtures.

---

## 3. Formal Classification

In accordance with audit instructions:
- **Operation:** `write_reg16(0x1A84, 0xFFFF)` (`05 FA 9A 84 00 01 FF FF`)
- **Classification:** `SOURCE-OBSERVED BUT HARDWARE SEMANTICS UNVERIFIED`

---

## 4. Remediation Recommendation

1. **Do not remove the operation entirely:** Factory code unconditionally executes it; omitting it entirely risks leaving power-on interrupt latches asserted, which could immediately trigger an unhandled interrupt storm once the host kernel unmasks Pad 229 in Stage D7.
2. **Transition to Defensive Status-Acknowledge:** In future remediation (Stage D6.2):
   - Replace the blanket `0xFFFF` write with a read of `0x1A82` followed by writing back only observed active bits (`status & 0x003F`).
   - If `status == 0`, omit the write to `0x1A84`.
   - Log any non-zero power-on status for diagnostic verification.
