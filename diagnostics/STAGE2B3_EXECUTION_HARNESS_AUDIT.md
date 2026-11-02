# Stage 2B.3 — Execution Harness Static Audit & Safety Guarantees

**Document ID:** `STAGE2B3_EXECUTION_HARNESS_AUDIT`  
**Date:** 2026-11-02  
**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FT9362 / FW9369  
**Status:** COMPLETE — STATIC HARNESS AUDIT  
**Hardware Execution Authorization:** NONE (Static Analysis Only)

---

## 1. Executive Summary

This investigation performs a rigorous static audit of the existing Stage 2A execution harness (`tools/stage2a-identify.c`, `scripts/stage2a-test.sh`, and `scripts/run-stage2a-in-terminal.sh`) to determine whether its architecture can enforce all mandatory safety invariants for future execution stages.

The audit confirms that the Stage 2A pure-spidev architecture successfully isolates the sensor, prevents GPIO/UIO interference, and enforces clean host-side rollback. However, the audit identifies **one critical weakness** in `scripts/stage2a-test.sh:583` regarding character device fallback selection (`head -n 1`) that must be corrected before any future execution harness is deployed.

---

## 2. Invariant Verification Matrix

We verify the 15 architectural safety guarantees against the source code of the execution harness:

| Safety Invariant | Enforced in Current Harness? | Source Citation | Evidence & Mechanism |
| :--- | :---: | :--- | :--- |
| **1. Exact Device Selection** | **YES** | `stage2a-test.sh:33, 442` | Explicitly targets `spi-FTE3600:00` at `/sys/bus/spi/devices/spi-FTE3600:00`. Fails if absent. |
| **2. Zero GPIO Access** | **YES** | `stage2a-test.sh:485-500` | Harness does not open `/dev/gpiochip*` or export GPIO lines. Reads pinctrl only via debugfs if available. |
| **3. Zero UIO Access** | **YES** | `stage2a-test.sh:478-484` | Asserts `fte3600` module ABSENT; does not touch `/dev/uio*` or `uio_pdrv_genirq`. |
| **4. No Unrelated SPI Access** | **PARTIAL** *(Weakness Identified)*| `stage2a-test.sh:580-587` | See Section 3 for analysis of loose `/dev/spidev*` fallback. |
| **5. No Unlisted Transactions**| **YES** | `stage2a-identify.c:191-360`| Fixed transaction sequence: `0xC6=1`, `0xC6` read, `0x1A8B` read, and conditional `0x1816` read. Zero additional packets. |
| **6. Zero Automatic Retries** | **YES** | `stage2a-identify.c:16-17` | All SPI transfers are single-shot; any negative `ioctl` returns immediate failure code. |
| **7. Zero Recovery Writes** | **YES** | `stage2a-identify.c:19` | On transport or CRC mismatch, tool closes `fd` and exits; no fallback registers written. |
| **8. Zero Reset Pulses** | **YES** | `stage2a-test.sh:336` | PCH Pad 14 is never requested or toggled; no reset GPIO lines are claimed. |
| **9. Zero OTP/EFUSE Access** | **YES** | `stage2a-identify.c:21` | No registers `0xF0`–`0xFF` accessed. |
| **10. Zero FIFO Reads** | **YES** | `stage2a-identify.c:1-375` | No burst read command (`0x06 0xF9`) or FIFO buffers allocated. |
| **11. No Calibration Progression**| **YES** | `stage2a-identify.c:374` | Binary exits with code 0 or error code; contains no calibration code. |
| **12. Proper FD Closure** | **YES** | `stage2a-identify.c:95,100,111,123,135,147,204,242,267,301,324,334,349,362` | `fd` is explicitly closed on every return and error path. |
| **13. Rollback on Failure** | **YES** | `stage2a-test.sh:174, 201-365` | `cleanup()` is registered via shell traps (`EXIT`, `INT`, `TERM`) with signal deferral during critical sections. |
| **14. Incomplete Rollback Reporting**| **YES** | `stage2a-test.sh:341-355` | Tracks `CLEANUP_ERRORS`; outputs prominent error banner and returns non-zero if any step fails. |
| **15. No Internal State Restoration Claim**| **YES** | `stage2a-test.sh:334-340` | Explicit notice printed stating host cleanup restores host baseline only, not sensor-internal state. |

