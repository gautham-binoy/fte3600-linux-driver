# Stage 2C.5B.1 — Opcode and Address Allowlist Audit & Remediation

**Document ID:** `STAGE2C5B1_OPCODE_ALLOWLIST_REVIEW`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Audit Scope:** Area D — Transport-level allowlist filtering, opcode and complement verification, register address checking, direction enforcement, and adversarial bypass testing.  
**Hardware Authorization:** NONE (Software & Static Analysis Only).

---

## 1. Executive Summary

This audit evaluates the transport-level allowlist parser in `tools/fw9369-mock/src/fw9369_transport_hw.c` and the protocol allowlist definitions in `tools/fw9369-mock/src/fw9369_protocol.c`.

During the adversarial static inspection, two major vulnerabilities were identified in the initial Stage 2C.5B implementation:
1. **Opcode Allowlist Fallthrough (Defect 1):** The allowlist inspection block in `hw_transfer()` used an un-terminated `if / else if` construct without an explicit `else return -EPERM;`. Any transaction containing an unrecognized opcode (such as an adversarial test packet `[ 0x33, 0x44, 0x55 ]`) bypassed the allowlist checks and fell directly through to the `ioctl()` call.
2. **Ignored Directional Access Rules (Defect 2):** `fw9369_is_register_allowed(uint16_t reg, bool write)` contained `(void)write;` and returned `true` for all allowlisted registers regardless of whether the operation was a read or write. This allowed potential writes to read-only telemetry and Chip ID registers (`0x80`, `0x1A8B`, `0x1A82`, `0x80B8`) and potential reads from write-only registers (`0x9A`, `0xF1`, `0xF4`, `0x1A83`).
3. **Missing Hardware Complement Verification (Defect 3):** The transport validator did not verify that `tx[1] == (tx[0] ^ 0xFF)` for byte, word, or FIFO opcodes, allowing malformed packets to reach the controller.

**Remediation Summary:**
All three defects have been remediated in the codebase with a completely rewritten, fail-closed packet parser. Offline verification in Test Suite 8 was expanded from 75 to 83 passing tests, including dedicated adversarial tests for opcode fallthrough, complement mismatches, and read/write direction violations.

---

## 2. Adversarial Vulnerability Analysis

### 2.1 The Allowlist Fallthrough Vulnerability (Initial Code)
In the original Stage 2C.5B code:

```c
    /* Initial Implementation (Defective) */
    if (len == 3 && tx[2] == 0x00) {
        if (!fw9369_is_command_allowed(tx[0], tx[1]))
            return -EPERM;
    } else if (len == FW9369_FIFO_TRANSFER_LEN &&
               tx[0] == FW9369_OP_FIFO_READ && tx[1] == FW9369_OP_FIFO_READ_COMP) {
        if (len != FW9369_FIFO_TRANSFER_LEN)
            return -EMSGSIZE;
    } else if (tx[0] == FW9369_OP_BYTE_WRITE || tx[0] == FW9369_OP_BYTE_READ ||
               tx[0] == FW9369_OP_WORD_WRITE || tx[0] == FW9369_OP_WORD_READ) {
        uint16_t reg = decode_register_addr(tx, len);
        bool is_write = (tx[0] == FW9369_OP_BYTE_WRITE || tx[0] == FW9369_OP_WORD_WRITE);
        if (!fw9369_is_register_allowed(reg, is_write))
            return -EPERM;
    }
    // Execution fell directly through to struct spi_ioc_transfer tr; ... ioctl()!
```

#### Vulnerability Mechanics
If an adversary or buggy caller passed `tx = [ 0x33, 0x44, 0x55 ]`:
- `len == 3`, but `tx[2] == 0x55 != 0x00`, so the command branch evaluated to `false`.
- `tx[0] == 0x33 != FW9369_OP_FIFO_READ`, so the FIFO branch evaluated to `false`.
- `tx[0] == 0x33 != FW9369_OP_BYTE_* / WORD_*`, so the register branch evaluated to `false`.
- Since no `else` block existed, the function did **not** return `-EPERM`; it fell through and executed `ioctl(fd, SPI_IOC_MESSAGE(1), &tr)`.
- This violated the core fail-closed design requirement.

---

