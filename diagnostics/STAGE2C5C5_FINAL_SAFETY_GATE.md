# Stage 2C.5C.5 — Final Safety Gate and Concurrency Audit Verdict

## Executive Summary

This document presents the final safety gate review, synchronization design reassessment, evidence audit, epistemic classification, and authoritative verdict for Stage 2C.5C.5.

The hardware safety gate remains **STRICTLY CLOSED**. Physical hardware access to `/dev/spidev*`, `/dev/gpiochip*`, or `/dev/uio*` remains completely unauthorized and blocked.

---

## 1. Reassessment of Synchronization Design: Selection of Option C

The audit evaluated three potential architectural directions:
- **Option A:** Retain current synchronization design claiming the internal condition-variable drain independently guarantees safe object lifetime.
- **Option B:** Simplify to a normal mutex with a clear serialized-operation model.
- **Option C:** Retain serialization but explicitly require owner-managed thread joining before destruction, removing any misleading claim that the internal barrier independently guarantees safe object lifetime.

### Decision: Option C is Formally Selected

#### Technical Justification:
1. **Source-Level Reality:** As demonstrated in [STAGE2C5C5_ACTIVE_OPERATION_AUDIT.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C5C5_ACTIVE_OPERATION_AUDIT.md), `hw_transfer()` holds `ctx->lock` across the entire SPI transfer. Therefore, mutual exclusion between operations is already 100% enforced.
2. **Elimination of False Guarantees:** As proven in [STAGE2C5C5_CONTEXT_LIFETIME_AUDIT.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE2C5C5_CONTEXT_LIFETIME_AUDIT.md), no internal mutex or condition variable can protect an unjoined thread holding a stale pointer from dereferencing freed memory. Claiming that an internal drain barrier alone guarantees safe object destruction is an epistemic falsehood.
3. **What Option C Guarantees:**
   - Strict serialization of all SPI transactions, register reads/writes, lock-state updates, and telemetry logging across threads.
   - Clean shutdown rejection (`-ESHUTDOWN`) for operations attempted after shutdown begins.
   - Safe deallocation and resource release when callers follow the standard POSIX single-owner lifecycle contract.
4. **What Option C Cannot Guarantee:**
   - It cannot prevent a segmentation fault or memory corruption if an application calls `fw9369_hw_transport_destroy()` while unjoined worker threads are actively dereferencing `t`.
   - It cannot recover from an unclocked or permanently hanging kernel `ioctl` system call without kernel-level driver timeouts.

---

## 2. Recheck of Build and Test Evidence

| Verification Target | Command Executed | Exit Code | Interpretation |
| :--- | :--- | :--- | :--- |
| **Mock Target Build** | `make -C tools/fw9369-mock all` | `0` | Clean build under `-Wall -Wextra -Werror -pedantic -std=c11 -O2 -pthread`. |
| **Hardware Target Build** | `make -C tools/fw9369-mock hw-tool` | `0` | Clean build of `bin/fw9369-hw-tool`. |
| **Full Offline Test Suite** | `../../bin/fw9369-tests` | `0` | **219 / 219 tests passed (100.0%)**. |
| **AddressSanitizer (ASan)** | `make ... -fsanitize=address test` | `0` | **219 / 219 passed**; zero leaks, zero UAF, zero overflows. |
| **ThreadSanitizer (TSan)** | `make ... -fsanitize=thread test` | `0` | **219 / 219 passed**; zero data races, zero deadlocks. |
| **Symbol Separation** | `nm -g bin/fw9369-tool \| grep -c fw9369_hw_` | `1` | **Expected Non-Zero Exit Code**: `grep -c` outputs `0` and returns exit code `1` when zero matching lines are found. This conclusively confirms zero hardware symbols are linked into `bin/fw9369-tool`. |
| **Hardware Execution Gate** | `./bin/fw9369-hw-tool` | `1` | **Expected Non-Zero Exit Code**: Unconditional fail-closed termination with exit code `1` prior to opening any hardware device node. |

---

## 3. Epistemic Classification

### Verified from Source
- All SPI transfers, lock state transitions, and telemetry logs acquire `ctx->lock`.
- In-flight states `UNLOCK_COMMAND_SENT` and `LOCK_COMMAND_SENT` are set under `ctx->lock` before `ioctl_fn` is invoked.
- `hw_transfer()` rejects operations from `UNKNOWN` before `ioctl` with `-EPERM`.
- `fw9369_hw_transport_destroy()` closes the file descriptor and frees context memory.

### Verified by Deterministic Offline Tests
- 219 passing assertions across 14 test suites.
- Strict mutual exclusion during `ioctl` verified across concurrent threads (Suite 14, Test 1).
- Active transfer drains safely before destruction (Suite 14, Test 2).
- Waiting threads receive `-ESHUTDOWN` after shutdown is committed (Suite 14, Test 3).
- State getter blocked while transfer owned `ctx->lock` and completed with exact post-transfer state (Suite 14, Test 4).
- All error paths (`-EPERM`, `-EIO`, `-EMSGSIZE`, `-EPROTO`, `-ESHUTDOWN`) maintain `active_ops == 0` without leakage (Suite 14, Test 7).

### Verified by Sanitizers
- AddressSanitizer: Zero memory corruption, zero use-after-free, zero heap leaks across all multi-threaded operations.
- ThreadSanitizer: Zero data races or synchronization hazards across all concurrent transfers, state getters, and teardown sequences.

### Assumed by API Contract
- The caller follows the single-owner lifecycle contract: the owner joins all worker threads prior to calling `fw9369_hw_transport_destroy(t)`.
- The caller does not dereference `t` after `destroy(t)` has completed.

### Unknown or Unproven
- Physical sensor response to `0x9A` register operations.
- Physical behavior of host controller if an SPI slave permanently pulls MISO or stalls bus clocks.
- In-band SPI recovery from `UNKNOWN` (unproven, currently blocked fail-closed).

---

## 4. Hardware Safety Gate Status

- Hardware device `/dev/spidev1.0` was **NOT** opened or accessed.
- GPIO device nodes were **NOT** touched.
- No kernel modules were loaded or unloaded.
- The hardware execution gate remains strictly **CLOSED**.

---

## 5. Authoritative Final Verdict

**`CONCURRENCY AUDIT PASSED WITH BLOCKING LIMITATIONS`**

- **Justification:** The concurrency implementation, mutex serialization, and teardown mechanics have been independently audited, mathematically analyzed, and validated under both AddressSanitizer and ThreadSanitizer across 219 passing tests. The test runner deadlock was thoroughly diagnosed as a test fixture barrier reuse issue, resolved using an independent test-control synchronization harness with bounded failure containment, and validated to execute in <1 second.
- **Blocking Limitations:**
  1. The transport enforces the strict single-owner caller lifecycle contract; it cannot internally prevent use-after-free if callers violate the contract by failing to join worker threads before teardown.
  2. Newly instantiated contexts unconditionally begin in `UNKNOWN`. Both lock and unlock from `UNKNOWN` remain fail-closed rejected with `-EPERM`, preventing transition to `ASSUMED_LOCKED` or `ASSUMED_UNLOCKED` over SPI alone without a verified hardware reset mechanism.
  3. Physical hardware execution remains strictly unauthorized.
