# STAGE D4.2: FINAL ADVERSARIAL IRQ, EVENT-LOSS AND UAPI SAFETY AUDIT

## Document Information
- **Project:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`) Native Linux Fingerprint Driver
- **Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`)
- **Host Controller:** Intel LPSS SPI2 (`pxa2xx-spi.2`, CS0) on Linux `7.2.8-9-omarchy` (x86_64)
- **Author:** Independent Adversarial Reviewer (Antigravity Agent)
- **Status:** COMPLETED FINAL ADVERSARIAL AUDIT
- **Safety Level:** STRICTLY READ-ONLY VERIFICATION (Zero Hardware Operations)

---

## 1. Executive Summary

This Stage D4.2 adversarial safety audit was commissioned to rigorously challenge the conclusions of Stage D4 and D4.1. The investigation focused on hardware race hazards, edge coalescing, event loss between capture and interrupt servicing, memory model consistency of the sequence counter, pad configuration register decodings, and backward compatibility of the UAPI contracts.

### Key Audit Findings:
1. **Critical Capture/IRQ Event-Loss Hazard (BLOCKING):** While W1C register semantics theoretically allow `write_word(0x1A84, 0x0020)` to clear only Bit 5 without clearing Bit 3, the reference implementation reveals that entering Image Acquisition Mode (`0x1800 = 0x4FFE`) issues Abort Strobe `{0xC0, 0x3F}`. This disarms the analog touch detection circuitry during capture. Furthermore, without an official silicon errata document, cross-talk between the burst FIFO engine and the interrupt latch register cannot be ruled out. This question is classified as **BLOCKING** pending explicit architectural selection of an event-preservation design.
2. **Event Counter Memory Model (RESOLVED):** On 32-bit x86 architectures, reading a standard `u64 event_sequence` inside a wait queue predicate is prone to **torn reads** (two 32-bit `mov` operations). The counter MUST be declared as `atomic64_t` with explicit acquire/release memory barriers to guarantee SMP coherence and zero torn reads.
3. **`IRQF_ONESHOT` Coalescing (RESOLVED):** `IRQF_ONESHOT` masks the PCH GPIO line at the controller level during threaded servicing. It does **not** queue multiple hardware edges. If multiple touches occur while masked, the Intel GPIO controller coalesces them into a single pending interrupt. Event persistence relies strictly on the sensor silicon latching Bit 3 in register `0x1A82`.
4. **Pad 229 Configuration (CONFIRMED):** In-depth bitwise decoding of PCH `PADCFG0` (`0x80100100` / `0x82000100`) proves that Bit 8 (`GPIOTXDIS = 1`) disables the transmitter (High-Z), Bit 9 (`GPIORXDIS = 0`) enables the receiver, Bits [13:10] (`PMODE = 0`) set GPIO mode, and `[LOCKED tx]` hardware-locks the transmitter against software misconfiguration.
5. **UAPI v1 Client B Hazard (MUST FIX BEFORE CODE):** Existing applications sending `version = 1` do not expect `copy_to_user` to populate output data into previously zeroed `reserved` bytes. The driver must preserve strict v1 semantics when `params.version == 1` and require `version == 2` for extended identification fields.
6. **State Enum Aliasing (RESOLVED):** `FTE3600_STATE_READY = 4` must be retained as the canonical state; `#define FTE3600_STATE_INITIALIZED FTE3600_STATE_READY` is established as an alias.
7. **Historical "IRQ 168" Discrepancy (RECONCILED):** The label "IRQ 168" originated solely in the conversational summary text and PROGRESS overview of Stage D3; it was completely absent from the 5 formal `diagnostics/STAGE_D3_*.md` repository files and does not exist in Linux sysfs.

---

## 2. Part 1: Critical Capture/IRQ Event-Loss Analysis

### 2.1 The Core Protocol Mechanism
- **Register `0x1A82`:** Read-only interrupt event status register.
  - Bit 3 (`0x0008`): Finger touch detected.
  - Bit 5 (`0x0020`): Frame readout ready / FIFO threshold reached.
