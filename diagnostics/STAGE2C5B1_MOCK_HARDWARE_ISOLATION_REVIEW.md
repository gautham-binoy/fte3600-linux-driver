# Stage 2C.5B.1 — Mock / Hardware Separation and Symbol Isolation Audit

**Document ID:** `STAGE2C5B1_MOCK_HARDWARE_ISOLATION_REVIEW`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Audit Scope:** Area G — Build-level isolation, binary symbol scoping, default build targets, and mock runtime autonomy.  
**Hardware Authorization:** NONE (Software & Static Analysis Only).

---

## 1. Executive Summary

This audit reviews the isolation boundary separating the mock software implementation from the newly developed hardware SPI transport backend.

A paramount safety concern in this phase is preventing accidental physical device access during automated testing, continuous integration, or regular development workflows. 

The audit confirms:
1. **Target-Level Separation:** The default Makefile build (`make all`) produces only `bin/fw9369-tool` (the pure in-memory mock simulator) and `bin/fw9369-tests` (the unit test suite). The hardware tool binary `bin/fw9369-hw-tool` is completely excluded from the default target and can only be compiled via an explicit `make hw-tool` invocation.
2. **Binary Symbol Isolation:** Inspection of the public symbol table of `bin/fw9369-tool` with `nm(1)` reveals **zero `fw9369_hw_*` symbols**. The mock CLI cannot execute hardware code even if commanded, because hardware transport object files are not linked into the binary.
3. **Hardware Rejection Stub:** Within the mock transport object (`src/fw9369_transport_mock.c`), the public factory function `fw9369_hardware_transport_create()` is retained as a hard stub that unconditionally returns `NULL` with `errno = ENOTSUP` without attempting to open character device nodes.
4. **Offline Hardware Test Isolation:** Hardware transport tests in `bin/fw9369-tests` operate via dependency-injected mock system-call operations (`struct fw9369_hw_ops test_ops`), exercising transport logic without opening physical device nodes.

---

## 2. Makefile Target & Dependency Decomposition

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

### 2.1 Architectural Assessment
- The default target `all` depends solely on `$(TOOL_BIN)` and `$(TEST_BIN)`.
- Running `make` or `make all` builds only the mock tool and test runner.
- `TOOL_BIN` has zero dependency on `src/fw9369_transport_hw.o`.
- `HW_TOOL_BIN` requires an explicit, intentional command: `make hw-tool`.

---

## 3. Binary Symbol Verification with `nm`

### 3.1 Verification of Mock Tool (`bin/fw9369-tool`)
Running `nm -g bin/fw9369-tool | grep fw9369_hw_` produces:
```text
Zero hw symbols in fw9369-tool (exit code 1)
```
- Total `fw9369_hw_*` symbols linked into `bin/fw9369-tool`: **0**.
- The binary links solely with `fw9369_mock_*` symbols.
- This provides compile-time and link-time certainty that the mock simulator cannot access `/dev/spidev1.0` or issue hardware ioctls.

### 3.2 Verification of Hardware Tool (`bin/fw9369-hw-tool`)
Running `nm -g bin/fw9369-hw-tool | grep fw9369_hw_` produces:
```text
0000000000002980 T fw9369_hw_get_fd
0000000000002920 T fw9369_hw_get_last_tx
0000000000002900 T fw9369_hw_get_tx_count
0000000000002890 T fw9369_hw_transport_create
00000000000023d0 T fw9369_hw_transport_create_with_ops
00000000000028a0 T fw9369_hw_transport_destroy
0000000000002350 T fw9369_hw_validate_config
0000000000002320 T fw9369_hw_validate_device_path
```
- In `bin/fw9369-hw-tool`, the mock simulator transport (`src/fw9369_transport_mock.o`) is omitted.
- The hardware tool links with `src/fw9369_transport_hw.o` and `src/fw9369_main_hw.o`.
- As verified in Stage 2C.5B, `src/fw9369_main_hw.c` embeds an unconditional fail-closed execution gate that prevents physical device access and terminates with exit code 1.

---

## 4. Test Suite Runtime Isolation

In `tools/fw9369-mock/tests/test_runner.c`:
- Test Suite 1 through 7 test CRC, protocol framing, calibration algorithms, and baseline generation using pure in-memory memory structures (`struct mock_context`).
- Test Suite 8 validates the hardware transport implementation (`fw9369_transport_hw.c`). However, instead of opening `/dev/spidev1.0` or issuing kernel system calls, it instantiates the transport via dependency injection:
  ```c
  static struct fw9369_hw_ops test_ops = {
      .open_fn = fake_open,
      .ioctl_fn = fake_ioctl,
      .close_fn = fake_close,
  };
  struct fw9369_transport *t = fw9369_hw_transport_create_with_ops(&cfg, &test_ops);
  ```
- All ioctl transactions, parameter settings, and allowlist validations are intercepted by user-space mock function pointers.
- Test 13 in Test Suite 8 programmatically checks binary isolation by running `nm -g ../../bin/fw9369-tool | grep -c fw9369_hw_` and asserting `sym_count == 0`.

---

## 5. Audit Assessment Summary

| Isolation Dimension | Requirement | Implementation Evidence | Verdict |
| :--- | :--- | :--- | :---: |
| **Build Target Scoping** | `make all` must not build hardware tool | `all: $(TOOL_BIN) $(TEST_BIN)` | **PASSED** |
| **Object File Isolation** | `fw9369-tool` must not link `transport_hw.o` | Explicit `OBJS_COMMON` list | **PASSED** |
| **Symbol Visibility** | Zero `fw9369_hw_*` symbols in mock binary | `nm -g bin/fw9369-tool` returns 0 matches | **PASSED** |
| **Zero Hardware Test Execution** | Tests must not touch `/dev/spidev*` | Simulated `test_ops` system call hooks | **PASSED** |

**Conclusion:** Mock and hardware tooling are strictly and cleanly decoupled across build targets, source objects, symbol tables, and test harnesses.
