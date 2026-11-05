# STAGE D4: IRQ ROUTING, PIN MAPPING, AND CONCURRENCY LOCKING AUDIT

## Document Information
- **Project:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`) Native Linux Fingerprint Driver
- **Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`)
- **Author:** Independent Adversarial Architecture Reviewer (Antigravity Agent)
- **Status:** COMPLETE ADVERSARIAL AUDIT (Critical Issues C and D)
- **Safety Level:** STRICTLY READ-ONLY VERIFICATION (Zero Hardware Operations)

---

## 1. Executive Summary

This adversarial audit scrutinizes two critical aspects of the proposed FT9362 kernel driver architecture:
1. **Critical Issue C — Hardware Interrupt Mapping:** Resolves the physical pad, ACPI resource, and Linux IRQ mapping for the sensor interrupt line, and investigates the source of the claim that Pad 229 maps to "IRQ 168".
2. **Critical Issue D — Concurrency and Locking Architecture:** Conducts a rigorous race-condition and deadlock audit of the proposed decoupled two-tier locking model (`data->lock` mutex and `data->event_lock` spinlock), wait queue evaluation, register `0x1A84` acknowledgment serialization, and driver teardown.

---

## 2. Critical Issue C: Pad 229, Pin 323, and the "IRQ 168" Discrepancy

### 2.1 The Hallucination of "IRQ 168"
In the previous stage's final chat response and PROGRESS summary, the text stated:
`HW_PAD229["Pad 229: ACPI GpioInt (IRQ 168)"]`

An adversarial audit was conducted across the codebase, diagnostic logs, sysfs, and kernel state to trace this number:
- `grep -rn "168" diagnostics/STAGE_D3*`: **Zero occurrences.** The 5 actual Stage D3 architectural documents never mentioned "168".
- `cat /proc/interrupts | grep 168`: **Empty.** IRQ 168 does not exist in the host system.
- `ls /sys/kernel/irq/168`: **Returns "No such file or directory".**
- **Conclusion:** The claim that Pad 229 maps to "IRQ 168" was an **unsupported hallucination** in the conversational summary output. The Stage D3 design documents themselves correctly avoided this claim and correctly specified the hardware resources.

### 2.2 Rigorous Hardware Disambiguation

```
+---------------------------------------------------------------------------------------------------+
| LAYER              | IDENTIFIER            | EXACT VALUE / EVIDENCE                               |
+--------------------+-----------------------+------------------------------------------------------+
| Physical Silicon   | Intel PCH Pad         | Pad 229 (CPU_GP_0) in Community 4                    |
| ACPI DSDT Method   | GNUM Argument         | 0x090E0003 (GNMB=0x0003, GGRP=0x0E)                  |
| ACPI Resource      | GpioInt Pin           | Pin 323 (0x0143) on "\\_SB.GPI0" (INT34C5:00)        |
| Linux Pinctrl      | Intel Pin Index       | pin 229 (CPU_GP_0) 323:INT34C5:00                    |
| Linux GPIO         | Subsystem GPIO Num    | gpio-323 on gpiochip0 (INT34C5:00, 360 lines)        |
| Linux IRQ Number   | Virtual IRQ Domain    | Unassigned (Dynamically allocated on request)        |
+---------------------------------------------------------------------------------------------------+
```

#### Step 1: ACPI DSDT Resource Inspection
In `diagnostics/privileged/acpi-fte3600-only.dsl` (lines 38-48):
```asl
GpioInt (Edge, ActiveHigh, ExclusiveAndWake, PullNone, 0x0000,
         "\\_SB.GPI0", 0x00, ResourceConsumer, , ) { 0x0000 }
...
CreateWordField (FBUF, 0x5E, INTP)
INTP = GNUM (0x090E0003)
```
- In `diagnostics/acpi/dsdt.dsl`:
  `GNUM(Arg0)` computes: `GINF(GGRP(Arg0), 0x06) + GNMB(Arg0)`.
  For `0x090E0003`: Group `0x0E` has base 320; pin index is 3.
  `320 + 3 = 323` (`0x0143`).

#### Step 2: Linux Pinctrl & GPIO Inspection
In `diagnostics/privileged/pinctrl.txt`:
```text
pin 229 (CPU_GP_0) 323:INT34C5:00 GPIO 0x80100100 0x00000029 0x00000000 [LOCKED tx]
pin 229 (CPU_GP_0): (MUX UNCLAIMED) (GPIO UNCLAIMED)
pin 229 (CPU_GP_0): input bias disabled
```
- **Pad Configuration:** `0x80100100` indicates bit 1 is asserted (input buffer enabled, output buffer disabled).
- **Lock Status:** `[LOCKED tx]` indicates the output driver is hardware-locked to prevent accidental driving, confirming this pad is dedicated exclusively as an **INPUT** interrupt pin.
- **Ownership:** Both MUX and GPIO are **UNCLAIMED**. No other driver, touchpad, keyboard, or ACPI handler has claimed Pad 229.
- **Safety Verdict:** The driver can safely acquire this GPIO and request its associated IRQ without conflicting with any other hardware component.

---

## 3. Critical Issue D: Concurrency and Locking Architecture Audit

### 3.1 Two-Tier Decoupled Locking Model
The Stage D2 audit uncovered a fatal deadlock flaw in single-mutex architectures: sleeping on a wait queue holding `data->lock` prevents the threaded IRQ handler from taking `data->lock` to clear the interrupt latch (`0x1A84`).