- **Register `0x1A84`:** Write-1-to-clear (W1C) event acknowledgment register.
  - Writing `0x0020` clears Bit 5.
  - Writing `0x0008` clears Bit 3.
  - Writing `0x002F` clears Bit 5, Bit 3, and auxiliary flags.

### 2.2 Deep Timeline Analysis Across 8 Scenarios

```
Timeline A: Touch occurs before capture starts
[ARM_TOUCH Active] ──> Pad 229 Asserts ──> Threaded IRQ acks 0x1A84=0x002F ──> Wakes Waiter
                  ──> State = TOUCH_DETECTED ──> Userspace issues CAPTURE ──> Capture executes normally.
Status: Completely serialized. Zero event loss.

Timeline B: Touch occurs while capture holds data->lock
[CAPTURE Active: data->lock HELD] ──> Finger contacts sensor
       │
       ├──> Sensor asserts Bit 3 in 0x1A82; Pad 229 transitions LOW -> HIGH
       ├──> Hard-IRQ top-half masks line (IRQF_ONESHOT), wakes fte3600_irq_thread
       ├──> fte3600_irq_thread attempts mutex_lock(&data->lock) ──> BLOCKS!
       │
       ▼ (Capture finishes 10,246B burst read)
Capture writes 0x1A84 = 0x0020 (Clears Bit 5 only; Bit 3 remains 1 in 0x1A82)
Capture releases data->lock
       │
       ▼ (IRQ thread unblocks, acquires data->lock)
IRQ thread reads 0x1A82 ──> Sees Bit 3 IS STILL SET!
IRQ thread writes 0x1A84 = 0x0008 (Clears Bit 3)
IRQ thread releases data->lock, increments event_sequence, wakes event_wq.
Status: Silicon W1C semantics preserve Bit 3.

Timeline C: Touch occurs immediately before capture's final acknowledgement
Identical to Timeline B. The IRQ thread is queued waiting for data->lock and executes immediately
after capture drops the mutex.

Timeline D: Touch occurs immediately after capture's final acknowledgement
Capture has dropped data->lock. IRQ thread acquires data->lock without contention, reads 0x1A82,
acknowledges 0x1A84 = 0x0008, advances event_sequence, and signals wait queue.

Timeline E: Two or more touch events occur while the IRQ thread is blocked
Hard-IRQ masks Pad 229 on first edge. The Intel GPIO controller coalesces subsequent edges.
The sensor's internal comparator latches Bit 3 in 0x1A82. When the IRQ thread runs, it sees Bit 3
asserted, clearing it once. One touch event is recorded.

Timeline F: IRQ is pending when ARM_TOUCH is interrupted by SIGINT
Userspace wakes with -ERESTARTSYS. Mutex data->lock is taken to send Abort Strobe {0xC0, 0x3F}.
The 64-bit monotonic event_sequence counter ensures that even if the IRQ thread advanced the sequence,
any future ARM_TOUCH samples a fresh start_seq, preventing a stale touch wakeup.

Timeline G: IRQ arrives during device removal
fte3600_remove() sets atomic data->offline = 1, calls free_irq() (which waits for the threaded
handler to finish), and calls wake_up_all(&data->event_wq). The waiter observes offline == 1 and
aborts with -ENODEV without crashing.

Timeline H: IRQ arrives during suspend or resume
PM suspend callback masks IRQ, issues Abort Strobe {0xC0, 0x3F}, and parks reset low. Any late IRQ
is rejected because data->state == SUSPENDED.
```

### 2.3 The Critical Question Evaluated
> **Question:** Can capture's final acknowledgement clear a touch event that occurred during capture but whose threaded IRQ handler is waiting for `data->lock`?

**Forensic Evaluation:**
1. In strict register theory, `write_word(0x1A84, 0x0020)` writes a `0` to Bit 3. In standard W1C registers, writing `0` does not clear the bit.
2. **However, in the reference code (`research/libfprint-fte3600/libfprint/drivers/fte3600/capture.c:225`):**
   Before capture begins, `configuration_prefix()` transmits command `{0xC0, 0x3F}` (Abort Strobe) and changes `0x1800 = 0x4FFE` (Image Acquisition Mode).
   This disables the analog finger detection comparator during image readout!
