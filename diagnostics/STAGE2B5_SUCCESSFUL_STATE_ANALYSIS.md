# Stage 2B.5 — Sensor Internal State Analysis on Successful 2C.1B Completion

**Document ID:** `STAGE2B5_SUCCESSFUL_STATE_ANALYSIS`  
**Date:** 2026-11-02  
**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FT9362 / FW9369  
**Status:** COMPLETE — STATIC SOURCE CODE AUDIT  
**Hardware Execution Authorization:** NONE (Static Analysis Only)

---

## 1. Executive Summary

This investigation examines the physical and operational state of the FocalTech FT9362 sensor following the hypothetical 100% successful execution of the proposed Sub-Stage 2C.1B sequence:
$$\text{Wakeup } (0xC6=1) \longrightarrow \text{Verify } (0xC6=1) \longrightarrow \text{Command } 0x5A \longrightarrow \text{Status } (0x80=0x50) \longrightarrow \text{Command } 0xA5 \longrightarrow \textbf{Host Rollback}$$

The investigation establishes that:
1. **Successful Execution Leaves an Active Silicon State:** The sensor's internal oscillator remains un-gated (`0xC6 = 0x01`) and the microcontroller remains locked in `MCU_STATE_IDLE` (`0x80 = 0x50`).
2. **Idle Mode is an Operational Configuration Staging State, Not a Standby Mode:** In all reference drivers, reaching idle is immediately followed by AFE calibration, DAC sweeping, or threshold loading. The reference driver never places the sensor in idle and then abandons it.
3. **No Sensor Teardown Exists:** The reference driver has zero code to gate the oscillator back to sleep (`0xC6 = 0x00`).
4. **Host Rollback Restores Host Baseline Only:** Unbinding `spidev` and unloading modules does not alter sensor-internal RAM or power gating. The sensor remains running internal clocks indefinitely until the laptop is power-cycled.

---

## 2. Granular Answers to Investigation Questions

### 2.1. What is the Expected MCU State?
- The microcontroller state is locked in `MCU_STATE_IDLE` (`0x50`).
- The internal command listener is active, awaiting subsequent register writes or mode strobes.

### 2.2. What is the Expected 0xC6 Value?
- Volatile register `0xC6` retains `0x01`.
- The high-frequency internal oscillator and SPI transceiver decoders remain fully powered.

### 2.3. Does the Reference Driver Subsequently Send Another Command?
**YES, IMMEDIATELY.**
- Reaching idle is never an end-state in reference code:
  - In `fw9369_startup.c:371-420`: `idle(op)` is immediately followed by setting protected bank `0x9A = 0x5A`, writing baseline thresholds to `0x80B0`, writing control register `0x1881`, and arming detection (`0xC2 0x3D`).
  - In `fw9369_fdt.c:174`: `idle(op)` is immediately followed by writing AFE DAC registers `0x1801` and filter configurations.
  - In `capture.c:228`: `command(op, 0xa5, 0x5a)` is immediately followed by writing `0x1801` and `0x1800` (image capture window).

### 2.4. Is the Sensor Expected to Remain Idle Indefinitely?
**NO.**
- The sensor's idle state is designed to hold the MCU quiescent during the few milliseconds required for the host to write configuration registers.
- Leaving the sensor in idle indefinitely with clocks un-gated causes continuous internal clock distribution and quiescent power draw inside the laptop touchpad palm-rest.

### 2.5. Does the Reference Driver Ever Return 0xC6 to Zero?
**NO.**
- Comprehensive codebase grep across `research/libfprint-fte3600/`, `research/ftexx00-ubuntu/`, and vendor drivers reveals **zero instances** of writing `0xC6 = 0x00`.
- The reference driver architecture assumes that once awakened, the sensor remains awake until the host system enters system suspend (S3) or system shutdown (S5).

### 2.6. Is There Proof That the Resulting State is Safe for Future Initialization?
- Volatile registers: Writing `0xC6 = 0x01` when `0xC6` is already `0x01` is idempotent. Calling `idle()` when the sensor is already in `0x50` is also supported (as seen in `fw9369_fdt.c:174`).
- However, leaving the sensor running in this active state for extended periods between development sessions without calibration or watchdog servicing leaves the sensor in a non-baseline state that can only be cleared by an ACPI S5 power cycle.

### 2.7. Does Host-Side SPI Cleanup Change Any of These Sensor States?
**ABSOLUTELY NOT.**
- Host rollback actions:
  - `close(fd)` -> Hardware CS line deasserts high (via PCH pull-up). Bus clock halts.
  - `echo spi-FTE3600:00 > unbind` -> Detaches Linux kernel driver.
  - `modprobe -r spidev` -> Removes kernel module.
- Sensor silicon actions:
  - Sensor VCC remains supplied.
  - Sensor internal oscillator remains running (`0xC6 = 1`).
  - Sensor MCU remains in idle state (`0x80 = 0x50`).
- Host-side cleanup decouples the host OS, but **does not restore physical sensor baseline**.

---

## 3. Conclusion

Executing Sub-Stage 2C.1B in isolation successfully transitions the sensor into an active, running operational state (`0xC6=1`, `0x80=0x50`) without performing any of the calibration, baseline acquisition, or finger detection arming that this state is designed to enable. Executing this transition without immediate follow-up initialization provides zero standalone utility while altering hardware baseline state.
