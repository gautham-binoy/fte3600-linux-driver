# STAGE D6.3 — FINAL INDEPENDENT AUDIT VERDICT

**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Host Architecture:** Intel LPSS SPI2 (`spi-FTE3600:00`, CS0) on Dynabook Satellite Pro C40-J  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Date:** 2026-11-04  
**Author:** Antigravity (Advanced Agentic Pair Programming)  
**Classification:** FINAL AUDIT VERDICT & GOVERNANCE GATE

---

## 1. Audit Synthesis & Assessment Matrix

| Audit Domain | Assessment & Findings | Evidence Classification | Status |
| :--- | :--- | :--- | :--- |
| **Command Strobe Framing** | Verified 3-byte framing `{ cmd, ~cmd, 0x00 }` across all callers. Verified single-transaction continuous Chip-Select assertion. Byte-for-byte match with vendor reference code. | `VENDOR-REFERENCE-OBSERVED` | **VERIFIED** |
| **Transfer Accounting** | 16 transfers (66 B wire) normal path; 18 transfers (74 B wire) abort recovery; 3 transfers (19 B) Stage C identify; 20 combined operations from reset rigorously proven. | `SOURCE-OBSERVED` | **VERIFIED** |
| **Interrupt Clear Removal** | Zero `0x1A84` writes in baseline startup. Protects against undocumented upper-bit manipulation. However, vendor source does issue `write_word(0x1A84, 0xFFFF)` in `fw9369_startup.c:343, 371`. | `VENDOR-REFERENCE-OBSERVED` / `NOT ESTABLISHED` | **UNRESOLVED** |
| **Silicon IRQ Semantics** | Bit definitions for bits 6–15 of register `0x1A84` and whether omitting `0xFFFF` leaves power-on interrupt latches pending on physical IRQ pin remain undocumented. | `NOT ESTABLISHED` | **UNRESOLVED** |
| **READY State Transition** | Driver declares `READY` after foundry interrogation. However, vendor code requires `fdt_baseline()`, `image_baseline()`, and `arm()` before operational capture readiness. | `VENDOR-REFERENCE-OBSERVED` / `INFERRED` | **OVER-CLAIM** |
| **Status 0x80 & Abort Handling** | Abort strobe `{ 0xC0, 0x3F, 0x00 }` matches vendor source. Driver implements defensive retry read and fail-closed exit if status is non-idle (`!= 0x50`). | `VENDOR-REFERENCE-OBSERVED` / `SOURCE-OBSERVED` | **VERIFIED** |
| **Foundry Interrogation Loop** | 12 transfers across `0xF1`, `0xF4`, `0xF3` and fab read `0x9B` match vendor driver byte-for-byte. SMIC `0xAA` (150 us) vs default (200 us) logic verified. | `VENDOR-REFERENCE-OBSERVED` | **VERIFIED** |
| **Non-Volatile Storage Classification** | Acknowledges on-chip OTP / eFuse macro. Programming capability classified as NOT ESTABLISHED. Zero write/programming operations dispatched. | `SOURCE-OBSERVED` | **VERIFIED** |
| **Regression & ABI Integrity** | 32-byte UAPI struct layouts, ioctl encodings, Stage C compatibility, D5.4 refcounting, and offline poll semantics 100% preserved. | `SOURCE-OBSERVED` | **VERIFIED** |
| **Test Evidence** | Standalone userspace test harness: 50 / 50 unit tests passed (100%). Policy tests: 9 / 9 passed (100%). Kernel build: 0 warnings. | `SOURCE-OBSERVED` | **VERIFIED** |
| **Option 1 Retention** | Module remains Live (`fte3600 20480 0`), device node `511, 0`, state `8 (IDENTIFIED)`, SPI statistics strictly `transfers = 3`, `bytes = 19`. | `HARDWARE-VERIFIED` | **VERIFIED** |

---

## 2. Final Audit Verdict

### Verdict: **CONDITIONAL PASS**

**Formal Definition:**
`CONDITIONAL PASS — framing and implementation are correct, but specified protocol assumptions remain unresolved.`

### Detailed Verdict Justification:
1. **Why PASS on Framing and Implementation:**
   - The command strobe wire length defect from Stage D6 has been completely remediated: all strobes transmit exactly 3 bytes `{ cmd, (u8)~cmd, 0x00 }` under continuous Chip Select.
   - The volatile startup transfer sequences (16 transfers / 66 bytes normal, 18 transfers / 74 bytes abort recovery) are completely verified in source code and in mock tests.
   - The driver is memory-safe, concurrency-hardened, preserves all ABI guarantees, and compiles cleanly with 0 warnings.
   - Physical hardware invariants remain 100% untouched (`transfers = 3`, `bytes = 19`, state 8 `IDENTIFIED`).
2. **Why CONDITIONAL on Protocol Assumptions:**
   - **Protocol Assumption 1 (Interrupt Clear Removal):** Vendor source `fw9369_startup.c:343, 371` and `ft93xx.c:547, 797` explicitly performs `write_word(0x1A84, 0xFFFF)`. While removing this write protects against writing to unmapped upper bits (6–15), its omission diverges from vendor initialization. Whether omitting `0x1A84 = 0xFFFF` leaves power-on interrupt latches asserted on physical silicon remains `NOT ESTABLISHED`. This assumption must be resolved in Stage D7 (Touch/IRQ integration).
   - **Protocol Assumption 2 (`READY` State Transition):** Declaring the device `READY` after only foundry interrogation is an architectural over-claim. In authoritative vendor code, the device is uncalibrated and cannot capture images or detect touch without prior baseline acquisition (`fdt_baseline`, `image_baseline`) and threshold arming (`arm()`). An intermediate internal state (`FTE3600_STATE_INITIALIZED_UNCALIBRATED`) must be introduced before hardware capture authorization.

---

## 3. Mandatory Safety Notice

> [!CAUTION]
> **ABSOLUTE HARDWARE SAFETY BOUNDARY:**
> This CONDITIONAL PASS verdict is strictly a software and protocol evaluation.
> **A passing or conditional pass source audit does NOT authorize physical hardware initialization or execution.**
> Zero physical SPI transactions, zero register writes, and zero ioctl calls may be issued until explicit, separate operator authorization is granted under an authorized execution plan.
> The driver must remain loaded and bound under Option 1 in state `8 (IDENTIFIED)` with historical SPI statistics unchanged (`transfers = 3`, `bytes = 19`).
