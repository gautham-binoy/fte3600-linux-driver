# Stage 2A v2.0 — Final Pure-spidev Execution Readiness Audit

**Date:** 2026-11-02  
**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Device:** `spi-FTE3600:00` on Intel GSPI #1 (`pxa2xx-spi.2`)  
**Architecture Revision:** **v2.0 (Pure spidev Architecture)**  
**Audit Scope:** Final Static Implementation & Execution Readiness Audit (Zero Hardware Access)

---

## 1. Executive Verdict

### **Verdict: READY FOR EXPLICIT AUTHORIZATION**

The revised Stage 2A v2.0 pure-spidev implementation (`scripts/stage2a-test.sh`, `scripts/run-stage2a-in-terminal.sh`, and `tools/stage2a-identify.c`) has undergone an exhaustive line-by-line static audit.

- **0 Critical Findings**
- **0 High Findings**
- **0 Medium Findings**
- **0 Low Findings**
- **100% Offline Validation Passed** (`gcc -Wall -Wextra -Werror`, `bash -n`, CRC test vectors, CLI input bounds).

> [!CAUTION] Mandatory Authorization Gate
> A verdict of **`READY FOR EXPLICIT AUTHORIZATION`** is **NOT** authorization to execute. Hardware operations remain strictly unauthorized. Execution must not commence without a separate, explicit user instruction.

---

## 2. SPI Device Identity Assessment

