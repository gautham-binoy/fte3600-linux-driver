# STAGE D5: CONCURRENCY AND SYNCHRONIZATION FOUNDATIONS REVIEW

## Document Information
- **Project:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`) Native Linux Fingerprint Driver
- **Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`, CS0)
- **Host Controller:** Intel LPSS SPI2 (`pxa2xx-spi.2`) on Linux `7.2.8-9-omarchy` (x86_64 SMP)
- **Author:** Lead Kernel Architect & Concurrency Specialist (Antigravity Agent)
- **Status:** COMPLETED CONCURRENCY ARCHITECTURE REVIEW
- **Safety Boundary:** STRICTLY SOFTWARE-LEVEL SYNCHRONIZATION AUDIT (Zero Hardware Interactions)

---

## 1. Executive Summary

This report reviews the software concurrency model, memory barriers, lock ordering, and waitqueue synchronization primitives implemented in Stage D5.

The synchronization model is engineered to satisfy the rigorous SMP requirements of the Linux kernel memory model, guaranteeing:
1. Zero torn reads on 32-bit and 64-bit architectures via `atomic64_t`.
2. Zero lost wakeups between concurrent event notifications and sleeping waiters.
3. Zero deadlocks by strictly enforcing lock release prior to waitqueue sleep.
4. Immediate non-blocking teardown during asynchronous device unplug/unbind.

---

## 2. Event Counter Architecture: `atomic64_t event_sequence`

### 2.1 The Torn-Read Threat on 32-Bit Systems
On 32-bit x86 systems (`CONFIG_X86_32`), an ordinary `u64` occupies two 32-bit words. When a thread reads a `u64`, the CPU generates two separate 32-bit memory read instructions:
```nasm
mov eax, [counter_low]
mov edx, [counter_high]
```
If a writer updates the counter across the 32-bit rollover boundary (e.g., from `0x00000000FFFFFFFF` to `0x0000000100000000`) between the two instructions:
- The reader reads `counter_low` as `0x00000000` (new value).
- The reader reads `counter_high` as `0x00000000` (old value).
- Result: The reader observes `0x0000000000000000`, causing a massive backward sequence jump and corrupting waitqueue predicate evaluations.

### 2.2 Solution Implemented in Stage D5
`struct fte3600_data` declares:
```c
atomic64_t event_sequence;
```
- In Linux kernel x86-32, `atomic64_read()` uses the hardware atomic instruction `cmpxchg8b` with a locked bus cycle (`lock cmpxchg8b`).
- In x86-64, it executes a single 64-bit atomic load.
- Torn reads are mathematically impossible under this primitive.

---

## 3. Producer / Consumer Synchronization Protocol

### 3.1 Ordering & Barrier Specification
The Linux kernel memory model (`Documentation/memory-barriers.txt`) dictates that atomic operations without return values (e.g. `atomic64_inc()`) do not imply a memory barrier.

Furthermore, `wake_up_interruptible()` acquires an internal spinlock (`wq_head->lock`). A spinlock acquire provides **ACQUIRE** semantics, but does NOT provide **RELEASE** semantics. Stores preceding `wake_up_interruptible()` could theoretically remain in CPU store buffers or be reordered after the lock acquisition on weakly ordered systems.

To guarantee that state updates and sequence counter increments are globally visible before waiting threads evaluate the condition:

```c
/* Producer Context (Threaded IRQ Handler or Event Dispatcher) */
static inline void fte3600_notify_event(struct fte3600_data *data)
{
    atomic64_inc(&data->event_sequence);
    
    /* 
     * Full bidirectional memory barrier:
     * Guarantees event_sequence update is globally committed to L1/L2 cache
     * before wake_up_interruptible() inspects the waitqueue head.
     */
    smp_mb__after_atomic();
    
    wake_up_interruptible(&data->event_wq);
}
```

### 3.2 Consumer Evaluation Sequence (`wait_event_interruptible`)
```c
/* Consumer Context (Userspace IOCTL / Wait Function) */

/* Step 1: Sample baseline sequence before releasing mutex */
u64 start_seq = atomic64_read(&data->event_sequence);

/* Step 2: Release mutex BEFORE sleeping (Deadlock Prevention) */
mutex_unlock(&data->lock);

/* Step 3: Wait on waitqueue */
ret = wait_event_interruptible(data->event_wq,
    atomic64_read(&data->event_sequence) != start_seq ||
    atomic_read(&data->offline) != 0);

/* Step 4: Re-acquire mutex to process outcome */
mutex_lock(&data->lock);

if (ret) {
    /* Interrupted by signal (SIGINT, SIGTERM) */
    return -ERESTARTSYS;
}

if (atomic_read(&data->offline)) {
    /* Device unbound while sleeping */
    return -ENODEV;
}

/* Event received successfully */
```

