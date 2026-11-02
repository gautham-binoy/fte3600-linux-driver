# Stage 2B.5 — Critical Failure State Analysis: Aborted Handshake

**Document ID:** `STAGE2B5_FAILURE_STATE_ANALYSIS`  
**Date:** 2026-11-02  
**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FT9362 / FW9369  
**Status:** COMPLETE — STATIC FAILURE PATH AUDIT  
**Hardware Execution Authorization:** NONE (Static Analysis Only)

---

## 1. Executive Summary

This investigation performs a rigorous post-mortem analysis of the specific failure scenario introduced by the Stage 2B.4 minimal sequence:
$$\text{Transceiver Wakeup } (0xC6=1) \longrightarrow \text{Command } 0x5A \longrightarrow \text{Read } 0x80 \neq 0x50 \longrightarrow \textbf{Immediate Abort (No } 0xA5 \textbf{)}$$

The analysis proves that:
1. Omitting `0xA5` after transmitting `0x5A` creates a stranded microcontroller state that has **zero precedent and zero error-handling coverage** in the upstream reference code.
2. In all reference drivers, failure of the `0x80` check triggers exception recovery strobes (`C0 3F`) and **always concludes with `0xA5`**.
3. Leaving the MCU in an un-closed command state cannot be resolved by host-side SPI unbinding or module unloading.
4. Because software recovery cannot be guaranteed without toggling hardware reset (Pad 14, which is electrically unavailable), recovering from this stranded state requires a full cold reboot.
5. Consequently, **issuing command `0x5A` in isolation without an integrated, complete initialization sequence introduces severe asymmetric risk**.

---

## 2. Granular Answers to Investigation Questions

### 2.1. What MCU State Might Remain?
- Command `0x5A` (`wake`) alerts the internal microcontroller execution loop to expect an idle transition.
- If status `0x80` returns something other than `0x50` (e.g. `0x00` due to boot delay or `0x54` from previous capture state), the MCU instruction pointer is either:
  1. Stalled in an internal state-transition wait loop;
  2. Awaiting a secondary command opcode or exception strobe (`0xC0`);
  3. Desynchronized from the host SPI bus framing.
- By failing to transmit `0xA5` (`end`), the host leaves the MCU's SPI command decoder in an open, un-terminated transaction state.

### 2.2. Does the Reference Implementation Have a Corresponding Failure Path?
**NO.**
- In `fw9369_release.c:458-464`:
```c
if (value != 0x50)
  {
    if (!transfer (stop, sizeof (stop), io, d))
      return 0;
    io->wait_ms (io->context, 1);
  }
return transfer (end, sizeof (end), io, d);
```
- In `ft93xx.c:268-280`:
```c
for (unsigned int i = 0; value != 0x50 && i < 5; ++i)
  {
    if (!command (op, 0xc0))
      return 0;
    ...
  }
return command (op, 0xa5);
```
In both drivers, even on complete failure, `0xA5` (`end`) is transmitted to close the transaction. There is **no path in reference code that aborts after `0x5A` without sending `0xA5`**.

### 2.3. Does the Reference Implementation Issue a Cleanup Command?
- When `0x80 != 0x50`, the reference driver transmits `command(0xC0, 0x3F)` (named `stop`).
- However, our safety constraints strictly prohibit automatic recovery writes (`0xC0 0x3F`) because sending blind exception strobes into an indeterminate state risks unpredictable hardware behavior.
- Therefore, a safety harness constrained by "no recovery writes" has no sanctioned protocol mechanism to recover.

### 2.4. Is Leaving the MCU in That State Known to Be Safe?
**UNKNOWN — HIGH RISK.**
- The silicon documentation is proprietary.
- Leaving the MCU in an un-terminated command mode could cause internal lockup, watchdog timeout resets, or bus contention if the internal state machine attempts to clock lines.
- It is not known to be safe.

### 2.5. Can the Host Restore Original Sensor State Without Hardware Reset?
**NO.**
- Closing `/dev/spidev*` and unloading the module merely halts the host SPI clock and sets CS high.
- The sensor remains powered by VCC.
- The sensor's volatile register `0xC6` remains set to `0x01`.
- The MCU remains stranded in its transitional state.
- Host rollback cannot reset the sensor silicon.

### 2.6. Is There Evidence That a Cold Reboot is Required?
**YES.**
- In `diagnostics/STAGE1_GPIO_RESTORATION_INVESTIGATION.md` and `diagnostics/STAGE2B2_PAD229_FINAL_REVIEW.md`, we proved that PCH Pad 14 (reset) cannot be toggled safely without root/kernel intervention.
- The reference driver relies on hardware reset line pulsing (`recovery.c:73-78`) when the SPI command interface locks up.
- Without Pad 14 control, only an ACPI S5 cold reboot (cutting power to the PCH/sensor power rail) guarantees a reset of the sensor's microcontroller to power-on defaults.

### 2.7. Is It Safer Not to Send 0x5A at All Until Complete Initialization Exists?
**YES, UNEQUIVOCALLY.**
- Sending `0x5A` in isolation provides zero user-facing capability.
- If it encounters any timing jitter on `0x80`, it leaves the hardware stranded in an unrecoverable state requiring a laptop power cycle.
- Deferring `0x5A` completely preserves the pristine, verified hardware baseline established in Stage 2A.
