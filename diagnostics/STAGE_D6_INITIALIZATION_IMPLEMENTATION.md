# Stage D6 — FT9362 Initialization Implementation Report (Source-Only)

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Hardware:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`, Intel LPSS SPI2)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Date:** 2026-11-04  
**Classification:** `PASS — SOURCE-ONLY INITIALIZATION IMPLEMENTATION COMPLETE`

---

## 1. Executive Summary

In Stage D6, the driver implementation was extended to include the FT9362 / FW9369 volatile hardware startup protocol within the Linux kernel driver (`kernel/fte3600.c` and `kernel/fte3600.h`). 

In strict adherence to the **Absolute Hardware Safety Restrictions**, no physical hardware communication, module reloads, or ioctls were executed against the live sensor. The loaded kernel module remains untouched in state `8 (IDENTIFIED)` with exactly 3 transfers and 19 bytes on the SPI bus.

The implementation was validated using comprehensive static compile-time assertions and a hardened 50-test userspace simulation suite in `kernel/fte3600-test.c` (including 12 dedicated mock tests for startup).

---

## 2. Source Modifications Summary

### 2.1 `kernel/fte3600.h` (UAPI Header)
1. **Control Flags:**
   - Defined `FTE3600_INIT_FLAG_IDENTIFY_ONLY 0x00000000U`: Legacy Stage C behavior. Performs bus wake, reads chip ID `0x9362`, and advances to `IDENTIFIED` (state 8).
   - Defined `FTE3600_INIT_FLAG_FULL_STARTUP (1U << 0)`: Combined initialization flag. Allows clients starting in `RESET_SEQUENCE_ISSUED` (state 2) to perform identification and automatically advance through volatile startup to `READY` (state 4) in a single atomic ioctl.
2. **State & ABI Invariant Protection:**
   - Compile-time static assertions (`static_assert`) guarantee that `sizeof(struct fte3600_init_params)` remains exactly 32 bytes with no alignment drift.
   - `FTE3600_MAX_PUBLIC_STATE` remains strictly 8.

### 2.2 `kernel/fte3600.c` (Driver Core)
1. **Device Data Structure Extensions:**
   - Added `u8 foundry_code`: Stores silicon fab identification (`0xAA` for SMIC Fab AA, `0x00` default).
   - Added `u16 line_period_us`: Stores sensor ADC integration line timing period (`150` µs for SMIC, `200` µs default).
2. **Protocol Framing Helpers:**
   - `fte3600_spi_cmd_strobe(data, cmd, complement)`: Issues a 2-byte command strobe `{ cmd, complement }`.
   - `fte3600_spi_write_sfr(data, addr, val)`: Issues a 4-byte 8-bit SFR register write `{ 0x09, 0xF6, addr, val }`.
   - `fte3600_spi_read_sfr(data, addr, *val)`: Issues a 5-byte 8-bit SFR register read `{ 0x08, 0xF7, addr, 0x00, 0x00 }` and returns `rx[4]`.
   - `fte3600_spi_write_reg16(data, addr, val)`: Issues an 8-byte 16-bit register write `{ 0x05, 0xFA, 0x80 | (addr >> 8), addr, 0x00, 0x01, val >> 8, val & 0xFF }`.
   - `fte3600_spi_read_reg16(data, addr, *val)`: Marked `__maybe_unused`, implements 12-byte 16-bit register read framing.
3. **Volatile Startup Routine (`fte3600_hw_startup`):**
   - Implements the complete 8-step volatile startup sequence:
     * Pre-wake interrupt latch clear (`0x1A84 = 0xFFFF`).
     * Wake strobe 1 (`CMD_WAKE`: `{ 0x5A, 0xA5 }`) with 1 ms stabilization delay.
     * Hardware status poll (`read_sfr(0x80) == 0x50`) with fallback abort recovery strobe (`{ 0xC0, 0x3F }`).
     * Wake strobe 2 (`CMD_IDLE_ACK`: `{ 0xA5, 0x5A }`).
     * Sense-amp foundry interrogation loop (2 iterations: `0xF1 = 0x03` and `0xF1 = 0x13`, strobing `0xF4` four times and dummy-reading `0xF3`).
     * Foundry identification read (`read_sfr(0x9B)`) with SMIC Fab AA detection (`(fab >> 2) == 0x13`).
     * Post-interrogation interrupt latch clear (`0x1A84 = 0xFFFF`).
     * Transition to `FTE3600_STATE_READY` (state 4).
4. **State Machine & IOCTL Dispatching (`FTE3600_IOC_INIT`):**
   - **Case A (`RESET_SEQUENCE_ISSUED`):**
     * Executes `fte3600_hw_identify()`.
     * If `flags == 0`: Transitions to `IDENTIFIED` (100% backward compatible with Stage C).
     * If `flags & FTE3600_INIT_FLAG_FULL_STARTUP`: Proceeds to `fte3600_hw_startup()`, transitioning to `READY`.
   - **Case B (`IDENTIFIED`):**
     * Executes `fte3600_hw_startup()`, transitioning from `IDENTIFIED` to `READY`.
   - **Case C (Any other state):**
     * Returns `-EPERM` without touching hardware.

---

## 3. Backward Compatibility

Stage D6 provides dual-mode flexibility:
1. **Two-Stage Legacy Path:**
   - Step 1: `FTE3600_IOC_RESET` $\rightarrow$ State 2 (`RESET_SEQUENCE_ISSUED`).
   - Step 2: `FTE3600_IOC_INIT` with `flags = 0` $\rightarrow$ State 8 (`IDENTIFIED`). (Stage C ABI)
   - Step 3: `FTE3600_IOC_INIT` with `flags = 0` $\rightarrow$ State 4 (`READY`). (Stage D6 startup)
2. **Single-Stage Accelerated Path:**
   - Step 1: `FTE3600_IOC_RESET` $\rightarrow$ State 2 (`RESET_SEQUENCE_ISSUED`).
   - Step 2: `FTE3600_IOC_INIT` with `flags = FTE3600_INIT_FLAG_FULL_STARTUP` $\rightarrow$ State 4 (`READY`).

Existing userspace utilities calling INIT with `flags = 0` remain 100% functional and transition seamlessly through `IDENTIFIED`.

---

## 4. Verification Summary

- **Kernel Module Build:** Built cleanly with GCC 15 against host kernel `7.2.8-9-omarchy` headers (0 errors, 0 warnings).
- **Userspace Unit Test Suite:** 50 out of 50 tests passed (100.0% pass rate).
- **Policy Suite:** 9 out of 9 architectural policy groups passed.
- **Hardware Protection:** Live kernel driver remains loaded and bound under Option 1 (`state 8`, transfers 3, bytes 19).
