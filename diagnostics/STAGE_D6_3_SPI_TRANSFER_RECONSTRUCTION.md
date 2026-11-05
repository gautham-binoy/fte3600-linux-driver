# STAGE D6.3 — INDEPENDENT SPI TRANSFER & WIRE BYTE RECONSTRUCTION

**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Host Architecture:** Intel LPSS SPI2 (`spi-FTE3600:00`, CS0) on Dynabook Satellite Pro C40-J  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Date:** 2026-11-04  
**Author:** Antigravity (Advanced Agentic Pair Programming)  
**Classification:** TRANSFER RECONSTRUCTION & PROTOCOL ACCOUNTING

---

## 1. Executive Summary

This document independently reconstructs every SPI transfer executed across the driver's identification and startup paths. Every transaction is derived directly from the source code in [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c).

The mathematical and transfer metrics are proven:
- **Stage C Identification:** 3 SPI transfers, 19 wire bytes.
- **Stage D6.2 Volatile Startup (Normal Path):** 16 SPI transfers, 66 wire bytes.
- **Stage D6.2 Volatile Startup (Abort Recovery Path):** 18 SPI transfers, 74 wire bytes.
- **Stage D6.2 Volatile Startup (Persistent Failure):** 4 SPI transfers, 16 wire bytes.
- **Full Combined Sequence from Reset:** 20 total hardware operations (1 GPIO reset pulse sequence + 3 SPI identification transfers + 16 SPI startup transfers = 19 SPI transfers, 85 SPI wire bytes).

---

## 2. Stage C Identification Transfer Reconstruction (`fte3600_hw_identify`)

Precondition: Driver state is `FTE3600_STATE_RESET_SEQUENCE_ISSUED`.

| # | Sequence Name | Wire TX Bytes | Wire RX Bytes | Len | CS Continuous? | Source Code Location |
| :-: | :--- | :--- | :--- | :-: | :-: | :--- |
| **C.1** | Wake write `0xC6 = 0x01` | `[ 0x09, 0xF6, 0xC6, 0x01 ]` | `NULL` | 4 B | YES (`t.cs_change=0`) | [`fte3600.c:361-365`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L361-L365) |
| --- | *Stabilization Delay* | *None (4 ms sleep)* | *None* | 0 B | *CS De-asserted HIGH* | [`fte3600.c:372`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L372) |
| **C.2** | Wake readback `0xC6` | `[ 0x08, 0xF7, 0xC6, 0x00, 0x00 ]` | `[ x, x, x, x, 0x01 ]` | 5 B | YES (`t.cs_change=0`) | [`fte3600.c:375-381`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L375-L381) |
| **C.3** | Interrogate ID `0x1A8B` | `[ 0x04, 0xFB, 0x9A, 0x8B, 0, 0, 0, 0, 0, 0 ]` | `[ x, x, x, x, x, x, 0x93, 0x62, 0x1C, 0x53 ]` | 10 B | YES (`t.cs_change=0`) | [`fte3600.c:397-403`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L397-L403) |

**Stage C Totals:**
- SPI Transfers: $1 + 1 + 1 = 3\text{ transfers}$ `[SOURCE-OBSERVED]`, `[HARDWARE-VERIFIED]`
- Wire Bytes: $4 + 5 + 10 = 19\text{ bytes}$ `[SOURCE-OBSERVED]`, `[HARDWARE-VERIFIED]`
- Hardware Counter Verification: Observed on physical hardware at `/sys/bus/spi/devices/spi-FTE3600:00/statistics/`: `transfers = 3`, `bytes = 19`.

---

## 3. Stage D6.2 Volatile Startup: Baseline Normal Path (16 Transfers, 66 Wire Bytes)

Precondition: Driver state is `FTE3600_STATE_IDENTIFIED`.

