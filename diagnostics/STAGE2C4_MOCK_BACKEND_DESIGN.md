# Stage 2C.4 — Mock Backend Design & Safety Isolation

**Document ID:** `STAGE2C4_MOCK_BACKEND_DESIGN`  
**Date:** 2026-11-02  
**Target Silicon:** FocalTech FT9362 / FW9369  
**Source Code Baseline:** `tools/fw9369-mock/` (`include/fw9369_transport.h`, `src/fw9369_transport_mock.c`)  
**Scope:** Design specification of the abstract SPI transport, in-memory sensor simulation, failure injection engine, and hardware access prevention.  
**Execution Boundary:** **STRICTLY MOCK-ONLY.** Physical hardware execution is completely prohibited.

---

## 1. Architectural Principles

The mock transport backend decouples protocol execution from physical Linux kernel I/O:
1. **Zero Device Node Discovery:** The mock backend has zero awareness of `/dev/spidev*`, `/dev/uio*`, or sysfs nodes. It performs all operations within standard userspace memory buffers.
2. **Deterministic Silicon Simulation:** Simulates the internal register state machine of the FT9362, including AFE DAC centering responses, status polling, analog trim bank protection, and burst FIFO streams.
3. **Hard Hardware Rejection Stub:** The hardware transport factory function (`fw9369_hardware_transport_create`) is compiled as an explicit refusal stub that immediately returns `NULL` and sets `errno = ENOTSUP`. It never executes `open()`, `stat()`, or any I/O system calls.

---

## 2. Abstract Transport Interface

In [include/fw9369_transport.h](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/include/fw9369_transport.h):
```c
struct fw9369_transport {
    void *context;
    int (*transfer)(void *context, const uint8_t *tx, uint8_t *rx, size_t len);
    void (*delay_ms)(void *context, uint32_t ms);
    void (*delay_us)(void *context, uint32_t us);
    void (*close)(void *context);
};
```
- **Context Isolation:** Protocol logic communicates strictly through function pointers in `struct fw9369_transport`, never directly referencing OS file descriptors.
- **Full-Duplex Wire Semantics:** `transfer()` accepts separate `tx` (MOSI) and `rx` (MISO) buffers and a total transaction length `len`.

---

## 3. In-Memory Simulated Silicon State

In [src/fw9369_transport_mock.c](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_mock.c):
The mock context encapsulates:
```c
struct mock_context {
    struct fw9369_mock_config config;

    uint8_t  reg_byte[256];
    uint16_t reg_word_18xx[256];
    uint16_t reg_word_1axx[256];
    uint16_t reg_trim[16];   /* 0x00C0 - 0x00CF */
    uint16_t reg_thresh[16]; /* 0x80B0 - 0x80BF */

    uint8_t  mcu_state;
    bool     protected_unlocked;
    uint32_t fdt_sample_counter;

    struct log_entry log[MAX_LOG_PACKETS];
    size_t log_count;
};
```

### Key Simulated Behaviors
1. **Transceiver Wakeup (`0xC6`):** Defaults to `0x00`. When `write_byte(0xC6, 0x01)` is received, sets `reg_byte[0xC6] = 0x01` and returns `0x01` on readback.
2. **Silicon Identity (`0x1A8B`):** Word read at `0x1A8B` returns `0x9362` and hardware CRC `0x1C53` (or `0x9368` and bad CRC under failure injection).
3. **MCU Idle Dialog (`0x5A 0xA5` / `0x80` / `0xA5 0x5A`):** Receiving command `0x5A 0xA5` transitions `mcu_state` from `0x00` to `0x50` (`FW9369_STATUS_IDLE`).
4. **Protected Trim Latch (`0x9A`):**
   - Writing `0x9A = 0x5A` asserts `protected_unlocked = true`.
   - While unlocked, reads and writes to `0x00C0`..`0x00CF` succeed.
   - Writing `0x9A = 0x00` asserts `protected_unlocked = false`.
   - While locked, reads to `0x00C0` return `0x0000`, and writes are discarded.
5. **Dynamic Analog AFE Response:**
   - In `FW9369_REG_ADC_80B8`, the simulated ADC reading responds dynamically to the programmed DAC setting in `0x1801`:
     $$\text{simulated\_base} = 512 + ((\text{dac} - 27) \times 6)$$
   - This models physical analog frontend convergence, allowing the hybrid DAC search loop to find the exact zero-crossing boundary deterministically.
6. **Burst FIFO Readout (10,246 Bytes):**
   - When command prefix `0x06 0xF9 0x9A 0x05 0x14 0x00` is received with `len == 10246`, the mock generates a deterministic 5,120-pixel gradient pattern with median 512 counts.

---

## 4. Hardware Rejection Guarantee

To eliminate any possibility of accidental hardware interaction:
```c
struct fw9369_transport *fw9369_hardware_transport_create(const char *device_path)
{
    (void)device_path;
    /*
     * ABSOLUTE SAFETY GUARANTEE:
     * This stub unconditionally rejects hardware selection.
     * It NEVER calls open(), stat(), access(), or any I/O syscall.
     */
    errno = ENOTSUP;
    return NULL;
}
```
Unit test `test_mock_transport_suite` explicitly invokes this stub and asserts that it returns `NULL` and `errno == ENOTSUP` without opening any path.

---

## 5. Transaction Logging and Inspection

The mock maintains a fixed-size ring buffer of all SPI transactions (`struct log_entry log[512]`). Each entry stores:
- Full MOSI byte buffer transmitted.
- Full MISO byte buffer returned.
- Exact byte length `len`.

Tests can inspect the log at any point to verify:
- Sequence of registers accessed.
- Absence of forbidden commands.
- Exact number of SPI transactions executed (e.g. 60 transactions in a standard clean pipeline run).
