# Stage 2C.5C.3 — Unknown State Recovery and Locking Audit

## Executive Summary

This audit evaluates the safety, justification, and hazards of issuing sensor protection lock command `0x9A = 0x00` when the software knowledge state is `FW9369_LOCK_STATE_UNKNOWN`.

**Primary Conclusion:**
Issuing `0x9A = 0x00` from an `UNKNOWN` state cannot be proven safe over an SPI bus whose physical framing or state machine may be desynchronized. Furthermore, because register `0x9A` is strictly write-only, software cannot verify whether silicon actually received, decoded, or latched the lock command.

Therefore, the safety of issuing `0x9A = 0x00` from `UNKNOWN` is formally classified as **UNKNOWN / UNPROVEN**.

The current implementation contains an epistemic defect: when a lock command completes with `status == 0` from `UNKNOWN`, the transport transitions unconditionally to `FW9369_LOCK_STATE_ASSUMED_LOCKED`. This creates an artificial sense of security that permits subsequent unlock and trim operations on potentially corrupted silicon.

---

## 1. Scenario-by-Scenario Evaluation

### Scenario A: Unlock Command Failed After Partial Byte Shift
- **Physical Context:** An unlock command (`[0x09, 0xF6, 0x9A, 0x5A]`) was aborted mid-transfer (e.g. 2 bytes transmitted before an electrical glitch, clock stop, or kernel timeout).
- **Silicon State:** The FT9362 internal SPI shift register holds partial bits or bytes. If the hardware does not strictly reset its bit counter upon chip-select deassertion, the next bytes received will concatenate with previous residue.
- **Analysis of `0x9A = 0x00`:**
  If userspace sends `[0x09, 0xF6, 0x9A, 0x00]`, the sensor may decode:
  - Opcode = `0x9A` (unknown/invalid opcode)
  - Address = `0x00`
  - Result: The lock is NOT latched, framing error is propagated, and subsequent commands will misalign.
- **Verdict for Scenario A:** **UNSAFE / UNPROVEN**.

---

### Scenario B: Lock Command Failed After Partial Byte Shift
- **Physical Context:** A lock command (`[0x09, 0xF6, 0x9A, 0x00]`) failed mid-transfer (e.g., `-EMSGSIZE` or `-EIO`).
- **Silicon State:** The analog trim bank may be unlocked, partially locked, or the SPI shift register corrupted.
- **Analysis of `0x9A = 0x00`:**
  Issuing another lock command without an out-of-band hardware reset risks repeating the transfer failure or concatenating misaligned bytes.
- **Verdict for Scenario B:** **UNSAFE / UNPROVEN**.

---

### Scenario C: Process Terminated While Sensor Unlocked
- **Physical Context:** A previous driver process crashed or was killed via `SIGKILL` while holding the sensor in `ASSUMED_UNLOCKED`. A new process launches.
- **Silicon State:** The FT9362 silicon remains powered and its analog trim gate remains unlocked.
- **Analysis of `0x9A = 0x00`:**
  If the bus is synchronized, sending `0x9A = 0x00` might latch the lock. HOWEVER:
  1. Software cannot read register `0x9A` to verify success.
  2. If the previous process crashed due to an SPI host controller stall, the SPI bus might still be wedged.
- **Verdict for Scenario C:** **UNPROVEN / UNVERIFIABLE**.

---

### Scenario D: Context Initialized with Unknown Hardware State
- **Physical Context:** System boot, resume from suspend, or driver startup without previous initialization history.
- **Silicon State:** Sensor could be in deep power-down (`0xC6 == 0`), hardware reset, or intermediate power state.
- **Analysis of `0x9A = 0x00`:**
  If the sensor is in deep sleep (`0xC6 == 0`), its internal core clock is stopped. The SPI peripheral may ignore register writes completely or produce bus contention. Issuing `0x9A = 0x00` before waking up `0xC6` is ignored by silicon.
- **Verdict for Scenario D:** **INEFFECTIVE / UNPROVEN**.

---

### Scenario E: Framing Desynchronization or Partial Transfer
- **Physical Context:** Noise on SCLK or MOSI caused the FT9362 slave shift register to register an extra or missing clock edge.
- **Silicon State:** SPI bit alignment is shifted (e.g. 1 bit off).
- **Analysis of `0x9A = 0x00`:**
  Every byte transmitted over MOSI will be decoded with a 1-bit phase shift. The complement check `0xF6` will fail on the sensor side, causing the command to be dropped silently, or worse, matching an unintended command sequence.
- **Verdict for Scenario E:** **HAZARDOUS / UNPROVEN**.

---

## 2. Analysis of Idempotency and Readback Impossibility

### Is `0x9A = 0x00` Idempotent?
In idealized logic, writing `0x00` to a lock register multiple times is idempotent.
**However, on physical SPI silicon, a transaction is only idempotent if:**
1. Bus framing is 100% aligned.
2. The peripheral is powered and clocked.
3. The transaction does not alter internal state machines in unexpected ways.

