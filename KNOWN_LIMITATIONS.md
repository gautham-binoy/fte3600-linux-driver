# Known Limitations and Verification Classifications

**Target Device:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Host Controller:** Intel LPSS SPI (`pxa2xx-spi.2` / `idma64`, PCI `0000:00:1e.3`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Last Updated:** Stage D18 Final Source Implementation Release (2026-11-05)

---

## 1. Classification Categories

To prevent conflation between code correctness, mock execution, and actual silicon behavior, all driver components and behaviors are partitioned into five strictly mutually exclusive categories:

1. **Source Verified:** Verified by manual static analysis, mathematical proof, or source cross-referencing against Linux kernel sources (`v7.2.8`) and vendor reference drivers (`research/`).
2. **Mock Tested:** Executed and validated in userspace or kernel unit test environments against synthetic mock SPI transport and synthetic register responses (160/160 unit tests and 9 policy groups passing).
3. **Build Verified:** Successfully compiled without warnings under `W=1`, BTF generation, and Clang static analysis against the target Linux `7.2.8-9-omarchy` kernel build tree.
4. **Hardware Tested:** Executed against physical silicon on the Dynabook Satellite Pro C40-J under an explicitly authorized historical diagnostic stage (Stages A, B, C only).
5. **Physically Verified:** Full end-to-end functionality (e.g. valid biometric image capture matched by `libfprint` or verified electrical signaling on an oscilloscope/protocol analyzer) confirmed on physical hardware.

---

## 2. Feature Verification Status Matrix

| Component / Subsystem | Verification Level | Evidence / Documentation | Limitations & Open Uncertainties |
| :--- | :--- | :--- | :--- |
| **ACPI Device Match & Probe** | **Hardware Tested** | Stage A / Stage 2E.6 (`dmesg` probe logs) | Verified ACPI match on `FTE3600`. Power management across S3/S5 not verified on silicon. |
| **Pad 14 Electrical Reset** | **Hardware Tested** | Stage B (`stage-b-results.txt`) | Verified 20 ms active-low GPIO pulse on Pad 14. Pin voltage level not read back. |
| **Chip ID & CRC Identification** | **Hardware Tested** | Stage C (`stage-c-results.txt`) | Verified ID `0x9362`, CRC `0x1C53` over SPI wire. Proves identity only, not readiness. |
| **Startup Prefix (`5A A5`)** | **Mock Tested** | Test Suite 8, 16 (`fte3600-test.c`) | Fully verified in simulation. Physical silicon MCU response timing not physically verified. |
| **FDT Calibration (DAC Sweep)**| **Mock Tested** | Test Suites 11, 13, 16 (`fte3600-test.c`) | Convergence algorithm verified in simulation. Sensor silicon DAC linearity unverified. |
| **10,246-Byte Image FIFO Read** | **Mock Tested** | Test Suites 14, 16 (`fte3600-test.c`) | Readout burst verified in simulation. Physical burst unexecuted on silicon. |
| **Asynchronous SPI Lifetime** | **Mock Tested** | Test Suites 10, 12, 16 (`fte3600-test.c`) | Two-ref `kref` lifecycle and 250 ms timeout proven and simulated. |
| **DMA Buffer Retention Invariant** | **Mock Tested** | Test Suites 12, 16 (`fte3600-test.c`) | Policy $\mathtt{FREED}(t) \equiv 0$ proven and simulated; physical silicon drain unproven. |
| **Kernel Module Compilation** | **Build Verified** | `make W=1` (`kernel/fte3600.ko`) | 0 warnings, 0 errors, BTF emitted cleanly. |
| **Userspace Diagnostic Tool** | **Build Verified** | `tools/fte3600-tool` | 0 warnings under `-Wall -Wextra -O2`, ASan/UBSan clean. |
| **Session-Gated UAPI** | **Mock Tested** | Test Suites 11, 15, 16 (`fte3600-test.c`) | Exclusive OFD lease, token checking, 5000 ms watchdog verified. |
| **Touch IRQ & Autonomous Arming** | **Mock Tested** | Test Suites 13, 15, 16 (`fte3600-test.c`) | Operational register sequencing verified. Physical edge triggering unverified. |
| **Full Biometric Recognition** | **NOT VERIFIED** | None | Neither hardware-tested nor physically verified. No biometric pipeline exists yet. |

---

## 3. Critical Known Limitations

1. **Hardware Quiescence Limitation:**  
   Intel LPSS SPI DMA controller (`idma64`) does not guarantee physical silicon cessation upon `terminate_all()`. Consequently, any timed-out SPI transfer must permanently retain its DMA buffers in host memory ($\mathtt{FREED}(t) \equiv 0$) until platform reboot.
2. **Module Unload Permanence on Fault:**  
   If a hardware lockup occurs and a domain enters `DOMAIN_QUARANTINED`, the module reference is permanently converted to a domain pin. Standard `rmmod` will permanently return `-EBUSY`. Machine cold reboot is required to clear faulted hardware.
3. **Controller Identity Under Dynamic Topology:**  
   Physical silicon persistence cannot be guaranteed across hotplug add-in cards without hardware serial numbers. The driver implements a conservative fail-closed strategy: if controller identity cannot be resolved with 100% certainty, the domain is treated as quarantined.
4. **Table Capacity Bound:**  
   The driver enforces a compile-time limit of `FTE3600_MAX_DOMAINS 4`. Systems with more than 4 SPI controllers will fail closed with `-ENOSPC` upon probe of the 5th controller.
5. **No Persistent Frame Storage in Kernel:**  
   The kernel driver does not retain fingerprint images in memory across sessions. Raw frame data must be transferred immediately to userspace or discarded.
