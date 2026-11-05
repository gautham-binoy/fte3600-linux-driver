# STAGE D7.9: SPI Asynchronous Submission and Callback Ownership Audit

**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Peripheral Device:** FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Active Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Host Controller:** Intel LPSS SPI (`pxa2xx-spi` / `idma64`)  
**Source Provenance:** Linux Kernel v7.2.8 (`drivers/spi/spi.c:4530-4645`, `2250-2305`)  
**Audit Classification:** STRICTLY READ-ONLY Source-Proven Verification  

---

## 1. Executive Summary

This audit performs an exact, source-level trace of `spi_async()` in the running Linux 7.2.8 kernel to establish the ownership timeline and reference-counting contract of `struct fte3600_async_xfer`.

### Primary Audit Discoveries:
1. **Immediate SMP Callback Concurrency:** On an SMP kernel, the completion callback `msg->complete()` can execute on another CPU core **before `spi_async()` returns to the caller**. Therefore, the dual-ownership reference count (`kref = 2`) must be established **prior** to invoking `spi_async()`.
2. **Submission Failure Reference Leak:** If `spi_async()` returns an error ($< 0$), the message was never accepted into the controller queue, and **the completion callback will never execute**. The submitting thread must explicitly drop the callback reference to avoid a permanent memory leak.
3. **Queue Acceptance Point:** A message is accepted by the SPI core inside `__spi_queued_transfer()` under `ctlr->queue_lock`, transitioning `msg->status` to `-EINPROGRESS`.

---

## 2. Kernel Source Call Graph and Exact Line Tracing

### 2.1 Entry Point: `spi_async()` (`drivers/spi/spi.c:4624-4645`)

```c
// drivers/spi/spi.c:4624-4645 [SOURCE-PROVEN: Linux 7.2.8]
int spi_async(struct spi_device *spi, struct spi_message *message)
{
	struct spi_controller *ctlr = spi->controller;
	int ret;
	unsigned long flags;

	ret = spi_maybe_optimize_message(spi, message);
	if (ret)
		return ret;

	spin_lock_irqsave(&ctlr->bus_lock_spinlock, flags);

	if (ctlr->bus_lock_flag)
		ret = -EBUSY;
	else
		ret = __spi_async(spi, message);

	spin_unlock_irqrestore(&ctlr->bus_lock_spinlock, flags);

	return ret;
}
EXPORT_SYMBOL_GPL(spi_async);
```

### 2.2 Dispatch: `__spi_async()` (`drivers/spi/spi.c:4539-4566`)

```c
// drivers/spi/spi.c:4539-4566 [SOURCE-PROVEN: Linux 7.2.8]
static int __spi_async(struct spi_device *spi, struct spi_message *message)
{
	struct spi_controller *ctlr = spi->controller;
	struct spi_transfer *xfer;

	if (!ctlr->transfer)
		return -ENOTSUPP;

	SPI_STATISTICS_INCREMENT_FIELD(ctlr->pcpu_statistics, spi_async);
	SPI_STATISTICS_INCREMENT_FIELD(spi->pcpu_statistics, spi_async);

	trace_spi_message_submit(message);

	if (!ctlr->ptp_sts_supported) {
		list_for_each_entry(xfer, &message->transfers, transfer_list) {
			xfer->ptp_sts_word_pre = 0;
			ptp_read_system_prets(xfer->ptp_sts);
		}
	}

	return ctlr->transfer(spi, message);
}
```

### 2.3 Queue Enqueue: `__spi_queued_transfer()` (`drivers/spi/spi.c:2266-2295`)

For queued controllers (`spi-pxa2xx`), `ctlr->transfer` points to `spi_queued_transfer`:

```c
// drivers/spi/spi.c:2266-2295 [SOURCE-PROVEN: Linux 7.2.8]
static int __spi_queued_transfer(struct spi_device *spi,
				 struct spi_message *msg,
				 bool need_pump)
{
	struct spi_controller *ctlr = spi->controller;
	unsigned long flags;

	spin_lock_irqsave(&ctlr->queue_lock, flags);

	if (!ctlr->running) {
		spin_unlock_irqrestore(&ctlr->queue_lock, flags);
		return -ESHUTDOWN;
	}
	msg->actual_length = 0;
	msg->status = -EINPROGRESS;

	list_add_tail(&msg->queue, &ctlr->queue);
	ctlr->queue_empty = false;
	if (!ctlr->busy && need_pump)
		kthread_queue_work(ctlr->kworker, &ctlr->pump_messages);

	spin_unlock_irqrestore(&ctlr->queue_lock, flags);
	return 0;
}
```

---

## 3. Detailed Audit Questions and Verified Answers

### 3.1 At what point is the message accepted by the SPI core?
**Source Fact (`spi.c:2279-2283`):**  
The message is accepted at line 2280 when `list_add_tail(&msg->queue, &ctlr->queue)` executes under `spin_lock_irqsave(&ctlr->queue_lock)`. At this instant, `msg->status` is set to `-EINPROGRESS`. Once `queue_lock` is released, the message is officially queued.

