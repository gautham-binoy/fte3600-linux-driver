# Stage 2C.5C.7 — Teardown and Shutdown Limitations Audit

**Target Silicon:** FocalTech FT9362 / FW9369  
**Component:** `tools/fw9369-mock/src/fw9369_transport_hw.c`  
**Audit Scope:** Kernel System Call Interfaces, In-Flight SPI ioctl Cancellation & Bounded Teardown  
**Audit Mode:** Software-Only Static Analysis & Systems Architecture Review

---

## 1. Analysis of Bounded Destruction Claims

A critical question in real-time and device-driver software architecture is:  
**Can `fw9369_hw_transport_destroy()` guarantee a bounded teardown duration?**

### Finding: **NO. Userspace CANNOT guarantee a bounded destruction time in isolation.**

Destruction duration is strictly bounded if and only if all in-flight kernel SPI transactions complete within a bounded time. If the kernel driver or hardware bus hangs indefinitely during an SPI transfer, userspace teardown will block indefinitely waiting for `ctx->lock`.

---

## 2. Technical Explanation of Kernel and Userspace Boundaries

### 2.1 The Synchronous SPI ioctl Call
In Linux userspace SPI communication (`spidev`), transactions are submitted via:
```c
ioctl(ctx->fd, SPI_IOC_MESSAGE(1), &tr);
```
- This ioctl invokes the kernel `spidev_ioctl()` function, which dispatches `spi_sync()` to the underlying SPI controller driver (e.g., `spi-intel` or `designware-spi`).
- The userspace thread transitions into kernel mode and enters a sleeping state (often `TASK_UNINTERRUPTIBLE`) awaiting the hardware interrupt or DMA completion event.
- Until the SPI controller's interrupt service routine (ISR) fires or the kernel driver's internal transfer timer expires, the thread does not return from the `ioctl()` system call.

### 2.2 Why Userspace Mutex Timeouts Cannot Cancel an In-Flight ioctl
Some systems attempt to use timed mutexes (`pthread_mutex_timedlock()`) in the destructor:
- Even if the destructor thread times out waiting for `ctx->lock`, the transfer thread is **still blocked inside the kernel `ioctl` holding the lock**.
- If the destructor were to proceed and deallocate `ctx` anyway, the moment the transfer thread eventually returns from kernel space, it will execute user instructions touching freed memory, leading to memory corruption, heap corruption, or `SIGSEGV`.
- A mutex timeout only bounds the waiting of the *destructor thread*; it **cannot cancel the physical transfer or the worker thread**.

### 2.3 Unsafe Cancellation Anti-Patterns (Explicitly Prohibited)
The following techniques are well-known anti-patterns that create severe hazards and are strictly prohibited in this codebase:
1. **Closing the File Descriptor Concurrently (`close(fd)`):**
   - Attempting to close an `fd` while another thread is blocked in `ioctl(fd, ...)` violates POSIX specifications (IEEE Std 1003.1 Section 2.9.7).
   - In Linux kernels, closing a descriptor blocked in another thread causes race conditions with descriptor reuse (`EBADF` or targeting a completely different device opened by another subsystem).
2. **Asynchronous Thread Cancellation (`pthread_cancel()`):**
   - Canceling a thread executing an ioctl or holding a mutex leaves mutexes locked permanently, leaks heap memory, and risks leaving the sensor in a vulnerable, unlocked state (e.g. register `0x9A` left open).
3. **Forced Context Deallocation:**
   - Freeing `ctx` while a worker thread is running causes immediate undefined behavior.

---

## 3. Lawful and Documented Mitigation Path

Because forced cancellation is unsafe, the driver relies on safe, documented Linux kernel mechanisms:

1. **Kernel-Level SPI Driver Timeouts:**
   - Linux SPI controller drivers implement hardware timeout watchdogs (typically 500 ms to 2000 ms) via `wait_for_completion_timeout()`.
   - If the bus or peripheral fails to clock data, the kernel controller driver times out, aborts the hardware DMA/FIFO, and returns an error code (such as `-ETIMEDOUT` or `-EIO`) to userspace.
   - Upon receiving this error, `hw_transfer()` updates the software lock state to `FW9369_LOCK_STATE_UNKNOWN`, logs the failure, and releases `ctx->lock`.
   - The destructor, which was blocked in `pthread_mutex_lock(&ctx->lock)`, immediately unblocks, cleans up the transport, and returns.

2. **Caller Pre-Teardown Coordination:**
   - In production, higher-level frameworks (such as libfprint or driver daemon threads) must implement timeout boundaries before calling `destroy()`. If a worker thread does not return from an operation within expected tolerances, the parent process can trigger a hardware reset or subsystem restart.

---

## 4. Summary Table of Shutdown Boundary Guarantees

| Condition | Kernel Behavior | Transport Behavior | Teardown Bounded? |
|---|---|---|---|
| Normal Operation | ioctl completes in microseconds | Destructor waits for current transaction (<1 ms), then tears down | **YES (Bounded <5 ms)** |
| Controller Timeout | Kernel controller driver times out (e.g. 1 sec) and returns `-ETIMEDOUT` | Destructor waits until kernel returns, marks state `UNKNOWN`, tears down | **YES (Bounded by kernel timeout)** |
| Hardware / Bus Freeze | SPI controller driver has no timeout or hangs in `TASK_UNINTERRUPTIBLE` | Destructor waits indefinitely on `ctx->lock` | **NO (Indefinite wait; requires system reset)** |

**Audit Conclusion:** Bounded shutdown is strictly contingent on kernel controller driver completion. The transport implementation correctly avoids unsafe forced cancellation and truthfully documents this boundary.
