# Stage 2C.5B.2 — Register Permission & Protection Allowlist Final Audit

**Document ID:** `STAGE2C5B2_REGISTER_PERMISSION_FINAL_AUDIT`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Audit Scope:** Register allowlist matrix verification against `FW9369_UNIFIED_INITIALIZATION_SPECIFICATION.md`, directional enforcement, and Register `0x9A` protection hardening.  
**Hardware Authorization:** NONE (Software & Static Analysis Only).

---

## 1. Executive Summary

This document presents the final static safety audit of the register permission allowlist implemented in `tools/fw9369-mock/src/fw9369_protocol.c` and enforced at the transport boundary in `tools/fw9369-mock/src/fw9369_transport_hw.c`.

The audit confirms that:
1. Every register access defined in `FW9369_UNIFIED_INITIALIZATION_SPECIFICATION.md` is strictly categorized by directional permission (Read-Only, Write-Only, or Read/Write).
2. All read-only telemetry and identification registers strictly reject write operations.
3. All write-only control and strobe registers strictly reject read operations.
4. Any register address not explicitly listed in the specification is unconditionally rejected with `-EPERM`.
5. Crucially, protection register `0x9A` has been hardened: only the canonical key values `0x5A` (unlock) and `0x00` (lock) are accepted; arbitrary byte writes are rejected; repeated unlock commands are blocked; and access to protected analog trim registers (`0x00C0..0x00C2`) is barred while locked.

---

## 2. Comprehensive Register Permission Matrix

The canonical mapping verified against the codebase is detailed below:

| Register Address | Name / Function | Allowed Direction | Read Action | Write Action | Allowlist Policy |
| :---: | :--- | :---: | :---: | :---: | :--- |
| **`0x80`** | MCU Status | **Read-Only** | Allowed (Poll idle state `0x50`) | **REJECTED (`-EPERM`)** | Telemetry register; write prohibited |
| **`0x1A8B`** | Chip ID | **Read-Only** | Allowed (Interrogate `0x9362` + CRC) | **REJECTED (`-EPERM`)** | Silicon ID; write prohibited |
| **`0x1A82`** | ADC Status | **Read-Only** | Allowed (Poll ADC ready bit 3) | **REJECTED (`-EPERM`)** | Status latch; write prohibited |
| **`0x80B8`** | ADC Measurement Vector | **Read-Only** | Allowed (Read 4-ch raw FDT data) | **REJECTED (`-EPERM`)** | ADC measurement bank; write prohibited |
| **`0x8E`** | Frame Burst Config | **Write-Only** | **REJECTED (`-EPERM`)** | Allowed (Arm frame readout) | Config strobe; read prohibited |
| **`0x90`** | Frame Burst Byte 0 | **Write-Only** | **REJECTED (`-EPERM`)** | Allowed (Arm frame readout) | Config strobe; read prohibited |
| **`0x91`** | Frame Burst Byte 1 | **Write-Only** | **REJECTED (`-EPERM`)** | Allowed (Arm frame readout) | Config strobe; read prohibited |
| **`0x92`** | Frame Burst Byte 2 | **Write-Only** | **REJECTED (`-EPERM`)** | Allowed (Arm frame readout) | Config strobe; read prohibited |
| **`0x9A`** | Analog Protection Key | **Write-Only** | **REJECTED (`-EPERM`)** | Allowed (`0x5A` or `0x00` only) | Special key validation enforced |
| **`0xF1`** | Foundry Clock Gating | **Write-Only** | **REJECTED (`-EPERM`)** | Allowed (`0x03`, `0x13`) | Foundry test control; read prohibited |
| **`0xF4`** | Foundry Test Mode | **Write-Only** | **REJECTED (`-EPERM`)** | Allowed (`0xC0`) | Foundry test control; read prohibited |
| **`0x1885`** | FDT Scan Trigger | **Write-Only** | **REJECTED (`-EPERM`)** | Allowed (`0x0001`) | Scan strobe; read prohibited |
| **`0x1A83`** | Foundry Strobe Pulse | **Write-Only** | **REJECTED (`-EPERM`)** | Allowed (`0x0010`, `0x0016`) | Test strobe; read prohibited |
| **`0x1A84`** | IRQ Clear / Latch Config| **Write-Only** | **REJECTED (`-EPERM`)** | Allowed (`0xFFFF`, `0x002F`) | Interrupt clear/arm; read prohibited |
| **`0xC6`** | Transceiver Clock Gate | **Read / Write** | Allowed (Verify clock `0x01`) | Allowed (Un-gate clock `0x01`) | Bi-directional transceiver control |
| **`0x1800`** | AFE Integration Control | **Read / Write** | Allowed | Allowed | AFE configuration table word |
| **`0x1801`** | FDT DAC Setting | **Read / Write** | Allowed | Allowed | Calibration tuning word (`0..127`) |
| **`0x1804`** | Pixel Shift Control | **Read / Write** | Allowed | Allowed | Pixel engine timing word |
| **`0x1807`** | Pixel Period Control | **Read / Write** | Allowed | Allowed | Pixel engine timing word |
| **`0x1808`** | Pixel Row Control | **Read / Write** | Allowed | Allowed | Pixel engine timing word |
| **`0x1881`** | AFE Channel Timing | **Read / Write** | Allowed | Allowed | AFE configuration table word |
| **`0x1887`** | Channel Scan Mode | **Read / Write** | Allowed | Allowed | AFE configuration table word |
| **`0x00C0`** | Protected Analog Trim 0 | **Read / Write** | Allowed (Unlocked only) | Allowed (Unlocked only) | Gated by `is_unlocked == true` |
| **`0x00C1`** | Protected Analog Trim 1 | **Read / Write** | Allowed (Unlocked only) | Allowed (Unlocked only) | Gated by `is_unlocked == true` |
| **`0x00C2`** | Protected Analog Trim 2 | **Read / Write** | Allowed (Unlocked only) | Allowed (Unlocked only) | Gated by `is_unlocked == true` |
| **`0x80B0`** | FDT Down Threshold 0 | **Read / Write** | Allowed | Allowed | Calibrated detection threshold word |
| **`0x80B1`** | FDT Down Threshold 1 | **Read / Write** | Allowed | Allowed | Calibrated detection threshold word |
| **`0x80B2`** | FDT Down Threshold 2 | **Read / Write** | Allowed | Allowed | Calibrated detection threshold word |
| **`0x80B3`** | FDT Down Threshold 3 | **Read / Write** | Allowed | Allowed | Calibrated detection threshold word |

