# Stage 2C.5C.1 — Hardware Execution Gate and Isolation Final Audit

**Document ID:** `STAGE2C5C1_HARDWARE_GATE_FINAL_AUDIT`  
**Date:** 2026-11-02  
**Target Executables:**
- `bin/fw9369-tool` (Production Userspace Mock Tool)
- `bin/fw9369-hw-tool` (Hardware Transport Preflight Utility)
- `bin/fw9369-tests` (Mock and Offline Verification Test Runner)
**Execution Boundary:** Strictly static code analysis and offline mock test verification. Zero hardware operations authorized.

---

## 1. Executive Summary

The integrity of the pre-execution safety boundary depends entirely on the principle that no software executable, command-line parameter, environment variable, or automated test can initiate physical communication with the fingerprint sensor without an explicit, multi-layered authorization gate.

This audit presents an exhaustive, adversarial inspection of all entry points, build artifacts, symbol tables, and execution branches in the project codebase.

### Audit Verdict:
- **Hardware Execution Gate is CLOSED and IMPENETRABLE.**
- **Hardware Binary (`bin/fw9369-hw-tool`):** Contains a hardcoded, unconditional fail-closed gate halting execution with exit code 1 prior to any transport instantiation or device opening.
- **Zero Environment Variable Bypasses:** Static grep confirmed zero calls to `getenv()` anywhere in the codebase.
- **Mock Binary (`bin/fw9369-tool`):** Verified to contain **ZERO** hardware symbols (`fw9369_hw_*`) via symbol table inspection. It is physically incapable of issuing SPI transactions.
- **Sysfs Pre-Open Verification:** `fw9369_hw_verify_device_identity()` enforces a 4-point topology check before any hardware device node can be opened.

---

## 2. Source Audit of the Hardware Gate (`src/fw9369_main_hw.c`)

The hardware utility entry point is implemented in [`tools/fw9369-mock/src/fw9369_main_hw.c`](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_main_hw.c). Every control flow path from `main()` was audited:

```mermaid
flowchart TD
    Start(["main(argc, argv)"]) --> Parse["Parse Command-Line Arguments"]
    Parse --> HelpCheck{"--help?"}
    HelpCheck -- Yes --> PrintHelp["print_usage() & return 0"]
    HelpCheck -- No --> UnknownCheck{"Unknown arg?"}
    UnknownCheck -- Yes --> PrintErr["fprintf(stderr) & return 1"]
    UnknownCheck -- No --> ValidateCfg["fw9369_hw_validate_config(&cfg)"]
    
    ValidateCfg --> CfgFail{"Config Valid?"}
    CfgFail -- No --> ExitCfgErr["return 1 (code -ret)"]
    CfgFail -- Yes --> CheckValOnly{"--validate-config?"}
    
    CheckValOnly -- Yes --> ExitValOnly["printf(Offline OK) & return 0"]
    CheckValOnly -- No --> FailClosedGate["FAIL-CLOSED HARDWARE GATE<br>printf(Physical device access blocked)<br>return 1"]
```

### 2.1 Complete Branch Analysis

1. **Argument Parsing (Lines 35–50):**
   - Supported flags: `--validate-config`, `--device=<path>`, `--speed=<hz>`, `--help`.
   - Unknown options trigger immediate termination: `return 1;` (line 48).
   - No hidden flags, undocumented parameters, or test bypasses exist.
2. **Offline Configuration Validation (Lines 59–64):**
   - `fw9369_hw_validate_config(&cfg)` validates that the path is strictly `/dev/spidev1.0`, speed $\le 1\text{ MHz}$, mode is 0, bits per word is 8, and MSB-first.
   - If invalid (e.g. `--device=/dev/spidev0.0` or `--speed=2000000`), execution terminates immediately with exit code 1.
3. **The `--validate-config` Branch (Lines 66–69):**
   - If `--validate-config` is provided, the tool prints `[INFO] Offline validation complete. Exiting cleanly without device access.` and executes `return 0;`.
   - **Crucial Safety Guarantee:** It does not open `/dev/spidev1.0`, does not call `fw9369_hw_transport_create()`, and does not execute any system calls.
