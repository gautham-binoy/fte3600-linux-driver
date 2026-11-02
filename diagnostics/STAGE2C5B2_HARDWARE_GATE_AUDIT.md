# Stage 2C.5B.2 — Hardware Execution Safety Gate & Isolation Audit

**Document ID:** `STAGE2C5B2_HARDWARE_GATE_AUDIT`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Audit Scope:** Inspection of hardware execution binary `bin/fw9369-hw-tool`, CLI entry points, execution gates, bypass resistance, and device node access prevention.  
**Hardware Authorization:** NONE (Software & Static Analysis Only).

---

## 1. Executive Summary

This audit evaluates the safety gates embedded within the standalone hardware tool source (`tools/fw9369-mock/src/fw9369_main_hw.c`) and binary (`bin/fw9369-hw-tool`).

The primary objective of this review is to ensure that:
1. Physical device access remains 100% impossible during software development and audit stages.
2. The execution safety gate cannot be subverted or bypassed via command-line arguments, environment variables, configuration files, or fallback logic.
3. Offline preflight testing (`--validate-config`) is strictly non-invasive and issues zero device access calls.
4. The default userspace binary (`bin/fw9369-tool`) remains incapable of executing hardware transport logic.

---

## 2. Source Code Inspection: `src/fw9369_main_hw.c`

Inspection of lines 20–80 confirms:

### 2.1 Unconditional Fail-Closed Exit
```c
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
```

### 2.2 Absence of Hardware Instantiation Call
- Neither `fw9369_hw_transport_create()` nor `ops.open_fn()` is ever invoked in `main()`.
- The binary does not contain any code path that transitions from configuration validation to actual transport creation or transaction execution.
- If invoked without arguments or with any argument other than `--validate-config` or `--help`, the executable terminates unconditionally with exit code 1.

---

## 3. Adversarial Bypass Resistance Analysis

| Bypass Vector | Mechanism Evaluated | Source Finding | Assessment |
| :--- | :--- | :--- | :---: |
| **CLI Flag Bypass** | `--force`, `--run`, `--execute`, `--hardware` | Options loop rejects unrecognized flags and terminates | **IMMUNE** |
| **Environment Variable** | `FW9369_FORCE_HW=1`, `ALLOW_HW=1` | Zero calls to `getenv(3)` in `fw9369_main_hw.c` | **IMMUNE** |
| **Config File Injection** | Local dotfiles, `/etc/fw9369.conf` | Zero file parsing routines in `fw9369_main_hw.c` | **IMMUNE** |
| **Offline Preflight Leak** | `--validate-config` accessing devices | Calls `fw9369_hw_validate_config()`; purely in-memory checks | **IMMUNE** |
| **Mock CLI Re-direction** | `./bin/fw9369-tool --hardware` | Explicitly caught, prints alert, exits code 1 | **IMMUNE** |
| **Link-Time Contamination** | Hardware symbols in `bin/fw9369-tool` | `nm -g bin/fw9369-tool` confirms 0 `fw9369_hw_*` symbols | **IMMUNE** |

---

## 4. Empirical Binary Execution Audit

### 4.1 Invocation with `--validate-config`
```bash
./bin/fw9369-hw-tool --validate-config
```
**Output:**
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
*(Exit code: 0; zero device nodes accessed)*

### 4.2 Default Hardware Invocation
```bash
./bin/fw9369-hw-tool
```
**Output:**
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
*(Exit code: 1; execution halted by fail-closed gate)*

---

## 5. Audit Conclusion

The hardware safety gate is robust, immutable, and fail-closed. Physical sensor communication remains completely impossible under any software configuration in this stage.