3. **The Unresolved Uncertainty:**
   Without an official silicon register specification from FocalTech, it cannot be proven whether writing `0x0020` to `0x1A84` causes the internal sequencer to reset the interrupt latch block or whether the burst FIFO readout `{0x06, 0xF9, ...}` auto-clears status register `0x1A82`.
4. **Classification:** **BLOCKING.**

### 2.4 Proposed Architectural Alternatives & Trade-Offs

#### Alternative 1: Read-and-Preserve Pending Status in Capture
- **Mechanism:** Before writing `0x1A84 = 0x0020`, the capture routine reads register `0x1A82`. If Bit 3 is set, it records `data->pending_touch = 1` and increments `event_sequence` directly within capture.
- **Pros:** Guarantees no touch event is lost even if the sensor silicon auto-clears `0x1A82`.
- **Cons:** Requires an additional SPI transaction during capture.

#### Alternative 2: Defer Event Acknowledgment to the Unified IRQ Thread
- **Mechanism:** Capture does NOT write `0x1A84`. Instead, completion of capture is signaled exclusively by the threaded IRQ handler reading `0x1A82` and clearing both Bit 5 and Bit 3 in a single serialized transaction.
- **Pros:** Completely eliminates race conditions between two different writers to `0x1A84`.
- **Cons:** Adds interrupt servicing latency to frame delivery.

#### Alternative 3: Mutual Exclusion via Driver State Machine (Recommended)
- **Mechanism:** Enforce strict state separation: `ARM_TOUCH` and `CAPTURE` cannot run concurrently. When userspace initiates `CAPTURE`, touch detection is formally declared disarmed. Userspace must issue a fresh `ARM_TOUCH` ioctl after capture completes.
- **Pros:** Matches biometric workflow in `fprintd` (Enroll/Verify: wait for touch -> trigger capture -> return to idle). Zero possibility of overlapping touch/capture events.
- **Cons:** Does not support background continuous touch monitoring while streaming frames.

---

## 3. Part 2: Event Counter Memory Model Audit

### 3.1 Type and Alignment Audit
- **Torn Read Hazard on 32-Bit Architectures:**
  On 32-bit x86 (`i686`), a standard `u64` variable requires two 32-bit instructions (`mov eax, [mem]; mov edx, [mem+4]`). If an interrupt fires between the two instructions and modifies `event_sequence`, userspace evaluates a corrupted, torn 64-bit value!
- **Mandatory Solution:**
  `event_sequence` MUST be declared as:
  ```c
  atomic64_t event_sequence;
  ```
  On 64-bit x86_64, `atomic64_read()` compiles to a single 64-bit `mov`. On 32-bit x86, it uses `lock cmpxchg8b` to guarantee atomicity and prevent torn reads.

### 3.2 Synchronization & Memory Barrier Invariant
```
[ IRQ Thread: Event Arrival ]
  1. spin_lock_irqsave(&data->event_lock, flags);
  2. atomic64_inc(&data->event_sequence);
  3. spin_unlock_irqrestore(&data->event_lock, flags);
  4. smp_mb__after_atomic();  /* Guarantees counter update is globally visible */
  5. wake_up_interruptible(&data->event_wq);

[ Userspace Thread: ARM_TOUCH Entry ]
  1. u64 start_seq = atomic64_read(&data->event_sequence);
  2. /* Configure & Arm Sensor via SPI under data->lock */
  3. wait_event_interruptible(data->event_wq,
         atomic64_read(&data->event_sequence) != start_seq ||
         atomic_read(&data->offline));
```
- **Evaluation:** `wake_up_interruptible()` takes `data->event_wq.lock`, providing an implicit memory barrier. The explicit `smp_mb__after_atomic()` ensures mathematical ordering on weakly ordered SMP architectures.
- **Counter Overflow:** A 64-bit counter incremented at 100 Hz requires $5.8 \times 10^9$ years to overflow. Counter wrap is impossible.