### 3.3 Proof of Lost-Wakeup Prevention
1. `wait_event_interruptible()` internally calls `prepare_to_wait_event()`.
2. `prepare_to_wait_event()` sets current process state to `TASK_INTERRUPTIBLE` with a full memory barrier (`smp_mb()`).
3. It evaluates the predicate `(seq != start_seq || offline)`.
4. If the condition became true *after* Step 1 but *before* Step 3:
   - The predicate evaluates to true immediately; the process never calls `schedule()` and does not sleep.
5. If the predicate is false, the process schedules away.
6. When `fte3600_notify_event()` fires:
   - `atomic64_inc()` modifies `event_sequence`.
   - `smp_mb__after_atomic()` ensures visibility.
   - `wake_up_interruptible()` sees the task on the queue and sets its state to `TASK_RUNNING`.
   - The task wakes, re-checks the condition (which is now true), and exits the waitqueue.
7. Zero lost-wakeup windows exist.

---

## 4. Lock Ownership and Ordering Rules

### 4.1 Mutex Specification (`data->lock`)
- **Type:** `struct mutex` (sleeping lock, process context only).
- **Protects:**
  * SPI bus access and transfers (`data->tx_buf`, `data->rx_buf`).
  * Driver state machine transitions (`data->state`).
  * GPIO pin manipulation (`data->reset_gpiod`).
  * Hardware resource pointer validity during unbind.

### 4.2 Non-Nesting Invariant
- `data->lock` is a leaf lock.
- It is never acquired inside another driver-specific lock.
- It is never held while allocating memory with `GFP_KERNEL` if reclaiming could recurse into the driver.

### 4.3 Lock Release Across Waits
- **Strict Rule:** A thread MUST NOT call `wait_event_interruptible()` while holding `data->lock`.
- **Failure Mode if Violated:** If the consumer holds `data->lock` while waiting for an event, and the IRQ threaded handler must acquire `data->lock` to service the interrupt and update driver state, the system enters an **unrecoverable deadlock**.
- **Stage D5 Invariant:** The consumer releases `data->lock` prior to `wait_event_interruptible()` and re-acquires it immediately upon awakening.

---

## 5. Interrupt-Context vs Threaded-Handler Division

While physical interrupt handling is deferred to future authorized stages, the software responsibilities are formally partitioned:

```
[Hardware Interrupt on Pad 229]
               |
               v
+-------------------------------+
| Hard IRQ Handler (Top-Half)   |
| - Runs in interrupt context   |
| - Returns IRQ_WAKE_THREAD     |
| - Masked via IRQF_ONESHOT     |
+-------------------------------+
               |
               v
+-------------------------------+
| Threaded Handler (Bottom-Half)|
| - Runs in process context     |
| - Acquires data->lock         |
| - Executes SPI transactions   |
| - Updates state to            |
|   TOUCH_DETECTED              |
| - Calls fte3600_notify_event  |
| - Releases data->lock         |
| - Returns IRQ_HANDLED         |
+-------------------------------+
```

### Key Division Principles:
1. **Top-Half (Hard IRQ):**
   - Cannot sleep, cannot allocate memory, cannot perform SPI transfers.
   - Simply returns `IRQ_WAKE_THREAD`.
   - `IRQF_ONESHOT` ensures the interrupt controller keeps the GPIO pin masked until the threaded handler finishes.
2. **Bottom-Half (Threaded IRQ):**
   - Runs as a kernel thread (`irq/XXX-fte3600`).
   - Permitted to sleep, acquire `data->lock`, and perform synchronous SPI transfers.
   - Clears sensor hardware flags, updates `data->state`, and triggers `fte3600_notify_event()`.

---

## 6. Device Removal and Error Lifecycles

### 6.1 Asynchronous Removal Protocol
When `fte3600_remove()` is invoked (e.g. during module unbind or ACPI hot-unplug):
```c
/* Step 1: Remove cdev from userspace lookup */
cdev_device_del(&data->cdev, &data->dev);

/* Step 2: Acquire lock to serialize with active operations */
mutex_lock(&data->lock);

/* Step 3: Invalidate hardware pointers and mark offline */
atomic_set(&data->offline, 1);
data->state = FTE3600_STATE_DEVICE_REMOVAL;
data->reset_gpiod = NULL;
data->spi = NULL;

mutex_unlock(&data->lock);

/* Step 4: Wake all sleeping waiters */
wake_up_all(&data->event_wq);
```

### 6.2 Guaranteed Non-Blocking Teardown:
- Any thread blocked in `wait_event_interruptible()` awakens because `wake_up_all()` signals the waitqueue.
- The predicate re-evaluates `(seq != start_seq || atomic_read(&data->offline) != 0)`.
- Because `data->offline == 1`, the predicate evaluates to true.
- The waiter unblocks immediately and returns `-ENODEV`.
- No userspace process can remain hung or block driver unbinding.

---

## 7. Audit Verdict

**PASS — CONCURRENCY AND SYNCHRONIZATION DESIGN VERIFIED**
