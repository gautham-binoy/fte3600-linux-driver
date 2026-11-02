# Stage 2C.5C.2 — Context Recreation Safety and Fail-Closed Initialization Policy

**Document ID:** `STAGE2C5C2_CONTEXT_RECREATION_SAFETY`  
**Date:** 2026-11-02  
**Target Hardware:** FocalTech FT9362 / FW9369  
**Source Under Audit:**
- [`src/fw9369_transport_hw.c`](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c)
- [`include/fw9369_transport_hw.h`](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/include/fw9369_transport_hw.h)
**Execution Boundary:** Strictly static architecture and offline unit testing. Zero hardware operations authorized.

---

## 1. Executive Summary

A major architectural vulnerability identified in Stage 2C.5C.1 was the implicit trust placed in context recreation. When `fw9369_hw_transport_create()` allocated a new transport context via `calloc(1, sizeof(*ctx))`, the boolean `ctx->is_unlocked` was set to `false`.

Under the simplistic boolean model, `is_unlocked == false` was interpreted as **"the sensor is safely locked."**

This created an acute safety hazard:
If an earlier process crashed or encountered an uncertain transfer while register `0x9A` was unlocked, destroying the old context and instantiating a new one would **reset the software model to "locked" without any physical change in silicon**. A subsequent call would immediately issue protected-register operations or send redundant unlock commands under the false assumption that the chip was starting from a clean state.

Stage 2C.5C.2 eliminates this vulnerability by implementing a strict **fail-closed initialization policy**.

---

## 2. The Context Memory vs. Silicon State Disconnect

A userspace context structure is merely an in-memory accounting structure stored on the process heap:

```text
       +-------------------------------------------------------------+
       |   THE HEAP INITIALIZATION FALLACY                           |
       |                                                             |
       |   Zeroing heap memory with calloc() clears userspace bytes. |
       |   It DOES NOT emit clock cycles on SCLK.                    |
       |   It DOES NOT assert CS# or write registers on MOSI.        |
       |   It DOES NOT pulse the hardware reset pin (Pad 14).        |
       |                                                             |
       |   Allocating a context proves ZERO facts about the sensor.  |
       +-------------------------------------------------------------+
```

### The Recreation Hazard Scenarios:
1. **Abnormal Process Exit (`SIGKILL` / Crash) while Unlocked:**
   - Process A unlocked the sensor (`0x9A = 0x5A`) and began writing trims.
   - Process A terminated abnormally. The Linux kernel closed `/dev/spidev1.0`.
   - The FT9362 silicon remained powered by standby rail $V_{DDIO}$ and physically unlocked.
   - Process B launched and called `fw9369_hw_transport_create()`.
   - *Previous Behavior:* Process B started with `is_unlocked = false`. It assumed the device was locked, sent an unlock command, and attempted writes.
   - *Hardened Behavior:* Process B starts with `lock_state = FW9369_LOCK_STATE_UNKNOWN`. Protected writes are immediately blocked with `-EPERM`.

2. **Recreation After an Uncertain Lock Failure:**
   - Process A attempted a lock write (`0x9A = 0x00`), but `ioctl` returned `-EIO`.
   - Process A destroyed its transport context.
   - Process B created a new transport context.
   - *Hardened Behavior:* The new transport context begins unconditionally in `UNKNOWN`.

---

## 3. The Fail-Closed Initialization Policy

The hardened initialization architecture enforces four mandatory invariants:

