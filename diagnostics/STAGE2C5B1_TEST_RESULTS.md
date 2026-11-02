# Stage 2C.5B.1 — Build Verification & Test Suite Execution Report

**Document ID:** `STAGE2C5B1_TEST_RESULTS`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Audit Scope:** Build verification, compiler diagnostics, and test suite execution logs following Stage 2C.5B.1 software remediations.  
**Hardware Authorization:** NONE (Software & Static Analysis Only).

---

## 1. Executive Summary

This report documents the compilation diagnostics, linker outputs, and test execution results for the remediated Stage 2C.5B.1 codebase.

Following the remediation of allowlist fallthrough (Defect 1), missing opcode complement validation (Defect 2), and ignored directional register permissions (Defect 3), the test suite was expanded with 8 new test assertions across Test Suites 2 and 8.

All builds compile cleanly with zero warnings under `-Wall -Wextra -Werror -pedantic -std=c11 -O2`. All 83 test assertions pass with 100.0% success in pure userspace memory.

---

## 2. Compilation and Build Logs

### 2.1 Default Target Build (`make clean && make all`)
```bash
make -C tools/fw9369-mock clean && make -C tools/fw9369-mock all
```

**Build Output:**
```text
make: Entering directory '/home/titan/fte3600-linux-driver/tools/fw9369-mock'
rm -f src/*.o tests/*.o ../../bin/fw9369-tool ../../bin/fw9369-tests ../../bin/fw9369-hw-tool
make: Leaving directory '/home/titan/fte3600-linux-driver/tools/fw9369-mock'
make: Entering directory '/home/titan/fte3600-linux-driver/tools/fw9369-mock'
cc -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Iinclude -c src/fw9369_crc.c -o src/fw9369_crc.o
cc -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Iinclude -c src/fw9369_protocol.c -o src/fw9369_protocol.o
cc -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Iinclude -c src/fw9369_transport_mock.c -o src/fw9369_transport_mock.o
cc -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Iinclude -c src/fw9369_calibration.c -o src/fw9369_calibration.o
cc -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Iinclude -c src/fw9369_state.c -o src/fw9369_state.o
cc -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Iinclude -c src/fw9369_main.c -o src/fw9369_main.o
cc -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Iinclude src/fw9369_crc.o src/fw9369_protocol.o src/fw9369_transport_mock.o src/fw9369_calibration.o src/fw9369_state.o src/fw9369_main.o -o ../../bin/fw9369-tool
cc -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Iinclude -c src/fw9369_transport_hw.c -o src/fw9369_transport_hw.o
cc -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Iinclude -c tests/test_runner.c -o tests/test_runner.o
cc -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Iinclude src/fw9369_crc.o src/fw9369_protocol.o src/fw9369_transport_mock.o src/fw9369_calibration.o src/fw9369_state.o src/fw9369_transport_hw.o tests/test_runner.o -o ../../bin/fw9369-tests
make: Leaving directory '/home/titan/fte3600-linux-driver/tools/fw9369-mock'
```

### 2.2 Explicit Hardware Tool Build (`make hw-tool`)
```bash
make -C tools/fw9369-mock hw-tool
```

**Build Output:**
```text
make: Entering directory '/home/titan/fte3600-linux-driver/tools/fw9369-mock'
cc -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Iinclude -c src/fw9369_main_hw.c -o src/fw9369_main_hw.o
cc -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Iinclude src/fw9369_crc.o src/fw9369_protocol.o src/fw9369_transport_hw.o src/fw9369_calibration.o src/fw9369_state.o src/fw9369_main_hw.o -o ../../bin/fw9369-hw-tool
make: Leaving directory '/home/titan/fte3600-linux-driver/tools/fw9369-mock'
```

---

## 3. Test Runner Execution Results (`bin/fw9369-tests`)

```bash
./bin/fw9369-tests
```

