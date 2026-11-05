# Stage D7.6 — Exact Image-Baseline Frame Count Adversarial Audit

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source Code Audit  
**Deliverable:** `diagnostics/STAGE_D7_6_IMAGE_FRAME_COUNT_AUDIT.md`  
**Date:** 2026-11-04  

---

## 1. Executive Summary & Objective

In Stage D7.5, the frame acquisition count for cold image baseline calibration was reported as "4–12 total frames in one section, up to 13 total frames in another," broken down into 2–10 coarse trial frames, 1 fine trial frame, and 1 baseline frame.

This adversarial audit traces the complete vendor call graph in the reference implementation ([`research/libfprint-fte3600/libfprint/drivers/fte3600/`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/)) to establish the exact, source-proven frame counts, loop limits, early exits, and edge cases.

### Key Adversarial Findings:
1. **Loop Limit is 30, Not 10:** The constant governing the search loop is `FTE3600_IMAGE_MAX_SEARCH_CAPTURES = 30` ([`include/fte3600_image.h:16`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/include/fte3600_image.h#L16)), not 10. The figure "10" was an algorithmic convergence heuristic ($\log_2(128) \approx 7$ plus local steps), not a hard source constraint.
2. **Minimum Successful Frames is 3, Not 4:** If the median crossing occurs on trial 1 and `fte3600_image_refine()` finds balanced edge counts, refine exits immediately with zero captures. Total frames: 2 search + 0 refine + 1 baseline = **3 frames**.
3. **The "13th Frame" Mechanism Uncovered:** In `image.c:109-116`, when a descending DAC crossing occurs (`previous_dac == state->dac + 1`), the search routine performs an **explicit re-capture** at `previous_dac` before invoking `fte3600_image_refine()`.
4. **Theoretical Maximum Count is 32 Frames:** In worst-case source execution where search converges at trial 28, plus 1 descending re-capture, plus 1 refinement capture, plus 1 baseline capture, total frames can reach **32 frames**.

---

## 2. Complete Call Graph & Code Provenance

The call graph for automated cold image baseline generation is:

```
image_baseline()                                [fw9369_startup.c:264]
  |
  +--> fte3600_image_search()                   [image.c:76]
  |      |
  |      +--> capture()                         [fw9369_startup.c:201]  (Loop: 1..30 trials)
  |      |      +--> fte3600_capture_frames()   [capture.c:285]
  |      |             +--> fifo()              [capture.c:186]         (10,246 B SPI transfer)
  |      |
  |      +--> fte3600_image_measure()           [image.c:7]             (Calculates median & edge counts)
  |      |
  |      +--> [Conditional Re-capture]          [image.c:114]           (If descending crossing)
  |      |      +--> capture()
  |      |
  |      +--> fte3600_image_refine()            [image.c:46]
  |             +--> capture()                  [image.c:64]            (Conditional: 0 or 1 capture)
  |
  +--> capture()                                [fw9369_startup.c:287]  (Final baseline frame)
  |
  +--> memcpy(s->baseline, s->work_image)       [fw9369_startup.c:298]  (10,240 B matrix stored)
```

---

## 3. Detailed Source Examination

### 3.1. Search Loop Bounds: `image.c:93-120`
```c
/* research/libfprint-fte3600/libfprint/drivers/fte3600/image.c:93-120 */
for (unsigned int trial = 0; trial < FTE3600_IMAGE_MAX_SEARCH_CAPTURES; ++trial)
  {
    struct fte3600_image_statistics observed;
    unsigned int median;
    if (capture (context, state->dac, pixels) != 0)
      return FTE3600_IMAGE_CAPTURE_ERROR;
    (void) fte3600_image_measure (pixels, &observed);
    median = observed.median;

    /* Widen before comparing adjacency: 255 and 0 are not adjacent. */
    if (median > 512 && previous_median <= 512 &&
        (unsigned int) state->dac == (unsigned int) previous_dac + 1)
      {
        state->stored_median = observed.median;
        return fte3600_image_refine (state, pixels, capture, context);
      }
    if (median <= 512 && previous_median > 512 &&
        (unsigned int) previous_dac == (unsigned int) state->dac + 1)
      {
        state->dac = previous_dac;
        state->stored_median = previous_median;
        if (capture (context, state->dac, pixels) != 0)
          return FTE3600_IMAGE_CAPTURE_ERROR;
        return fte3600_image_refine (state, pixels, capture, context);
      }
    if (trial + 1 == FTE3600_IMAGE_MAX_SEARCH_CAPTURES)
      return FTE3600_IMAGE_FAILED;
```
- Constant definition: `#define FTE3600_IMAGE_MAX_SEARCH_CAPTURES 30` in [`include/fte3600_image.h:16`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/include/fte3600_image.h#L16).
- **Trial 0 Invariant:** On trial 0, `previous_dac` equals `state->dac` (line 91), and `previous_median` is 0. Neither crossing condition can be satisfied. Thus, trial 0 **never terminates**. Minimum search trials is strictly 2.
- **Descending Crossing Re-capture (The 13th Frame Origin):** Look at lines 109–116: when `previous_dac == state->dac + 1`, the code restores `state->dac = previous_dac;` and immediately invokes `capture(context, state->dac, pixels)` before calling `fte3600_image_refine()`. This adds **1 additional frame** to the search phase.

### 3.2. Refinement Phase: `image.c:46-73`
```c
/* research/libfprint-fte3600/libfprint/drivers/fte3600/image.c:56-65 */
(void) fte3600_image_measure (pixels, &before);
original_dac = state->dac;
if (before.low_count < 20 && before.high_count > 20)
  state->dac = (uint8_t) (state->dac - 1);
else if (before.low_count > 20 && before.high_count < 20)
  state->dac = (uint8_t) (state->dac + 1);
else
  return FTE3600_IMAGE_OK; /* ZERO EXTRA CAPTURES */
if (capture (context, state->dac, pixels) != 0)
  return FTE3600_IMAGE_CAPTURE_ERROR;
```
- If edge counts are balanced (`low_count >= 20` and `high_count >= 20`, or neither condition holds), line 63 returns `FTE3600_IMAGE_OK` without capturing! Refine captures **0 or 1 frame**.

### 3.3. Baseline Acquisition: `fw9369_startup.c:287-295`
```c
/* research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c:287-295 */
if (capture (op, s->image_dac, s->work_image))
  return op->error;
for (unsigned int i = 1; i < s->image_count; ++i)
  {
    if (capture (op, s->image_dac, op->w->image))
      return op->error;
    ...
  }
```
- In `fw9369_startup.c:381`, `op->s->image_count = 1;`. Thus the loop `i = 1; i < 1` does not execute. Exactly **1 baseline frame** is captured.
- Notice: `image_baseline()` in vendor source contains **no discard frame**. The concept of a discard frame was introduced in Design C's modern calibration protocol to purge analog sensor residue before acquiring the dark reference.

---

## 4. Comprehensive Frame Execution Matrix

| Execution Scenario | Search Trials | Descending Re-capture | Refinement Captures | Baseline Captures | Total Frames | Evidence Classification |
| :--- | :---: | :---: | :---: | :---: | :---: | :--- |
| **Minimum Successful (Ascending, Balanced)** | 2 | 0 | 0 | 1 | **3** | `SOURCE-VERIFIED` |
| **Minimum Successful (Ascending, Refined)** | 2 | 0 | 1 | 1 | **4** | `SOURCE-VERIFIED` |
| **Minimum Successful (Descending, Refined)** | 2 | 1 | 1 | 1 | **5** | `SOURCE-VERIFIED` |
| **Typical Algorithmic Convergence (Ascending)** | 6–10 | 0 | 1 | 1 | **8–12** | `INFERRED` (Heuristic) |
| **Typical Algorithmic Convergence (Descending)** | 6–10 | 1 | 1 | 1 | **9–13** | `INFERRED` (Heuristic) |
| **Maximum Source-Proven Successful** | 29 | 1 | 1 | 1 | **32** | `SOURCE-VERIFIED` |
| **Immediate Abort on Bus Failure** | 1 | 0 | 0 | 0 | **1** | `SOURCE-VERIFIED` |
| **Exhausted Search Failure (Loop Limit)** | 30 | 0 | 0 | 0 | **30** | `SOURCE-VERIFIED` |

---

## 5. Architectural Implications for Design C Implementation

1. **Primitive Flexibility:** In Design C, `FTE3600_IOC_CALIB_READ_FRAME` is an on-demand primitive called by userspace. The kernel does not enforce a hard limit of 10 or 12 frames. Userspace can execute as few as 2 frames (1 discard + 1 baseline if DAC is already calibrated) or up to 32 frames if performing an exhaustive cold search.
2. **Watchdog Budget:** At ~100 ms per frame, 32 frames require $\approx 3.2\text{ seconds}$. A 5-second session watchdog window provides sufficient headroom for the worst-case 32-frame search without risking premature expiration.