---

## 3. Harness Weakness Analysis & Correction Requirement

### 3.1. Identified Weakness: Character Device Resolution (`head -n 1`)
In `scripts/stage2a-test.sh:580-587`:
```bash
/* scripts/stage2a-test.sh:580-587 */
if [ -c "/dev/spidev1.0" ]; then
    SPIDEV_NODE="/dev/spidev1.0"
else
    candidate=$(ls -d /dev/spidev* 2>/dev/null | head -n 1 || true)
    if [ -n "$candidate" ] && [ -c "$candidate" ]; then
        SPIDEV_NODE="$candidate"
    fi
fi
```
- **Vulnerability:** If `/dev/spidev1.0` is absent and multiple other SPI devices exist on the system (e.g., `/dev/spidev0.0` or external USB-SPI bridges), `head -n 1` picks the first lexicographical character device, regardless of whether it belongs to `spi-FTE3600:00`.
- **Severity:** Moderate (on the test laptop, only `spidev1.0` appeared, but as an architectural invariant, loose fallbacks violate strict device isolation).

### 3.2. Mandatory Correction for Future Harnesses
Any future harness (e.g. for Stage 2C.1B or Stage 2C.2) must resolve the character device strictly by inspecting the sysfs child of `spi-FTE3600:00`:
```bash
# Corrected sysfs device resolution architecture:
SPIDEV_CHILD_DIR="/sys/bus/spi/devices/spi-FTE3600:00/spidev"
if [ -d "$SPIDEV_CHILD_DIR" ]; then
    spidev_name=$(ls "$SPIDEV_CHILD_DIR" | head -n 1)
    if [ -n "$spidev_name" ] && [ -c "/dev/$spidev_name" ]; then
        SPIDEV_NODE="/dev/$spidev_name"
    fi
fi
if [ -z "$SPIDEV_NODE" ] || [ ! -c "$SPIDEV_NODE" ]; then
    echo "Error: Could not resolve exact /dev node for spi-FTE3600:00!" >&2
    exit 1
fi
```
This guarantees that only the character device spawned directly by `spi-FTE3600:00` can ever be opened.

---

## 4. Signal Deferral & Rollback Robustness Audit

### 4.1. Critical Section Protection
In `scripts/stage2a-test.sh:154-177`:
- Modifying steps (`modprobe spidev`, `echo "spidev" > driver_override`, `echo "$SPI_DEV" > bind`) are wrapped in `enter_critical` and `leave_critical`.
- Signals `SIGINT` and `SIGTERM` are trapped and deferred to ensure no operation is aborted mid-sysfs write.
- State reconciliation variables (`SPIDEV_LOADED_BY_US`, `DRIVER_OVERRIDE_SET_BY_US`, `SPIDEV_BOUND_BY_US`) are updated only after command exit code and sysfs readback verification succeed.

### 4.2. Rollback Completeness
The rollback routine in `scripts/stage2a-test.sh:201-365`:
1. Unbinds `spi-FTE3600:00` from `spidev`.
2. Restores original `driver_override`.
3. Unloads `spidev` module via `modprobe -r spidev`.
4. Verifies `spidev` is `ABSENT` in `/proc/modules`.
5. Verifies `spi-FTE3600:00` is `UNBOUND` in `/sys/bus/spi/devices/`.
6. Verifies no `/dev/spidev*` character devices remain.
7. Logs explicit warnings if sensor-internal register `0xC6` remains un-gated.

---

## 5. Audit Conclusion

The Stage 2A execution harness provides an exceptionally strong architectural foundation for safe hardware experimentation. When updated with strict sysfs child device resolution (eliminating the loose `head -n 1` fallback), the harness fully guarantees all 15 hardware safety invariants.