**Execution Log:**
```text
===================================================================
 FocalTech FW9369 Automated Mock Tooling Test Suite
 Target Silicon: FT9362 / FW9369
 Mode: Strictly In-Memory Mock Testing (Zero Hardware Access)
===================================================================

--- Test Suite 1: CRC Implementations & Vectors ---
  [PASS] Wire CRC matches Chip ID 0x9362 test vector (0x1C53)
  [PASS] Host LE CRC matches [0, 0, 0, 0] test vector (0x313E)
  [PASS] Host LE CRC matches [512, 512, 512, 512] test vector (0x5B7C)
  [PASS] Host LE CRC matches [482, 482, 482, 482] test vector (0xE4AA)
  [PASS] Byte ordering distinction verified (BE wire != LE host word)

--- Test Suite 2: Protocol Framing & Allowlist ---
  [PASS] Byte write packet framing correct
  [PASS] Byte read packet framing correct
  [PASS] Word write packet framing correct
  [PASS] Command wake framing correct
  [PASS] FIFO burst read packet length (10,246 B) and prefix correct
  [PASS] Allowlist permits register 0xC6 write
  [PASS] Allowlist permits register 0xC6 read
  [PASS] Allowlist permits register 0x1A8B read
  [PASS] Allowlist rejects register 0x1A8B write (read-only)
  [PASS] Allowlist rejects register 0x80 write (read-only)
  [PASS] Allowlist rejects register 0x9A read (write-only)
  [PASS] Allowlist rejects unauthorized register 0x0000
  [PASS] Allowlist rejects unauthorized register 0xFFFF
  [PASS] Command allowlist permits wake (0x5A 0xA5)
  [PASS] Command allowlist permits end (0xA5 0x5A)
  [PASS] Command allowlist rejects unauthorized opcode (0x12 0x34)

--- Test Suite 3: Mock Transport Invariants & Safety ---
  [PASS] Mock transport created successfully in memory
  [PASS] Initial 0xC6 register is 0x00
  [PASS] Initial protected trim bank is locked
  [PASS] Mock accurately stores byte register 0xC6 write
  [PASS] Mock returns 0x01 on 0xC6 readback
  [PASS] Transaction logger accurately records packet count
  [PASS] Hardware transport creation is REJECTED with -ENOTSUP without touching devices

--- Test Suite 4: FDT Calibration & Minimum Tracking ---
  [PASS] FDT calibration executes cleanly on mock
  [PASS] Calibration valid flag asserted
  [PASS] DAC centering search converges at target setting
  [PASS] Early exit condition triggered when 3 consecutive stable samples observed
  [PASS] All tracked minima within valid bounds [300, 700]
  [PASS] Threshold derivation strictly matches fdt_down = minima - 30
  [PASS] FDT threshold CRC matches 4-word LE calculation

--- Test Suite 5: Image Baseline & Single-Frame Copy ---
  [PASS] Image baseline acquired cleanly on mock
  [PASS] Baseline valid flag asserted
  [PASS] Image DAC nominal setting preserved
  [PASS] Cold baseline is an exact un-averaged copy of captured raw frame
  [PASS] Baseline CRC covers exactly first 128 words (256 bytes)

--- Test Suite 6: Full Pipeline & Protected Register Cleanup ---
  [PASS] Full 5-phase pipeline executes to completion
  [PASS] Final phase is FW9369_PHASE_COMPLETED
  [PASS] Silicon identified as FT9362 (0x9362)
  [PASS] Hardware Chip ID CRC validated (0x1C53)
  [PASS] MCU acknowledged idle handshake (0x50)
  [PASS] Autonomous finger detection successfully armed
  [PASS] Lock command 0x9A=0x00 submitted cleanly
  [PASS] Simulated analog trim bank relocked

--- Test Suite 7: Deterministic Failure Injection Scenarios ---
  [PASS] Failure Injection 1: Transfer failure aborts cleanly
  [PASS] Failure Injection 2: Unexpected Chip ID (0x9368) aborts cleanly
  [PASS] Failure Injection 3: Corrupt wire CRC aborts cleanly
  [PASS] Failure Injection 4: ADC status polling timeout aborts cleanly
  [PASS] Failure Injection 5: Out of bounds minima (< 300) aborts cleanly
  [PASS] Failure Injection 6: 10,246-byte FIFO transfer failure aborts cleanly
  [PASS] Failure Injection 7: Lock write failure reports FW9369_LOCK_TRANSMISSION_FAILED
  [PASS] Failure Injection 8: Interruption in trim access caught and handled
  [PASS] Failure Injection 8: Cleanup handler unconditionally submitted lock command

--- Test Suite 8: Hardware Transport Offline Validation & Isolation ---
  [PASS] Valid device path /dev/spidev1.0 accepted offline
  [PASS] Arbitrary device /dev/spidev0.0 rejected with -ENODEV (no fallback)
  [PASS] Invalid device /dev/null rejected with -ENODEV
  [PASS] Null device path rejected with -EINVAL
  [PASS] Valid hardware configuration accepted offline
  [PASS] Speed > 1 MHz rejected with -EINVAL
  [PASS] Non-zero SPI mode rejected with -EINVAL
  [PASS] Non-8-bit word size rejected with -EINVAL
  [PASS] Zero max transfer length rejected with -EINVAL
  [PASS] Hardware transport instantiated with fake SPI system-call hooks
  [PASS] Hardware transport acquired simulated file descriptor 42
  [PASS] Valid byte write packet passes allowlist and executes with continuous CS (cs_change=0)
  [PASS] Unauthorized register write rejected with -EPERM at transport boundary
  [PASS] Unauthorized command opcode rejected with -EPERM at transport boundary
  [PASS] Unknown opcode rejected with -EPERM (fail-closed fallthrough prevention)
  [PASS] Malformed opcode complement rejected with -EPERM
  [PASS] Direction violation (write to read-only reg 0x80) rejected with -EPERM
  [PASS] Direction violation (read from write-only reg 0x9A) rejected with -EPERM
  [PASS] Transfer exceeding max length rejected with -EMSGSIZE
  [PASS] Short SPI transfer detected and reported as -EMSGSIZE
  [PASS] Ioctl -EIO error cleanly propagated without retries
  [PASS] Valid byte read packet executes full-duplex transfer cleanly
  [PASS] Hardware transaction logger accurately records packet count and status
  [PASS] File descriptor deterministically closed upon transport destruction
  [PASS] Mock hardware rejection stub returns NULL with -ENOTSUP without touching devices
  [PASS] Default binary isolation verified: bin/fw9369-tool contains zero hardware backend symbols

===================================================================
 Final Test Results: 83 / 83 Passed (100.0%)
===================================================================
```

