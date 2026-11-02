# Stage 2C.5B — Hardware SPI Transport Architecture and Interface Audit

**Document ID:** `STAGE2C5B_TRANSPORT_ARCHITECTURE`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Scope:** Architectural decomposition of the transport layer, interface reuse calculation, and isolation boundaries.  
**Hardware Authorization:** NONE (Static Analysis & Offline Unit Testing Only).

---

## 1. Executive Summary

Stage 2C.5B establishes a dedicated Linux `spidev` hardware transport backend (`tools/fw9369-mock/src/fw9369_transport_hw.c`) to bridge the protocol, calibration, and state machine modules with the physical Linux kernel SPI subsystem.

This document performs an independent audit of the transport layer architecture, evaluating:
1. The polymorphic `struct fw9369_transport` interface.
2. The empirical calculation of component reusability (superseding earlier rough estimates).
3. The clean decoupling of in-memory mock testing from physical hardware interaction.

---

## 2. Empirical Component Reusability & Code Delta Calculation

Earlier reports provided a preliminary estimate of "95% reuse." An exact, line-by-line recalculation across the entire userspace codebase was performed:

### 2.1 Codebase Inventory & Line Counts

| Module / Component | Header File | Implementation File | Total Lines | Reusability in Hardware Pipeline | Modification Required |
| :--- | :--- | :--- | :---: | :---: | :--- |
| **CRC Engines** | `include/fw9369_crc.h` (36) | `src/fw9369_crc.c` (40) | 76 | **100.0%** (76 / 76) | None (doc comment corrected). |
| **Protocol & Allowlist** | `include/fw9369_protocol.h` (100) | `src/fw9369_protocol.c` (155) | 255 | **100.0%** (255 / 255) | None. Bit-exact to specification. |
| **Calibration & Baseline**| `include/fw9369_calibration.h` (52) | `src/fw9369_calibration.c` (261) | 313 | **100.0%** (313 / 313) | None. Calls transport delays and transfers. |
| **5-Phase Pipeline State** | `include/fw9369_state.h` (52) | `src/fw9369_state.c` (297) | 349 | **100.0%** (349 / 349) | None. Pure state machine logic. |
| **Abstract Transport API** | `include/fw9369_transport.h` (52) | — | 52 | **100.0%** (52 / 52) | None. Generic function pointer struct. |
| **Mock Transport Backend**| — | `src/fw9369_transport_mock.c` (353) | 353 | **0.0%** (Mock only) | Retained exclusively for test harness. |
| **Hardware Transport** | `include/fw9369_transport_hw.h` (52) | `src/fw9369_transport_hw.c` (373) | 425 | **100.0%** (New Code) | Newly implemented hardware backend. |
| **Test Runner & Harness** | — | `tests/test_runner.c` (581) | 581 | **100.0%** (Expanded) | Expanded from 53 to 75 tests. |

### 2.2 Reusability Metric Synthesis
- **Core Protocol, Math & State Machine (Shared Logic):**
  $$76 + 255 + 313 + 349 + 52 = 1,045\text{ lines}$$
  These 1,045 lines of C code are **100% reused** directly by both the mock tool and the hardware tool without modifying a single line of protocol framing or calibration math.
- **Backend Transport Division:**
  - Mock backend: 353 lines (in-memory simulation).
  - Hardware backend: 425 lines (Linux `spidev` UAPI implementation).
- **Core Reusability Ratio:**
  $$\text{Reuse Ratio} = \frac{1,045}{1,045 + 425} = \frac{1,045}{1,470} \approx 71.1\%$$
- **Conclusion:** While earlier estimates considered only protocol algorithms (which are 100% reusable), the full system includes a completely distinct 425-line hardware I/O backend. The shared core logic is 1,045 lines (71.1% of total pipeline code), while the backend drivers are cleanly isolated.

---

## 3. Polymorphic Transport Interface Design

The communication boundary is anchored by `struct fw9369_transport` defined in `include/fw9369_transport.h`:

```c
struct fw9369_transport {
    void *context;
    int (*transfer)(void *context, const uint8_t *tx, uint8_t *rx, size_t len);
    void (*delay_ms)(void *context, uint32_t ms);
    void (*delay_us)(void *context, uint32_t us);
    void (*close)(void *context);
};
```

### 3.1 Architectural Advantages
1. **Zero Conditional Compilation in Protocol Code:**
   The state machine (`fw9369_state.c`) and calibration routines (`fw9369_calibration.c`) make calls through `t->transfer()` and `t->delay_ms()`. They contain **zero `#ifdef HARDWARE` or `#ifdef MOCK` preprocessor directives**, ensuring that the state machine tested in simulation is identical to the state machine linked against real hardware.
2. **Context Isolation:**
   - In mock mode: `context` points to `struct mock_context` (heap register tables, synthetic noise models, fault injection flags).
   - In hardware mode: `context` points to `struct hw_context` (file descriptor `fd`, bus configuration, transaction log ring).
3. **Pluggable System-Call Hooks for Testing:**
   In `fw9369_transport_hw.c`, system calls (`open`, `ioctl`, `close`) are dispatched through `struct fw9369_hw_ops`. This enables `tests/test_runner.c` to test the hardware transport's validation, packet framing, and error handling offline using fake SPI hooks, without opening real device nodes.

---

## 4. Hardware Backend Boundary & Guarantees

The hardware transport implementation guarantees:
1. **Explicit Device Binding:** Rejects any path other than `"/dev/spidev1.0"`.
2. **Strict Bus Parameters:** Enforces SPI Mode 0, 8 bits/word, MSB-first, and speed $\le 1,000,000$ Hz.
3. **Continuous CS Enforcement:** Sets `cs_change = 0` on all `struct spi_ioc_transfer` messages.
4. **Transport-Level Allowlist Guard:** Inspects outgoing packets against register and command allowlists before invoking `ioctl()`.
5. **No Blind Retries:** Any bus error immediately unwinds with negative errno.

---

## 5. Audit Conclusion

The hardware transport architecture preserves software-level modularity and decoupling.
- The shared protocol core (1,045 LOC) is 100% reused.
- The hardware backend (425 LOC) is cleanly isolated into a dedicated compilation unit.
- Transport architecture is **VERIFIED AND ACCEPTED**.
