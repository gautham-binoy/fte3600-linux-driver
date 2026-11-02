# Stage 2C.5B — Fail-Closed Safety Design and Failure Handling Review

**Document ID:** `STAGE2C5B_FAILURE_HANDLING_REVIEW`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Execution Authorization:** NONE. Static safety review and error containment audit only.

---

## 1. Executive Summary

A core requirement of the FTE3600 Linux driver project is that the hardware interface must be **strictly fail-closed**. When interacting with physical silicon, speculative retries, undocumented reset pulses, or opportunistic device probing can easily turn a transient communication fault into permanent silicon lockup or electrical bus contention.

This document reviews the fail-closed error containment design enforced across the hardware transport (`src/fw9369_transport_hw.c`), state machine (`src/fw9369_state.c`), and future execution harnesses.

---

## 2. Fail-Closed Rejection Matrix

The hardware transport and state machine enforce strict, unconditional rejection across eight distinct fault vectors:

| Fault Vector | Rejecting Component | Error Code | Failure Handling Rule |
| :--- | :--- | :---: | :--- |
| **Unexpected SPI Device Path** | `fw9369_hw_validate_device_path()` | `-ENODEV` | Rejects any path other than `"/dev/spidev1.0"`. **Zero device fallback permitted.** |
| **Unsupported Chip ID** | `fw9369_pipeline_run()` | `-ENODEV` | Rejects any ID other than `0x9362`. Halts pipeline immediately. |
| **Hardware Wire CRC Mismatch** | `fw9369_pipeline_run()` | `-EBADMSG` | Rejects any mismatch with `0x1C53` or calculated CRC. Halts pipeline immediately. |
| **Short SPI Transfer** | `hw_transfer()` | `-EMSGSIZE` | Detects `ret < len`. Rejects partial byte transmission. No attempt to resume. |
| **Oversized Transfer** | `hw_transfer()` | `-EMSGSIZE` | Rejects transfers exceeding configured buffer limit (`10246`). |
| **Invalid Register Address** | `hw_transfer()` (Allowlist Guard) | `-EPERM` | Rejects write or read to unlisted registers before submitting `ioctl`. |
| **Invalid Command Opcode** | `hw_transfer()` (Allowlist Guard) | `-EPERM` | Rejects unauthorized 2-byte command strobes before submitting `ioctl`. |
| **Protected Trim Lock Failure** | `fw9369_pipeline_abort_cleanup()` | `-EIO` | Sets `lock_outcome = LOCK_TRANSMISSION_FAILED`. Halts bus activity. |

---

## 3. Strict Safety Prohibitions

The hardware backend codebase explicitly excludes the following anti-patterns:

1. **No Automatic Retries:**
   If an SPI ioctl returns `-EIO`, `-ETIMEDOUT`, or `-EMSGSIZE`, the transport does **not** retry the operation. It immediately bubbles the error to the state machine, which triggers structured abort unwinding.
2. **No Speculative Software Reset Pulses:**
   Because Pad 14 cannot be reached from userspace in pure-spidev mode, the driver does not attempt to issue dummy GPIO toggles or unmanaged ioctls on `/dev/gpiochip0`.
3. **No Undocumented Recovery Writes:**
   The driver never writes arbitrary magic bytes or non-specification sequences to "wake up" a silent sensor.
4. **No Device Node Fallbacks:**
   If `/dev/spidev1.0` is missing, the driver will never fall back to `/dev/spidev0.0` or any other SPI master.

---

## 4. Truthful Sensor State Reporting Upon Abort

A foundational safety rule is that **host-side resource cleanup must never be claimed to restore physical sensor state**:

1. **Host-Side Rollback:**
   When an error occurs, the host closes `/dev/spidev1.0`, unbinds the driver from `spi-FTE3600:00`, restores `driver_override` to `''`, and unloads `spidev`. This guarantees **100% clean host OS isolation**.
2. **Sensor-Side Silicon Reality:**
   If a failure occurred after volatile register `0xC6 = 0x01` was written:
   - The sensor transceiver clocks may remain active.
   - If `0x9A` was unlocked and relock failed, the analog trim bank may remain write-enabled.
   - If a FIFO read aborted mid-stream, the internal FIFO address pointer may be frozen mid-frame.
3. **Operator Reporting Mandate:**
   The hardware tool must explicitly output:
   ```text
   [WARNING] Pipeline aborted after physical register write.
   Host-side resources released cleanly.
   Internal sensor silicon state is PHYSICALLY UNKNOWN.
   A full system cold reboot (ACPI S5 power down) is recommended.
   ```

---

## 5. Audit Conclusion

The hardware transport implementation conforms to fail-closed principles:
- Rejects all unauthorized parameters and devices at the UAPI boundary.
- Enforces strict allowlist validation before ioctl submission.
- Prohibits blind retries and undocumented recovery attempts.
- Truthfully reports physical sensor uncertainty on abort.
- Failure handling design is **AUDITED AND APPROVED**.