4. **The Fail-Closed Execution Gate (Lines 71–80):**
   ```c
   /*
    * FAIL-CLOSED HARDWARE GATE:
    * In Stage 2C.5B, physical hardware communication is strictly prohibited.
    * The hardware tool will NEVER open device nodes or issue SPI transactions.
    */
   printf("\n[SECURITY ALERT] Stage 2C.5B is static preparation only.\n");
   printf("[SAFETY GATE PASS] Physical device access is blocked by policy.\n");
   printf("To verify hardware transport mechanics offline, execute bin/fw9369-tests.\n");
   return 1;
   ```
   - If any operator runs `bin/fw9369-hw-tool` without `--validate-config`, execution unconditionally terminates at line 79 with exit code 1.
   - No call to `open()`, `ioctl()`, `fw9369_hw_transport_create()`, or any hardware routine is made.

---

## 3. Environment Variable and Dynamic Configuration Audit

A rigorous search of all source and header files was conducted to detect any runtime bypass mechanisms:

1. **System Calls to `getenv()`:**
   - Command executed: `grep -rn "getenv" tools/fw9369-mock/`
   - Result: **0 matches.** No environment variable queries exist.
2. **Configuration Files:**
   - Static analysis of `src/fw9369_main_hw.c` and `src/fw9369_transport_hw.c` confirmed that no configuration files (e.g. `/etc/fte3600.conf`, `.env`, or `/tmp/*`) are opened or parsed.
3. **ACPI / DMI Overrides:**
   - No DMI table or ACPI override mechanisms exist in userspace tooling.

---

## 4. Binary Symbol and Compilation Isolation

The project build system ([`tools/fw9369-mock/Makefile`](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/Makefile)) enforces strict separation between mock and hardware build artifacts:

### 4.1 Default Target Isolation
- Running `make all` builds only:
  - `../../bin/fw9369-tool` (Mock executable)
  - `../../bin/fw9369-tests` (Mock test runner)
- `../../bin/fw9369-hw-tool` is built **only** when explicitly invoking `make hw-tool`.

### 4.2 Symbol Table Inspection of Mock Binary (`bin/fw9369-tool`)
A symbol table dump of the mock binary was executed:
```bash
nm -g bin/fw9369-tool | grep -i hw
```
- **Result:** **0 matches.**
- The mock binary links only `fw9369_transport_mock.o`, `fw9369_state.o`, `fw9369_calibration.o`, `fw9369_crc.o`, and `fw9369_protocol.o`.
- Hardware transport object `fw9369_transport_hw.o` is **NEVER** linked into `bin/fw9369-tool`.
- It is mathematically and architecturally impossible for `bin/fw9369-tool` to open `/dev/spidev*` or communicate with hardware.

---

## 5. Device Identity Pre-Flight Verification

Even if the execution gate in `main()` were modified in a future authorized stage, the hardware transport layer contains an independent safety gate: [`fw9369_hw_verify_device_identity()`](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L76-L150).

### 5.1 Verification Logic
Before opening any character device, `fw9369_hw_verify_device_identity()` traverses `/sys` to verify four mandatory invariants:
1. **Device Path:** Must strictly equal `/dev/spidev1.0`.
2. **Controller Parentage:** Resolving `/sys/class/spidev/spidev1.0/device` must contain `pxa2xx-spi.2`.
3. **SPI Master Bus:** Must resolve to `spi1`.
4. **Target Silicon Modalias:** Reading `modalias` under the device node or `/sys/bus/spi/devices/spi-FTE3600:00/modalias` must contain string `FTE3600`.

### 5.2 Fail-Closed Behavior on Live Host
Because the live platform is currently unbound:
- `/sys/class/spidev/spidev1.0` does not exist.
- Calling `fw9369_hw_verify_device_identity("/dev/spidev1.0", NULL)` returns `-ENODEV`.
- The transport layer refuses to proceed and aborts before calling `open()`.

---

## 6. Audit Conclusion

The hardware execution gate is verified as completely fail-closed:
- `bin/fw9369-hw-tool` terminates with exit code 1 and zero device access.
- `bin/fw9369-tool` contains zero hardware transport symbols.
- No environment variables, config files, or CLI flags can bypass the gate.
- The gate remains **STRICTLY CLOSED**.