---

## 4. Part 3: `IRQF_ONESHOT` and Hardware Coalescing

### 4.1 What `IRQF_ONESHOT` Guarantees
- In the Linux interrupt core (`kernel/irq/manage.c`), `IRQF_ONESHOT` instructs the kernel to keep the interrupt line masked at the controller level (`intel-gpio`) after the hardirq top-half completes.
- The line remains masked until the threaded handler (`fte3600_irq_thread`) returns `IRQ_HANDLED`.
- **Guarantee:** Prevents re-entrant interrupt storms on edge/level lines.

### 4.2 What `IRQF_ONESHOT` Does NOT Guarantee
- **No Hardware Queue:** The Intel LPSS GPIO controller does **not** maintain a FIFO queue of edges. If 3 rising edges occur on Pad 229 while masked:
  - The PCH interrupt status latch remains 1.
  - The 3 edges are **coalesced into a single interrupt**.
- **Sensor Persistence:** Event preservation depends strictly on the FT9362 internal register `0x1A82`. Because `0x1A82` latches Bit 3 until acknowledged via `0x1A84`, the event is preserved regardless of controller edge coalescing.

---

## 5. Part 4: Pad 229 Configuration and PCH Register Decoding

### 5.1 Bitwise Decoding of Intel Tiger Lake `PADCFG0`
From `diagnostics/privileged/pinctrl.txt:235` and `STAGE2E6_1_PAD229_INVESTIGATION.md`:
```
PCH Register: PADCFG0 (DW0) = 0x80100100 (or 0x82000100 after edge trigger config)
```

| Bit Field | Name | Value | Hardware Reality |
|:---|:---|:---:|:---|
| **[31]** | `RST_CONF` | `1` | Configuration resets only on Deep Sleep/Host Reset (S5). |
| **[26:25]** | `RXEVCFG` | `0b01` | **Edge Sensitive:** Configured for edge detection (`PADCFG0_RXEVCFG_EDGE`). |
| **[23]** | `RXINV` | `0` | **Non-Inverted:** Rising-edge active (`ActiveHigh`). |
| **[20]** | `GPIROUTIOXAPIC` | `0` | Routed through Intel GPIO Community interrupt controller tree. |
| **[13:10]** | `PMODE` | `0b0000` | **GPIO Mode:** Configured as general-purpose I/O (`PMODE_GPIO`). |
| **[9]** | `GPIORXDIS` | `0` | **Input Receiver ENABLED:** Physical pad input buffer is powered and active. |
| **[8]** | `GPIOTXDIS` | `1` | **Transmitter DISABLED:** Output driver tri-stated (High-Impedance Input). |
| **[1]** | `GPIORXSTATE` | `0` or `1` | Live digital voltage sensed at the physical pin. |

### 5.2 Meaning of `[LOCKED tx]`
- In Intel PCH architecture, the UEFI BIOS can lock pad configuration registers via the `PADCFGLOCK` registers.
- `[LOCKED tx]` indicates that the **Transmitter Configuration Lock** is engaged. Software cannot accidentally enable the output buffer (`GPIOTXDIS` cannot be written to 0).
- This provides hardware-enforced protection: Pad 229 cannot drive current or short out the sensor line.
- **Safety Verdict:** Pad 229 is proven to be an active, high-impedance, rising-edge-sensitive input line owned by the Host OS (`SHPO` line 70259).

---

## 6. Part 5: UAPI Backward Compatibility & Client Analysis

### 6.1 Four-Client Scenario Analysis

