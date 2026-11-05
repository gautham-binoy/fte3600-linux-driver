# Stage D8 — Final Architecture Freeze: Blueprint and Engineering Plan

**Target Device:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Host Controller:** Intel LPSS SPI (`pxa2xx-spi.2` / `idma64`, PCI `0000:00:1e.3`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Stage:** Stage D8 — Final Architecture Freeze  
**Mandate:** Author the definitive, internally consistent architectural freeze covering all driver subsystems before modifying any source code.  
**Safety Gate:** STRICTLY READ-ONLY ARCHITECTURAL DESIGN — ZERO HARDWARE ACCESS AUTHORIZED.

---

## 1. Mission and Engineering Objectives

The objective of Stage D8 is to freeze the complete software and hardware architecture of the `fte3600` Linux kernel driver. No line of implementation source code will be modified until all state machines, session management protocols, locking hierarchies, UAPI data structures, vendor wire protocol commands, error handling paths, and object lifetime rules are unified into an unambiguous, mathematically proven specification.

---

## 2. Architecture Decomposition and Deliverable Map

Stage D8 produces eight definitive architectural specifications in `diagnostics/`:

```
+---------------------------------------------------------------------------------------------------------+
|                                    STAGE D8 ARCHITECTURAL SPECIFICATION MAP                             |
+---+----------------------------------------------------+------------------------------------------------+
| # | Specification Document                             | Subsystem / Engineering Scope                  |
+---+----------------------------------------------------+------------------------------------------------+
| 1 | STAGE_D8_STATE_MACHINE_SPECIFICATION.md            | 14-State Internal Model & Public UAPI Mappings |
| 2 | STAGE_D8_SESSION_MANAGEMENT_SPECIFICATION.md      | Session Tokens, OFD Ownership, Watchdogs, Dup  |
| 3 | STAGE_D8_LOCK_ORDER_AND_CONCURRENCY_SPEC.md       | L1 < L2 < L3 Poset, Atomic Contexts, Deadlocks |
| 4 | STAGE_D8_UAPI_SPECIFICATION.md                     | Struct Alignment, Compat Ioctls, Errno Mapping |
| 5 | STAGE_D8_PROTOCOL_ENGINE_SPECIFICATION.md         | Strobe Framing, FDT Calibration, Baseline Image|
| 6 | STAGE_D8_ERROR_HANDLING_MATRIX.md                  | Complete Errno Invariants across All Call Paths|
| 7 | STAGE_D8_RESOURCE_LIFETIME_AND_TEARDOWN_SPEC.md    | DMA Retention, Worker Safety, Teardown Invars  |
| 8 | STAGE_D8_FINAL_VERDICT.md                          | Formal Stage D8 Freeze Verdict & D9 Gate       |
+---+----------------------------------------------------+------------------------------------------------+
```

---

## 3. Strict Implementation Preconditions for Subsequent Stages

1. **Frozen UAPI Compatibility:** All existing ioctl commands (`GET_STATE`, `RESET`, `INIT`, `CAPTURE`) are preserved. Session-based calibration commands (`SESSION_BEGIN`, `SESSION_FDT_SAMPLE`, `SESSION_READ_FRAME`, `SESSION_SET_THRESHOLDS`, `SESSION_COMMIT`, `SESSION_ABORT`) are layered cleanly into the UAPI without breaking legacy binaries.
2. **Four Mandated Adversarial Constraints:**
   - Worker refcount delta $\equiv 0$.
   - Fail-closed conservative controller identity.
   - Code-enforced table capacity ($N=4$) and fail-closed table exhaustion gating (`-ENOSPC`).
   - Watchdog work cancellation executed *prior* to acquiring `data->lock`.
3. **Zero Hardware Operations:** All testing in subsequent stages (D9–D16) will execute strictly against mock SPI transports, synthetic frames, and fault injection harness.

---

## 4. Execution Schedule

1. Author Specifications 1 through 7.
2. Synthesize findings and render formal Stage D8 Freeze Verdict.
3. Update `PROGRESS.md`.
4. Proceed autonomously to Stage D9 (Core Driver Refactoring).
