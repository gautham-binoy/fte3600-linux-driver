# STAGE D6.5: STATE MACHINE AND INIT ENTRY CONDITION AUDIT

**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Workspace:** `/home/titan/fte3600-linux-driver/`  
**Safety Status:** STRICTLY READ-ONLY SOURCE AUDIT. Zero hardware communication.  

---

## 1. Executive Summary

This independent audit evaluates every state transition, entry condition, and edge case in the FTE3600 kernel driver state machine following the Stage D6.4 implementation. The audit inspects the source code in `kernel/fte3600.c` and `kernel/fte3600.h`, cross-referencing them against the userspace simulation suite in `kernel/fte3600-test.c`.

### Key Audit Findings
1. **Truthful State Modeling:** `[SOURCE-OBSERVED]` The volatile startup sequence (`fte3600_hw_startup()`) transitions exclusively to `FTE3600_STATE_INITIALIZED_UNCALIBRATED` (14). The operational state `FTE3600_STATE_READY` (4) is **never assigned** anywhere in `kernel/fte3600.c`.
2. **Fail-Closed Precondition Gating:** `[SOURCE-OBSERVED]` Calling `FTE3600_IOC_INIT` from `INITIALIZED_UNCALIBRATED` (14), `READY` (4), `UNKNOWN` (7), `UNINITIALIZED` (0), or `DEVICE_REMOVAL` (13) is strictly rejected with `-EPERM` (or `-ENODEV` if offline) before touching the SPI bus.
3. **Repeated Initialization Protection:** `[SOURCE-OBSERVED]` Repeated calls to `INIT` cannot re-execute the volatile prefix sequence or overwrite state without an intervening electrical reset via `FTE3600_IOC_RESET`.
4. **Unvalidated Upper Bits in `init_params.flags`:** `[SOURCE-OBSERVED]` `FTE3600_IOC_INIT` validates that `params.reserved[0..5] == 0`, but does not test `params.flags & ~FTE3600_INIT_FLAG_FULL_STARTUP`. Passing unknown upper bits in `flags` does not return `-EINVAL`; rather, the driver evaluates `flags & FTE3600_INIT_FLAG_FULL_STARTUP` and treats non-matching bits as identification-only.

---

## 2. Complete State Transition Matrix

The table below catalogs every possible driver state and the resulting transition, lock behavior, hardware access, and returned status for each ioctl and driver event:

| Current State (Internal) | Invoked Command / Event | Guard / Lock Conditions | Next State (Internal) | Hardware SPI/GPIO Action | Return Value | Evidence Tag |
|---|---|---|---|---|---|---|
| **Any non-offline state** | `FTE3600_IOC_RESET` | `data->lock`, `reset_gpiod != NULL` | `RESET_SEQUENCE_ISSUED` (2) | Assert GPIO Pin 14 low 10ms, deassert 20ms | `0` | `[SOURCE-OBSERVED]` |
| `UNINITIALIZED` (0) | `FTE3600_IOC_INIT` | `data->lock` | `UNINITIALIZED` (0) | None (precondition check) | `-EPERM` | `[SOURCE-OBSERVED]` |
| `RESET_SEQUENCE_ISSUED` (2) | `FTE3600_IOC_INIT` (`flags=0`) | `data->lock`, `spi != NULL` | `IDENTIFIED` (8) | 3 SPI transfers (wake + read ID 0x9362) | `0` | `[SOURCE-OBSERVED]` |
| `RESET_SEQUENCE_ISSUED` (2) | `FTE3600_IOC_INIT` (`flags=FULL_STARTUP`) | `data->lock`, `spi != NULL` | `INITIALIZED_UNCALIBRATED` (14) | 19 SPI transfers (identify + 6-step prefix) | `0` | `[SOURCE-OBSERVED]` |
| `RESET_SEQUENCE_ISSUED` (2) | `FTE3600_IOC_INIT` (identify fails) | `data->lock`, `spi != NULL` | `UNKNOWN` (7) | 1–3 SPI transfers | `-EIO` / `-ENODEV` | `[SOURCE-OBSERVED]` |
| `IDENTIFIED` (8) | `FTE3600_IOC_INIT` (any flags) | `data->lock`, `spi != NULL` | `INITIALIZED_UNCALIBRATED` (14) | 16 SPI transfers (6-step volatile prefix) | `0` | `[SOURCE-OBSERVED]` |
| `IDENTIFIED` (8) | `FTE3600_IOC_INIT` (startup fails) | `data->lock`, `spi != NULL` | `UNKNOWN` (7) | 1–16 SPI transfers | Negative errno | `[SOURCE-OBSERVED]` |
| `INITIALIZED_UNCALIBRATED` (14) | `FTE3600_IOC_INIT` (any flags) | `data->lock` | `INITIALIZED_UNCALIBRATED` (14) | None (rejected before SPI transfer) | `-EPERM` | `[SOURCE-OBSERVED]` |
| `READY` (4) | `FTE3600_IOC_INIT` (any flags) | `data->lock` | `READY` (4) | None (rejected before SPI transfer) | `-EPERM` | `[SOURCE-OBSERVED]` |
| `UNKNOWN` (7) | `FTE3600_IOC_INIT` (any flags) | `data->lock` | `UNKNOWN` (7) | None (rejected before SPI transfer) | `-EPERM` | `[SOURCE-OBSERVED]` |
| **Any state != READY** | `FTE3600_IOC_CAPTURE` | `data->lock` | Unchanged | None (gated before SPI FIFO transfer) | `-EPERM` | `[SOURCE-OBSERVED]` |
| `READY` (4) | `FTE3600_IOC_CAPTURE` | `data->lock` | `READY` (4) | None (milestone stub) | `-EOPNOTSUPP` | `[SOURCE-OBSERVED]` |
| **Any state** | `FTE3600_IOC_GET_STATE` | `data->lock` | Unchanged | None (memory snapshot copy) | `0` | `[SOURCE-OBSERVED]` |
| **Any state** | `fte3600_remove()` | `data->lock` | `DEVICE_REMOVAL` (13) | Clears `spi`, `reset_gpiod`, sets `offline=1` | `void` | `[SOURCE-OBSERVED]` |
| `DEVICE_REMOVAL` / Offline | Any hardware ioctl (`RESET`, `INIT`, `CAPTURE`) | Fast-path check / `data->lock` | Unchanged | None (rejected immediately) | `-ENODEV` | `[SOURCE-OBSERVED]` |
| `DEVICE_REMOVAL` / Offline | `FTE3600_IOC_GET_STATE` | `data->lock` | Unchanged | None (reports public `0` + `FLAG_OFFLINE`) | `0` | `[SOURCE-OBSERVED]` |

