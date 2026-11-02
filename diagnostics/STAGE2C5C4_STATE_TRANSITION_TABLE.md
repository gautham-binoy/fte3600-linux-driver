# Stage 2C.5C.4 — Complete State Transition Table (Post-Correction)

## Executive Summary

This document provides the exhaustive state transition matrix governing the FW9369 hardware transport backend post-Stage 2C.5C.4 correction. The table documents every initial state, operational event, system-call invocation status, expected return code, and resulting software knowledge state.

---

## 1. Complete Five-State Transition Matrix

| Initial State | Operation / Attempted Command | Permitted? | Is `ioctl` called? | Expected Return | Resulting State | Subsequent Permitted Operations | Source Reference |
| :--- | :--- | :---: | :---: | :---: | :--- | :--- | :--- |
| **`UNKNOWN`** | Unlock (`0x9A = 0x5A`) | **NO** | **NO** | `-EPERM` | `UNKNOWN` (unchanged) | Normal registers only; protected operations blocked | [hw:278](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L278) |
| **`UNKNOWN`** | Lock (`0x9A = 0x00`) | **NO** | **NO** | `-EPERM` | `UNKNOWN` (unchanged) | Normal registers only; protected operations blocked | [hw:281](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L281) |
| **`UNKNOWN`** | Protected Trim Write (`0x00C0`) | **NO** | **NO** | `-EPERM` | `UNKNOWN` (unchanged) | Normal registers only; protected operations blocked | [hw:306](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L306) |
| **`UNKNOWN`** | Normal Register Write (`0xC6 = 0x01`)| **YES** | **YES** | `0` or `-errno` | `UNKNOWN` (unchanged) | Normal registers only | [hw:286](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L286) |
| **`UNKNOWN`** | Normal Register Read (`0x1A8B`) | **YES** | **YES** | `0` or `-errno` | `UNKNOWN` (unchanged) | Normal registers only | [hw:295](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L295) |
| **`UNKNOWN`** | Context Destruction | **YES** | **NO** | `void` | `Dead Context` | None (freed) | [hw:577](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L577) |
| **`ASSUMED_LOCKED`** | Unlock (`0x9A = 0x5A`) — ioctl success | **YES** | **YES** | `0` | `ASSUMED_UNLOCKED` | Protected trim writes, lock `0x9A=0x00`, normal registers | [hw:388](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L388) |
| **`ASSUMED_LOCKED`** | Unlock (`0x9A = 0x5A`) — ioctl error (`-EIO`) | **YES** | **YES** | `-EIO` | `UNKNOWN` | Normal registers only; protected operations blocked | [hw:390](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L390) |
| **`ASSUMED_LOCKED`** | Unlock (`0x9A = 0x5A`) — short transfer | **YES** | **YES** | `-EMSGSIZE` | `UNKNOWN` | Normal registers only; protected operations blocked | [hw:390](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L390) |
| **`ASSUMED_LOCKED`** | Unlock (`0x9A = 0x5A`) — signal interrupt | **YES** | **YES** | `-EINTR` | `UNKNOWN` | Normal registers only; protected operations blocked | [hw:390](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L390) |
| **`ASSUMED_LOCKED`** | Lock (`0x9A = 0x00`) (redundant) | **NO** | **NO** | `-EPERM` | `ASSUMED_LOCKED` (unchanged) | Unlock `0x9A=0x5A`, normal registers | [hw:281](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L281) |
| **`ASSUMED_LOCKED`** | Protected Trim Write (`0x00C0`) | **NO** | **NO** | `-EPERM` | `ASSUMED_LOCKED` (unchanged) | Unlock `0x9A=0x5A`, normal registers | [hw:306](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L306) |
| **`ASSUMED_LOCKED`** | Normal Register Access | **YES** | **YES** | `0` or `-errno` | `ASSUMED_LOCKED` (unchanged) | Unlock `0x9A=0x5A`, normal registers | [hw:286](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L286) |
| **`UNLOCK_COMMAND_SENT`**| Protected Trim Write (`0x00C0`) | **NO** | **NO** | `-EPERM` | `UNLOCK_COMMAND_SENT` | None while in flight | [hw:306](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L306) |
| **`UNLOCK_COMMAND_SENT`**| Unlock (`0x9A = 0x5A`) | **NO** | **NO** | `-EPERM` | `UNLOCK_COMMAND_SENT` | None while in flight | [hw:278](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L278) |
| **`UNLOCK_COMMAND_SENT`**| Lock (`0x9A = 0x00`) | **NO** | **NO** | `-EPERM` | `UNLOCK_COMMAND_SENT` | None while in flight | [hw:281](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L281) |
| **`ASSUMED_UNLOCKED`** | Unlock (`0x9A = 0x5A`) (redundant) | **NO** | **NO** | `-EPERM` | `ASSUMED_UNLOCKED` (unchanged) | Protected trim writes, lock `0x9A=0x00`, normal registers | [hw:278](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L278) |
| **`ASSUMED_UNLOCKED`** | Protected Trim Write (`0x00C0`) — success | **YES** | **YES** | `0` | `ASSUMED_UNLOCKED` (unchanged) | Protected trim writes, lock `0x9A=0x00`, normal registers | [hw:306](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L306) |
| **`ASSUMED_UNLOCKED`** | Protected Trim Write (`0x00C0`) — error | **YES** | **YES** | `-errno` | `ASSUMED_UNLOCKED` (unchanged) | Must attempt lock cleanup before aborting | [hw:306](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L306) |
| **`ASSUMED_UNLOCKED`** | Lock (`0x9A = 0x00`) — ioctl success | **YES** | **YES** | `0` | `ASSUMED_LOCKED` | Unlock `0x9A=0x5A`, normal registers | [hw:394](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L394) |
| **`ASSUMED_UNLOCKED`** | Lock (`0x9A = 0x00`) — ioctl error (`-EIO`) | **YES** | **YES** | `-EIO` | `UNKNOWN` | Normal registers only; protected operations blocked | [hw:396](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L396) |
| **`ASSUMED_UNLOCKED`** | Lock (`0x9A = 0x00`) — short transfer | **YES** | **YES** | `-EMSGSIZE` | `UNKNOWN` | Normal registers only; protected operations blocked | [hw:396](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L396) |
| **`LOCK_COMMAND_SENT`**  | Protected Trim Write (`0x00C0`) | **NO** | **NO** | `-EPERM` | `LOCK_COMMAND_SENT` | None while in flight | [hw:306](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L306) |
| **`LOCK_COMMAND_SENT`**  | Unlock (`0x9A = 0x5A`) | **NO** | **NO** | `-EPERM` | `LOCK_COMMAND_SENT` | None while in flight | [hw:278](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L278) |
| **`LOCK_COMMAND_SENT`**  | Lock (`0x9A = 0x00`) | **NO** | **NO** | `-EPERM` | `LOCK_COMMAND_SENT` | None while in flight | [hw:281](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L281) |
| **Any State** | Transfer during active shutdown (`is_shutting_down`) | **NO** | **NO** | `-ESHUTDOWN` | Unchanged | None (teardown in progress) | [hw:248](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L248) |

---

## 2. Invariants Formally Verified

1. **Strict Trim Isolation:**
   Lines 305-307 check `if (ctx->lock_state == FW9369_LOCK_STATE_ASSUMED_UNLOCKED)`. Trim writes (`0x00C0..0x00C2`) are rejected with `-EPERM` in all four other states (`UNKNOWN`, `ASSUMED_LOCKED`, `UNLOCK_COMMAND_SENT`, `LOCK_COMMAND_SENT`).
2. **Strict Rejection of Blind Operations from UNKNOWN:**
   Lines 277-283 reject both `0x9A = 0x5A` and `0x9A = 0x00` before ioctl invocation.
3. **No Symmetrical Silent Recovery:**
   Any transfer error during an unlock or lock operation unconditionally transitions to `UNKNOWN`. Once in `UNKNOWN`, software cannot send further unlock or lock commands.
