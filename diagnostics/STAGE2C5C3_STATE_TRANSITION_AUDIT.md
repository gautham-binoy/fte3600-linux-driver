# Stage 2C.5C.3 — Independent Lock-State Transition Audit

## Executive Summary & Scope

An independent, software-only static audit of the FW9369 Linux SPI transport lock knowledge model implemented in Stage 2C.5C.2 was conducted in `/home/titan/fte3600-linux-driver/`. The audit evaluates the mathematical completeness, deterministic safety boundaries, error-path transitions, and source-code justifications of the five-state lock model governing sensor protection register `0x9A` and analog trim registers `0x00C0..0x00C2`.

Physical hardware communication remained **strictly prohibited**. All investigations were executed via source-code inspection, architectural analysis, and offline synthetic test runs.

---

## 1. Formal 5-State Knowledge Model Definition

The software tracks the protection state of register `0x9A` via `enum fw9369_lock_state` defined in [fw9369_transport_hw.h](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/include/fw9369_transport_hw.h#L30-L43):

```c
enum fw9369_lock_state {
    FW9369_LOCK_STATE_UNKNOWN = 0,         /* Initial state; physical silicon state is completely unknown */
    FW9369_LOCK_STATE_ASSUMED_LOCKED,      /* Lock command transmitted and verified shifted; assumed locked */
    FW9369_LOCK_STATE_UNLOCK_COMMAND_SENT, /* Unlock command currently in flight / transmission attempted */
    FW9369_LOCK_STATE_ASSUMED_UNLOCKED,    /* Unlock command transmitted and verified shifted; assumed unlocked */
    FW9369_LOCK_STATE_LOCK_COMMAND_SENT,   /* Lock command currently in flight / transmission attempted */
};
```

### Epistemic Foundations
- **Software Knowledge, Not Hardware Readback:** Because register `0x9A` is write-only with zero readable feedback bits in FT9362 silicon, states `ASSUMED_LOCKED` and `ASSUMED_UNLOCKED` represent *userspace software hypotheses* conditioned on completed SPI transfers. They do NOT represent silicon-confirmed physical states.
- **Fail-Closed Default:** Context creation unconditionally allocates memory with `calloc(1, sizeof(*ctx))` in [fw9369_transport_hw.c:493](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L493), setting `lock_state = FW9369_LOCK_STATE_UNKNOWN = 0`.

---

## 2. Permitted vs. Rejected Operations per State

The transport enforces allowlist and permission checks in `hw_transfer()` in [fw9369_transport_hw.c:253-335](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L253-L335).

| State | Unlock Command (`0x9A = 0x5A`) | Lock Command (`0x9A = 0x00`) | Protected Trim Write (`0x00C0..0x00C2`) | Normal Register Access (`0xC6`, `0x1A8B`, etc.) | Opcode Commands (`0x5A 0xA5`, etc.) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **`UNKNOWN`** | **PERMITTED** (line 272) | **PERMITTED** (line 274) | **REJECTED** (`-EPERM`, line 296) | **PERMITTED** | **PERMITTED** |
| **`ASSUMED_LOCKED`** | **PERMITTED** (line 272) | **PERMITTED** (line 274) | **REJECTED** (`-EPERM`, line 296) | **PERMITTED** | **PERMITTED** |
| **`UNLOCK_COMMAND_SENT`** | **PERMITTED** (line 272) | **PERMITTED** (line 274) | **REJECTED** (`-EPERM`, line 296) | **PERMITTED** | **PERMITTED** |
| **`ASSUMED_UNLOCKED`** | **REJECTED** (`-EPERM`, line 272) | **PERMITTED** (line 274) | **PERMITTED** (line 296) | **PERMITTED** | **PERMITTED** |
| **`LOCK_COMMAND_SENT`** | **PERMITTED** (line 272) | **PERMITTED** (line 274) | **REJECTED** (`-EPERM`, line 296) | **PERMITTED** | **PERMITTED** |

### Key Behavioral Invariants
1. **Protected Trim Isolation:** Access to analog trim registers `0x00C0..0x00C2` is strictly rejected with `-EPERM` before `ioctl` invocation unless the state is explicitly `ASSUMED_UNLOCKED`.
2. **Redundant Unlock Suppression:** When already `ASSUMED_UNLOCKED`, submitting another `0x9A = 0x5A` unlock packet is rejected with `-EPERM` at line 272 without reaching the SPI bus.
3. **Lock Idempotency Allowance:** Lock packet `0x9A = 0x00` is permitted in all states.

---

## 3. Comprehensive State Transition Matrix

The table below maps the complete set of permitted operational events, system calls, and transfer outcomes from each state:

| Current State | Event / Trigger | Next State | Return Code | Source Statement |
| :--- | :--- | :--- | :---: | :--- |
| **`UNKNOWN`** | Context initialized (`calloc`) | `UNKNOWN` | `0` | [fw9369_transport_hw.c:493](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L493) |
| **`UNKNOWN`** | Submit `0x9A = 0x5A` (pre-ioctl) | `UNLOCK_COMMAND_SENT` | — | [fw9369_transport_hw.c:348](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L348) |
| **`UNKNOWN`** | Submit `0x9A = 0x00` (pre-ioctl) | `LOCK_COMMAND_SENT` | — | [fw9369_transport_hw.c:350](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L350) |
| **`UNKNOWN`** | Attempt Trim Write (`0x00C0`) | `UNKNOWN` | `-EPERM` | [fw9369_transport_hw.c:296](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L296) |
| **`UNKNOWN`** | Normal Register Access | `UNKNOWN` | Status of transfer | Unchanged |
| **`UNKNOWN`** | Context Destroy (`free`) | Dead Context | `void` | [fw9369_transport_hw.c:520](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L520) |
| **`UNLOCK_COMMAND_SENT`** | ioctl exact success (`ret == 4`) | `ASSUMED_UNLOCKED` | `0` | [fw9369_transport_hw.c:392](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L392) |
| **`UNLOCK_COMMAND_SENT`** | ioctl negative return (`ret < 0`) | `UNKNOWN` | `-errno` | [fw9369_transport_hw.c:394](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L394) |
| **`UNLOCK_COMMAND_SENT`** | Short transfer (`ret < 4`) | `UNKNOWN` | `-EMSGSIZE` | [fw9369_transport_hw.c:394](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L394) |
| **`UNLOCK_COMMAND_SENT`** | Overflow return (`ret > 4`) | `UNKNOWN` | `-EPROTO` | [fw9369_transport_hw.c:394](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L394) |
| **`UNLOCK_COMMAND_SENT`** | System signal interruption (`-EINTR`) | `UNKNOWN` | `-EINTR` | [fw9369_transport_hw.c:394](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L394) |
| **`ASSUMED_UNLOCKED`** | Submit `0x9A = 0x5A` (redundant) | `ASSUMED_UNLOCKED` | `-EPERM` | [fw9369_transport_hw.c:272](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L272) |
| **`ASSUMED_UNLOCKED`** | Submit Trim Write (`0x00C0`) | `ASSUMED_UNLOCKED` | `0` | [fw9369_transport_hw.c:296](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L296) |
| **`ASSUMED_UNLOCKED`** | Submit `0x9A = 0x00` (pre-ioctl) | `LOCK_COMMAND_SENT` | — | [fw9369_transport_hw.c:350](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L350) |
| **`ASSUMED_UNLOCKED`** | Context Destroy (`free`) | Dead Context | `void` | [fw9369_transport_hw.c:520](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L520) |
| **`LOCK_COMMAND_SENT`** | ioctl exact success (`ret == 4`) | `ASSUMED_LOCKED` | `0` | [fw9369_transport_hw.c:398](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L398) |
| **`LOCK_COMMAND_SENT`** | ioctl negative return (`ret < 0`) | `UNKNOWN` | `-errno` | [fw9369_transport_hw.c:400](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L400) |
| **`LOCK_COMMAND_SENT`** | Short transfer (`ret < 4`) | `UNKNOWN` | `-EMSGSIZE` | [fw9369_transport_hw.c:400](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L400) |
| **`LOCK_COMMAND_SENT`** | Overflow return (`ret > 4`) | `UNKNOWN` | `-EPROTO` | [fw9369_transport_hw.c:400](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L400) |
| **`LOCK_COMMAND_SENT`** | System signal interruption (`-EINTR`) | `UNKNOWN` | `-EINTR` | [fw9369_transport_hw.c:400](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L400) |
| **`ASSUMED_LOCKED`** | Submit `0x9A = 0x5A` (pre-ioctl) | `UNLOCK_COMMAND_SENT` | — | [fw9369_transport_hw.c:348](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L348) |
| **`ASSUMED_LOCKED`** | Submit `0x9A = 0x00` (pre-ioctl) | `LOCK_COMMAND_SENT` | — | [fw9369_transport_hw.c:350](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L350) |
| **`ASSUMED_LOCKED`** | Attempt Trim Write (`0x00C0`) | `ASSUMED_LOCKED` | `-EPERM` | [fw9369_transport_hw.c:296](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L296) |
| **`ASSUMED_LOCKED`** | Context Destroy (`free`) | Dead Context | `void` | [fw9369_transport_hw.c:520](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L520) |

---

## 4. Source Code Mapping and Statement-by-Statement Tracing

### Pre-Transmission In-Flight Latching
In [fw9369_transport_hw.c:342-351](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L342-L351):
```c
bool is_unlock_cmd = (len == 4 && tx[0] == FW9369_OP_BYTE_WRITE &&
                      tx[2] == FW9369_REG_PROTECT_9A && tx[3] == FW9369_KEY_UNLOCK);
bool is_lock_cmd   = (len == 4 && tx[0] == FW9369_OP_BYTE_WRITE &&
                      tx[2] == FW9369_REG_PROTECT_9A && tx[3] == FW9369_KEY_LOCK);

if (is_unlock_cmd) {
    ctx->lock_state = FW9369_LOCK_STATE_UNLOCK_COMMAND_SENT;
} else if (is_lock_cmd) {
    ctx->lock_state = FW9369_LOCK_STATE_LOCK_COMMAND_SENT;
}
```
**Audit Finding (Verified from source code):**
The in-flight state is assigned strictly before `ioctl_fn()` is called at line 367. No early returns exist between line 351 and line 390.

### Post-Transmission Outcome Resolution
In [fw9369_transport_hw.c:390-402](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L390-L402):
```c
if (is_unlock_cmd) {
    if (status == 0) {
        ctx->lock_state = FW9369_LOCK_STATE_ASSUMED_UNLOCKED;
    } else {
        ctx->lock_state = FW9369_LOCK_STATE_UNKNOWN;
    }
} else if (is_lock_cmd) {
    if (status == 0) {
        ctx->lock_state = FW9369_LOCK_STATE_ASSUMED_LOCKED;
    } else {
        ctx->lock_state = FW9369_LOCK_STATE_UNKNOWN;
    }
}
```
**Audit Finding (Verified from source code):**
Any non-zero status unconditionally transitions the state to `UNKNOWN`. A failed lock command never produces `ASSUMED_LOCKED`.

---

## 5. Epistemic Verification Summary

| Finding | Classification | Justification |
| :--- | :---: | :--- |
| Zero-initialization to `UNKNOWN` | **Verified from source code** | `calloc(1, sizeof(*ctx))` explicitly zeroes memory at line 493. |
| Rejection of trim writes in `UNKNOWN` | **Verified from source & tests** | Line 296 enforces exact match with `ASSUMED_UNLOCKED`. Test Suite 11 Test 2 verifies `-EPERM`. |
| Rejection of trim writes during in-flight states | **Verified from tests** | Test Suite 12 Tests 6 & 7 verify rejection during active ioctl hook. |
| Rejection of redundant unlock | **Verified from source & tests** | Line 272 explicitly rejects `0x9A = 0x5A` when already `ASSUMED_UNLOCKED`. |
| Relock from `UNKNOWN` transitions to `ASSUMED_LOCKED` | **Verified from source code** | Line 398 sets `ASSUMED_LOCKED` on `status == 0` without checking prior state. (See Unknown State Recovery Audit for defect analysis). |
