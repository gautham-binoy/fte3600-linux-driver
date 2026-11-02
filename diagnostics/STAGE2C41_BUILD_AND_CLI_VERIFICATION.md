# Stage 2C.4.1 — Build, Test, and CLI Verification Audit

**Document ID:** `STAGE2C41_BUILD_AND_CLI_VERIFICATION`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Audit Scope:** Build process, test runner execution (53/53 tests), CLI invocation modes, and exit code investigation  
**Hardware Authorization:** NONE (Software & Static Analysis Only)

---

## 1. Executive Summary

This document presents the complete empirical verification of the build system, automated test suite, and command-line interface (CLI) for `tools/fw9369-mock/`. All tests were run directly on the Dynabook host in unprivileged userspace (`titan`) with zero hardware device interaction.

### Key Verification Metrics
- **Build Status:** Clean compilation with `gcc -Wall -Wextra -Werror -pedantic -std=c11 -O2` (0 warnings, 0 errors).
- **Test Suite Pass Rate:** **53 / 53 passed (100.0%)** across 7 distinct test suites.
- **Hardware Stub Enforcement:** `--hardware` rejected unconditionally with exit code 1 without probing device nodes.
- **CLI Robustness:** Successfully verified across standard execution, dry-run mode, and all 9 failure injection modes.
- **Exit Code Investigation:** Confirmed that prior exit-code-1 events were deliberate, expected responses to `--hardware` and failure injection tests, not unintended runtime crashes.

---

## 2. Build Verification Audit

The mock implementation was cleaned and rebuilt from scratch:

```bash
make -C tools/fw9369-mock clean && make -C tools/fw9369-mock all
```

### Compiler Output Log
```text
make: Entering directory '/home/titan/fte3600-linux-driver/tools/fw9369-mock'
rm -f src/*.o tests/*.o ../../bin/fw9369-tool ../../bin/fw9369-tests
make: Leaving directory '/home/titan/fte3600-linux-driver/tools/fw9369-mock'
make: Entering directory '/home/titan/fte3600-linux-driver/tools/fw9369-mock'
cc -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Iinclude -c src/fw9369_crc.c -o src/fw9369_crc.o
cc -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Iinclude -c src/fw9369_protocol.c -o src/fw9369_protocol.o
cc -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Iinclude -c src/fw9369_transport_mock.c -o src/fw9369_transport_mock.o
cc -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Iinclude -c src/fw9369_calibration.c -o src/fw9369_calibration.o
cc -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Iinclude -c src/fw9369_state.c -o src/fw9369_state.o
cc -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Iinclude -c src/fw9369_main.c -o src/fw9369_main.o
cc -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Iinclude src/fw9369_crc.o src/fw9369_protocol.o src/fw9369_transport_mock.o src/fw9369_calibration.o src/fw9369_state.o src/fw9369_main.o -o ../../bin/fw9369-tool
cc -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Iinclude -c tests/test_runner.c -o tests/test_runner.o
cc -Wall -Wextra -Werror -pedantic -std=c11 -O2 -Iinclude src/fw9369_crc.o src/fw9369_protocol.o src/fw9369_transport_mock.o src/fw9369_calibration.o src/fw9369_state.o tests/test_runner.o -o ../../bin/fw9369-tests
make: Leaving directory '/home/titan/fte3600-linux-driver/tools/fw9369-mock'
```

**Verification Verdict:** Exit code `0`. Zero compiler warnings or errors produced.

---

## 3. Test Runner Execution & Suite Breakdown

The test executable `bin/fw9369-tests` was executed directly:

```bash
bin/fw9369-tests
```

### Full Test Output Log
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
  [PASS] Allowlist permits register 0xC6
  [PASS] Allowlist permits register 0x1A8B
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

===================================================================
 Final Test Results: 53 / 53 Passed (100.0%)