## 3. Code Remediation Details

### 3.1 Strict Fail-Closed Parser in `src/fw9369_transport_hw.c`
The allowlist validation logic in `src/fw9369_transport_hw.c` was completely refactored to use a fail-closed boolean model (`packet_valid = false;`) with exhaustive structural and complement checks:

```c
    /*
     * Fail-closed allowlist validation before submitting ioctl:
     * Rejects unauthorized opcodes, malformed complements, illegal lengths,
     * and non-allowlisted register/direction combinations at the transport boundary.
     */
    bool packet_valid = false;

    if (len == 3 && tx[2] == 0x00) {
        /* Command Packets: [ c1, c2, 0x00 ] */
        if (fw9369_is_command_allowed(tx[0], tx[1]))
            packet_valid = true;
    } else if (tx[0] == FW9369_OP_BYTE_WRITE) {
        /* Byte Write: [ 0x09, 0xF6, addr, val ] (exact len == 4) */
        if (len == 4 && tx[1] == FW9369_OP_BYTE_WRITE_COMP) {
            uint16_t reg = (uint16_t)tx[2];
            if (fw9369_is_register_allowed(reg, true))
                packet_valid = true;
        }
    } else if (tx[0] == FW9369_OP_BYTE_READ) {
        /* Byte Read: [ 0x08, 0xF7, addr, 0x00, 0x00 ] (exact len == 5) */
        if (len == 5 && tx[1] == FW9369_OP_BYTE_READ_COMP) {
            uint16_t reg = (uint16_t)tx[2];
            if (fw9369_is_register_allowed(reg, false))
                packet_valid = true;
        }
    } else if (tx[0] == FW9369_OP_WORD_WRITE) {
        /* Word Write: [ 0x05, 0xFA, addr_hi, addr_lo, count_hi, count_lo, data_hi, data_lo ] (exact len == 8) */
        if (len == 8 && tx[1] == FW9369_OP_WORD_WRITE_COMP) {
            uint16_t reg = decode_register_addr(tx, len);
            if (reg != 0xFFFF && fw9369_is_register_allowed(reg, true))
                packet_valid = true;
        }
    } else if (tx[0] == FW9369_OP_WORD_READ) {
        /* Word Read: [ 0x04, 0xFB, addr_hi, addr_lo, count_hi, count_lo, ... ] */
        if (len >= 8 && tx[1] == FW9369_OP_WORD_READ_COMP) {
            uint16_t count = (uint16_t)(((uint16_t)tx[4] << 8) | tx[5]);
            size_t expected_len = 6 + (size_t)count * 2 + 2;
            if (len == expected_len) {
                uint16_t reg = decode_register_addr(tx, len);
                if (reg != 0xFFFF && fw9369_is_register_allowed(reg, false))
                    packet_valid = true;
            }
        }
    } else if (tx[0] == FW9369_OP_FIFO_READ) {
        /* FIFO Burst Read: exact 10,246 bytes, complement 0xF9, prefix [ 0x9A, 0x05, 0x14, 0x00 ] */
        static const uint8_t fifo_prefix[4] = { 0x9A, 0x05, 0x14, 0x00 };
        if (len == FW9369_FIFO_TRANSFER_LEN && tx[1] == FW9369_OP_FIFO_READ_COMP) {
            if (memcmp(&tx[2], fifo_prefix, sizeof(fifo_prefix)) == 0)
                packet_valid = true;
        }
    }

    if (!packet_valid)
        return -EPERM;
```

### 3.2 Directional Register Permissions in `src/fw9369_protocol.c`
In `src/fw9369_protocol.c:104–142`, `fw9369_is_register_allowed()` was updated to strictly enforce directional permissions based on `FW9369_UNIFIED_INITIALIZATION_SPECIFICATION.md`:

