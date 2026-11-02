# Stage 2C.5C.4 — UNKNOWN-State Fail-Closed Correction

## Executive Summary

Stage 2C.5C.4 implements a software-only correction of the FW9369 hardware SPI transport backend ([`src/fw9369_transport_hw.c`](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c)) to resolve the architectural defect identified during the Stage 2C.5C.3 audit:

**The Prior Defect:**
Under Stage 2C.5C.2, when the software knowledge state was `FW9369_LOCK_STATE_UNKNOWN`:
1. Submitting unlock command `0x9A = 0x5A` was permitted because the code checked `ctx->lock_state != ASSUMED_UNLOCKED`.
2. Submitting lock command `0x9A = 0x00` was unconditionally permitted (`packet_valid = true`).
3. Upon a successful ioctl return (`status == 0`), the transport transitioned to `FW9369_LOCK_STATE_ASSUMED_LOCKED`.

This behavior created an unjustified assumption of safety: a successful userspace `ioctl` only confirms that bytes were shifted into the host controller FIFO, but provides zero proof that FT9362 silicon accepted or latched the lock command. Transitioning to `ASSUMED_LOCKED` enabled subsequent unlock commands and protected trim writes (`0x00C0..0x00C2`) on potentially desynchronized or corrupted silicon.

**The Correction:**
Both unlock (`0x9A = 0x5A`) and lock (`0x9A = 0x00`) are now **strictly rejected before `ioctl` invocation** with `-EPERM` whenever `ctx->lock_state == FW9369_LOCK_STATE_UNKNOWN`. The software state is preserved as `UNKNOWN`, and zero SPI transactions are submitted.

---

## 1. Source-Code Implementation

In [src/fw9369_transport_hw.c:270-285](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L270-L285):

```c
if (reg == FW9369_REG_PROTECT_9A) {
    /*
     * Protection Register 0x9A Hardening (Stage 2C.5C.4):
     * Strict Fail-Closed Policy:
     * - When state is FW9369_LOCK_STATE_UNKNOWN: REJECT BOTH unlock (0x5A) and lock (0x00).
     * - Unlock (0x5A) is ONLY permitted when state is FW9369_LOCK_STATE_ASSUMED_LOCKED.
     * - Lock (0x00) is ONLY permitted when state is FW9369_LOCK_STATE_ASSUMED_UNLOCKED.
     * - In-flight states, redundant operations, or unverified states are rejected before ioctl.
     */
    if (val == FW9369_KEY_UNLOCK) {
        if (ctx->lock_state == FW9369_LOCK_STATE_ASSUMED_LOCKED)
            packet_valid = true;
    } else if (val == FW9369_KEY_LOCK) {
        if (ctx->lock_state == FW9369_LOCK_STATE_ASSUMED_UNLOCKED)
            packet_valid = true;
    }
}
```

### Allowlist Enforcement Mechanics
1. When `ctx->lock_state == FW9369_LOCK_STATE_UNKNOWN`:
   - `val == 0x5A`: Evaluates to `false` (state is not `ASSUMED_LOCKED`).
   - `val == 0x00`: Evaluates to `false` (state is not `ASSUMED_UNLOCKED`).
   - `packet_valid` remains `false`.
   - Lines 344-349 decrement `active_ops`, broadcast condition variable, release mutex, and return `-EPERM` immediately without calling `ioctl_fn()`.
2. Zero State Mutation:
   Because the rejection occurs at the transport boundary before line 357, `ctx->lock_state` is untouched and remains `UNKNOWN`.
3. Zero Bus Transmission:
   No bytes are shifted on MOSI, eliminating the hazard of concatenating framing errors on an uncalibrated or desynchronized bus.

---

## 2. Permitted Normal Lifecycle

Under the corrected architecture, state transitions for protection register `0x9A` are strictly confined to justified protocol paths:

```
                      [ Initial: UNKNOWN ]
                               │
            (All 0x9A and trim writes REJECTED with -EPERM)
                               │
       ┌───────────────────────┴───────────────────────┐
       ▼                                               ▼
[ ASSUMED_LOCKED ]                           [ In-Flight States ]
       │                                     (UNLOCK_COMMAND_SENT)
       │─── Unlock attempted (0x9A=0x5A) ───► (LOCK_COMMAND_SENT)
       │                                               │
       │◄─── ioctl success (ret == 4) ─────────────────┤
       │                                               ▼
       ▼                                     ioctl failure / short
[ ASSUMED_UNLOCKED ]                                   │
       │                                               │
       │─── Lock attempted (0x9A=0x00) ──────► (to UNKNOWN)
       │                                               │
       ▼                                               ▼
[ ASSUMED_LOCKED ]                           [ State: UNKNOWN ]
```

### Exact Lifecycle Transitions
1. **`ASSUMED_LOCKED` $\rightarrow$ `UNLOCK_COMMAND_SENT`:**
   - Permitted: Unlock command `0x9A = 0x5A`.
   - In-flight state latched before `ioctl`.
   - On `status == 0` $\rightarrow$ Transitions to `ASSUMED_UNLOCKED`.
   - On `status != 0` (negative errno, short transfer, overflow, interruption) $\rightarrow$ Transitions to `UNKNOWN`.
2. **`ASSUMED_UNLOCKED` $\rightarrow$ `LOCK_COMMAND_SENT`:**
   - Permitted: Lock command `0x9A = 0x00` and protected trim writes (`0x00C0..0x00C2`).
   - In-flight state latched before `ioctl`.
   - On `status == 0` $\rightarrow$ Transitions to `ASSUMED_LOCKED`.
   - On `status != 0` $\rightarrow$ Transitions to `UNKNOWN`.
3. **`UNKNOWN` (Terminal Fail-Closed):**
   - Unlock (`0x9A = 0x5A`) $\rightarrow$ **REJECTED** (`-EPERM`).
   - Lock (`0x9A = 0x00`) $\rightarrow$ **REJECTED** (`-EPERM`).
   - Protected trim writes $\rightarrow$ **REJECTED** (`-EPERM`).
   - Normal register reads/writes (`0xC6`, `0x1A8B`) $\rightarrow$ Permitted.

---

## 3. Context Creation and Epistemic Policy

### Zero-Initialization
In [src/fw9369_transport_hw.c:523](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L523), `calloc(1, sizeof(*ctx))` initializes all memory to zero, ensuring:
`ctx->lock_state = FW9369_LOCK_STATE_UNKNOWN = 0;`

### Epistemic Boundary Notice
- A newly created context begins in `UNKNOWN` and **remains unable to perform protected operations** until a genuinely verified hardware reset sequence exists.
- The software driver **does not invent an API** that pretends a physical reset occurred when it cannot verify that event.
- For unit testing of transitions from `ASSUMED_LOCKED` and `ASSUMED_UNLOCKED`, an explicit mock fixture injector [`fw9369_hw_set_lock_state_for_test()`](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L664) is provided exclusively for offline tests.