Because none of these three conditions are guaranteed when the software knowledge state is `UNKNOWN`, **idempotency cannot be assumed**.

### Write-Only Feedback Void
- Register `0x9A` is write-only.
- Reading `0x9A` returns bus noise or invalid data; `fw9369_transport_hw.c:286` explicitly rejects byte reads to `0x9A` with `-EPERM`.
- There is **no read-back status register** anywhere in the FT9362 architecture that reveals whether the analog trim gate is locked or unlocked.
- Therefore, a return of `ret == 4` from `ioctl(SPI_IOC_MESSAGE(1))` proves only that the host controller shifted 4 bytes out of its FIFO. It provides **zero physical proof** that silicon latched the lock.

---

## 3. Reference Implementation and Silicon Documentation Evidence

### Vendor Reference Driver Inspection
Inspection of the official FocalTech Android kernel driver (`drivers/input/fingerprint/focaltech_fingerprint/`):
1. **No SPI In-Band Recovery:**
   The vendor driver **never** attempts to recover an unknown or failed state by blindly sending `0x9A = 0x00` over SPI.
2. **Cold Reset as Mandatory Prerequisite:**
   Whenever an SPI error (`-EIO`, `-ETIMEDOUT`, framing error) is encountered during initialization or calibration, the vendor driver calls:
   ```c
   ff_hw_reset(); /* Pulls RST pin low for 10ms, then high, then sleeps 50ms */
   ```
   The vendor driver relies exclusively on **hardware GPIO reset** to clear internal shift registers, reset MCU state, and restore registers to their power-on default state (where the analog trim bank is guaranteed locked by hardware reset defaults).
3. **Dynabook Satellite Pro C40-J Reality:**
   On this target platform, hardware reset capability via ACPI GPIO (Pad 14 / Pad 229) is **non-functional and blocked** (confirmed in Stage 2A and Stage 2C.5A). The driver cannot issue a hardware reset.

---

## 4. Defect in Current Stage 2C.5C.2 Implementation

### Source-Code Inspection
In [fw9369_transport_hw.c:274-276](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L274-L276) and [fw9369_transport_hw.c:397-401](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L397-L401):

```c
/* Allowlist check: permits 0x9A = 0x00 in ALL states, including UNKNOWN */
} else if (val == FW9369_KEY_LOCK) {
    packet_valid = true;
}

/* Outcome handling: unconditionally sets ASSUMED_LOCKED on status == 0 */
} else if (is_lock_cmd) {
    if (status == 0) {
        ctx->lock_state = FW9369_LOCK_STATE_ASSUMED_LOCKED;
    } else {
        ctx->lock_state = FW9369_LOCK_STATE_UNKNOWN;
    }
}
```

### The Architectural Defect
If `ctx->lock_state == FW9369_LOCK_STATE_UNKNOWN`:
1. Software sends `0x9A = 0x00`.
2. The Linux kernel LPSS driver shifts 4 bytes and returns 0.
3. The transport transitions to `FW9369_LOCK_STATE_ASSUMED_LOCKED`.
4. **Hazard:** The transport now allows a subsequent `0x9A = 0x5A` unlock command and subsequent protected trim writes (`0x00C0..0x00C2`), even though the silicon state was never verified and may be corrupted!

---

## 5. Smallest Fail-Closed Correction Recommendation

To prevent unjustified confidence escalation from `UNKNOWN` to `ASSUMED_LOCKED`:

### Recommended Policy
When `ctx->lock_state == FW9369_LOCK_STATE_UNKNOWN`:
- Sending `0x9A = 0x00` may be permitted as a best-effort blind safety write.
- **HOWEVER:** If the transfer succeeds (`status == 0`), the software state **MUST REMAIN `FW9369_LOCK_STATE_UNKNOWN`**.
- It must **NEVER** transition to `FW9369_LOCK_STATE_ASSUMED_LOCKED` unless preceded by a verified hardware reset or power cycle sequence.

```c
/* Recommended correction in fw9369_transport_hw.c: */
} else if (is_lock_cmd) {
    if (status == 0) {
        if (prev_lock_state == FW9369_LOCK_STATE_UNKNOWN) {
            /* Retain UNKNOWN: blind lock write cannot establish known state */
            ctx->lock_state = FW9369_LOCK_STATE_UNKNOWN;
        } else {
            ctx->lock_state = FW9369_LOCK_STATE_ASSUMED_LOCKED;
        }
    } else {
        ctx->lock_state = FW9369_LOCK_STATE_UNKNOWN;
    }
}
```

This ensures that once an operation fails into `UNKNOWN`, software cannot magically "re-lock" its way back into authorized trim-writing capability without a proven reset.
