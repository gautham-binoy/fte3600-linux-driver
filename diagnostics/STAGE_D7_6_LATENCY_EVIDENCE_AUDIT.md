# Stage D7.6 — Transaction Latency & Empirical Claims Adversarial Audit

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source & Latency Claim Evaluation  
**Deliverable:** `diagnostics/STAGE_D7_6_LATENCY_EVIDENCE_AUDIT.md`  
**Date:** 2026-11-04  

---

## 1. Executive Summary & Objective

In Stage D7.5, the transaction latency for `FTE3600_IOC_CALIB_READ_FRAME` was reported as having an **"empirical worst-case ceiling of $\le 120\text{ ms}$"** and a **"nominal execution latency of ~89 ms."**

This adversarial audit critically investigates the provenance and evidential support for these numbers to determine whether they represent measured hardware benchmarks or analytical models.

### Key Adversarial Findings:
1. **Zero Empirical Measurements Were Conducted:** Physical hardware benchmarks were **never executed** on this Dynabook laptop during any stage (D1–D7.5). The absolute safety boundary strictly prohibited hardware communication, preserving the historical counters at `transfers = 3, bytes = 19`.
2. **Numbers Are Strictly an Analytical Model:** The ~89 ms nominal and 106–120 ms figures were synthesized purely from a theoretical summation of clock arithmetic, sleep durations, and estimated OS scheduling jitter.
3. **Misleading Nomenclature Corrected:** Describing $\le 120\text{ ms}$ as an "empirical ceiling" was inaccurate. It is an **analytical engineering estimate under non-real-time Linux**.
4. **No Real-Time Guarantees Under `PREEMPT_DYNAMIC`:** The host kernel (`Linux 7.2.8-9-omarchy`) uses standard desktop dynamic preemption, not hard real-time `PREEMPT_RT`. Under CPU starvation, disk thrashing, or high interrupt load, latency can exceed 120 ms.
5. **Exact Pure Wire Time Established:** Pure physical SPI wire time at 1.0 MHz is mathematically **82.46 ms**. Everything beyond 82.46 ms is driver settling time and OS overhead.

---

## 2. Deconstruction of the Latency Model

The 6-layer latency calculation presented in Stage D7.5 was composed as follows:

$$\begin{aligned}
T_{\text{nominal}} &= 82.46\text{ ms} + 2.00\text{ ms} + 1.50\text{ ms} + 0.80\text{ ms} + 0.01\text{ ms} + 2.00\text{ ms} = \mathbf{88.77\text{ ms}} \\
T_{\text{worst-case}} &= 82.46\text{ ms} + 2.00\text{ ms} + 4.00\text{ ms} + 2.50\text{ ms} + 0.01\text{ ms} + 15.00\text{ ms} = \mathbf{105.97\text{ ms}}
\end{aligned}$$

### Layer-by-Layer Evidentiary Audit:

| Layer | Component | Claimed Nominal | Claimed Worst-Case | Evidential Basis | Adversarial Classification |
| :---: | :--- | :---: | :---: | :--- | :--- |
| **1** | SPI Wire Time | 82.46 ms | 82.46 ms | Mathematical calculation: $(10246 + 3 + 40)\text{ bytes} \times 8\text{ bits} / 1\text{ MHz}$ | `MATHEMATICALLY-DERIVED` |
| **2** | Hardware Settling Delays | 2.00 ms | 2.00 ms | Two explicit vendor `msleep(1)` statements in `capture.c:268, 277` | `SOURCE-VERIFIED` |
| **3** | Polling Loop Overhead | 1.50 ms | 4.00 ms | Vendor loop `for (poll=0; poll<10; ++poll)` reading register `0x80` | `INFERRED` (Analytical Estimate) |
| **4** | DMA Setup & Completion | 0.80 ms | 2.50 ms | `dma_map_single`, LPSS ring buffer submission, IDMA64 ISR | `INFERRED` (Analytical Estimate) |
| **5** | `copy_to_user` Memory Copy | 0.01 ms | 0.01 ms | 10,240 bytes copied across x86_64 L1/L2 cache (~15 GB/s bandwidth) | `MATHEMATICALLY-DERIVED` |
| **6** | OS Scheduling Jitter | 2.00 ms | 15.00 ms | Timer granularity (CONFIG_HZ=1000) and thread wakeup jitter | `UNVERIFIED` (Heuristic Assumption) |

---

## 3. Pure Wire Time vs. Total Transaction Time

### 3.1. Pure Physical SPI Wire Time
The SPI bus clock frequency is configured at 1,000,000 Hz (1.0 MHz, $1\,\mu\text{s}$ per bit).
- **Trigger Strobe:** 3 bytes = 24 bits = $24\,\mu\text{s} = 0.024\text{ ms}$.
- **Status Read:** 5 bytes = 40 bits = $40\,\mu\text{s} = 0.040\text{ ms}$.
- **Word Config Writes:** $2 \times 8\text{ bytes} = 128\text{ bits} = 0.128\text{ ms}$.
- **FIFO Bulk Read:** 10,246 bytes = 81,968 bits = $81.968\text{ ms}$.
- **Total Physical Wire Time:** **$82.16\text{ ms}$ to $82.50\text{ ms}$** depending on poll count.

### 3.2. Disconnect Between Wire Time and Execution Guarantees
- The hardware cannot transfer the frame in less than **82 ms** due to SPI clock physics.
- The software adds at least **2 ms** of explicit sleeps (`msleep(1)` in `capture.c`).
- In Linux, `msleep(1)` frequently sleeps for 1.5 ms to 2.5 ms due to timer tick quantization ($HZ = 1000$ or timer wheel slack).
- Under `PREEMPT_DYNAMIC`, if a high-priority task preempts the thread or CPU frequency scaling transitions the core, thread scheduling delays can introduce tens of milliseconds of latency.
- Therefore: **$\le 120\text{ ms}$ is a typical engineering expectation under normal desktop operating conditions, but CANNOT be guaranteed as a hard real-time ceiling.**

---

## 4. Architectural Impact on Session Watchdog

Because $\le 120\text{ ms}$ is an analytical expectation rather than a guaranteed bound:
1. **The 250 ms SPI Timeout:** A 250 ms timeout provides a generous $2\times$ margin over the expected 120 ms latency, accommodating transient scheduling spikes without false aborts.
2. **The 5-Second Watchdog Timer:** Even if frame reads experience severe scheduling delays (e.g. 200 ms per frame), an entire multi-frame calibration sequence (such as 10 frames = ~2 seconds) will easily finish within the 5000 ms session window.
3. **No Safety Vulnerability:** The fact that 120 ms is an analytical estimate rather than an empirical benchmark does not compromise hardware safety, because the watchdog deadline is 5000 ms (a $40\times$ safety margin).