---

## 3. Register `0x9A` Protection Hardening Audit

Register `0x9A` controls the silicon analog trim lock. If corrupted or left unlocked, accidental writes could alter the sensor's analog bias levels.

In Stage 2C.5B.2, the transport layer enforces the following four safety rules:

### 3.1 Strict Value Filtering
The transport validator inspects the payload byte `val = tx[3]` for any write to `0x9A`:
```c
if (reg == FW9369_REG_PROTECT_9A) {
    if (val == FW9369_KEY_UNLOCK) {
        if (!ctx->is_unlocked)
            packet_valid = true;
    } else if (val == FW9369_KEY_LOCK) {
        packet_valid = true;
    }
}
```
- Only `FW9369_KEY_UNLOCK` (`0x5A`) and `FW9369_KEY_LOCK` (`0x00`) are permitted.
- Writes with any other value (e.g., `0x12`, `0x01`, `0xFF`, `0x5B`) are rejected with `-EPERM` **before** `ioctl()` is invoked.

### 3.2 Repeated Unlock Prevention
- The transport tracks `ctx->is_unlocked`.
- If a write of `0x5A` to `0x9A` is submitted while `ctx->is_unlocked == true`, the parser rejects the transaction with `-EPERM`.
- This guarantees that software cannot submit redundant, dangling, or un-synchronized unlock requests.

### 3.3 Protected Trim Bank Gating
- Registers `0x00C0`, `0x00C1`, and `0x00C2` reside in the protected analog trim bank.
- `hw_transfer()` enforces that any word read or write to `0x00C0..0x00C2` must fail closed with `-EPERM` unless `ctx->is_unlocked == true`.
- Once locked (`0x9A = 0x00`), any access to `0x00C0..0x00C2` is blocked immediately.

### 3.4 Failure State Containment
- If an unlock write succeeds, `ctx->is_unlocked` transitions to `true`.
- If a lock write (`0x9A = 0x00`) succeeds, `ctx->is_unlocked` transitions to `false`.
- If a lock write **fails** (e.g. ioctl returns `-EIO`), `ctx->is_unlocked` **remains `true`**.
- The driver treats failure to lock as an unsafe/unknown state, immediately halting the pipeline with `FW9369_LOCK_TRANSMISSION_FAILED` and executing zero automatic retries.

---

## 4. Prohibited Operation & Anti-Destruction Verification

The following operations are confirmed to be completely omitted from the protocol encoding tables and rejected at the transport filter:
1. **OTP / EFUSE Programming:** No opcodes or register addresses associated with one-time-programmable silicon fuses exist in the driver.
2. **Flash & EEPROM Writes:** No non-volatile programming sequences are implemented.
3. **Firmware Download:** No bootloader or RAM patch loading commands are present.
4. **PCH MMIO Bypasses:** All communication occurs strictly via character device ioctls on `/dev/spidev1.0`.

---

## 5. Audit Assessment Summary

| Review Category | Requirement | Verified Result |
| :--- | :--- | :---: |
| **Read-Only Verification** | Writes to `0x80, 0x1A8B, 0x1A82, 0x80B8` rejected | **PASSED (100%)** |
| **Write-Only Verification** | Reads from `0x9A, 0xF1, 0xF4, 0x1A83, 0x1A84, 0x1885` rejected | **PASSED (100%)** |
| **Unlisted Addresses** | Arbitrary addresses (`0x42, 0x1900, 0xFFFF`) rejected | **PASSED (100%)** |
| **0x9A Key Hardening** | Only keys `0x5A` and `0x00` permitted | **PASSED (100%)** |
| **Trim Bank Gating** | Bank `0x00C0..0x00C2` blocked when locked | **PASSED (100%)** |
| **Fail-Closed Locking** | Failed lock retains unlocked flag; 0 retries | **PASSED (100%)** |

**Conclusion:** The register permission architecture is strictly defined, completely fail-closed, and robustly protected against unauthorized or destructive operations.