| # | Step | Operation | Wire TX Bytes | Wire RX Bytes | Len | CS Continuous? | Source Code Location |
| :-: | :-: | :--- | :--- | :--- | :-: | :-: | :--- |
| **1** | Step 1 | Wake strobe 1 (`CMD_WAKE`) | `[ 0x5A, 0xA5, 0x00 ]` | `NULL` | 3 B | YES (`cs_change=0`) | [`fte3600.c:496`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L496), [`L243-248`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L243-L248) |
| --- | Step 2 | *Stabilization Delay* | *None (1 ms sleep)* | *None* | 0 B | *CS De-asserted* | [`fte3600.c:503`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L503) |
| **2** | Step 3 | Read HW_STATUS `0x80` | `[ 0x08, 0xF7, 0x80, 0x00, 0x00 ]` | `[ x, x, x, x, 0x50 ]` | 5 B | YES (`cs_change=0`) | [`fte3600.c:507`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L507), [`L260-270`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L260-L270) |
| **3** | Step 4 | Wake strobe 2 (`CMD_IDLE_ACK`) | `[ 0xA5, 0x5A, 0x00 ]` | `NULL` | 3 B | YES (`cs_change=0`) | [`fte3600.c:531`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L531), [`L243-248`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L243-L248) |
| **4** | Step 5.0a | Write SFR `0xF1 = 0x03` | `[ 0x09, 0xF6, 0xF1, 0x03 ]` | `NULL` | 4 B | YES (`cs_change=0`) | [`fte3600.c:541`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L541), [`L251-257`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L251-L257) |
| **5** | Step 5.0b | Write SFR `0xF4 = 0xC0` | `[ 0x09, 0xF6, 0xF4, 0xC0 ]` | `NULL` | 4 B | YES (`cs_change=0`) | [`fte3600.c:544`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L544), [`L251-257`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L251-L257) |
| **6** | Step 5.0c | Write SFR `0xF4 = 0xC1` | `[ 0x09, 0xF6, 0xF4, 0xC1 ]` | `NULL` | 4 B | YES (`cs_change=0`) | [`fte3600.c:547`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L547), [`L251-257`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L251-L257) |
| **7** | Step 5.0d | Write SFR `0xF4 = 0xC0` | `[ 0x09, 0xF6, 0xF4, 0xC0 ]` | `NULL` | 4 B | YES (`cs_change=0`) | [`fte3600.c:550`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L550), [`L251-257`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L251-L257) |
| **8** | Step 5.0e | Write SFR `0xF4 = 0xC0` | `[ 0x09, 0xF6, 0xF4, 0xC0 ]` | `NULL` | 4 B | YES (`cs_change=0`) | [`fte3600.c:553`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L553), [`L251-257`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L251-L257) |
| **9** | Step 5.0f | Read SFR `0xF3` (dummy) | `[ 0x08, 0xF7, 0xF3, 0x00, 0x00 ]` | `[ x, x, x, x, dummy ]` | 5 B | YES (`cs_change=0`) | [`fte3600.c:556`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L556), [`L260-270`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L260-L270) |
| **10** | Step 5.1a | Write SFR `0xF1 = 0x13` | `[ 0x09, 0xF6, 0xF1, 0x13 ]` | `NULL` | 4 B | YES (`cs_change=0`) | [`fte3600.c:541`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L541), [`L251-257`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L251-L257) |
| **11** | Step 5.1b | Write SFR `0xF4 = 0xC0` | `[ 0x09, 0xF6, 0xF4, 0xC0 ]` | `NULL` | 4 B | YES (`cs_change=0`) | [`fte3600.c:544`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L544), [`L251-257`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L251-L257) |
| **12** | Step 5.1c | Write SFR `0xF4 = 0xC1` | `[ 0x09, 0xF6, 0xF4, 0xC1 ]` | `NULL` | 4 B | YES (`cs_change=0`) | [`fte3600.c:547`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L547), [`L251-257`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L251-L257) |
| **13** | Step 5.1d | Write SFR `0xF4 = 0xC0` | `[ 0x09, 0xF6, 0xF4, 0xC0 ]` | `NULL` | 4 B | YES (`cs_change=0`) | [`fte3600.c:550`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L550), [`L251-257`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L251-L257) |
| **14** | Step 5.1e | Write SFR `0xF4 = 0xC0` | `[ 0x09, 0xF6, 0xF4, 0xC0 ]` | `NULL` | 4 B | YES (`cs_change=0`) | [`fte3600.c:553`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L553), [`L251-257`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L251-L257) |
| **15** | Step 5.1f | Read SFR `0xF3` (dummy) | `[ 0x08, 0xF7, 0xF3, 0x00, 0x00 ]` | `[ x, x, x, x, dummy ]` | 5 B | YES (`cs_change=0`) | [`fte3600.c:556`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L556), [`L260-270`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L260-L270) |
| **16** | Step 6 | Read SFR `0x9B` (Fab Code) | `[ 0x08, 0xF7, 0x9B, 0x00, 0x00 ]` | `[ x, x, x, x, fab ]` | 5 B | YES (`cs_change=0`) | [`fte3600.c:564`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L564), [`L260-270`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L260-L270) |

### Mathematical Verification:
- **Total Transfers:** $1\text{ (wake1)} + 1\text{ (st80)} + 1\text{ (wake2)} + 6\text{ (loop0)} + 6\text{ (loop1)} + 1\text{ (fab)} = \mathbf{16\text{ transfers}}$.
- **Total Wire Bytes:**
  $$\begin{aligned}
  \text{Bytes} &= 3\text{ (wake1)} + 5\text{ (st80)} + 3\text{ (wake2)} + [4+4+4+4+4+5]\text{ (loop0)} + [4+4+4+4+4+5]\text{ (loop1)} + 5\text{ (fab)} \\
  &= 3 + 5 + 3 + 25 + 25 + 5 \\
  &= \mathbf{66\text{ wire bytes}}.
  \end{aligned}$$
`[SOURCE-OBSERVED]`, `[VERIFIED]`

---

## 4. Stage D6.2 Volatile Startup: Abort Recovery Path (18 Transfers, 74 Wire Bytes)

If Transfer #2 (Read `0x80`) returns `status_80 != 0x50`:

| # | Step | Operation | Wire TX Bytes | Wire RX Bytes | Len | Source Code Location |
| :-: | :-: | :--- | :--- | :--- | :-: | :--- |
| **1** | Step 1 | Wake strobe 1 (`CMD_WAKE`) | `[ 0x5A, 0xA5, 0x00 ]` | `NULL` | 3 B | [`fte3600.c:496`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L496) |
| **2** | Step 3a | Read HW_STATUS `0x80` (not 0x50) | `[ 0x08, 0xF7, 0x80, 0x00, 0x00 ]` | `[ x, x, x, x, 0xXX ]` | 5 B | [`fte3600.c:507`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L507) |
| **2A** | Step 3b | **Abort strobe (`CMD_ABORT`)** | `[ 0xC0, 0x3F, 0x00 ]` | `NULL` | 3 B | [`fte3600.c:516`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L516) |
| --- | Step 3c | *Stabilization Delay* | *None (1 ms sleep)* | *None* | 0 B | [`fte3600.c:519`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L519) |
| **2B** | Step 3d | **Retry Read HW_STATUS `0x80`** | `[ 0x08, 0xF7, 0x80, 0x00, 0x00 ]` | `[ x, x, x, x, 0x50 ]` | 5 B | [`fte3600.c:520`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L520) |
| **3** | Step 4 | Wake strobe 2 (`CMD_IDLE_ACK`) | `[ 0xA5, 0x5A, 0x00 ]` | `NULL` | 3 B | [`fte3600.c:531`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L531) |
| **4–15** | Step 5 | Foundry loop (12 transfers) | *(Same as normal path)* | *(Same as normal path)* | 50 B | [`fte3600.c:538-559`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L538-L559) |
| **16** | Step 6 | Read Fab Register `0x9B` | `[ 0x08, 0xF7, 0x9B, 0x00, 0x00 ]` | `[ x, x, x, x, fab ]` | 5 B | [`fte3600.c:564`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L564) |

### Mathematical Verification:
- **Total Transfers:** $16 + 2\text{ (abort strobe + retry read)} = \mathbf{18\text{ transfers}}$.
- **Total Wire Bytes:** $66 + 3\text{ (abort strobe)} + 5\text{ (retry read)} = \mathbf{74\text{ wire bytes}}$.
`[SOURCE-OBSERVED]`, `[VERIFIED]`

---

## 5. Combined Lifecycle Operation Accounting (20 Operations)

The term **"20 combined operations"** is defined precisely as:
The sequence of discrete hardware-affecting actions dispatched by the driver when userspace issues:
1. `FTE3600_IOC_RESET` (invokes `fte3600_hw_reset()`)
2. `FTE3600_IOC_INIT` with `flags = FTE3600_INIT_FLAG_FULL_STARTUP` (invokes `fte3600_hw_identify()` followed by `fte3600_hw_startup()`).

### Operational Breakdown:
1. **Operation 1:** GPIO Hardware Reset Pulse Sequence:
   - 40 ms active-low hardware pulse (`msleep(10) -> gpiod_set_value_cansleep(data->reset_gpiod, 1) -> msleep(20) -> gpiod_set_value_cansleep(..., 0) -> msleep(10)`).
   - Category: Non-SPI GPIO operation. Count = 1.
2. **Operations 2–4:** Stage C Identification Sequence:
   - Operation 2 (SPI Transfer C.1): Write `0xC6 = 0x01` (4 bytes).
   - Operation 3 (SPI Transfer C.2): Read back `0xC6` (5 bytes).
   - Operation 4 (SPI Transfer C.3): Read Chip ID `0x1A8B` (10 bytes).
   - Category: SPI Transfers. Count = 3 transfers, 19 wire bytes.
3. **Operations 5–20:** Stage D6.2 Volatile Startup Sequence:
   - Operation 5 (SPI Transfer 1): Wake strobe 1 (3 bytes).
   - Operation 6 (SPI Transfer 2): Read HW_STATUS `0x80` (5 bytes).
   - Operation 7 (SPI Transfer 3): Wake strobe 2 (3 bytes).
   - Operations 8–19 (SPI Transfers 4–15): Foundry interrogation loop (12 transfers, 50 bytes).
   - Operation 20 (SPI Transfer 16): Read Silicon Fab Register `0x9B` (5 bytes).
   - Category: SPI Transfers. Count = 16 transfers, 66 wire bytes.

### Grand Totals:
- **Total Hardware Operations:** $1\text{ (GPIO reset)} + 3\text{ (SPI identify)} + 16\text{ (SPI startup)} = \mathbf{20\text{ operations}}$.
- **Total SPI Transfers:** $3 + 16 = \mathbf{19\text{ SPI transfers}}$.
- **Total SPI Wire Bytes:** $19 + 66 = \mathbf{85\text{ wire bytes}}$.

**Verification Status:** Confirmed by `test_d6_startup_scenario_10_full_startup_flag_combined` in `kernel/fte3600-test.c:2171-2180`, which strictly asserts `ctx.hardware_access_count == 20`. `[SOURCE-OBSERVED]`, `[VERIFIED]`
