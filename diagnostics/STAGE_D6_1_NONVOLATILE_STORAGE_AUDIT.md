# Stage D6.1 — Audit of Non-Volatile Storage & Persistence Claims

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Hardware:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`, Intel LPSS SPI2)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Document Ref:** `diagnostics/STAGE_D6_1_NONVOLATILE_STORAGE_AUDIT.md`  

---

## 1. Assessment of the Stage D6 Non-Volatile Claim

In Stage D6, the safety classification report (`STAGE_D6_SAFETY_CLASSIFICATION.md`) asserted:
> *"Non-volatile writes confirmed non-existent (FT9362 utilizes mask ROM with no EEPROM or flash VPP)."*

This audit independently assesses that statement across all eight technical dimensions mandated by the safety gate.

---

## 2. Technical Evidence Inventory

### 2.1 Mask ROM Evidence
- **Observed Behavior:** Neither the Dynabook Windows OEM driver package (`ftWbioUmdfDriverV2.dll`, `ftWbioEngineAdapter.dll`) nor the reference Linux driver (`libfprint-fte3600`) downloads any binary firmware payload (`.bin`, `.hex`, `.rom`) to the FT9362 controller upon boot. This contrasts with other FocalTech models (FT9338, FT9348, FT9361, FT9368, FT9536) which require external firmware distribution.
- **Inference vs Proof:** The absence of host-side firmware loading indicates that the primary control sequencer is hardwired on-chip. However, whether the on-chip storage is strictly metal-mask ROM, factory-programmed internal ROM, or write-protected embedded flash is an engineering inference, not an authoritative silicon fact.
- **Rating:** `PARTIALLY SUPPORTED (INFERRED FROM HOST DRIVER BEHAVIOR)`.

### 2.2 EEPROM & Internal Flash
- **Observed Behavior:** The FT9362 register set does not expose standard JEDEC serial NOR/NAND flash opcodes (such as `0x06` Write Enable, `0x20` Sector Erase, `0x02` Page Program).
- **Silicon Reality:** No flash programming routines exist in any known host driver. However, the absence of host flash programming does not prove that an internal EEPROM or flash macro is completely non-existent on the die.
- **Rating:** `NOT ESTABLISHED (UNVERIFIED WITHOUT DIE ARCHITECTURE SCHEMATIC)`.

### 2.3 One-Time Programmable (OTP) & EFUSE Memory
- **Direct Contradiction:** The D6 claim that non-volatile memory is "non-existent" is **DIRECTLY CONTRADICTED** by the codebase itself:
  1. `research/libfprint-fte3600/libfprint/drivers/fte3600/ft93xx.c:596-628`: Implements function `efuse_byte()` using registers `0xF1`, `0xF4`, and `0xF3`.
  2. `research/libfprint-fte3600/libfprint/drivers/fte3600/discovery.c:63-81`: Implements function `discover_otp()` using registers `0xF1`, `0xF4`, and `0xF3`.
  3. `STAGE2B2_OTP_SENSING_REVIEW.md`: Documents that registers `0xF1` and `0xF4` clock on-chip OTP sense amplifiers to latch wafer foundry metadata into register `0x9B`.
  4. Windows driver binary symbols: Contain export `fw9369_Get_OTP_Info`.
- **Finding:** Non-volatile OTP/eFuse storage **DOES EXIST** on FT9362 silicon. It stores manufacturing metadata burned during wafer testing.
- **Rating:** `CONTRADICTED (OTP/EFUSE MACROS ARE PRESENT AND ACTIVELY STROBED)`.

### 2.4 Persistent Calibration Storage
- **Observed Behavior:** Neither baseline analog DAC settings (`0x1801`) nor finger detection thresholds (`0x00E0`) are permanently stored on the sensor.
- Calibration is dynamically calculated by userspace algorithms during each session and written to volatile registers.
- When hardware reset is asserted or power is cut, all calibration registers revert to uncalibrated power-on defaults.
- **Rating:** `PARTIALLY SUPPORTED (OBSERVED IN FACTORY CODE; VOLATILE AT SESSION LEVEL)`.

### 2.5 Programming Voltage ($V_{PP}$) vs Alternative Programming Mechanisms
- **Flawed Inference in D6:** The D6 report inferred that persistent programming is impossible because no external high-voltage $V_{PP}$ rail is supplied by the Dynabook motherboard.
- **Semiconductor Engineering Reality:**
  * While legacy UV-EPROMs required external 12V–21V $V_{PP}$, modern embedded antifuse (eFuse) macros in standard CMOS technologies (e.g., TSMC/SMIC 55nm/110nm) can be programmed at low voltages or via on-chip capacitive charge pumps powered by standard $V_{DD}$ (3.3V or 1.8V).
  * Inferring that persistent silicon alteration is impossible simply because no external $V_{PP}$ pin exists is a fundamental violation of semiconductor engineering principles.
- **Rating:** `NOT ESTABLISHED (UNSUPPORTED BY ELECTRICAL PRINCIPLES)`.

### 2.6 Register Writes with Persistent Side Effects
- While the intended purpose of the `0xF1`/`0xF4` sequence is reading OTP data via sense amplifiers, there is zero silicon documentation establishing what safeguards prevent an errant or malformed write from altering eFuse state or entering a test-mode burn state.
- **Rating:** `NOT ESTABLISHED (LACKS SILICON SAFEGUARD VERIFICATION)`.

---

## 3. Classification of the Stage D6 Claim

In accordance with audit instructions, the Stage D6 claim is classified as:

| Statement in Stage D6 Report | Audit Classification |
| :--- | :---: |
| *"Non-volatile memory verified non-existent"* | **CONTRADICTED** |
| *"Persistent programming impossible due to absence of flash VPP"* | **NOT ESTABLISHED** |
| **Overall Stage D6 Non-Volatile Claim Verdict** | **CONTRADICTED** |

---

## 4. Formal Correction of the Safety Classification

The Stage D6 safety classification must be corrected as follows:

1. **Non-Volatile Presence:** FT9362 / FW9369 silicon **does contain non-volatile memory** in the form of factory-programmed One-Time Programmable (OTP) / eFuse cells storing wafer lot and foundry parameters.
2. **Operational Intent:** The host driver's volatile initialization sequence uses registers `0xF1`, `0xF4`, and `0xF3` strictly to *interrogate* and *read out* pre-burned OTP metadata into register `0x9B`.
3. **Persistence Risk:** While the operations are intended to be read-only, their lack of persistence cannot be proven by the absence of an external $V_{PP}$ pin. Because authoritative silicon errata and macro specifications are unavailable, operations interacting with registers `0xF1` and `0xF4` must be classified as **`SOURCE-OBSERVED BUT HARDWARE SEMANTICS UNVERIFIED`**, not "verified non-existent".