---

## 3. Investigation of Repeated and Invalid Entry Conditions

### 3.1 `FULL_STARTUP` from `RESET_SEQUENCE_ISSUED`
- **Source Path:** `kernel/fte3600.c:731-734`
- **Execution Flow:** The driver executes `fte3600_hw_identify(data)`. If identification succeeds (chip ID 0x9362 and wire CRC match), it immediately evaluates `params.flags & FTE3600_INIT_FLAG_FULL_STARTUP`. Since bit 0 is asserted, it directly invokes `fte3600_hw_startup(data)`.
- **Outcome:** Transitions from `RESET_SEQUENCE_ISSUED` (2) -> `INITIALIZED_UNCALIBRATED` (14).
- **Safety Evaluation:** **SAFE**. Exactly 1 GPIO reset pulse + 3 identify SPI transfers + 16 startup SPI transfers = 20 total hardware operations. Verified in `kernel/fte3600-test.c` Suite 8 Scenario 10. `[TEST-OBSERVED]`

### 3.2 `FULL_STARTUP` from `IDENTIFIED`
- **Source Path:** `kernel/fte3600.c:735-736`
- **Execution Flow:** Driver evaluates `else if (data->state == FTE3600_STATE_IDENTIFIED)`. It proceeds directly to `fte3600_hw_startup(data)` without repeating the 3 identify transfers.
- **Outcome:** Transitions from `IDENTIFIED` (8) -> `INITIALIZED_UNCALIBRATED` (14).
- **Safety Evaluation:** **SAFE**. Exactly 16 startup SPI transfers. Verified in `kernel/fte3600-test.c` Suite 8 Scenario 1. `[TEST-OBSERVED]`

### 3.3 `FULL_STARTUP` from `INITIALIZED_UNCALIBRATED`
- **Source Path:** `kernel/fte3600.c:737-739`
- **Execution Flow:** `data->state` is 14. It matches neither `RESET_SEQUENCE_ISSUED` (2) nor `IDENTIFIED` (8). The `else` branch is taken: `ret = -EPERM`.
- **Outcome:** Returns `-EPERM`. State remains `INITIALIZED_UNCALIBRATED` (14). Zero SPI transfers executed.
- **Safety Evaluation:** **SAFE**. Prevents unintended repetition of startup strobes or foundry loops. Verified in `kernel/fte3600-test.c` Suite 6 Scenario 13 and Suite 8 Scenario 9. `[TEST-OBSERVED]`

### 3.4 `FULL_STARTUP` from `READY`
- **Source Path:** `kernel/fte3600.c:737-739`
- **Execution Flow:** `data->state` is 4. Enters `else`: `ret = -EPERM`.
- **Outcome:** Returns `-EPERM`. State remains `READY` (4). Zero SPI transfers executed.
- **Safety Evaluation:** **SAFE**. Verified in `kernel/fte3600-test.c` Suite 8 Scenario 9. `[TEST-OBSERVED]`

