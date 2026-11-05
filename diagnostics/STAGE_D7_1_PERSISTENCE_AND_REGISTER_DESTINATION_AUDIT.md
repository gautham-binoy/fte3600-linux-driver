# Stage D7.1 — Adversarial Audit of Register Destinations & Persistent Storage Blast Radius

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source and Documentation Audit  
**Audit Classification:** Adversarial Challenge & Evidence Qualification  

---

## 1. Executive Summary & Challenge to Stage D7 Findings

In Stage D7, the report concluded: *"Every single operation performed during fdt_baseline(), image_baseline(), and arm() targets volatile registers, volatile SFRs, or volatile analog front-end flip-flops. Zero non-volatile memory registers are accessed."*

This Stage D7.1 audit independently challenges the certainty of that assertion:
1. **Absence of Silicon Datasheets:**  
   Without an official hardware register manual, describing registers as "established volatile SFRs" is an extrapolation from vendor software semantics, not empirical silicon proof.
2. **The "No External VPP" Fallacy:**  
   Modern CMOS sensor ASICs frequently integrate on-chip charge pumps capable of generating internal programming voltages (12V–18V) from standard 3.3V VDD lines. The absence of an external VPP pin or explicit high-voltage line on the FT9362 packaging does **not** prove that non-volatile modification is physically impossible.
3. **Indirect Addressing & Bank Switching Opacity:**  
   The procedure unlocks an internal memory bank via `write 0x9A = 0x5A` and modifies registers `0xC0..0xC2` and `0x80E0`/`0x80B0`. The physical silicon nature of this bank (whether SRAM, flip-flops, or shadow-latched EEPROM) is completely undocumented by FocalTech.
4. **Command Strobe Opacity:**  
   Strobes such as `0xC2 0x3D` (FDT trigger) and `0xC4 0x3B` (capture trigger) invoke internal silicon state machines whose complete gate-level actions are proprietary black boxes.

---

## 2. Comprehensive Destination Classification Matrix

Every register address, multi-byte bank, FIFO, and strobe accessed across `fdt_baseline()`, `image_baseline()`, and `arm()` was audited and classified under rigorous evidentiary standards:

| Address / Range | Opcode / Access | Software Function | Observed Source Provenance | Rigorous Persistence Classification |
|---|---|---|---|---|
| `0x80` | Read Byte (`0x08 0xF7`) | Idle / Engine status (`0x50` / `0x54`) | `fw9369_fdt.c:74`, `capture.c:270` | **Volatile SFR established by source** |
| `0x8E` | Write Byte (`0x09 0xF6`) | Timing / FIFO clock divider (`0xF4`) | `capture.c:173` | **Volatile SFR established by source** |
| `0x90` | Write Byte (`0x09 0xF6`) | Sequencer strobe (`0x00`, then `0x01`)| `capture.c:174, 177` | **Volatile SFR established by source** |
| `0x91` | Write Byte (`0x09 0xF6`) | Sequencer length high byte (`0x07`) | `capture.c:175` | **Volatile SFR established by source** |
| `0x92` | Write Byte (`0x09 0xF6`) | Sequencer length low byte (`0xD0`) | `capture.c:176` | **Volatile SFR established by source** |
| `0x9A` | Write Byte (`0x09 0xF6`) | Bank unlock gate (`0x5A` / `0x00`) | `fw9369_fdt.c:215`, `fw9369_startup.c:396` | **Volatile SFR gate (Target memory unverified)** |
| `0xC0..0xC2` | Read/Modify Word | FDT analog threshold settings | `fw9369_fdt.c:219–221` (under `0x9A=0x5A`) | **Unknown persistence (Bank-switched memory)** |
| `0xC9..0xDB` | Read/Modify Word | 8-channel mode threshold settings | `fw9369_fdt.c:218` (under `0x9A=0x5A`) | **Unknown persistence (Bank-switched memory)** |
| `0x80E0` / `0x80B0` | Write Multi-Word (`0x05 0xFA`)| 8-word FDT comparator threshold bank | `fw9369_startup.c:388` (under `0x9A=0x5A`)| **Unknown persistence (Bank-switched memory)** |
| `0x80E8` / `0x80B8` | Read Multi-Word (`0x04 0xFB`) | 4-channel raw ADC readout | `fw9369_fdt.c:267` | **Volatile ADC read buffer** |
| `0x1800` | Read/Write Word | Sensing window & FIFO read enable | `fw9369_fdt.c:195`, `capture.c:232, 276` | **Volatile SFR established by source** |
| `0x1801` | Read/Write Word | Analog DAC bias (`0xFC80 \| dac`) | `fw9369_fdt.c:177`, `capture.c:229` | **Volatile SFR established by source** |
| `0x1804` | Read/Write Word | Gain shift & integrator multiplier | `fw9369_fdt.c:196`, `capture.c:159` | **Volatile SFR established by source** |
| `0x1805` | Read/Modify Word | Sequencer timing | `fw9369_fdt.c:210`, `capture.c:168` | **Volatile SFR established by source** |
| `0x1806` | Read/Modify Word | Integrator timing | `fw9369_fdt.c:202`, `capture.c:160` | **Volatile SFR established by source** |
| `0x1807` | Read/Write Word | Line period timing | `fw9369_fdt.c:198`, `capture.c:163` | **Volatile SFR established by source** |
| `0x1808` | Read/Write Word | FDT scan row count | `fw9369_fdt.c:199` | **Volatile SFR established by source** |
| `0x180A, 0x180B`| Read/Modify Word | Analog frontend delays | `fw9369_fdt.c:203–204`, `capture.c:161–162` | **Volatile SFR established by source** |
| `0x180C` | Read/Write Word | Calibration mode flag (`0x0000`/`0x0600`) | `fw9369_fdt.c:182` | **Volatile SFR established by source** |
| `0x180D` | Read/Write Word | Integrator bias control | `fw9369_fdt.c:212` | **Volatile SFR established by source** |
| `0x1811` | Read/Modify Word | Window filter parameter | `capture.c:169` | **Volatile SFR established by source** |
| `0x1880` | Read/Modify Word | Threshold high/low limits | `fw9369_fdt.c:225` | **Volatile SFR established by source** |
| `0x1881` | Read/Modify Word | Scan timing, trigger enable, arm mode| `fw9369_fdt.c:190, 227`, `fw9369_startup.c:408`| **Volatile SFR established by source** |
| `0x1884` | Read/Modify Word | Hardware sample averaging count | `fw9369_fdt.c:228` | **Volatile SFR established by source** |
| `0x1885` | Write Word | FDT scan conversion trigger | `fw9369_fdt.c:247` | **Volatile SFR established by source** |
| `0x1887` | Read/Write Word | Channel mode configuration | `fw9369_fdt.c:201`, `capture.c:164` | **Volatile SFR established by source** |
| `0x1888` | Read/Modify Word | Sequencer clock gating | `fw9369_fdt.c:213` | **Volatile SFR established by source** |
| `0x1A82` | Read Word | Event status register | `fw9369_fdt.c:250`, `fw9369_release.c:318` | **Volatile SFR (Read-only)** |
| `0x1A83` | Read/Modify Word | Event mask register | `fw9369_fdt.c:230`, `fw9369_startup.c:413` | **Volatile SFR established by source** |
| `0x1A84` | Write Word | Event ACK register | `fw9369_startup.c:416`, `capture.c:280` | **Volatile SFR established by source** |
| `0x1A8A` | Write Word | Interrupt routing control (`0x00FF`) | `fw9369_fdt.c:229` | **Volatile SFR established by source** |
| `0x9A05` | Read Multi-Byte | Image FIFO buffer (10,246 wire bytes)| `capture.c:186` | **Volatile Hardware FIFO SRAM** |
| `0x5A/0xA5` | 3-Byte Strobe | Enter Idle Command | `fw9369_fdt.c:71`, `capture.c:221` | **Command Strobe (No storage)** |
| `0xA5/0x5A` | 3-Byte Strobe | Exit Idle Command | `fw9369_fdt.c:79`, `capture.c:228` | **Command Strobe (No storage)** |
| `0xC0/0x3F` | 3-Byte Strobe | Abort / Soft Wake Strobe | `fw9369_fdt.c:76`, `capture.c:225` | **Command Strobe (No storage)** |
| `0xC2/0x3D` | 3-Byte Strobe | FDT Scan / Arm Trigger Strobe | `fw9369_fdt.c:245`, `fw9369_startup.c:417` | **Command Strobe (No storage)** |
| `0xC4/0x3B` | 3-Byte Strobe | Image Exposure Trigger Strobe | `capture.c:267` | **Command Strobe (No storage)** |