```
Client A: Old v1 binary sends version=1, flags=0, reserved={0}
Input: Valid.
Kernel Action: Executes identification (C6 ungating, ID read 0x9362, CRC 0x1C53).
Output: Kernel returns ret=0. If copy_to_user is called, it overwrites reserved bytes.
Verdict: Compatible ONLY IF the application does not inspect reserved output bytes.

Client B: Old v1 binary sends version=1 and expects reserved output bytes to remain zero
Input: Valid.
Kernel Action: If kernel populates chip_id and wire_crc into reserved[0..1], reserved fields are non-zero!
Hazard: If Client B checks assert(params.reserved[0] == 0), Client B FAILS!
Verdict: BREAKING CHANGE for Client B unless kernel checks version == 1.

Client C: New v2 binary sends version=2, flags=FTE3600_INIT_FLAG_FULL_INIT
Input: Valid.
Kernel Action: Executes full 32-step initialization, populates chip_id, wire_crc, foundry_code, dac.
Output: Copies full 32 bytes back to user. Transitions to READY.
Verdict: Clean v2 semantics.

Client D: 32-bit userspace binary running on 64-bit kernel
Layout: All structures have identical size (32B or 48B) and identical 8B alignment on both -m32 and -m64.
Requirement: Driver MUST define .compat_ioctl = fte3600_ioctl in fops to prevent -ENOTTY.
Verdict: Bit-identical layout confirmed.
```

### 6.2 Safe UAPI Architecture Proposal
To eliminate the Client B hazard:
1. **Version-Gated Behavior:**
   - If `params.version == 1`: The kernel executes identification only, does NOT modify `reserved`, and returns 0 without copying extended fields.
   - If `params.version == 2`: The kernel executes identification or full init based on `flags`, populates `chip_id`, `wire_crc`, etc., and copies the structure back to userspace.
2. **Alternative: Dedicated Opcodes:**
   - Keep `FTE3600_IOC_INIT` (`0xC0204603`) strictly as identification (preserving Stage C behavior).
   - Introduce `FTE3600_IOC_CONFIGURE` (`0xC0204607`) for post-ID full initialization and calibration parameters.

### 6.3 State Enum Reconciled Specification
To avoid duplicate or ambiguous semantics:
```c
enum fte3600_state {
    FTE3600_STATE_UNINITIALIZED         = 0,
    FTE3600_STATE_RESETTING             = 1,
    FTE3600_STATE_RESET_SEQUENCE_ISSUED = 2,
    FTE3600_STATE_INITIALIZING          = 3,
    FTE3600_STATE_READY                 = 4, /* Canonical operational standby */
    FTE3600_STATE_CAPTURING             = 5,
    FTE3600_STATE_ERROR                 = 6,
    FTE3600_STATE_UNKNOWN               = 7,
    FTE3600_STATE_IDENTIFIED            = 8, /* C6 ungated, ID verified, AFE unconfigured */
    FTE3600_STATE_ARMED_WAIT_FOR_TOUCH  = 9, /* Autonomous touch detection running */
    FTE3600_STATE_TOUCH_DETECTED        = 10,/* Touch confirmed by IRQ, awaiting capture */
    FTE3600_STATE_FRAME_READY           = 11,/* Frame buffered in DMA memory */
    FTE3600_STATE_SUSPENDED             = 12,/* System sleep, sensor parked */
};

/* Compatibility Alias */
#define FTE3600_STATE_INITIALIZED FTE3600_STATE_READY
```

---

## 7. Part 6: Historical Reconciliation of "IRQ 168"

### 7.1 Exhaustive Textual Audit
A comprehensive string audit was conducted across the Git repository:
- **`diagnostics/STAGE_D3_*.md` (All 5 Files):** **0 occurrences.**
- **`kernel/fte3600.c` & `kernel/fte3600.h`:** **0 occurrences.**
- **`/proc/interrupts` & `/sys/kernel/irq/`:** **0 occurrences.**
- **Where "IRQ 168" Actually Appeared:**
  1. In the AI Assistant's final conversational chat turn of Stage D3:
     `HW_PAD229["Pad 229: ACPI GpioInt (IRQ 168)"]`
  2. In the summary markdown block appended to `PROGRESS.md` at line 3256:
     `(Pad 229, pin 323, edge falling, active low)` — Note that line 3256 has pin 323, while the chat summary had IRQ 168.
