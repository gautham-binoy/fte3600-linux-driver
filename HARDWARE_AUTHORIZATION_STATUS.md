# Hardware Authorization Status

**Target Device:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Host Controller:** Intel LPSS SPI (`pxa2xx-spi.2` / `idma64`, PCI `0000:00:1e.3`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Last Updated:** Stage D18 Final Source Implementation Release (2026-11-05)  
**Authority:** Master Execution Safety Policy

---

## 1. Historical Authorizations (Consumed and Closed)

The following authorizations were granted and fully consumed during earlier diagnostic stages:

1. **Stage A (Passive Probe):** Completed historically. Read-only sysfs and ACPI inventory inspection.
2. **Stage B (Reset Pulse):** Exactly one GPIO active-low reset pulse executed on Pad 14 under separate explicit authorization.
3. **Stage C (Identification-Only SPI Sequence):** Executed under separate explicit authorization.
   - Sequence: Clock enable write (`0xC6 = 0x01`), readback `0xC6`, chip ID interrogation at `0x1A8B`, CRC-16 validation.
   - Result: Chip ID `0x9362`, CRC `0x1C53`, 3 messages, 19 TX bytes, 15 RX bytes, 0 SPI errors.
   - Historical State: Transitioned to `IDENTIFIED`.
   - **Scope Limit:** Proved identification only. Did not prove initialization, calibration, ADC sampling, or image capture.

---

## 2. Current Authorization Status: NO HARDWARE AUTHORIZATION EXISTS

**All historical hardware authorizations have expired and are permanently closed.**

There is **STRICTLY ZERO** authorization to communicate with the physical fingerprint hardware.

### Absolutely Prohibited Operations:
- Electrical or GPIO reset pulses (Pad 14).
- Full or partial hardware initialization on physical silicon.
- Volatile startup prefixes or strobes (`0x5A`, `0xC0`, `0xC2`, `0xC4`, `0xA5`) on physical SPI bus.
- FDT calibration or ADC sampling on physical sensor.
- Sensor DAC sweeps or threshold tuning on physical hardware.
- Image exposure or image capture FIFO reads from physical hardware.
- Autonomous touch detection or interrupt arming on physical hardware.
- Firmware, OTP, EFUSE, or Flash access or programming.
- Any unreviewed SPI transaction or register access.
- Any attempt to load, insert, bind, or reload `fte3600.ko` to physical controller.
- Execution of hardware ioctls (`FTE3600_IOC_RESET`, `FTE3600_IOC_INIT`, `FTE3600_IOC_CAPTURE`) against physical `/dev/fte3600`.

---

## 3. Permitted Activities (Source-Only & Hardware-Independent)

The following activities are fully authorized:
- Source-code inspection and editing (`kernel/`, `tools/`, `include/`).
- Kernel-source inspection and cross-referencing against Linux `7.2.8`.
- Protocol engine implementation and state machine refinement.
- Mock transport implementation and synthetic unit testing.
- Fault-injection testing and concurrency fuzzing.
- Kernel module compilation against target kernel headers (`/lib/modules/7.2.8-9-omarchy/build`).
- Static analysis (`W=1`, Clang `scan-build`, ASan, UBSan, `checkpatch.pl`).
- Documentation maintenance and safety gate audits.

---

## 4. Mandatory Future Hardware Authorization Gate

A future hardware validation phase (post-Stage D18 physical validation) requires:
1. Complete source-only implementation and test verification (**COMPLETED in Stage D18**).
2. An independent, adversarial source audit finding zero defects (**COMPLETED in Stage D17**).
3. A formal, dedicated Hardware Validation Plan specifying every packet and expected outcome (**COMPLETED in `HARDWARE_VALIDATION_PLAN.md`**).
4. An explicit, unambiguous authorization request presented to the human operator.
5. Written approval from the human operator.

**General instructions to complete the driver do NOT constitute hardware authorization.**
