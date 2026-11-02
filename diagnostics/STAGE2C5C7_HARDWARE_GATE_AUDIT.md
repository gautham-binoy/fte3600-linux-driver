# Stage 2C.5C.7 — Hardware Safety Gate and Isolation Audit

**Target Silicon:** FocalTech FT9362 / FW9369  
**Host System:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Scope:** Verification of Physical Device Isolation, Runtime Safety Gates & Build Boundaries  
**Audit Date:** 2026-11-02  
**Audit Mode:** Software-Only / Zero Hardware Execution

---

## 1. Source-Level Safety Gate Inspection (`fw9369_main_hw.c`)

The entry point for `bin/fw9369-hw-tool` is located at `tools/fw9369-mock/src/fw9369_main_hw.c`:

```c
int main(int argc, char **argv)
{
    // ... parameter parsing and config validation ...

    int val_ret = fw9369_hw_validate_config(&cfg);
    if (val_ret != 0) {
        fprintf(stderr, "[CONFIG ERROR] Invalid hardware configuration: code %d\n", val_ret);
        return 1;
    }
    printf("[PASS] Offline configuration validation: OK (Mode 0, 8-bit, MSB-first, <= 1 MHz)\n");

    if (validate_only) {
        printf("[INFO] Offline validation complete. Exiting cleanly without device access.\n");
        return 0;
    }

    /*
     * FAIL-CLOSED HARDWARE GATE:
     * In Stage 2C.5B, physical hardware communication is strictly prohibited.
     * The hardware tool will NEVER open device nodes or issue SPI transactions.
     */
    printf("\n[SECURITY ALERT] Stage 2C.5B is static preparation only.\n");
    printf("[SAFETY GATE PASS] Physical device access is blocked by policy.\n");
    printf("To verify hardware transport mechanics offline, execute bin/fw9369-tests.\n");
    return 1;
}
```

### 1.1 Gate Analysis
1. **Unconditional Exit Code 1:** If invoked without `--validate-config`, the tool prints a security alert and unconditionally terminates with exit code 1.
2. **Zero Transport Instantiation:** There is no code path in `main()` that invokes `fw9369_hw_transport_create()`, `open()`, `ioctl()`, or any system call accessing `/dev/spidev*`.
3. **No Hidden Options:** Inspection of the argument loop confirms that only `--validate-config`, `--device=<path>`, `--speed=<hz>`, and `--help` are parsed. No secret flag or debug option exists to bypass the return statement at line 79.

---

## 2. Build-Level Tooling Isolation (`Makefile`)

The Makefile configuration ensures complete separation between mock production tools and hardware transport implementations:

```makefile
SRCS_COMMON = src/fw9369_crc.c \
              src/fw9369_protocol.c \
              src/fw9369_transport_mock.c \
              src/fw9369_calibration.c \
              src/fw9369_state.c

OBJS_COMMON = $(SRCS_COMMON:.c=.o)

$(TOOL_BIN): $(BIN_DIR) $(OBJS_COMMON) src/fw9369_main.o
	$(CC) $(CFLAGS) $(INCLUDES) $(OBJS_COMMON) src/fw9369_main.o $(LDFLAGS) -o $@
```

### 2.1 Symbol Isolation Proof
- `bin/fw9369-tool` links strictly against `OBJS_COMMON` and `src/fw9369_main.o`.
- `src/fw9369_transport_hw.c` is NOT part of `OBJS_COMMON`.
- Independent symbol verification via GNU `nm`:
  ```bash
  nm -g bin/fw9369-tool | grep "fw9369_hw_"
  ```
  **Output: 0 matching symbols.**
- It is mathematically and mechanically impossible for `bin/fw9369-tool` to execute any hardware SPI code.

---

## 3. Runtime Verification of Hardware Invariants

During Stage 2C.5C.7, the following safety invariants were strictly maintained and verified:

1. **Zero Device Node Probing:**
   - `/dev/spidev*`, `/dev/gpiochip*`, `/dev/uio*` were NEVER opened, read, or written.
   - All offline unit tests ran via memory mock system-call tables (`struct fw9369_hw_ops fake_ops`).
2. **Zero Bus Transactions:**
   - No SPI packets were transmitted on physical MOSI/MISO lines.
   - No GPIO pins (reset, interrupt, wake) were toggled.
3. **Zero Kernel State Modifications:**
   - No kernel modules were inserted (`insmod`/`modprobe`) or removed (`rmmod`).
   - Device `spi-FTE3600:00` remains in its existing un-bound state.
   - No `sudo` or superuser privileges were exercised.
4. **Zero Hardware Scripts Executed:**
   - Neither `scripts/stage1-test.sh` nor `scripts/stage2a-test.sh` was invoked.

**Audit Status:** **PASS (Hardware gate remains fully closed and enforced)**.
