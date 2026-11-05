# Stage D7.9-R6 — Final Adversarial Audit Verdict and Stage D8 Implementation Authorization

**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Host Controller:** Intel LPSS SPI (`pxa2xx-spi.2` / `idma64`, PCI `0000:00:1e.3`)  
**Audit Scope:** Independent Adversarial Verification of Stage D7.9-R5 Claims and Deliverables  
**Final Audit Verdict:** **CONDITIONAL PASS (Approved for Bounded Source Implementation under 4 Mandated Constraints)**

---

## 1. Final Verdict Determination

### Verdict: **CONDITIONAL PASS**

The architecture designed across Stages D7.7 through D7.9-R5 is **suitable for bounded source-only implementation in Stage D8**, subject to **four strictly mandated adversarial constraints**. 

R5's unconditional PASS verdict cannot be sustained in its original form because four critical claims were weakened or refuted during adversarial scrutiny:
1. **Worker Module-Put:** Calling `module_put(THIS_MODULE)` from a worker is fatally flawed; worker refcount contribution must be strictly zero.
2. **Controller Identity:** R5's claim of "zero false positives/negatives" was refuted due to peripheral HID conflation and volatile MMIO BARs; replaced with a fail-closed conservative isolation model.
3. **Memory Bounds:** R5's 33,920-byte bound was an unproven inference from machine inventory; code-enforced bound is 67,840 bytes, strictly conditioned on compile-time table capacity and fail-closed exhaustion gating.
4. **Workqueue Cancellation Lock Order:** Discovered a deadlock hazard between `cancel_delayed_work_sync(&data->watchdog_work)` and `data->lock` in the unbind path; resolved by reordering cancellation outside the lock.

Because none of these flaws permit unsafe buffer reclamation or hardware domain reuse under the corrected contract, **Stage D8 Source-Only Implementation is AUTHORIZED under these four constraints.**

---

## 2. Re-evaluation of Stage D7.9-R5 Claims

```
+---------------------------------------------------------------------------------------------------------+
|                                    STAGE D7.9-R5 CLAIM EVALUATION MATRIX                                |
+------------------------------+--------------------+-----------------------------------------------------+
| Stage D7.9-R5 Claim          | R6 Audit Status    | Adversarial Evaluation & Source Reality             |
+------------------------------+--------------------+-----------------------------------------------------+
| 1. Worker module_put() safe  | REFUTED / FLAWED   | try_stop_module() succeeds instantly on refcount 0; |
|    via destroy_workqueue()   |                    | mod->exit() starts BEFORE worker returns.           |
|                              |                    | Rule: Worker Refcount Contribution must be ZERO.    |
+------------------------------+--------------------+-----------------------------------------------------+
| 2. Quarantine reference      | CONFIRMED PROVEN   | try_stop_module() returns -EWOULDBLOCK because      |
|    conversion immune to GOING|                    | refcount is >= 2 during active transactions.        |
+------------------------------+--------------------+-----------------------------------------------------+
| 3. Zero false +/- controller | REFUTED / WEAKENED | ACPI FTE3600 belongs to child sensor, not host ctlr.|
|    hardware fingerprint      |                    | BARs volatile. Replaced with fail-closed strategy.  |
+------------------------------+--------------------+-----------------------------------------------------+
| 4. Global memory ceiling of  | WEAKENED           | 33,920 B is machine-specific inventory inference.   |
|    33,920 bytes              |                    | Code bound is 67,840 B (N=4), gated by -ENOSPC.     |
+------------------------------+--------------------+-----------------------------------------------------+
| 5. Five-state machine with   | CONFIRMED PROVEN   | u64 generation counter mathematically guarantees    |
|    generation counters       |                    | that callback G cannot corrupt generation G+1.      |
+------------------------------+--------------------+-----------------------------------------------------+
| 6. DAG Lock hierarchy        | CONFIRMED PROVEN   | L1 (mutex) < L2 (domain spinlock) < L3 (queue lock) |
|    L1 < L2 < L3 acyclic      | (WITH FIX)         | is acyclic. Watchdog cancel reordered outside L1.   |
+------------------------------+--------------------+-----------------------------------------------------+
| 7. DMA retention invariant   | CONFIRMED PROVEN   | idma64 cannot prove silicon quiescence.             |
|    FREED(t) == 0             |                    | Permanent DRAM retention re-affirmed 100%.          |
+------------------------------+--------------------+-----------------------------------------------------+
```

---

## 3. Four Mandated Adversarial Design Constraints for Stage D8

To proceed with source code implementation in Stage D8, `kernel/fte3600_core.c` and `kernel/fte3600.h` must strictly adhere to the following four implementation contracts:

### Constraint 1: Strict Zero-Refcount Deferred Worker
- **Mandate:** `fte3600_xfer_release_work()` must **NEVER invoke `module_put(THIS_MODULE)`**.
- **Mechanism:** The worker exclusively frees SLUB allocations (`kfree(rx_buf)`, `kfree(tx_buf)`, `kfree(xfer)`) and puts the `spi_device` (`spi_dev_put(spi)`). All module reference manipulation is restricted to process-context ioctls and VFS open/close.

### Constraint 2: Fail-Closed Conservative Controller Identity
- **Mandate:** The controller matching algorithm must **strip child peripheral HID (`FTE3600`)** and **strip volatile MMIO BAR base addresses** from equality predicates.
- **Mechanism:** Matching is keyed strictly by host PCI Vendor ID (`0x8086`), Device ID (`0xa0ab`), and BDF (`0000:00:1e.3`). If physical identity cannot be verified with certainty, the driver must fail closed (treat domain as quarantined).

### Constraint 3: Code-Enforced Table-Exhaustion Gating
- **Mandate:** The domain table must have a compile-time fixed capacity (`FTE3600_MAX_DOMAINS 4`).
- **Mechanism:** `fte3600_probe()` must check `fte3600_domain_count >= FTE3600_MAX_DOMAINS` under spinlock. If exhausted, probe must **abort with `-ENOSPC`**. No untracked controller may ever issue transactions. The code-enforced global memory ceiling is strictly **67,840 bytes**.

### Constraint 4: Watchdog Cancellation Deadlock Remediation
- **Mandate:** In `fte3600_spi_remove()`, `cancel_delayed_work_sync(&data->watchdog_work)` must be invoked **PRIOR to taking `data->lock`**.
- **Mechanism:** Prevents self-deadlock between unbind thread holding `data->lock` and watchdog worker blocking on `data->lock`.

---

## 4. Stage D8 Authorization Statement

Stage D8 (Source-Only Implementation) is **APPROVED TO PROCEED**, restricted exclusively to implementing the driver source code, headers, and mock unit tests in accordance with the Four Mandated Constraints.

**Zero Hardware Authorization is Granted for Stage D8.**

---

## 5. Explicit Statement of Zero Hardware Operations

Throughout the entirety of Stage D7.9-R6:
- **Zero driver modules were loaded, inserted, reloaded, or bound.**
- **Zero SPI bus transfers or sensor register accesses were executed.**
- **Zero hardware commands (RESET, INIT, CAPTURE, CALIBRATION, OTP, EFUSE, FLASH) were issued.**
- **Zero GPIOs, pinctrl, PADCFG, or controller configurations were modified.**
- **Zero changes were made to the legacy userspace safety gate.**
- **The passive system state remains 100% verified (`fte3600: NOT LOADED`, `/dev/fte3600: ABSENT`, SPI `transfers: 0`, `bytes: 0`).**
