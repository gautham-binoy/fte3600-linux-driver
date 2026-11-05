# STAGE D5.1: CONCURRENCY AND SYNCHRONIZATION FINDINGS

## Document Information
- **Project:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`) Native Linux Fingerprint Driver
- **Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor (`spi-FTE3600:00`, CS0)
- **Host Controller:** Intel LPSS SPI2 (`pxa2xx-spi.2`) on Linux `7.2.8-9-omarchy` (x86_64 SMP)
- **Author:** Adversarial Kernel Auditor & Concurrency Specialist (Antigravity Agent)
- **Status:** COMPLETED CONCURRENCY AUDIT
- **Safety Boundary:** STRICTLY READ-ONLY AUDIT (Zero Hardware Access)

---

## 1. Executive Summary

This document reports on the concurrency, memory ordering, and waitqueue synchronization audit of Stage D5.

The investigation examined every line of `kernel/fte3600.c` relating to `event_sequence`, `event_wq`, memory barriers, and the `fte3600_cdev_poll()` handler.

### Key Audit Findings:
1. **The Sequence Counter is Dormant:** `data->event_sequence` is declared and initialized, but has **zero reads** anywhere in the driver and is incremented only in a helper that has **zero callers**.
2. **Userspace Has No Access to `event_sequence`:** No ioctl, read, or poll handler exposes or evaluates `event_sequence`.
3. **`wait_event*()` Does Not Exist in Driver:** Contrary to the theoretical discussions in the Stage D5 report, the actual driver source contains zero calls to `wait_event_interruptible()`.
4. **Asymmetric Synchronization in Poll:** `fte3600_cdev_poll()` inspects `data->state` without holding `data->lock` and without `READ_ONCE()`, while state writers modify `data->state` under `data->lock`.
5. **Memory Barrier Contract is Incomplete:** `smp_mb__after_atomic()` on the producer side is insufficient on its own to prevent lost wakeups without an explicit consumer-side `set_current_state()` memory barrier sequence.

---

## 2. Detailed Responses to the Six Concurrency Inquiries

### Inquiry 1: Which events increment `event_sequence`?
**Finding: NONE.**
In the current kernel source (`kernel/fte3600.c`), `event_sequence` is incremented exclusively inside `fte3600_notify_event()`:
```c
static inline void fte3600_notify_event(struct fte3600_data *data)
{
	atomic64_inc(&data->event_sequence);
	smp_mb__after_atomic();
	wake_up_interruptible(&data->event_wq);
}
```
A codebase-wide grep confirms that `fte3600_notify_event()` has **zero call sites** in `kernel/fte3600.c`. No hardware interrupt, reset sequence, identification routine, ioctl, or device removal triggers an increment of `event_sequence`.

### Inquiry 2: Whether any event producer fails to increment it?
**Finding: ALL current event producers fail to increment it.**
- **Reset Sequence (`fte3600_hw_reset_pulse`):** Modifies `data->state` to `RESETTING` and then `RESET_SEQUENCE_ISSUED`. It does NOT call `fte3600_notify_event()` and does NOT increment `event_sequence`.
- **Hardware Identification (`fte3600_hw_identify`):** Modifies `data->state` to `INITIALIZING` and then `IDENTIFIED` or `UNKNOWN`. It does NOT call `fte3600_notify_event()` and does NOT increment `event_sequence`.
- **Device Removal (`fte3600_remove`):** Modifies `data->state` to `DEVICE_REMOVAL` and sets `data->offline = 1`. It calls `wake_up_all(&data->event_wq)` directly, bypassing `fte3600_notify_event()` and leaving `event_sequence` unchanged.

### Inquiry 3: Whether userspace waiters actually use it?
**Finding: NO.**
- Userspace processes querying driver state execute `ioctl(fd, FTE3600_IOC_GET_STATE, &snap)`. The returned `struct fte3600_state_snapshot` has no field for `event_sequence` (its reserved fields are zeroed).
- Userspace processes waiting for readiness invoke `poll()`, `ppoll()`, or `epoll_wait()`. The VFS calls `fte3600_cdev_poll()`. `fte3600_cdev_poll()` only checks `data->offline` and `data->state`. It does **not** read `data->event_sequence`.
- Therefore, userspace waiters never observe, evaluate, or depend on `event_sequence`.

### Inquiry 4: Whether event reads and updates use appropriate synchronization?
**Finding: NO (Deficient on Reads).**
- **Updates:** `atomic64_inc()` and `atomic64_set()` are hardware-atomic primitives. In `fte3600_notify_event()`, `atomic64_inc()` is followed by `smp_mb__after_atomic()`.
- **Reads:** There are **zero reads** of `data->event_sequence` in `kernel/fte3600.c`.
- **State Reads in Poll:** In `fte3600_cdev_poll()`, `data->state` is read without holding `data->lock` and without `READ_ONCE()`:
  ```c
  if (data->state == FTE3600_STATE_TOUCH_DETECTED ||
      data->state == FTE3600_STATE_FRAME_READY)
      mask |= EPOLLIN | EPOLLRDNORM;
  ```
  Meanwhile, state writers in `hw_reset_pulse`, `hw_identify`, and `fte3600_remove` modify `data->state` under `mutex_lock(&data->lock)`. This asymmetry constitutes an unsynchronized read.

### Inquiry 5: Whether a waiter can miss an event between checking the condition and sleeping?
**Finding: In `poll()`, No (due to VFS protocol); In a direct waitqueue, Yes without consumer barrier.**
- For `fte3600_cdev_poll()`:
  1. `poll_wait(filp, &data->event_wq, wait)` is called first. This enqueues the wait entry onto `event_wq`.
  2. `data->state` is checked second.
  3. If an event occurred prior to `poll()`, `mask` is non-zero, so the VFS never sleeps.
  4. If an event occurs after `poll_wait()`, the waiter is already registered, so `wake_up_interruptible()` will flag the task as runnable.
- However, if an in-kernel waiter were implemented using a custom wait loop instead of `wait_event_interruptible()`, a missing consumer-side barrier would allow a race window.

### Inquiry 6: Whether wakeup ordering is correct under the Linux kernel memory model?
**Finding: Incomplete Specification.**
- The Stage D5 report claimed that `smp_mb__after_atomic()` prevents lost wakeups.
- Under the Linux Kernel Memory Model (`Documentation/memory-barriers.txt`), `smp_mb__after_atomic()` only guarantees that stores prior to the barrier (such as `atomic64_inc()` and `data->state = ...`) are visible before subsequent operations in program order.
- But `wake_up_interruptible()` acquires `wq_head->lock` (a spinlock), which provides **ACQUIRE** semantics, not **RELEASE** semantics.
- For complete lost-wakeup prevention, the consumer must execute a full memory barrier (`smp_mb()`), which is automatically provided by `set_current_state(TASK_INTERRUPTIBLE)` inside `prepare_to_wait_event()`.
- A producer-side barrier alone cannot guarantee absence of lost wakeups without the reciprocal consumer-side barrier.

---

## 3. Producer / Consumer Synchronization Protocol Analysis

### 3.1 Kernel Memory Model Synchronization Architecture

```
PRODUCER (Threaded IRQ / Notification)          CONSUMER (VFS Poll / In-Kernel Waiter)
---------------------------------------          --------------------------------------
[State Mutation]                                 [Queue Registration]
data->state = TOUCH_DETECTED;                    poll_wait(filp, &data->event_wq, wait);
atomic64_inc(&data->event_sequence);              (or prepare_to_wait_event(&data->event_wq, ...))
                                                              |