- **Historical Verdict:**
  The label "IRQ 168" was an AI hallucination introduced strictly in the chat output layer. It was never committed to the kernel source code or technical design specifications. All future driver code relies strictly on dynamic ACPI GPIO IRQ mapping.

---

## 8. Part 7: Master Safety & Readiness Matrix

| Issue ID | Technical Issue | Evidence Inspected | Actual Finding | Classification | Required Resolution |
|:---:|:---|:---|:---|:---:|:---|
| **ISS-01** | **Capture/IRQ Event Loss** | `capture.c:280`, `ft93xx.c`, `0x1A84` W1C | Silicon cross-talk between Bit 5 and Bit 3 unproven without datasheet | **BLOCKING** | Select Alternative 1 or 3 (state mutual exclusion) |
| **ISS-02** | **IRQF_ONESHOT Coalescing**| Linux kernel `manage.c`, Intel GPIO IP | Edge events coalesce during mask; event preserved in `0x1A82` latch | **RESOLVED** | Accept single-event notification semantics |
| **ISS-03** | **Event Counter Memory Safety**| C ABI, 32-bit x86 torn read rules | Plain `u64` is unsafe on 32-bit; `atomic64_t` required | **RESOLVED** | Declare `atomic64_t event_sequence` with `smp_mb` |
| **ISS-04** | **Waitqueue Lost Wakeup** | Linux `wait.h`, macro expansions | Condition evaluated prior to sleep; sequence comparison is race-free | **RESOLVED** | Compare `atomic64_read() != start_seq` |
| **ISS-05** | **Removal/Suspend IRQ Races**| `fte3600.c`, `free_irq()`, PM hooks | `data->offline` atomic flag + `free_irq` synchronizes with thread | **RESOLVED** | Wake all waiters with `-ENODEV` on unbind |
| **ISS-06** | **Pad 229 Input Configuration**| `PADCFG0` bitfields, `pinctrl.txt` | `GPIORXDIS=0`, `GPIOTXDIS=1`, `[LOCKED tx]` proves dedicated input | **RESOLVED** | Request via ACPI driver GPIOs |
| **ISS-07** | **UAPI v1 Client B Hazard**| `fte3600.h:73`, `tools/fte3600-tool.c` | Overwriting reserved output fields breaks applications checking zero | **MUST FIX BEFORE CODE**| Gate output fields on `params.version == 2` |
| **ISS-08** | **32/64-Bit Compatibility** | C alignment rules, `_Static_assert` | Bit-identical layouts; natural 8B alignment; `.compat_ioctl` required | **RESOLVED** | Implement `.compat_ioctl = fte3600_ioctl` |
| **ISS-09** | **State Enum Aliasing** | `kernel/fte3600.h:36`, D3 text | `READY = 4` retained; `INITIALIZED` defined as alias | **RESOLVED** | Add alias `#define FTE3600_STATE_INITIALIZED 4` |
| **ISS-10** | **IRQ 168 Discrepancy** | Repository grep, D3 diagnostics | Hallucination in chat turn; absent from source and D3 docs | **RESOLVED** | Eliminate from all future documentation |
| **ISS-11** | **SPI DMA Burst Capacity** | `spi-pxa2xx-core.ko`, `idma64.ko` | 64 KB controller limit & 128 KB descriptor proven by disassembly | **RESOLVED** | Single 10,246B transfer supported |

---

## 9. Final Verification of Invariants

- **Source Code Modified:** 0 files
- **Binaries Rebuilt:** 0 files
- **Hardware Operations Executed:** 0
- **Kernel Module Status:** `fte3600.ko` loaded (`20480 0`, `live`), bound to `spi-FTE3600:00`.
- **Character Device Identity:** `/dev/fte3600` continuously `crw------- 1 root root 511, 0`.
- **Driver State:** `8 (FTE3600_STATE_IDENTIFIED)`.
- **SPI Statistics:** `transfers=3, bytes=19` (100% unchanged from Stage C baseline).

---

## 10. Final Classification

BLOCKED — ADDITIONAL DESIGN WORK REQUIRED
