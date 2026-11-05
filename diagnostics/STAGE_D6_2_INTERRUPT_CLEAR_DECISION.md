# STAGE D6.2 — INTERRUPT-CLEAR OPERATION ISOLATION DECISION REPORT

**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Host Architecture:** Intel LPSS SPI2 (`spi-FTE3600:00`, CS0) on Dynabook Satellite Pro C40-J  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Date:** 2026-11-04  
**Author:** Antigravity (Advanced Agentic Pair Programming)  
**Classification:** ARCHITECTURAL ISOLATION DECISION REPORT

---

## 1. Executive Summary

During the Stage D6.1 independent safety audit, investigation of the operation `fte3600_spi_write_reg16(data, 0x1A84, 0xFFFF)` revealed that:
1. Register `0x1A84` does not appear as a startup operation in vendor hardware datasheets or factory driver code (`fw9369_startup.c`).
2. Its write-to-clear (W1C) semantics and the value `0xFFFF` originated exclusively from a mock test simulation fixture in `tests/test-fw9369-backend.c:473`.
3. Writing `0xFFFF` asserts 16 bits simultaneously, asserting bits 6–15 whose hardware definition on FT9362 silicon is undocumented and unverified.
4. Because touch arming (`{ 0xC2, 0x3D }`) remains strictly blocked and unperformed, no interrupt events or latches are actively generated.
5. Performing an unverified multi-bit write to `0x1A84` during baseline volatile startup introduces unnecessary electrical and hardware risk without operational necessity.

**Decision:**
In Stage D6.2, both occurrences of `0x1A84 = 0xFFFF` (Step 1 and Step 8 of D6) are completely removed from baseline volatile startup. Full interrupt latch management is isolated and deferred to Stage D7 (Touch/IRQ integration).

---

## 2. Evidence Audit and Provenance Analysis

### 2.1 Factory Startup Reference Comparison
An exhaustive audit of `fw9369_startup.c` (the factory implementation of FT9362 startup) shows the following complete volatile startup sequence:
1. Write byte `0xC6 = 0x01` (transceiver wake)
2. Stabilization delay (4 ms)
3. Interrogate chip ID `0x1A8B`
4. Send `CMD_WAKE { 0x5A, 0xA5, 0x00 }`
5. Delay (1 ms)
6. Read `0x80`, verify idle (`0x50`), abort recovery if not idle
7. Send `CMD_IDLE_ACK { 0xA5, 0x5A, 0x00 }`
8. Interrogate foundry code via loop (writes to `0xF1`, `0xF4`, read `0xF3`, read `0x9B`)

**Crucial Finding:**
In `fw9369_startup.c`, register `0x1A84` is **NEVER** written during volatile startup.

### 2.2 Provenance in Test Mock
In `tests/test-fw9369-backend.c` (lines 470–476):
```c
case 0x1a84:
    /* Interrupt clear register: Write 1 to clear */
    if (is_write) {
        sensor->irq_status &= ~val;
    }
    return 0;
```
The mock fixture accepted `0xFFFF` to reset its internal synthetic software flags. Translating this mock artifact directly into kernel driver startup code conflated test simulation scaffolding with actual silicon initialization requirements.

### 2.3 Undocumented Bit-Mask Risk
On FT9362 / FW9369 silicon:
- Register `0x1A84` is a 16-bit register.
- Only bits 0..5 are associated with known event sources (touch detection, image ready, FIFO threshold, etc.).
- Bits 6..15 have unknown circuit implications. In some digital signal processing silicon, writing ones to unmapped control fields can alter clock distribution, trigger calibration state transitions, or force test modes.
- Therefore, blindly writing `0xFFFF` violates the fail-safe principle.

---

## 3. Operational Impact Analysis

### 3.1 Impact on Baseline Startup
- **Sensor State:** The sensor does not require `0x1A84` clearing to complete volatile clock configuration, idle state stabilization (`0x80 == 0x50`), or foundry interrogation (`0x9B`).
- **Touch Events:** In the current driver milestone, background touch monitoring is disabled (`{ 0xC2, 0x3D }` is blocked). No touch events are armed, so no touch interrupts can fire or be lost.
- **Poll / Waitqueue:** Driver poll implementation already returns `0` (no false `EPOLLIN`) and wakes pollers only upon unbind/offline (`EPOLLERR | EPOLLHUP`).

### 3.2 Benefits of Removal
1. Eliminates 2 unverified 8-byte SPI transfers (`0x05, 0xFA, 0x9A, 0x84, 0x00, 0x01, 0xFF, 0xFF`).
2. Reduces baseline volatile startup SPI transfers from 18 to 16 transfers (66 wire bytes).
3. Eliminates any possibility of corrupting undocumented registers or inducing unwanted test modes in silicon.
4. Leaves `fte3600_spi_write_reg16()` available (`__maybe_unused`) for deliberate, audited IRQ management in Stage D7.

---

## 4. Remediation in Driver Implementation

In `kernel/fte3600.c`:
1. Step 1 (`fte3600_spi_write_reg16(data, 0x1A84, 0xFFFF)`) was deleted.
2. Step 8 (`fte3600_spi_write_reg16(data, 0x1A84, 0xFFFF)`) was deleted.
3. The remaining steps were consolidated into a clean 6-step volatile sequence.
4. Function comments and docstrings were updated to record the isolation decision.

---

## 5. Test Suite Accounting & Verification

In `kernel/fte3600-test.c`:
- `mock_spi_startup_profile_t` was updated to remove `irq_clear1_ret` and `irq_clear2_ret`.
- `sim_driver_hw_startup()` was updated to remove both IRQ clear steps.
- Scenario 8 was repurposed as an explicit check: `test_d6_startup_scenario_8_no_irq_clear_in_baseline`, asserting that exactly 16 transfers occur during startup and zero writes are dispatched to `0x1A84`.

---

## 6. Conclusion

Isolating the `0x1A84` register write from the baseline initialization path restores strict adherence to verified factory reference protocols, eliminates unverified register mutations, and establishes a secure, bounded foundation for Stage D7.
