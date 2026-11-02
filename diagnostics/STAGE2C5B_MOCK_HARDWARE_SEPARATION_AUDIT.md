# Stage 2C.5B — Mock and Hardware Transport Separation Audit

**Document ID:** `STAGE2C5B_MOCK_HARDWARE_SEPARATION_AUDIT`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Audit Scope:** Build system separation, binary symbol isolation, default executable purity, and offline test coverage.  
**Execution Authorization:** NONE. Software build verification and test harness execution only.

---

## 1. Executive Summary

A critical architectural mandate for Stage 2C.5B is that the introduction of a real Linux `spidev` transport backend (`src/fw9369_transport_hw.c`) must **never compromise the purity of the default mock executable** (`bin/fw9369-tool`).

This audit independently inspects:
1. The build system configuration (`Makefile`).
2. Binary symbol tables via `nm` to prove zero hardware leakage into the default executable.
3. Automated test coverage in Test Suite 8 verifying fail-closed isolation across 22 discrete assertions.
4. Total test runner results: **75 / 75 tests passed (100.0%)**.

---

## 2. Build Target Isolation & Makefile Architecture

In `tools/fw9369-mock/Makefile`:

```makefile
SRCS_COMMON = src/fw9369_crc.c \
              src/fw9369_protocol.c \
              src/fw9369_transport_mock.c \
              src/fw9369_calibration.c \
              src/fw9369_state.c

OBJS_COMMON = $(SRCS_COMMON:.c=.o)

SRCS_HW = src/fw9369_crc.c \
          src/fw9369_protocol.c \
          src/fw9369_transport_hw.c \
          src/fw9369_calibration.c \
          src/fw9369_state.c

OBJS_HW = $(SRCS_HW:.c=.o)

BIN_DIR = ../../bin
TOOL_BIN = $(BIN_DIR)/fw9369-tool
TEST_BIN = $(BIN_DIR)/fw9369-tests
HW_TOOL_BIN = $(BIN_DIR)/fw9369-hw-tool

.PHONY: all clean test hw-tool

all: $(TOOL_BIN) $(TEST_BIN)

hw-tool: $(HW_TOOL_BIN)
```

### 2.1 Isolation Verification Points
1. **Default Target (`all`):** Compiles only `$(TOOL_BIN)` (`bin/fw9369-tool`) and `$(TEST_BIN)` (`bin/fw9369-tests`).
2. **Mock Tool Dependency (`$(TOOL_BIN)`):** Links `$(OBJS_COMMON)` + `src/fw9369_main.o`.
   - `OBJS_COMMON` includes `src/fw9369_transport_mock.o`.
   - `src/fw9369_transport_hw.o` is **EXCLUDED** from `$(TOOL_BIN)`.
3. **Explicit Hardware Target (`hw-tool`):** Built only when `make hw-tool` is deliberately invoked by an operator.
4. **Test Runner Dependency (`$(TEST_BIN)`):** Links `src/fw9369_transport_hw.o` alongside `OBJS_COMMON` strictly to execute software-only unit tests against the hardware transport's validation routines and fake SPI system-call hooks.

---

## 3. Binary Symbol Table Audit (`nm`)

Automated symbol table inspection was executed across the compiled binaries:

### 3.1 Default Mock Tool Binary (`bin/fw9369-tool`)
```bash
nm -g bin/fw9369-tool | grep -E "fw9369_hw_"
```
- **Observed Output:** *(Empty string, exit code 1)*.
- **Verification:** **ZERO hardware transport symbols exist in `bin/fw9369-tool`.**

```bash
nm -g bin/fw9369-tool | grep -E "fw9369_mock_"
```
- **Observed Output:**
  ```text
  00000000000025a0 T fw9369_mock_get_last_tx
  0000000000002600 T fw9369_mock_get_reg_byte
  0000000000002630 T fw9369_mock_get_reg_word
  0000000000002580 T fw9369_mock_get_tx_count
  00000000000026b0 T fw9369_mock_is_protected_locked
  0000000000002470 T fw9369_mock_transport_create
  0000000000002530 T fw9369_mock_transport_destroy
  ```
- **Verification:** Default tool links **strictly to the mock backend**.

### 3.2 Hardware Tool Binary (`bin/fw9369-hw-tool`)
```bash
nm -g bin/fw9369-hw-tool | grep -E "fw9369_mock_"
```
- **Observed Output:** *(Empty string, exit code 1)*.
- **Verification:** Hardware tool contains **zero mock symbols**, confirming complete bidirectional isolation.

---

## 4. Test Suite 8: Software-Only Hardware Transport Testing

Test Suite 8 in `tests/test_runner.c` exercises the hardware transport completely offline using a fake SPI system-call layer (`test_ops`):

```text
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
  [PASS] Transfer exceeding max length rejected with -EMSGSIZE
  [PASS] Short SPI transfer detected and reported as -EMSGSIZE
  [PASS] Ioctl -EIO error cleanly propagated without retries
  [PASS] Valid byte read packet executes full-duplex transfer cleanly
  [PASS] Hardware transaction logger accurately records packet count and status
  [PASS] File descriptor deterministically closed upon transport destruction
  [PASS] Mock hardware rejection stub returns NULL with -ENOTSUP without touching devices
  [PASS] Default binary isolation verified: bin/fw9369-tool contains zero hardware backend symbols
```

### Comprehensive Pass Rate
```text
===================================================================
 Final Test Results: 75 / 75 Passed (100.0%)
===================================================================
```

---

## 5. Audit Conclusion

The mock and hardware transport backends are completely decoupled:
- Zero hardware symbols leak into `bin/fw9369-tool`.
- The default tool continues to reject `--hardware` with exit code 1.
- All hardware transport functions are thoroughly verified via offline fake syscall hooks without opening any device nodes.
- Mock and hardware separation audit is **PASSED**.