### Invariant 1: New Contexts Begin in `UNKNOWN`
The enumeration in [`include/fw9369_transport_hw.h`](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/include/fw9369_transport_hw.h#L16-L24) explicitly assigns `FW9369_LOCK_STATE_UNKNOWN = 0`.
When `calloc(1, sizeof(struct hw_context))` zeroes memory:
```c
struct hw_context *ctx = calloc(1, sizeof(*ctx));
/* ctx->lock_state is GUARANTEED to be FW9369_LOCK_STATE_UNKNOWN */
```
The transport layer starts with zero assumptions regarding peripheral silicon state.

### Invariant 2: Protected Writes Rejected in `UNKNOWN`
In [`src/fw9369_transport_hw.c`](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L294-L300):
```c
/* Protected analog trim bank (0x00C0..0x00C2) requires assumed unlocked state */
if (reg >= 0x00C0 && reg <= 0x00C2) {
    if (ctx->lock_state == FW9369_LOCK_STATE_ASSUMED_UNLOCKED)
        packet_valid = true;
}
```
If `lock_state == FW9369_LOCK_STATE_UNKNOWN`, `packet_valid` remains `false`.
The transport intercepts the transfer and immediately returns `-EPERM` **before** submitting any `ioctl` to the kernel.

### Invariant 3: No Assumed Initial State via Blind Unlock
Software must **NEVER** assume that blindly sending another unlock command (`0x9A = 0x5A`) establishes a known initial physical state.
- If the hardware is in an uncertain state, sending `0x5A` could latch corrupted data into adjacent registers.
- In software, sending `0x9A = 0x5A` transitions through `UNLOCK_COMMAND_SENT` to `ASSUMED_UNLOCKED` *only if* the transfer completes with exact byte count (`ret == 4`).
- But software explicitly documents that `ASSUMED_UNLOCKED` is a software knowledge state, not physical proof.

### Invariant 4: No Invented Register Readback Mechanism
Software cannot interrogate register `0x9A` because `0x9A` is write-only. Attempting to read `0x9A` violates protocol and is rejected by the allowlist. Software strictly forbids inventing mock readback mechanisms.

---

## 4. Why Protected-Register Hardware Operations Remain Blocked

The fail-closed initialization policy reveals an inescapable physical limitation:

> [!CRITICAL]
> **The Hardware Uncertainty Dilemma:**
> 1. A newly created userspace context starts in state `UNKNOWN`.
> 2. The sensor provides no register to read back the lock state.
> 3. The PCH Pad 14 hardware reset line is firmware-locked and cannot be driven from userspace to force a cold reset.
> 4. Therefore, userspace software has **no physically verifiable mechanism to prove the initial state of the silicon lock latch**.
> 5. Consequently, any post-Stage-2A operation that relies on protected analog trim registers (`0x00C0..0x00C2`) **REMAINS BLOCKED BY PLATFORM UNCERTAINTY** until the operator performs an ACPI S5 cold reboot.

---

## 5. Offline Test Verification

Test Suite 11 in `tests/test_runner.c` verifies context recreation safety across all scenarios:

| Test Scenario | Operational Steps | Verified Postcondition | Result |
| :--- | :--- | :--- | :---: |
| **New Context Default** | `fw9369_hw_transport_create()` | `get_lock_state() == UNKNOWN` | **PASS** |
| **Protected Write Blocked**| Attempt `trim_wr` on new context | Returns `-EPERM`; `fake_ioctl_called == false` | **PASS** |
| **Destruction while UNKNOWN** | Destroy context in `UNKNOWN` | Clean exit; no errors or leaked descriptors | **PASS** |
| **Recreation after Unlock Error**| Fail unlock (`-EIO`); destroy; recreate context | Recreated context begins in `UNKNOWN` | **PASS** |
| **Recreation after Lock Error**| Fail lock (`-EIO`); destroy; recreate context | Recreated context begins in `UNKNOWN` | **PASS** |
| **Blocked Trim After Recreation**| Attempt `trim_wr` on recreated context | Returns `-EPERM`; `fake_ioctl_called == false` | **PASS** |

---

## 6. Audit Conclusion

The fail-closed initialization policy eliminates the context recreation vulnerability. New transport contexts unconditionally begin in `UNKNOWN` state, strictly reject protected writes, and refuse to equate heap allocation with physical silicon safety.