---

## 3. Persistent Storage Blast-Radius Audit

### 3.1 Known OTP / eFuse Programming Pathways
Stage D6.1 established that OTP registers exist at:
- `0xF1`: OTP address register.
- `0xF4`: OTP control register (strobe bits `0xC0`, `0xC1`, `0x80`, `0x81`).
- `0xF3`: OTP data readout register.

**Audit Verification:**  
A global grep across `fw9369_fdt.c`, `capture.c`, `image.c`, and the calibration routines in `fw9369_startup.c` confirms that **neither `0xF1`, `0xF4`, nor `0xF3` is written or read during `fdt_baseline()`, `image_baseline()`, or `arm()`.**
The only place `0xF1/0xF4/0xF3` is touched in the startup pipeline is during `prefix()` (`fw9369_startup.c:349–354`), which was previously audited as an OTP read operation to detect the foundry code.

### 3.2 Bank Switching Opacity & Risk Assessment
The operations at `0x9A = 0x5A` access:
- `0xC0..0xC2` (FDT analog baseline configuration)
- `0x80E0` / `0x80B0` (FDT comparator threshold bank)

**Adversarial Risk Evaluation:**
- In vendor code, these writes are performed repeatedly during normal runtime finger-release cycles (`fw9369_release.c:273`).
- If these locations were flash or non-volatile EEPROM with limited write endurance (e.g. 10,000–100,000 cycles), repeatedly rewriting them on every finger touch and lift would wear out the silicon within weeks of normal laptop use.
- **Therefore, engineering evidence strongly supports that these bank-switched destinations are volatile SRAM or register latches.**
- However, strict scientific integrity requires classifying their persistence as **`[SOURCE-OBSERVED VOLATILE — UNPROVEN ON SILICON]`**.

---

## 4. Final Safety Determination

1. **No Deliberate Non-Volatile Writes:** There is zero source code instructing or executing writes to OTP, eFuse, or flash.
2. **Endurance Argument:** The runtime re-use of threshold banks proves that the vendor treats them as standard volatile RAM.
3. **Hardware Isolation Policy:** Any future implementation must preserve Option 1 and execute no hardware writes until explicitly authorized.