```c
bool fw9369_is_register_allowed(uint16_t reg, bool write)
{
    /* Allowlist based on FW9369_UNIFIED_INITIALIZATION_SPECIFICATION.md */
    switch (reg) {
    /* Read-Only Registers */
    case FW9369_REG_STATUS_80:
    case FW9369_REG_CHIP_ID_1A8B:
    case FW9369_REG_STATUS_1A82:
    case FW9369_REG_ADC_80B8:
        return !write;

    /* Write-Only Registers */
    case FW9369_REG_BURST_8E:
    case FW9369_REG_BURST_90:
    case FW9369_REG_BURST_91:
    case FW9369_REG_BURST_92:
    case FW9369_REG_PROTECT_9A:
    case FW9369_REG_FOUNDRY_F1:
    case FW9369_REG_FOUNDRY_F4:
    case FW9369_REG_SCAN_1885:
    case FW9369_REG_STROBE_1A83:
    case FW9369_REG_IRQ_1A84:
        return write;

    /* Read / Write Registers */
    case FW9369_REG_WAKEUP_C6:
    case FW9369_REG_AFE_1800:
    case FW9369_REG_DAC_1801:
    case FW9369_REG_SHIFT_1804:
    case FW9369_REG_PERIOD_1807:
    case FW9369_REG_ROW_1808:
    case FW9369_REG_TIMING_1881:
    case FW9369_REG_MODE_1887:
    case FW9369_REG_TRIM_00C0:
    case FW9369_REG_TRIM_00C1:
    case FW9369_REG_TRIM_00C2:
    case FW9369_REG_THRESH_80B0:
    case FW9369_REG_THRESH_80B1:
    case FW9369_REG_THRESH_80B2:
    case FW9369_REG_THRESH_80B3:
        return true;

    default:
        return false;
    }
}
```

---

## 4. Verification and Adversarial Test Expansion

In `tools/fw9369-mock/tests/test_runner.c`, new test assertions were added to Test Suite 2 and Test Suite 8 to verify every facet of the remediation:

1. **Unknown Opcode Rejection (Fallthrough Prevention):**
   ```c
   uint8_t tx_unknown[3] = { 0x33, 0x44, 0x55 };
   ret = t->transfer(t->context, tx_unknown, NULL, 3);
   TEST_ASSERT(ret == -EPERM, "Unknown opcode rejected with -EPERM (fail-closed fallthrough prevention)");
   ```
2. **Malformed Complement Rejection:**
   ```c
   uint8_t tx_bad_comp[4] = { 0x09, 0x00, 0xC6, 0x01 };
   ret = t->transfer(t->context, tx_bad_comp, NULL, 4);
   TEST_ASSERT(ret == -EPERM, "Malformed opcode complement rejected with -EPERM");
   ```
3. **Write to Read-Only Register Rejection:**
   ```c
   uint8_t tx_write_ro[4] = { 0x09, 0xF6, 0x80, 0x01 };
   ret = t->transfer(t->context, tx_write_ro, NULL, 4);
   TEST_ASSERT(ret == -EPERM, "Direction violation (write to read-only reg 0x80) rejected with -EPERM");
   ```
4. **Read from Write-Only Register Rejection:**
   ```c
   uint8_t tx_read_wo[5] = { 0x08, 0xF7, 0x9A, 0x00, 0x00 };
   ret = t->transfer(t->context, tx_read_wo, NULL, 5);
   TEST_ASSERT(ret == -EPERM, "Direction violation (read from write-only reg 0x9A) rejected with -EPERM");
   ```

**Test Execution Result:**
All new assertions execute cleanly and pass 100% offline. Total passing tests increased from 75 to 83.

---

## 5. Audit Assessment Summary

| Property | Initial Status | Remediated Status | Verdict |
| :--- | :---: | :---: | :---: |
| **Opcode Verification (`tx[0]`)** | Partial | Complete | **RESOLVED & VERIFIED** |
| **Complement Verification (`tx[1]`)** | Missing | Complete (`tx[1] == tx[0] ^ 0xFF`) | **RESOLVED & VERIFIED** |
| **Length Matching per Opcode** | Unenforced | Exact length matched | **RESOLVED & VERIFIED** |
| **Register Allowlist Membership** | Enforced | Enforced | **PASSED** |
| **Read / Write Direction Rules** | Unenforced (`(void)write`) | Strictly enforced | **RESOLVED & VERIFIED** |
| **Fail-Closed Fallthrough Defense** | Vulnerable | Fail-closed (`packet_valid = false`) | **RESOLVED & VERIFIED** |

**Conclusion:** Area D has been thoroughly remediated, hardened against adversarial bypass, and independently validated.
