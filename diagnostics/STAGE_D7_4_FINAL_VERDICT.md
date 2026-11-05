# Stage D7.4 — Final Architectural Audit Verdict & Evidence Classification

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source & Design Review  
**Deliverable:** `diagnostics/STAGE_D7_4_FINAL_VERDICT.md`

---

## 1. Formal Architectural Verdict

### Final Classification: **CONDITIONAL PASS**

The hybrid calibration architecture (Design C) evaluated in Stage D7.3 is fundamentally sound, robust, and verified to be far superior to monolithic in-kernel calibration (Design A) and stateless bounded operations (Design B).

However, this independent audit identified **four critical design discrepancies and synchronization hazards** in the Stage D7.3 proposals that must be formally corrected in the implementation specifications before Stage D8 implementation can commence:

1. **Condition 1 (Arm & Strobe Protocol Correction):**  
   The proposed 1-byte command strobes (`0xA2` for arm, `0xA0` for capture) have zero source support and violate hardware framing rules. The driver implementation in Stage D8 must emit strictly 3-byte inverted command packets: `{0xC2, 0x3D, 0x00}` for touch arming and `{0xC4, 0x3B, 0x00}` for image exposure, embedded within the full 8-step vendor register sequence (`fw9369_startup.c:385-421`).
2. **Condition 2 (Transient SPI DMA Buffer Accounting):**  
   Stage D7.3's claim of "zero in-kernel buffer memory" is qualified: while persistent multi-frame calibration tables in kernel memory are strictly eliminated, the Linux SPI core requires a transient, DMA-safe kernel RX buffer of $10,246\text{ bytes}$ (`FTE3600_FIFO_TRANSFER_LEN`) preallocated at driver probe time.
3. **Condition 3 (Deadlock-Free Watchdog Concurrency Architecture):**  
   The watchdog timer must not use raw softirq `timer_list` (which cannot acquire sleepable mutexes) and must not call `cancel_delayed_work_sync()` while holding `data->lock` (which causes a fatal ABBA deadlock). Stage D8 must implement the passive jiffies deadline check inside `data->lock` combined with process-context `delayed_work` on `system_wq`.
4. **Condition 4 (Realistic Latency Bound & Bus Timeout):**  
   The strict "86 ms bound" is an idealized calculation. Real-world execution under Linux DMA setup, 2 explicit $1\text{ ms}$ waits, and a 10-iteration status polling loop has a worst-case duration of $\le 120\text{ ms}$. The driver SPI transfer timeout must be set to $250\text{ ms}$.

---

## 2. Deliverable Synthesis

The findings of Stage D7.4 are documented across 9 dedicated technical deliverables:

| Deliverable File | Core Focus | Key Audit Finding |
| :--- | :--- | :--- |
| `STAGE_D7_4_ARM_COMMAND_AUDIT.md` | Command Strobe Verification | Refuted `0xA2` / `0xA0` / `0xA3`. Proved arm strobe is strictly `{0xC2, 0x3D, 0x00}` and capture strobe is `{0xC4, 0x3B, 0x00}` following `[CMD, ~CMD, 0x00]`. |
| `STAGE_D7_4_SESSION_LIFETIME_AUDIT.md` | VFS & Process Lifetime | Proved `cdev_release()` does not run on parent exit if child retains inherited fd; proved watchdog is mathematically mandatory. |
| `STAGE_D7_4_WATCHDOG_CONCURRENCY_AUDIT.md`| Concurrency & Deadlocks | Discovered and eliminated the ABBA deadlock between `cancel_delayed_work_sync` and `data->lock` using passive jiffies deadlines. |
| `STAGE_D7_4_FRAME_TRANSFER_ABI_AUDIT.md` | 32-Byte ABI & Memory | Qualified buffer accounting (10,246 B transient DMA buffer required). Validated `__u64` 64-bit alignment and `compat_ptr`. |
| `STAGE_D7_4_TRANSACTION_TIMING_AUDIT.md`| Execution Latency | Recalculated transaction timing: pure wire time is 81.97 ms, realistic worst-case is $\le 120$ ms, driver timeout set to 250 ms. |
| `STAGE_D7_4_ABORT_AND_RECOVERY_AUDIT.md` | Abort & Recovery Semantics | Proved ABORT performs software-only cleanup (zero SPI traffic); physical hardware recovery strictly enforced via `FTE3600_IOC_RESET`. |
| `STAGE_D7_4_READY_AND_COMMIT_AUDIT.md` | READY State Gating | Formulated 6-bit kernel progress bitmask. Proved READY cannot be claimed by userspace and preserved `-EOPNOTSUPP` capture stub. |
| `STAGE_D7_4_UAPI_COMPATIBILITY_AUDIT.md` | IOCTL Numbering & Encoding | Verified command range `0x20`–`0x25` is collision-free, all structs strictly 32 bytes, with zero-reserved defensive checking. |
| `STAGE_D7_4_FINAL_VERDICT.md` | Verdict & Taxonomy | Formal CONDITIONAL PASS verdict, synthesized conditions, and evidence taxonomy. |

