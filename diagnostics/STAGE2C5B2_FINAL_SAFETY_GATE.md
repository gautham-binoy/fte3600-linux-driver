# Stage 2C.5B.2 — Final Safety Gate and Transport Hardening Verdict

**Document ID:** `STAGE2C5B2_FINAL_SAFETY_GATE`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`)  
**Host Kernel:** Linux `7.2.8-9-omarchy` x86_64  
**Audit Scope:** Final synthesis of Stage 2C.5B.2 transport hardening, verified facts vs. assumptions, and safety gate sign-off.  
**Hardware Authorization:** NONE (Software & Static Analysis Only).

---

## 1. Executive Summary

Stage 2C.5B.2 conducted an exhaustive, software-only hardening and verification cycle on the Linux SPI hardware transport implementation in `tools/fw9369-mock/`.

During this stage:
1. **Device Identity Enforcement:** Designed and implemented `fw9369_hw_verify_device_identity()`, establishing an explicit four-point sysfs hierarchy verification (`pxa2xx-spi.2` -> `spi1` -> `spi-FTE3600:00` -> `acpi:FTE3600:`) decoupled from device opening, verified via 10 offline synthetic sysfs tests.
2. **Timeout Rigor:** Formally audited the reported 1,000 ms `spi-pxa2xx` timeout and proved that it is **UNVERIFIED** on the running `7.2.8-9-omarchy` kernel. The driver architecture strictly decouples settling delays from unverified kernel timeouts and avoids dangerous signal-based cancellations.
3. **Register & Protection Hardening:** Implemented strict key validation for register `0x9A` (allowing only `0x5A` and `0x00`), prevented repeated unlock requests, gated analog trim bank `0x00C0..0x00C2` access behind verified unlocked state, and ensured lock failures retain an unlocked/unsafe state without retries.
4. **Adversarial Test Expansion:** Expanded automated offline test coverage to 137 assertions across 10 test suites (achieving 100.0% pass rate under `-Wall -Wextra -Werror -pedantic -std=c11 -O2`), verifying that every malformed, truncated, or unauthorized packet is rejected *before* `fake_ioctl()` is invoked.
5. **Safety Gate Integrity:** Verified that `bin/fw9369-hw-tool` embeds an immutable fail-closed gate that unconditionally halts execution with exit code 1, and that `bin/fw9369-tool` contains zero hardware transport symbols.

---

## 2. Distinction Between Verified Facts and Documented Limitations

In strict adherence to engineering safety discipline, all verified technical facts are distinguished from documented limitations and unverified assumptions:

### 2.1 Verified Facts
- **Fact 1 (Host Baseline Integrity):** PCH Pad 229 remains locked at `0x82000100 [LOCKED tx]`; PCH Pad 14 remains locked at `0x84000201 [LOCKED]`. Sensor is in clean, pristine uninitialized hardware state.
- **Fact 2 (Binary Isolation):** `nm -g bin/fw9369-tool` contains exactly 0 `fw9369_hw_*` symbols. Mock userspace tooling cannot issue hardware ioctls.
- **Fact 3 (Pre-Ioctl Allowlist Interception):** All 44 adversarial test vectors in Test Suite 10 were verified empirically to fail with `fake_ioctl_called == false`.
- **Fact 4 (Fail-Closed Hardware Tool Gate):** Executing `bin/fw9369-hw-tool` outputs a security alert and terminates with exit code 1 without opening device nodes.
- **Fact 5 (Offline Test Coverage):** 137 / 137 test assertions compile with zero warnings and pass with 100.0% success in pure userspace memory.

### 2.2 Documented Limitations & Unverified Assumptions
- **Limitation 1 (Unverified Kernel Timeout):** The reported 1,000 ms `spi-pxa2xx` timeout is **unverified** on Linux `7.2.8-9-omarchy`. The Linux SPI core relies on `wait_for_completion()` in `TASK_UNINTERRUPTIBLE` state `D`. If an electrical fault grounds SCLK or freezes the controller, the calling thread could hang indefinitely.
- **Limitation 2 (Unverified Wire CS# Continuity):** Setting `tr.cs_change = 0` communicates a software directive to the kernel, but physical continuous CS# on the wire cannot be verified without oscilloscope inspection or physical silicon frame readout.
- **Limitation 3 (Kernel Buffer Prerequisite):** Standard `spidev.bufsiz = 4096` will reject the mandatory 10,246-byte single-burst FIFO read with `-EMSGSIZE`. The host operator must ensure `spidev.bufsiz=65536` is configured prior to live execution.
- **Limitation 4 (Sysfs Dependency):** `fw9369_hw_verify_device_identity()` requires `spi-FTE3600:00` to be actively bound to `spidev` before `/sys/class/spidev/spidev1.0/device` exists. On an unbound host, it safely returns `-ENODEV`.

---

## 3. Mandatory Operator Prerequisites Before Any Live Hardware Execution

No physical sensor transaction is authorized during this stage. Before a human operator authorizes any future hardware stage, the following prerequisites must be met:

1. **Explicit Operator Sign-Off:** A human operator must review the documented limitations and explicitly authorize hardware execution.
2. **Buffer Sizing Configuration:** Operator must verify `/sys/module/spidev/parameters/bufsiz >= 65536`.
3. **Sysfs Preflight Verification:** Operator must confirm `fw9369_hw_verify_device_identity("/dev/spidev1.0", NULL) == 0` following driver binding.
4. **Cold Boot Fallback Readiness:** Because sensor initialization is software-irreversible, operator must be prepared to execute an ACPI S5 cold power cycle if unexpected silicon behavior occurs.

---

## 4. Authoritative Safety Gate Verdict

Following the comprehensive implementation of identity verification, register protection hardening, pre-ioctl allowlist interception, and full adversarial test validation:

```text
===================================================================
 AUTHORITATIVE SAFETY GATE VERDICT:
 TRANSPORT HARDENING PASSED WITH DOCUMENTED LIMITATIONS
===================================================================
```