| Audit Item | Implementation Verification | Source Line Reference | Verdict |
| :--- | :--- | :--- | :---: |
| **ACPI Device Identity** | The target SPI slave is explicitly hardcoded to `spi-FTE3600:00` at sysfs path `/sys/bus/spi/devices/spi-FTE3600:00`. Read-only verification confirms its firmware node points to `\_SB.PC00.SPI1.FP01` (`FTE3600:00`). | [`scripts/stage2a-test.sh:33-34`](file:///home/titan/fte3600-linux-driver/scripts/stage2a-test.sh#L33-L34) | **PASS** |
| **Controller & Chip Select** | Parent controller is Intel GSPI #1 (`pxa2xx-spi.2` / `spi1.0`), confirmed via sysfs symlink `../../../devices/pci0000:00/0000:00:1e.3/pxa2xx-spi.2/spi_master/spi1/spi-FTE3600:00`. ACPI DSDT verifies CS is native hardware Chip Select 0. | [`scripts/stage2a-test.sh:34`](file:///home/titan/fte3600-linux-driver/scripts/stage2a-test.sh#L34) | **PASS** |
| **Pre-Test Unbound State** | Script strictly checks that `spi-FTE3600:00` has no active driver link (`get_bound_driver_canonical "$SPI_PATH" == "UNBOUND"`). If bound to any driver, the script aborts immediately. | [`scripts/stage2a-test.sh:452-459`](file:///home/titan/fte3600-linux-driver/scripts/stage2a-test.sh#L452-L459) | **PASS** |
| **Target Ambiguity Elimination** | No wildcards, globs, or bus searches are permitted. The device name is hardcoded. Pre-checks abort if `driver_override` is non-empty (`!= ""`). | [`scripts/stage2a-test.sh:439-451`](file:///home/titan/fte3600-linux-driver/scripts/stage2a-test.sh#L439-L451) | **PASS** |
| **Dynamic spidev Node Discovery** | Script does not blindly assume `/dev/spidev1.0`. It tests `[ -c "/dev/spidev1.0" ]`; if missing, it discovers candidate `/dev/spidev*` and strictly verifies `[ -c "$SPIDEV_NODE" ]`. If invalid, it halts immediately and triggers rollback. | [`scripts/stage2a-test.sh:578-594`](file:///home/titan/fte3600-linux-driver/scripts/stage2a-test.sh#L578-L594) | **PASS** |
| **Device Node Correspondence** | Pre-flight baseline audit checks `ls -d /dev/spidev*` and aborts if any pre-existing `/dev/spidev*` device exists. Consequently, the single character device node created after binding `spi-FTE3600:00` is guaranteed to correspond to the target sensor. | [`scripts/stage2a-test.sh:468-475`](file:///home/titan/fte3600-linux-driver/scripts/stage2a-test.sh#L468-L475) | **PASS** |

---

## 3. Module Lifecycle Assessment

| Audit Item | Implementation Verification | Source Line Reference | Verdict |
| :--- | :--- | :--- | :---: |
| **On-Demand Loading** | `spidev` is verified `ABSENT` at baseline. If already present, the script aborts immediately without modifying the system. It is loaded only at Step 2a after all pre-flight checks pass. | [`scripts/stage2a-test.sh:460-467`](file:///home/titan/fte3600-linux-driver/scripts/stage2a-test.sh#L460-L467), [`scripts/stage2a-test.sh:505-515`](file:///home/titan/fte3600-linux-driver/scripts/stage2a-test.sh#L505-L515) | **PASS** |
| **Ownership Tracking** | `SPIDEV_LOADED_BY_US=1` is set inside `enter_critical` strictly after `spidev_post_state == "PRESENT"` and `cmd_err == 0`. | [`scripts/stage2a-test.sh:511-514`](file:///home/titan/fte3600-linux-driver/scripts/stage2a-test.sh#L511-L514) | **PASS** |
| **No Pre-Existing Unload** | Pre-flight check aborts if `spidev` is already loaded. Cleanup unloads `spidev` strictly if `SPIDEV_LOADED_BY_US == 1`. It is impossible for this script to unload another process's pre-existing module. | [`scripts/stage2a-test.sh:286-307`](file:///home/titan/fte3600-linux-driver/scripts/stage2a-test.sh#L286-L307) | **PASS** |
| **Non-Forced Removal** | Cleanup executes standard `modprobe -r spidev`. No forced removal (`-f` / `--force`) is used anywhere in the codebase. | [`scripts/stage2a-test.sh:292`](file:///home/titan/fte3600-linux-driver/scripts/stage2a-test.sh#L292) | **PASS** |
| **Load Success Verification** | `get_module_state()` queries both `/sys/module/spidev` and `/proc/modules`. Both must report `PRESENT`; otherwise `STATE_UNCERTAIN=1` halts execution immediately. | [`scripts/stage2a-test.sh:65-91`](file:///home/titan/fte3600-linux-driver/scripts/stage2a-test.sh#L65-L91), [`scripts/stage2a-test.sh:511-525`](file:///home/titan/fte3600-linux-driver/scripts/stage2a-test.sh#L511-L525) | **PASS** |
| **Load Failure Isolation** | If `modprobe spidev` fails, `STATE_UNCERTAIN=1` triggers an immediate halt before any `driver_override` or binding write occurs. Cleanup runs safely and aborts. | [`scripts/stage2a-test.sh:516-525`](file:///home/titan/fte3600-linux-driver/scripts/stage2a-test.sh#L516-L525) | **PASS** |
| **Architectural Purity** | **Zero out-of-tree kernel modules are built or loaded.** Neither `fte3600.ko` nor `uio.ko` is used. Only the mainline Linux `spidev` module is loaded. | [`scripts/stage2a-test.sh:476-483`](file:///home/titan/fte3600-linux-driver/scripts/stage2a-test.sh#L476-L483) | **PASS** |

---

## 4. Driver Binding and Override Assessment

| Audit Item | Implementation Verification | Source Line Reference | Verdict |
| :--- | :--- | :--- | :---: |
| **Original Override Capture** | `ORIGINAL_DRIVER_OVERRIDE` is read from sysfs during pre-flight and normalized (`normalize_override`). Must be empty (`""`), or script aborts. | [`scripts/stage2a-test.sh:444-451`](file:///home/titan/fte3600-linux-driver/scripts/stage2a-test.sh#L444-L451) | **PASS** |
| **Exact Value Restoration** | Cleanup restores `ORIGINAL_DRIVER_OVERRIDE` (`""`) via `echo "$orig_norm" > "$SPI_PATH/driver_override"`, followed by immediate readback verification. | [`scripts/stage2a-test.sh:257-283`](file:///home/titan/fte3600-linux-driver/scripts/stage2a-test.sh#L257-L283) | **PASS** |
| **Targeted Device Writes** | Writes are strictly scoped to `$SPI_PATH/driver_override` and `$SPIDEV_DRIVER_PATH/bind`. Value written is `$SPI_DEV` (`spi-FTE3600:00`). | [`scripts/stage2a-test.sh:539`](file:///home/titan/fte3600-linux-driver/scripts/stage2a-test.sh#L539), [`scripts/stage2a-test.sh:559`](file:///home/titan/fte3600-linux-driver/scripts/stage2a-test.sh#L559) | **PASS** |
| **Safe Unbinding Enforcement** | Cleanup unbinds strictly if `SPIDEV_BOUND_BY_US == 1`. It resolves the bound driver symlink canonically and verifies it matches `CANONICAL_SPIDEV_DRIVER_PATH`. Refuses to unbind if bound to an unexpected driver. | [`scripts/stage2a-test.sh:218-254`](file:///home/titan/fte3600-linux-driver/scripts/stage2a-test.sh#L218-L254) | **PASS** |
| **Failure Path Restoration** | Cleanup is registered via `trap cleanup EXIT INT TERM`. Any error, exit, or signal triggers unbinding and override restoration in correct reverse order. | [`scripts/stage2a-test.sh:388-390`](file:///home/titan/fte3600-linux-driver/scripts/stage2a-test.sh#L388-L390) | **PASS** |
| **Bus Isolation** | No other SPI master, slave device, or sysfs directory is accessed or modified. | Full script audit | **PASS** |

---

## 5. Signal and Interruption Safety Assessment

| Interruption Point | Implementation Mechanism & Handling | Host State Outcome | Sensor State Outcome |
| :--- | :--- | :--- | :--- |
| **Before module loading** | Standard traps `on_int` / `on_term` invoke `cleanup()`. Ownership flags are 0; cleanup skips all unloads and unbinds. Exits with signal code (130/143). | 100% pristine baseline. | Untouched baseline. |
| **During module loading** | Inside `enter_critical`. Traps `on_critical_int`/`term` defer signal (`PENDING_SIGNAL=130/143`). Module load completes, `SPIDEV_LOADED_BY_US` is set. `leave_critical` dispatches signal exit. `EXIT` trap invokes `cleanup()`, unloading `spidev`. | 100% pristine baseline. | Untouched baseline. |
| **During device binding** | Inside `enter_critical`. Signal deferred until `DRIVER_OVERRIDE_SET_BY_US` and `SPIDEV_BOUND_BY_US` are reconciled. `leave_critical` triggers `cleanup()`, which unbinds device, clears override, and unloads `spidev`. | 100% pristine baseline. | Untouched baseline. |
| **After binding, before opening spidev** | Standard traps invoke `cleanup()`. Device is unbound, override cleared, `spidev` unloaded. Exits 130/143. | 100% pristine baseline. | Untouched baseline. |
| **During an SPI transaction** | Signal delivered to shell and `stage2a-identify`. Tool closes `/dev/spidev1.0` (deasserting CS and stopping SPI clock). Shell trap invokes `cleanup()`. Device unbound, override cleared, `spidev` unloaded. | 100% pristine baseline. | **Inferred residual `0xC6=0x01` if interrupted after Step 1.** Host cleanup cannot revert internal volatile flip-flop. |
| **Immediately after tool exits** | Tool exit code captured (`IDENTIFY_EXIT_CODE`). Script invokes `cleanup()`. Unbinds device, clears override, unloads `spidev`. Exits with tool's return code. | 100% pristine baseline. | Inferred residual `0xC6=0x01`. |
| **During rollback execution** | Inside `cleanup()`, traps are redirected to `on_cleanup_int`/`term`. `CLEANUP_INTERRUPTED` records signal. **Signal does not abort cleanup mid-step.** All 3 rollback steps run to completion. Exits with signal code after verification. | 100% pristine baseline. | Inferred residual `0xC6=0x01`. |

---

## 6. GPIO and PCH Isolation Assessment

A comprehensive grep of `scripts/stage2a-test.sh`, `scripts/run-stage2a-in-terminal.sh`, and `tools/stage2a-identify.c` was performed for all GPIO keywords:

| Search Target | Occurrences in Executable Code | Description / Context |
| :--- | :---: | :--- |
| `gpiod_*` / `gpio_*` | **0** | No GPIO consumer functions in userspace or scripts. |
| `/dev/gpiochip*` | **0** | No GPIO character devices accessed or created. |
| `/sys/class/gpio` | **0** | Legacy sysfs GPIO interface is never referenced. |
| `fte3600.ko` | **0** | Stage 1 kernel module is completely omitted. |
| `uio` / `/dev/uio*` | **0** | UIO subsystem is completely omitted. |
| PCH Pad 14 manipulation | **0** | Only inspected via read-only debugfs sanity check; never written. |
| PCH Pad 229 manipulation | **0** | Only inspected via read-only debugfs sanity check; never written. |

**Confirmation:** The revised Stage 2A implementation does **NOT** manipulate, claim, or reprogram any GPIO or PCH PADCFG register. Pad 14 remains in its BIOS output-high state (`0x84000201`), and Pad 229 remains completely unperturbed.

---

## 7. Identification Utility Verification

Inspection of [stage2a-identify.c](file:///home/titan/fte3600-linux-driver/tools/stage2a-identify.c) verifies strict adherence to approved operations:

```
MOSI Packet 1:  0x09 0xF6 0xC6 0x01                                    (4 bytes, Write 0xC6 = 0x01)
Delay:          usleep(4000)                                           (4 ms delay, CS deasserted high)
MOSI Packet 2:  0x08 0xF7 0xC6 0x00 0x00                              (5 bytes, Read 0xC6, verify 0x01)
MOSI Packet 3:  0x04 0xFB 0x9A 0x8B 0x00 0x00 0x00 0x00 0x00 0x00    (10 bytes, Read 0x1A8B Chip ID)
MOSI Packet 4*: 0x04 0xFB 0x98 0x16 0x00 0x00 0x00 0x00 0x00 0x00    (10 bytes, *Only if ID == 0x9391)
```

- **CRC Calculation:** Evaluated via `calc_crc_ccitt()` using CCITT polynomial `0x1021` with initial seed `0xFFFF` over payload bytes 6 and 7 ([`stage2a-identify.c:49-60`](file:///home/titan/fte3600-linux-driver/tools/stage2a-identify.c#L49-L60)).
- **Variant Gate:** Gated strictly on `0x9362`, `0x9365`, `0x9391` ([`stage2a-identify.c:255-267`](file:///home/titan/fte3600-linux-driver/tools/stage2a-identify.c#L255-L267)). Any other word halts with `EXIT_ERR_UNKNOWN_CHIP_ID` (8).
- **Prohibitions:** Zero commands (`0x5A/0xA5/0xC0`), zero OTP/EFUSE writes (`0xF0-0xFF`), zero analog configuration (`0xFD/0xFE`), zero reset toggling, zero retries.

---

## 8. Offline Validation Evidence

The following verification commands were executed strictly offline without accessing hardware:

1. **Compilation:**
   ```bash
   gcc -O2 -Wall -Wextra -Werror -o bin/stage2a-identify tools/stage2a-identify.c
   ```
   *Result:* Exited 0 with 0 warnings.
2. **Bash Syntax Verification:**
   ```bash
   bash -n scripts/stage2a-test.sh
   bash -n scripts/run-stage2a-in-terminal.sh
   ```
   *Result:* Exited 0 with 0 syntax errors on both scripts.
3. **Offline CLI Argument Boundaries:**
   - No arguments: Exited 1 with usage instructions.
   - Non-existent path: Exited 1 with `Cannot open: No such file or directory`.
   - Regular file (`README.md`): Exited 1 with `README.md is not a character device!` via `fstat(S_ISCHR)`.
4. **CRC Test Vector Mathematics:**
   - `0x9362` -> `0x1C53` [VERIFIED]
   - `0x9365` -> `0x6CB4` [VERIFIED]
   - `0x9391` -> `0xC32F` [VERIFIED]
   - `0x0FFF` -> `0x13C1` [VERIFIED]

---

## 9. Remaining Uncertainties

1. **Sensor-Internal State of Register `0xC6`:**
   - Writing `0xC6 = 0x01` ungates the sensor's internal SPI transceiver clock.
   - The available evidence does not prove the persistence, decay, or autonomous reset behavior of register `0xC6`. Whether it retains `0x01`, decays over time, or autonomously resets via an undocumented internal watchdog timer is an unknown sensor-internal state.
   - Host cleanup guarantees 100% host resource restoration, halts the SPI clock, and deasserts CS high, but cannot confirm the physical internal state of the sensor.
   - A full ACPI S5 cold reboot or power cycle should be performed if a return to the guaranteed unclocked power-on baseline is required.
2. **Installed Silicon Variant:**
   - The exact silicon revision installed in this laptop model will be established only upon reading register `0x1A8B`.

---

## 10. Confirmation of Zero Hardware Access

**I hereby confirm:**
- No kernel modules (`spidev`, `fte3600`, `uio`) were loaded or unloaded.
- No devices were bound or unbound.
- No SPI transactions were performed.
- No character devices (`/dev/spidev*`, `/dev/uio*`, `/dev/gpiochip*`) were opened.
- Neither `scripts/stage2a-test.sh` nor `scripts/run-stage2a-in-terminal.sh` was executed against hardware.
- The hardware state of the Dynabook Satellite Pro C40-J remains completely untouched in its pristine baseline state.

---

## 11. Final Authorization Boundary

**STOP.**  
The revised Stage 2A v2.0 pure-spidev implementation is **READY FOR EXPLICIT AUTHORIZATION**.  
Hardware operations remain strictly unauthorized. Awaiting your separate, explicit instruction.