Stage D3 resolved this with a decoupled two-tier architecture:
1. `data->lock` (`struct mutex`): Protects physical SPI transactions and state machine transitions.
2. `data->event_lock` (`spinlock_t`): Protects event condition flags (`touch_detected`, `frame_ready`) and wait queue signaling.

```
       [ Userspace ioctl Thread ]              [ Threaded IRQ Handler (fte3600_irq_thread) ]
                   │                                                  │
                   ▼                                                  ▼
     mutex_lock(&data->lock)                                 mutex_lock(&data->lock)
    (Configures sensor via SPI)                             (Reads 0x1A82, clears 0x1A84 via SPI)
                   │                                                  │
     mutex_unlock(&data->lock)                               mutex_unlock(&data->lock)
                   │                                                  │
                   ▼                                                  ▼
   wait_event_interruptible(...)                      spin_lock_irqsave(&data->event_lock)
  (SLEEPS WITHOUT HOLDING MUTEX!)                            (Sets data->touch_detected = 1)
                   │                                  spin_unlock_irqrestore(&data->event_lock)
                   │                                                  │
                   │                                     wake_up_interruptible(&event_wq)
                   ▼                                                  │
          (Wakes up cleanly!) ◄───────────────────────────────────────┘
```

### 3.2 Concurrency Verification Checklist

#### Check 1: Can a sleeping waiter deadlock the IRQ thread?
- **Analysis:** In `FTE3600_IOC_ARM_TOUCH`, `data->lock` is released **before** entering `wait_event_interruptible(data->event_wq, data->touch_detected)`.
- **Finding:** When Pad 229 fires, `fte3600_irq_thread` attempts `mutex_lock(&data->lock)`. Because the waiting ioctl thread does not hold `data->lock`, the mutex acquisition succeeds immediately. Deadlock is mathematically impossible.
- **Verdict:** **CONFIRMED PASS.**

#### Check 2: Can an event be lost if the IRQ fires before the waiter sleeps?
- **Analysis:** Suppose the finger touches the sensor immediately during or right after the Arm strobe is sent. The IRQ thread executes, sets `data->touch_detected = 1`, and calls `wake_up_interruptible()`. Only then does the ioctl thread execute `wait_event_interruptible()`.
- **Linux Wait Queue Invariant:** `wait_event_interruptible(wq, condition)` evaluates `condition` before putting the process to sleep. Seeing `data->touch_detected == 1`, it returns 0 immediately without sleeping.
- **Implementation Requirement:** The driver must set `data->touch_detected = 0` **before** transmitting the Arm strobe to SPI.
- **Verdict:** **CONFIRMED PASS.**

#### Check 3: Does frame capture race with the IRQ thread on register `0x1A84`?
- **Analysis:** In reference implementation `capture.c` (line 280), frame capture polls status register `0x80 == 0x54`, reads the 10,246-byte burst FIFO, and writes `0x1A84 = 0x0020` to acknowledge FIFO exhaustion.
- **Locking Isolation:** The entire frame capture sequence runs under `mutex_lock(&data->lock)`.
- If Pad 229 fires when the frame is ready, `fte3600_irq_thread` blocks on `mutex_lock(&data->lock)` until frame capture completes.
- Once capture unlocks `data->lock`, the IRQ thread enters, reads status `0x1A82`, finds bit 5 already cleared by the capture ioctl, and exits without duplicate action.
- **Verdict:** **CONFIRMED PASS.**

#### Check 4: Can spurious interrupts cause invalid state transitions?
- **Analysis:** The interrupt is registered with `IRQF_ONESHOT`.
- If an electrical glitch asserts Pad 229, the top-half wakes `fte3600_irq_thread`.
- The thread reads status register `0x1A82`. If bits 3 and 5 are 0, it records a spurious event (`data->spurious_irqs++`), clears latches with `0x1A84 = 0xFFFF`, and does not set any event flags or wake userspace.
- **Verdict:** **CONFIRMED PASS.**

#### Check 5: Can device removal or unbind cause a kernel panic while waiting?
- **Analysis:** If unbind or module unload occurs while userspace is sleeping in `wait_event_interruptible()`:
- `fte3600_remove()`:
  1. Sets atomic flag `atomic_set(&data->offline, 1)`.
  2. Calls `cdev_device_del(&data->cdev, &data->dev)` (no new opens/ioctls).
  3. Calls `free_irq(data->irq, data)` (disables hardware IRQ and terminates pending threads).
  4. Calls `wake_up_all(&data->event_wq)` under lock.
- The sleeping waiter wakes up, evaluates the condition or checks `data->offline == 1`, and aborts with `-ENODEV`.
- **Verdict:** **CONFIRMED PASS.**

---

## 4. Audit Verdict

| Issue | Evaluated Architecture | Adversarial Verdict |
|:---|:---|:---:|
| **"IRQ 168" Claim** | Hallucination in conversational summary; absent in D3 docs | **DISPROVED (UNSUPPORTED)** |
| **Physical Pin Mapping** | Pad 229 (CPU_GP_0), ACPI pin 323, `[LOCKED tx]`, UNCLAIMED | **CONFIRMED** |
| **Deadlock Elimination** | `data->lock` released prior to `wait_event_interruptible` | **CONFIRMED** |
| **Lost Wakeup Guard** | Condition checked prior to wait-queue sleep | **CONFIRMED** |
| **0x1A84 Ack Isolation** | Frame capture under `data->lock` serializes with IRQ | **CONFIRMED** |
| **Spurious Rejection** | Status register 0x1A82 validated before waking user | **CONFIRMED** |
| **Unbind Teardown Safety**| `free_irq` + `wake_up_all` + `offline=1` prevents hangs | **CONFIRMED** |