### 3.2 What guarantees exist after a successful return?
**Source Fact (`spi.c:4643`):**  
If `spi_async()` returns `0`, the Linux SPI core guarantees:
1. The message has been queued to `ctlr->queue` or handed to the active worker.
2. The controller queue worker or interrupt handler will process the message in FIFO order.
3. The callback `msg->complete(msg->context)` is guaranteed to be invoked unless the entire system panics or the controller hardware hangs permanently.

### 3.3 What happens if `spi_async()` returns an error?
**Source Fact (`spi.c:4631, 4637, 2275`):**  
`spi_async()` can return:
- `-EBUSY` (if `ctlr->bus_lock_flag` is set).
- `-ESHUTDOWN` (if `!ctlr->running`, e.g., controller queue is being destroyed).
- `-ENOTSUPP` (if `!ctlr->transfer`).
- Negative error from `spi_maybe_optimize_message()`.

**Critical Consequence:** In all error cases, `list_add_tail()` was **never executed**. The message is **NOT** queued. The SPI core will **NEVER** invoke `msg->complete()`.

### 3.4 Does the callback reference need to be released by the submitting thread on failure?
**Architectural Proof:**  
YES. Stage D7.8 proposed initializing `kref = 2` (Ref 1: Waiter, Ref 2: Callback). If `spi_async()` fails, the callback will never run to drop Ref 2. If the waiter thread only drops its own reference (Ref 1), `kref` remains at 1 forever, leaking the transaction and DMA buffer!  
**Required Implementation Pattern:**
```c
kref_init(&xfer->kref);          /* Count = 1 (Waiter Ref) */
kref_get(&xfer->kref);           /* Count = 2 (Callback Ref) */

ret = spi_async(spi, &xfer->msg);
if (ret < 0) {
    /* SUBMISSION FAILED: Callback will NEVER run. Drop Callback Ref! */
    kref_put(&xfer->kref, fte3600_xfer_release);
    /* Now drop Waiter Ref during error unwind */
    kref_put(&xfer->kref, fte3600_xfer_release);
    return ret;
}
```

### 3.5 Can the callback execute before `spi_async()` returns?
**Source Fact (`spi.c:2284`):**  
YES. Line 2284 invokes `kthread_queue_work(ctlr->kworker, &ctlr->pump_messages)`. On an SMP kernel (`PREEMPT_DYNAMIC`), the kworker thread running on a sibling CPU core can awaken, dequeue the message, execute the transfer, and invoke `msg->complete()` **before the submitting thread executes line 4641 (`spin_unlock_irqrestore`) or line 4643 (`return ret`)**.  
Therefore, initializing `kref = 1` and bumping to 2 *after* `spi_async()` returns would create an catastrophic use-after-free race. `kref = 2` **must be set before calling `spi_async()`**.

---

## 4. Reference Ownership Timeline

```mermaid
sequenceDiagram
    autonumber
    participant W as Waiter Thread (Process)
    participant K as Kernel SPI Core
    participant C as SPI Complete Callback (Atomic)

    Note over W: 1. Allocates xfer + rx_buf
    W->>W: kref_init(&xfer->kref) [kref = 1: Waiter]
    W->>W: kref_get(&xfer->kref)  [kref = 2: Waiter + Callback]

    alt Path 1: Successful Submission & Normal Completion
        W->>K: spi_async(spi, &xfer->msg) -> returns 0
        W->>W: wait_for_completion_timeout(250ms)
        K-->>C: Interrupt / DMA finishes
        C->>C: kref_put(&xfer->kref) [kref: 2 -> 1]
        C->>W: complete(&xfer->done)
        W->>W: Wakes up; reads data; kref_put() [kref: 1 -> 0 -> FREED]
    else Path 2: Submission Failure
        W->>K: spi_async(spi, &xfer->msg) -> returns -EBUSY / -ESHUTDOWN
        Note over W: Callback will NEVER execute!
        W->>W: kref_put() [Callback Ref: 2 -> 1]
        W->>W: kref_put() [Waiter Ref: 1 -> 0 -> FREED]
        W-->>W: Return -EBUSY (Zero Memory Leak)
    else Path 3: Timeout Before Completion
        W->>K: spi_async(spi, &xfer->msg) -> returns 0
        W->>W: wait_for_completion_timeout(250ms) -> EXPIRES
        W->>W: state = TIMED_OUT; kref_put() [Waiter Ref: 2 -> 1]
        Note over W: rx_buf REMAINS PINNED (Ref 2 held by Callback)
        Note over C: Late Callback arrives 100ms later
        C->>C: Sees TIMED_OUT; kref_put() [Callback Ref: 1 -> 0 -> FREED]
    end
```

---

## 5. Audit Verdict

**Classification:** SOURCE-PROVEN (Linux Kernel 7.2.8 `drivers/spi/spi.c`).  
**Remediation Mandated for Stage D8:** Submission failure path must explicitly release the callback reference to avoid permanent memory leak.