---

## 3. Evidence Classification Taxonomy

| Statement / Claim | Evidence Classification | Primary Verification Source |
| :--- | :--- | :--- |
| Exclusive single-client open lease | `SOURCE-VERIFIED` | `kernel/fte3600.c:647` |
| Driver state serialization under `data->lock` | `SOURCE-VERIFIED` | `kernel/fte3600.c:709` |
| `INIT` strictly gated from states 2 and 8 | `SOURCE-VERIFIED` | `kernel/fte3600.c:734-742` |
| `CAPTURE` strictly gated behind state 4 (`READY`) | `SOURCE-VERIFIED` | `kernel/fte3600.c:766-770` |
| Capture stub explicitly returns `-EOPNOTSUPP` | `SOURCE-VERIFIED` | `kernel/fte3600.c:772-776` |
| Touch arm strobe is strictly `{0xC2, 0x3D, 0x00}` | `VENDOR-SOURCE-VERIFIED` | `fw9369_startup.c:417`, `fw9369_release.c:279` |
| Image capture strobe is strictly `{0xC4, 0x3B, 0x00}` | `VENDOR-SOURCE-VERIFIED` | `capture.c:267` |
| Strobe commands follow `[CMD, ~CMD, 0x00]` invariant | `MATHEMATICALLY-DERIVED` | Formally proven across all 5 vendor command opcodes |
| Single-byte strobes `0xA2` / `0xA0` have zero source support | `SOURCE-VERIFIED` | Zero call sites in vendor codebase |
| `cdev_release()` does not run if child inherits fd | `MATHEMATICALLY-DERIVED` | Linux kernel VFS `f_count` reference accounting |
| ABBA deadlock between `cancel_delayed_work_sync` and mutex | `MATHEMATICALLY-DERIVED` | Formally proven via lock graph analysis |
| Passive jiffies deadline check is deadlock-free | `ARCHITECTURAL-PROPOSAL` | Specified in `STAGE_D7_4_WATCHDOG_CONCURRENCY_AUDIT.md` |
| Transient 10,246 B DMA-safe buffer mandatory for SPI core | `SOURCE-VERIFIED` | Linux kernel SPI core (`drivers/spi/spi.c:spi_sync()`) |
| FIFO wire time at 1 MHz is 81.968 ms | `MATHEMATICALLY-DERIVED` | Calculated via $\frac{10246 \times 8}{10^6}$ |
| Realistic worst-case transaction latency is $\le 120$ ms | `MATHEMATICALLY-DERIVED` | Calculated via wire time + waits + polling + DMA jitter |
| Proposed 32-byte UAPI structures (`0x20`–`0x25`) | `ARCHITECTURAL-PROPOSAL` | Specified in `STAGE_D7_4_UAPI_COMPATIBILITY_AUDIT.md` |
| Host kernel: Linux 7.2.8-9-omarchy, FT9362 on `spi-FTE3600:00` | `HOST-OBSERVED` | Direct sysfs/procfs inspection |

---

## 4. Preservation of Hardware Safety Invariants

- **Kernel Module State:** `fte3600.ko` Live (`20480 0`), Option 1 intact.
- **Hardware SPI Transfer Statistics:** `transfers = 3`, `bytes = 19` (100% untouched; exactly zero SPI commands transmitted).
- **Character Device:** `/dev/fte3600` major `511`, minor `0` (0 active leases).
- **Physical Safety Gate:** Active and unmodified.
- **Hardware Execution Authorization:** None granted.