### 3.5 `FULL_STARTUP` from `UNKNOWN`
- **Source Path:** `kernel/fte3600.c:737-739`
- **Execution Flow:** `data->state` is 7. Enters `else`: `ret = -EPERM`.
- **Outcome:** Returns `-EPERM`. State remains `UNKNOWN` (7). Zero SPI transfers executed.
- **Safety Evaluation:** **SAFE**. Fail-closed state requires electrical reset pulse recovery. Verified in `kernel/fte3600-test.c` Suite 3. `[TEST-OBSERVED]`

### 3.6 `FULL_STARTUP` from `OFFLINE`
- **Source Path:** `kernel/fte3600.c:648` (fast-path) and lines 716–719 (locked check).
- **Execution Flow:** `atomic_read(&data->offline)` is 1. Fast-path check immediately returns `-ENODEV`. If a race occurred with unbind while acquiring `data->lock`, line 716 detects `offline == 1` or `data->spi == NULL`, unlocks `data->lock`, and returns `-ENODEV`.
- **Outcome:** Returns `-ENODEV`. Zero hardware operations.
- **Safety Evaluation:** **SAFE**. Verified in `kernel/fte3600-test.c` Suite 8 Scenario 11. `[TEST-OBSERVED]`

### 3.7 Identification-Only `INIT` from `INITIALIZED_UNCALIBRATED`
- **Source Path:** `kernel/fte3600.c:737-739`
- **Execution Flow:** `params.flags = 0`. Since `data->state == 14`, it falls into `else`: `ret = -EPERM`.
- **Outcome:** Returns `-EPERM`. State remains `INITIALIZED_UNCALIBRATED` (14). Zero SPI transfers.
- **Safety Evaluation:** **SAFE**.

### 3.8 Identification-Only `INIT` from `READY`
- **Source Path:** `kernel/fte3600.c:737-739`
- **Execution Flow:** `params.flags = 0`. Since `data->state == 4`, it falls into `else`: `ret = -EPERM`.
- **Outcome:** Returns `-EPERM`. State remains `READY` (4). Zero SPI transfers.
- **Safety Evaluation:** **SAFE**.

### 3.9 `RESET` from Every State
- **Source Path:** `kernel/fte3600.c:675-696`
- **Execution Flow:** `FTE3600_IOC_RESET` permits execution from any operational or error state (`UNINITIALIZED`, `RESET_SEQUENCE_ISSUED`, `IDENTIFIED`, `INITIALIZED_UNCALIBRATED`, `UNKNOWN`, `ERROR`). It only rejects if offline (`atomic_read(&data->offline) || !data->reset_gpiod`).
- **Outcome:** Drives hardware active-low reset pulse on GPIO Pad 14. Resets internal state to `RESET_SEQUENCE_ISSUED` (2) and sets `data->last_error = 0`.
- **Safety Evaluation:** **SAFE**. This is the mandatory recovery mechanism that clears uncalibrated, fail-closed, or halted hardware states.

### 3.10 `INIT` Call with Unknown Flag Bits
- **Source Path:** `kernel/fte3600.c:704-710`, `731-736`
- **Execution Flow:**
  ```c
  if (params.version != FTE3600_UAPI_VERSION)
      return -EINVAL;

  for (i = 0; i < 6; i++) {
      if (params.reserved[i] != 0)
          return -EINVAL;
  }
  ```
  Notice that while `params.reserved[0..5]` are strictly validated to be 0, `params.flags` is **not masked** with `~FTE3600_INIT_FLAG_FULL_STARTUP`.
  At line 733, the condition evaluates:
  ```c
  if (ret == 0 && (params.flags & FTE3600_INIT_FLAG_FULL_STARTUP))
  ```
- **Outcome:** If userspace passes unknown bits (e.g. `flags = 0x00000002` or `flags = 0x80000000`), the driver does **not** return `-EINVAL`. If bit 0 is cleared, it treats the call as identification-only. If bit 0 is set, it proceeds with full startup.
- **Audit Assessment:** `[SOURCE-OBSERVED]` This is an unmasked input field limitation. While it does not introduce hardware hazards, strict defensive UAPI design would ideally reject undefined flag bits with `-EINVAL`.

---

## 4. State Invariant Summary

1. `data->state` is strictly serialized by `data->lock`.
2. No code path in `kernel/fte3600.c` assigns `FTE3600_STATE_READY` to `data->state`.
3. Precondition enforcement prevents skipping identification or repeating startup prefix without a reset pulse.
4. Error paths unconditionally set `data->state = FTE3600_STATE_UNKNOWN`, enforcing fail-closed semantics.
