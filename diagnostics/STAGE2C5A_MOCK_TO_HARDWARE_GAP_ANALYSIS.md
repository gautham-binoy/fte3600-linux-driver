# Stage 2C.5A — Mock-to-Hardware Gap Analysis & Interface Audit

**Document ID:** `STAGE2C5A_MOCK_TO_HARDWARE_GAP_ANALYSIS`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Audited Codebase:** `tools/fw9369-mock/`  
**Execution Boundary:** Static source-code gap analysis. Zero hardware access.

---

## 1. Executive Summary

The mock implementation in `tools/fw9369-mock/` has achieved 100% test passing (53/53 assertions) and has been independently accepted as a valid software model.

However, operating against physical hardware introduces physical bus constraints, kernel ioctl semantics, OS signal handling, and hardware timing that do not exist in an in-memory simulation.

This audit evaluates the codebase to establish:
1. **Reusability Matrix:** Which modules can be used directly without modification.
2. **Interface Gaps:** What modifications or new components are strictly necessary before a real hardware backend can be linked.
3. **Safety Verification:** Confirming that the mock continues to strictly reject hardware mode during this stage.

---

## 2. Component Reusability & Modification Analysis

| Subsystem / File | Reusability Status | Required Adaptations for Hardware Backend |
| :--- | :---: | :--- |
| **`fw9369_crc.h / .c`** | **100% REUSABLE AS-IS** | Exact match with hardware CRC-CCITT polynomial `0x1021`. Confirmed by Stage 2A physical reading `0x1C53`. |
| **`fw9369_protocol.h / .c`** | **100% REUSABLE AS-IS** | Packet framing (byte, word, command, FIFO) and allowlist validation strictly follow reference driver specifications. |
| **`fw9369_calibration.h / .c`** | **95% REUSABLE** | FDT DAC search, per-channel minimum tracking, threshold math, and cold baseline copy are identical. Requires adjusting timing delays from mock no-ops to physical microsecond sleeps (`usleep`). |
| **`fw9369_state.h / .c`** | **95% REUSABLE** | 5-phase pipeline and structured abort cleanup are structurally sound. Requires connecting signal handlers (`SIGINT`, `SIGTERM`) to trigger abort cleanup on userspace interruption. |
| **`fw9369_transport.h`** | **100% REUSABLE AS-IS** | `struct fw9369_transport` interface (`transfer`, `delay_ms`, `delay_us`, `close`) cleanly decouples transport implementation from protocol logic. |
| **`fw9369_transport_mock.c`** | **RETAINED FOR TESTING** | In-memory mock backend remains permanently in tree for regression testing. Stub `fw9369_hardware_transport_create()` must remain disabled. |
| **`fw9369_transport_hw.c`** | **NEW MODULE NEEDED** | A new file implementing real `ioctl(fd, SPI_IOC_MESSAGE(1), &tr)` will be required in Stage 2C.5B, guarded by strict allowlists and file descriptor safety. |
| **`fw9369_main.c`** | **ADAPTATION NEEDED** | CLI currently rejects `--hardware`. In Stage 2C.5B, CLI will link to the authorized hardware backend under an explicit `--confirm-hardware-safety` flag. |

---

## 3. Deep Dive into Specific Interface Gaps

### 3.1 Transport Interface & `SPI_IOC_MESSAGE`
- **Current Mock Behavior:**
  In `src/fw9369_transport_mock.c:66`, `mock_transfer()` directly simulates register responses in memory.
- **Hardware Requirement:**
  A real hardware transport must wrap the Linux `spidev` ioctl interface:
  ```c
  int fw9369_hw_transfer(void *context, const uint8_t *tx, uint8_t *rx, size_t len)
  {
      struct hw_context *ctx = context;
      struct spi_ioc_transfer tr = {
          .tx_buf = (unsigned long)tx,
          .rx_buf = (unsigned long)rx,
          .len = (uint32_t)len,
          .speed_hz = ctx->speed_hz,
          .delay_usecs = 0,
          .bits_per_word = 8,
          .cs_change = 0,
      };
      return ioctl(ctx->fd, SPI_IOC_MESSAGE(1), &tr);
  }
  ```
- **Allowlist Guard:** The hardware transport function must invoke `fw9369_is_register_allowed()` or `fw9369_is_command_allowed()` on the outgoing `tx` buffer **before** submitting the `ioctl()`, guaranteeing that no stray or unauthorized byte can ever reach the physical bus.

### 3.2 Timing and Delay Realism
- **Current Mock Behavior:**
  In `src/fw9369_transport_mock.c:54–64`, `mock_delay_ms()` and `mock_delay_us()` are no-ops (`(void)context; (void)ms;`).
- **Hardware Requirement:**
  Physical hardware requires real physical settling times:
  - 4 ms delay after writing `0xC6 = 0x01` (transceiver PLL spin-up).
  - 1 ms delay after command strobes (`0x5A`, `0xA5`, `0xC2 0x3D`).
  - Real microsecond sleeps must be implemented using `nanosleep()` or `usleep()`.

### 3.3 Transaction Logging Realism
- **Current Mock Behavior:**
  Stores up to 512 packets in an in-memory array (`struct log_entry log[512]`).
- **Hardware Requirement:**
  For the 10,246-byte FIFO transfer, storing multiple 10 KB packets in an un-bounded array consumes significant RAM. The hardware logger should log headers (first 16 bytes and last 4 bytes) for bulk packets, while writing complete binary frame dumps directly to a disk artifact (`diagnostics/stage2c5-raw-frame.bin`).

### 3.4 Asynchronous Signal Handling & Register `0x9A` Protection
- **Current Mock Behavior:**
  The mock handles simulated interruptions synchronously via `cfg.interrupt_in_trim`.
- **Hardware Requirement:**
  A physical userspace tool must register POSIX signal handlers (`sigaction`) for `SIGINT` (Ctrl+C), `SIGTERM`, and `SIGHUP`. If a signal is caught while `0x9A` is unlocked, the signal handler must:
  1. Synchronously execute `write_byte(fd, 0x9A, 0x00)`.
  2. Log the emergency relock attempt.
  3. Close the file descriptor and exit cleanly.

---

## 4. Hardware Rejection Verification

To verify that the current mock codebase in `tools/fw9369-mock/` continues to enforce strict hardware rejection:

1. **Hardware Stub Inspection (`src/fw9369_transport_mock.c:343-353`):**
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
2. **CLI Enforcement (`src/fw9369_main.c:32-38`):**
   ```c
   } else if (strcmp(argv[i], "--hardware") == 0) {
       printf("[SECURITY ALERT] Hardware mode is strictly prohibited in Stage 2C.4!\n");
       struct fw9369_transport *hw = fw9369_hardware_transport_create("/dev/spidev1.0");
       if (!hw) {
           printf("[SAFETY GATE PASS] Hardware backend was rejected without accessing any device node.\n");
           return 1;
       }
   }
   ```
3. **Audit Finding:**
   Hardware execution is **completely blocked**. Any invocation with `--hardware` exits with code 1 and never creates or opens any device file.

---

## 5. Audit Conclusion

The mock implementation in `tools/fw9369-mock/` provides a sound, 95% reusable protocol and calibration foundation. The gap to hardware is well understood, strictly confined to the transport layer (`fw9369_transport_hw.c`), real-time delays, and POSIX signal handling. The codebase remains safely locked in mock-only mode.