---

## 4. Hardware Tool Safety Gate Verification (`bin/fw9369-hw-tool`)

### 4.1 Offline Configuration Validation Check
```bash
./bin/fw9369-hw-tool --validate-config
```
```text
===================================================================
 FocalTech FW9369 Hardware Transport Tool (Stage 2C.5B)
 Safety Boundary: Physical hardware execution is STRICTLY PROHIBITED
 Target Device:   /dev/spidev1.0
 Target Speed:    1000000 Hz
===================================================================

[PASS] Offline configuration validation: OK (Mode 0, 8-bit, MSB-first, <= 1 MHz)
[INFO] Offline validation complete. Exiting cleanly without device access.
```
*(Exit code: 0)*

### 4.2 Fail-Closed Execution Gate Check
```bash
./bin/fw9369-hw-tool
```
```text
===================================================================
 FocalTech FW9369 Hardware Transport Tool (Stage 2C.5B)
 Safety Boundary: Physical hardware execution is STRICTLY PROHIBITED
 Target Device:   /dev/spidev1.0
 Target Speed:    1000000 Hz
===================================================================

[PASS] Offline configuration validation: OK (Mode 0, 8-bit, MSB-first, <= 1 MHz)

[SECURITY ALERT] Stage 2C.5B is static preparation only.
[SAFETY GATE PASS] Physical device access is blocked by policy.
To verify hardware transport mechanics offline, execute bin/fw9369-tests.
```
*(Exit code: 1)*

---

## 5. Test Suite Comparison Matrix

| Test Suite | Stage 2C.4.1 Count | Stage 2C.5B Count | Stage 2C.5B.1 (Remediated) Count | Passing Rate |
| :--- | :---: | :---: | :---: | :---: |
| **Suite 1: CRC Implementations** | 5 | 5 | 5 | 100.0% |
| **Suite 2: Protocol Framing & Allowlist** | 12 | 12 | 16 | 100.0% |
| **Suite 3: Mock Transport Invariants** | 7 | 7 | 7 | 100.0% |
| **Suite 4: FDT Calibration** | 7 | 7 | 7 | 100.0% |
| **Suite 5: Image Baseline Acquisition** | 5 | 5 | 5 | 100.0% |
| **Suite 6: Full Pipeline Execution** | 8 | 8 | 8 | 100.0% |
| **Suite 7: Deterministic Failure Injection** | 9 | 9 | 9 | 100.0% |
| **Suite 8: Hardware Transport Validation** | N/A | 22 | 26 | 100.0% |
| **Total Test Assertions** | **53** | **75** | **83** | **100.0%** |
