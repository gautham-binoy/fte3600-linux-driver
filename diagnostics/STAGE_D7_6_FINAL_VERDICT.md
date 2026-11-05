# Stage D7.6 — Adversarial Evidence Validation Final Verdict

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source, Kernel Architecture & Concurrency Audit  
**Deliverable:** `diagnostics/STAGE_D7_6_FINAL_VERDICT.md`  
**Date:** 2026-11-04  

---

## 1. Formal Readiness Verdict

$$\mathbf{VERDICT:\quad CONDITIONAL\ PASS}$$

### Final Determination
The Session-Gated Hybrid Calibration Architecture (Design C) is **architecturally viable and structurally sound**. However, because this adversarial audit uncovered four significant technical discrepancies and unsupported assumptions in the Stage D7.5 report, the verdict is **CONDITIONAL PASS**.

The four named conditions detailed below must strictly govern the source-only implementation in Stage D8.

---

## 2. Mandatory Remediation Conditions for Stage D8

| Issue # | Technical Domain | Identified Discrepancy in D7.5 | Mandatory Remediation for Implementation | Severity |
| :---: | :--- | :--- | :--- | :---: |
| **C1** | **SPI Core Timeout** | Claimed the active kernel enforces a 500 ms timeout floor on `spi_sync()`. Proven false: `__spi_sync()` uses unbounded `wait_for_completion(&done)`, and `spi-pxa2xx.c` never calls `spi_controller_xfer_timeout()`. | The driver implementation cannot rely on the SPI core to abort stuck hardware transfers. It must either accept that `spi_sync` blocks indefinitely on bus failure, or implement asynchronous dispatch via `spi_async()` paired with `wait_for_completion_timeout()`. | **MEDIUM** |
| **C2** | **Image Frame Bounds** | Claimed a rigid bound of "4–12 or 13 total frames." Proven false: search loop limit is 30 (`FTE3600_IMAGE_MAX_SEARCH_CAPTURES`), refine captures 0 or 1 frame, and descending crossing executes an extra re-capture. Theoretical source frame count spans **3 to 32 frames**. | Userspace calibration tooling and kernel watchdog budgeting must accommodate up to 32 frames (~3.2 s total execution time) without prematurely aborting or assuming a 10-trial limit. | **LOW** |
| **C3** | **Latency Provenance** | Claimed an "empirical worst-case ceiling of $\le 120\text{ ms}$." Proven false: zero physical measurements were ever taken on this laptop due to the read-only safety boundary. | Driver documentation must classify latency figures as **analytical engineering estimates under standard load**, not empirical benchmarks or hard real-time guarantees. Empirical benchmarking must be deferred to authorized hardware testing. | **LOW** |
| **C4** | **Watchdog & VFS Ownership** | Did not bind session ownership to open-file descriptions, and worker expiry only checked deadline, allowing subtle race conditions across session abort/restarts. | The driver implementation must bind calibration sessions to `struct file *owner_filp` (rejecting foreign `filp` access with `-EACCES`), and the watchdog worker must validate `session->id == work_token` to eliminate stale worker races. | **MEDIUM** |

---

## 3. Evidence Classification Taxonomy

Throughout Stage D7.6, all claims, parameters, and architectural invariants are strictly categorized according to the following evidentiary hierarchy:

| Classification | Definition | Application in D7.6 |
| :--- | :--- | :--- |
| `SOURCE-VERIFIED` | Proven by exact line references in vendor source (`research/libfprint-fte3600/`) or driver (`kernel/`). | FDT 10-sample loop, Channel Mode 1 exclusivity, 16-byte bank layout, reset 20 ms hold. |
| `KERNEL-SOURCE-VERIFIED` | Proven by inspecting active Linux kernel source code (`drivers/spi/`, `include/linux/spi/`). | Unbounded `wait_for_completion` in `__spi_sync`, `spi_map_buf` streaming DMA, LPSS IDMA64 max DMA len. |
| `MATHEMATICALLY-DERIVED` | Formally demonstrated via mathematical, probability, or state transition models. | Consecutive collision probability ($2.33 \times 10^{-10}$), lock hierarchy ($\text{data}\to\text{lock} \prec \text{system\_wq}$). |
| `INFERRED` | Analytical deductions based on source code heuristics or expected algorithmic behavior. | Binary search convergence (~10 trials), latency layer estimates (~89 ms nominal). |
| `UNVERIFIED` | Claims asserted in prior documentation without empirical or source-proven support. | The claimed "empirical 120 ms ceiling" and "500 ms core timeout enforcement". |

---

## 4. Deliverables Index for Stage D7.6

All nine Stage D7.6 adversarial audit deliverables have been authored, peer-checked, and mirrored to the brain artifact directory:

1. [STAGE_D7_6_IMAGE_FRAME_COUNT_AUDIT.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE_D7_6_IMAGE_FRAME_COUNT_AUDIT.md)  
   *Brain Mirror:* [STAGE_D7_6_IMAGE_FRAME_COUNT_AUDIT.md](file:///home/titan/.gemini/antigravity/brain/b45cb0fc-a222-4605-b5ac-d0240eac7c36/STAGE_D7_6_IMAGE_FRAME_COUNT_AUDIT.md)
2. [STAGE_D7_6_FDT_SAMPLING_AND_THRESHOLD_AUDIT.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE_D7_6_FDT_SAMPLING_AND_THRESHOLD_AUDIT.md)  
   *Brain Mirror:* [STAGE_D7_6_FDT_SAMPLING_AND_THRESHOLD_AUDIT.md](file:///home/titan/.gemini/antigravity/brain/b45cb0fc-a222-4605-b5ac-d0240eac7c36/STAGE_D7_6_FDT_SAMPLING_AND_THRESHOLD_AUDIT.md)
3. [STAGE_D7_6_SPI_TIMEOUT_IMPLEMENTATION_AUDIT.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE_D7_6_SPI_TIMEOUT_IMPLEMENTATION_AUDIT.md)  
   *Brain Mirror:* [STAGE_D7_6_SPI_TIMEOUT_IMPLEMENTATION_AUDIT.md](file:///home/titan/.gemini/antigravity/brain/b45cb0fc-a222-4605-b5ac-d0240eac7c36/STAGE_D7_6_SPI_TIMEOUT_IMPLEMENTATION_AUDIT.md)
4. [STAGE_D7_6_LATENCY_EVIDENCE_AUDIT.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE_D7_6_LATENCY_EVIDENCE_AUDIT.md)  
   *Brain Mirror:* [STAGE_D7_6_LATENCY_EVIDENCE_AUDIT.md](file:///home/titan/.gemini/antigravity/brain/b45cb0fc-a222-4605-b5ac-d0240eac7c36/STAGE_D7_6_LATENCY_EVIDENCE_AUDIT.md)
5. [STAGE_D7_6_SPI_DMA_PATH_AUDIT.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE_D7_6_SPI_DMA_PATH_AUDIT.md)  
   *Brain Mirror:* [STAGE_D7_6_SPI_DMA_PATH_AUDIT.md](file:///home/titan/.gemini/antigravity/brain/b45cb0fc-a222-4605-b5ac-d0240eac7c36/STAGE_D7_6_SPI_DMA_PATH_AUDIT.md)
6. [STAGE_D7_6_SESSION_SECURITY_AND_OWNERSHIP_AUDIT.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE_D7_6_SESSION_SECURITY_AND_OWNERSHIP_AUDIT.md)  
   *Brain Mirror:* [STAGE_D7_6_SESSION_SECURITY_AND_OWNERSHIP_AUDIT.md](file:///home/titan/.gemini/antigravity/brain/b45cb0fc-a222-4605-b5ac-d0240eac7c36/STAGE_D7_6_SESSION_SECURITY_AND_OWNERSHIP_AUDIT.md)
7. [STAGE_D7_6_WATCHDOG_AND_LIFETIME_AUDIT.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE_D7_6_WATCHDOG_AND_LIFETIME_AUDIT.md)  
   *Brain Mirror:* [STAGE_D7_6_WATCHDOG_AND_LIFETIME_AUDIT.md](file:///home/titan/.gemini/antigravity/brain/b45cb0fc-a222-4605-b5ac-d0240eac7c36/STAGE_D7_6_WATCHDOG_AND_LIFETIME_AUDIT.md)
8. [STAGE_D7_6_STATE_AND_RECOVERY_AUDIT.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE_D7_6_STATE_AND_RECOVERY_AUDIT.md)  
   *Brain Mirror:* [STAGE_D7_6_STATE_AND_RECOVERY_AUDIT.md](file:///home/titan/.gemini/antigravity/brain/b45cb0fc-a222-4605-b5ac-d0240eac7c36/STAGE_D7_6_STATE_AND_RECOVERY_AUDIT.md)
9. [STAGE_D7_6_FINAL_VERDICT.md](file:///home/titan/fte3600-linux-driver/diagnostics/STAGE_D7_6_FINAL_VERDICT.md)  
   *Brain Mirror:* [STAGE_D7_6_FINAL_VERDICT.md](file:///home/titan/.gemini/antigravity/brain/b45cb0fc-a222-4605-b5ac-d0240eac7c36/STAGE_D7_6_FINAL_VERDICT.md)

---

## 5. Absolute Safety Boundary Compliance Check

At the conclusion of Stage D7.6:
- Physical SPI transfers issued: **0**
- Ioctls executed against `/dev/fte3600`: **0**
- GPIO toggles requested: **0**
- Module load / unload / rebind cycles: **0**
- Hardware safety gate (`bin/fw9369-hw-tool`) touched: **NO**
- Driver status in memory: `fte3600 20480 0 - Live`, state 8 (`IDENTIFIED`), historical SPI counters strictly preserved at `transfers = 3, bytes = 19`. Option 1 is 100% intact.
