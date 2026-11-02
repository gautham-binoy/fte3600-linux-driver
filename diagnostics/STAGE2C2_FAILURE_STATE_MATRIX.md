# Stage 2C.2 — Comprehensive Initialization Failure-State Matrix

**Document ID:** `STAGE2C2_FAILURE_STATE_MATRIX`  
**Date:** 2026-11-02  
**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FT9362 / FW9369  
**Status:** COMPLETE — STATIC FAILURE ANALYSIS  
**Hardware Execution Authorization:** NONE (Static Analysis Only)

---

## 1. Executive Summary

This document presents a comprehensive failure-state matrix cataloging all 14 distinct failure modes across the 7-phase FW9369 initialization, calibration, and arming architecture. 

For each failure mode, the matrix analyzes:
- The exact failing operation.
- The physical sensor internal state remaining on silicon.
- The exposure status of the protected register bank (`0x9A`).
- The residual state of hardware interrupt flags (`0x1A82` / `0x1A84`).
- Whether the host can safely continue or retry.
- Known recovery paths and cold power cycle requirements.

---

## 2. Comprehensive Failure-State Matrix

| Phase & Failure Point | Failing Operation | Sensor State Remaining | Protected Bank 0x9A Status | Residual IRQ Flags | Can Host Continue? | Known Recovery Path | Power Cycle Required? |
| :--- | :--- | :--- | :---: | :---: | :---: | :--- | :---: |
| **0. Reset Line Fault** | PCH Pad 14 held low or floating | Sensor in hardware reset or erratic logic | Locked (`0x00`) | Indeterminate | **NO** | Restore Pad 14 to pull-up (`0x84000201`) | No (if Pad 14 recovered) |
| **1. Clock Enable Failure** | Write `0xC6 = 1` or read `0xC6 != 1` | Internal oscillator un-clocked; SPI bus un-gated | Locked (`0x00`) | Uncleared power-on flags | **NO** | Retry up to 31 times (`fw9369_startup.c:328`) | Conservative known path |
| **2a. MCU Handshake Mismatch** | Read `0x80 != 0x50` after `0x5A` | MCU in un-closed command dialog | Locked (`0x00`) | Uncleared power-on flags | **NO** | In ref: send `C0 3F`, poll up to 5 times. If stranded: power cycle | **YES (if aborted without A5)** |
| **2b. MCU Status Timeout** | `0x80` polls exhausted | MCU stalled or unresponsive | Locked (`0x00`) | Indeterminate | **NO** | Upstream: reset line pulse (`RST_N`) | **YES** |
| **2c. Foundry Sense Failure** | Read `0x9B` unrecognized code | Defaults to TSMC (`0x00`), period 200 ms | Locked (`0x00`) | Uncleared flags | **YES (Warning)** | Driver sets warning `UNRECOGNIZED_FOUNDRY` | No (falls back to default) |
| **3a. Bank 0x00C0 Unlock Error**| Write `0x9A = 0x5A` fails | Bank remains locked; AFE config skipped | **LOCKED** | None | **NO** | Abort configuration | No |
| **3b. Bank 0x00C0 Write Leak** | Transport error on `0x00C0`..`0x00C2` | Partial AFE trim; **0x9A LEAKED UNLOCKED** | **UNLOCKED (LEAK)** | None | **NO** | Mandatory `finally` lock write: `0x9A = 0x00` | Yes (if lock write fails) |
| **3c. FDT Measurement Timeout**| Poll `0x1A82 & 8` exhausted (5 ms) | AFE scan triggered but unfinished | Locked (`0x00`) | Bit 3 (`man`) may assert late | **NO** | Retry up to 3 calibration attempts | No (retry within session) |
| **3d. FDT ADC Out-of-Range** | `minima[c] < 300` or `> 700` | Saturated or floating capacitive channels | Locked (`0x00`) | Bit 3 (`man`) cleared | **NO** | Up to 3 retries. If persistent: hardware fault | No |
| **4a. Image Mode Poll Timeout**| Poll `0x80 == 0x54` exhausted (10 ms)| MCU failed to switch to capture mode | Locked (`0x00`) | Frame flag unasserted | **NO** | Re-enter idle (`5A A5`) | Conservative known path |
| **4b. FIFO Transfer Error** | `ioctl(SPI_IOC_MESSAGE(1))` fails / EMSGSIZE | Hardware FIFO read pointer advanced/stuck | Locked (`0x00`) | Bit 5 (`data`) uncleared | **NO** | In ref: hardware reset pulse (`RST_N`). No software register reset | **YES** |
| **4c. Image DAC Search Bound**| `dac < 1` or `dac > 125` / max search | AFE DAC unable to center histogram at 512 | Locked (`0x00`) | Bit 5 cleared | **NO** | Abort startup; check optical/capacitive surface | No |
| **4d. Baseline Drift Anomaly** | `warm_baseline()` diff > 300 unsupported | Baseline corrupted by localized noise | Locked (`0x00`) | None | **NO** | Invalidate baseline; force cold recalibration | No |
| **5. Detection Arming Error** | Protected write to `0x80B0` fails | Threshold vector partial; **0x9A risk** | **UNLOCKED RISK** | Bit 0..5 uncleared | **NO** | Guaranteed lock `0x9A = 0`; abort arming | Yes (if lock fails) |

---

## 3. Analysis of Critical Recovery Pathways

### 3.1. The Hardware Reset Line Dependency in Reference Code
Across all failure recovery paths in `recovery.c:73-78` and `linux_board.c:480-505`:
- When an SPI transaction fails mid-FIFO or the MCU becomes unresponsive on `0x80`, the upstream reference driver asserts an electrical pulse on the sensor's reset line:
  ```c
  /* recovery.c:73-74 */
  if (reset_pulse (io, o))
    return FTE3600_RECOVERY_IO_ERROR;
  ```
- **The Dynabook Hardware Reality:** On our laptop, Pad 14 (`SPKR`/Reset) cannot be pulsed from userspace without custom kernel modules and root privileges.
- **Architectural Consequence:** The software-based recovery paths designed into the upstream reference driver are **inoperative on our platform** in userspace.
- Therefore, for any failure that leaves internal FIFO pointers advanced or the MCU stalled, an **ACPI S5 cold reboot is the only guaranteed physical recovery mechanism**.

### 3.2. Protected Bank Unlock Exposure
- As proven in Section 2 (3b and 5), if an SPI transport error strikes during a protected bank write, the sensor's internal register decoders may leave `0x9A = 0x5A` active.
- If unhandled, subsequent accidental writes to addresses `0x00C0` or `0x80B0` will corrupt analog channel tuning.
- **Mitigation:** The driver MUST maintain an atomic in-memory state tracking the unlock, and MUST issue an unconditional SPI write `[ 0x09, 0xF6, 0x9A, 0x00 ]` inside a guaranteed exception/cleanup handler.