[Producer Barrier]                                            | [Consumer Barrier (in VFS / wait_event)]
smp_mb__after_atomic();                          set_current_state(TASK_INTERRUPTIBLE); // smp_mb()
       |                                                      |
       v                                                      v
wake_up_interruptible(&data->event_wq);          [Condition Evaluation]
  -> spin_lock(&wq_head->lock)                   if (data->state == TOUCH_DETECTED)
  -> __wake_up_common(...)                          mask |= EPOLLIN;
  -> spin_unlock(&wq_head->lock)                 else
                                                    schedule(); // sleep if condition false
```

### 3.2 Race Window Analysis in Current Code
In `kernel/fte3600.c`:
1. Producer side: `fte3600_notify_event()` has `smp_mb__after_atomic()`, but is **never called**.
2. Consumer side: `fte3600_cdev_poll()` relies on the VFS epoll/poll subsystem to provide the consumer-side barrier when `mask == 0`.
3. However, because `data->state` is read without `data->lock` and without `READ_ONCE()`, the compiler is theoretically permitted to reload or reorder `data->state` relative to other instructions in `poll()`.

---

## 4. Concurrency Audit Summary

1. `atomic64_t event_sequence` is currently dead code in the driver. It provides no actual concurrency protection because it is neither incremented by real operations nor inspected by any consumer.
2. The concurrency model of `fte3600_cdev_poll()` correctly registers on `event_wq` before inspecting state, but inspects state without synchronization.
3. The claim that lost wakeups are solved in Stage D5 is theoretical; the underlying mechanisms (`fte3600_notify_event`) are unreferenced stubs.

---

## 5. Audit Verdict

**FAIL — UNREFERENCED SYNCHRONIZATION PRIMITIVES AND ASYMMETRIC STATE ACCESS**