===================================================================
```

**Verification Verdict:** Exit code `0`. All 53 assertions passed.

---

## 4. CLI Binary Execution Verification

### 4.1 Standard Mock Execution
```bash
bin/fw9369-tool
```
- **Output:** Completed all 5 phases, identified Chip ID `0x9362`, validated wire CRC `0x1C53`, MCU status `0x50`, calibrated DAC `28`, minima `[518, 519, 518, 519]`, thresholds `[488, 489, 488, 489]`, threshold CRC `0x0225`, baseline CRC `0x913F`, lock clean, detection armed, 60 transactions logged.
- **Exit Code:** `0` (`SUCCESS`).

### 4.2 Dry-Run Simulation Mode
```bash
bin/fw9369-tool --dry-run
```
- **Output:** Identical to standard run with dry-run banner.
- **Exit Code:** `0` (`SUCCESS`).

### 4.3 Hardware Mode Rejection
```bash
bin/fw9369-tool --hardware
```
- **Output:**
  ```text
  [SECURITY ALERT] Hardware mode is strictly prohibited in Stage 2C.4!
  [SAFETY GATE PASS] Hardware backend was rejected without accessing any device node.
  ```
- **Exit Code:** `1` (Intentional rejection).

### 4.4 Failure Injection Matrix Verification
Each failure injection mode was executed and audited:

| Mode Flag | Injected Condition | Observed Pipeline Status | Error Code | Lock Outcome | Transactions Logged | Exit Code | Expected Behavior Verified? |
| :--- | :--- | :---: | :---: | :--- | :---: | :---: | :---: |
| `--inject-fail=transfer` | All SPI transfers return `-EIO` | `ABORTED` | `-5` (`-EIO`) | `TRANSMISSION FAILED` | 0 | 1 | **YES** |
| `--inject-fail=short` | SPI transfer truncated to 4 B | `ABORTED` | `-90` (`-EMSGSIZE`) | `TRANSMITTED CLEAN (0x00)` | 2 | 1 | **YES** |
| `--inject-fail=chipid` | Returns `0x9368` instead of `0x9362` | `ABORTED` | `-19` (`-ENODEV`) | `TRANSMITTED CLEAN (0x00)` | 4 | 1 | **YES** |
| `--inject-fail=crc` | Corrupts wire CRC to `0xE3AC` | `ABORTED` | `-74` (`-EBADMSG`) | `TRANSMITTED CLEAN (0x00)` | 4 | 1 | **YES** |
| `--inject-fail=timeout` | Bit 3 of `0x1A82` never asserts | `ABORTED` | `-110` (`-ETIMEDOUT`) | `TRANSMITTED CLEAN (0x00)` | 36 | 1 | **YES** |
| `--inject-fail=jitter` | Fluctuations $> 5$ counts | `COMPLETED` | `0` | `TRANSMITTED CLEAN (0x00)` | 95 | 0 | **YES** (Completed full 10-sample window) |
| `--inject-fail=bounds` | Tracked minima $< 300$ (ADC = 250) | `ABORTED` | `-71` (`-ERANGE`) | `TRANSMITTED CLEAN (0x00)` | 188 | 1 | **YES** |
| `--inject-fail=lock` | Byte write to `0x9A=0x00` fails | `ABORTED` | `-5` (`-EIO`) | `TRANSMISSION FAILED` | 26 | 1 | **YES** |
| `--inject-fail=fifo` | 10,246 B FIFO read fails | `ABORTED` | `-5` (`-EIO`) | `TRANSMITTED CLEAN (0x00)` | 53 | 1 | **YES** |

---

## 5. Investigation of Earlier Exit Code 1 Results

During earlier command chaining in predecessor sessions, shell commands combining `--hardware` and `--inject-fail=crc` returned exit code 1. This investigation conducted a root-cause trace of those exit codes:

1. **Hardware Rejection Code Path (`src/fw9369_main.c:37`):**
   ```c
   struct fw9369_transport *hw = fw9369_hardware_transport_create("/dev/spidev1.0");
   if (!hw) {
       printf("[SAFETY GATE PASS] Hardware backend was rejected without accessing any device node.\n");
       return 1;
   }
   ```
   *Analysis:* The CLI intentionally returns `1` when hardware mode is requested and safely blocked. This is standard Unix behavior to signal to calling scripts that the requested hardware operation was not performed.

2. **Pipeline Failure Code Path (`src/fw9369_main.c:100–102`):**
   ```c
   if (ret != 0) {
       printf("\n>> Pipeline Execution: FAILED (code: %d)\n", ret);
       return 1;
   }
   ```
   *Analysis:* When failure injection flags (`transfer`, `short`, `chipid`, `crc`, `timeout`, `bounds`, `lock`, `fifo`) trigger an intentional pipeline abort, `fw9369_pipeline_run()` returns a negative error code (e.g., `-EBADMSG` = `-74`). The CLI detects `ret != 0` and exits with code `1`.

3. **Conclusion on Exit Code 1:**
   - Exit code 1 is **not a crash, segfault, or tool defect**.
   - It is the **correct and intended Unix status code** indicating that hardware access was blocked or that a simulated pipeline error successfully halted execution.

---

## 6. Audit Conclusion

The build system, test suite, and CLI binary of `tools/fw9369-mock/` are fully verified:
- Clean build under `-Wall -Wextra -Werror -pedantic -std=c11 -O2`.
- 100% automated test pass rate (53 / 53 passed).
- Deterministic and truthful behavior across all 9 failure modes.
- Hardware rejection functions flawlessly without filesystem access.
- Build and CLI verification is **PASSED**.
