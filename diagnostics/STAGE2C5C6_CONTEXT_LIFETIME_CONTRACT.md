# Stage 2C.5C.6 — Context Lifetime Contract and Teardown Semantics

**Target Silicon:** FocalTech FT9362 / FW9369  
**Component:** `tools/fw9369-mock/src/fw9369_transport_hw.c`, `fw9369_transport_mock.c`  
**Evaluation Mode:** Architectural Specification & Formal Contract Definition

---

## 1. Resolution of Destruction Contract Inconsistency

In previous stages, an apparent tension existed between two testing paradigms:
1. **Test 14.2:** Asserted that `fw9369_hw_transport_destroy()` can be invoked while an active transfer thread is executing its SPI ioctl, and that destruction must block until the transfer finishes before deallocating memory.
2. **Stage 2C.5C.5 Audit:** Noted that in standard C programming, calling `destroy(t)` while worker threads are actively dispatching calls through `t->transfer(t->context, ...)` is fundamentally unsafe due to the lack of object-level reference counting on the `struct fw9369_transport` pointer itself.

Stage 2C.5C.6 formally reconciles this distinction.

---

## 2. Operational Caller Contract (Single-Owner Model)

For production applications and drivers utilizing the FW9369 transport layer:

> [!IMPORTANT]
> **Operational Teardown Contract:**  
> The transport context possesses **single-owner lifetime semantics**. The owner thread/process responsible for instantiating the transport via `fw9369_hw_transport_create()` is solely responsible for destroying it via `fw9369_hw_transport_destroy()`.  
> Prior to calling `fw9369_hw_transport_destroy()`, the owner MUST:
> 1. Stop dispatching new transactions to worker threads.
> 2. Synchronize, signal, or join all worker threads (`pthread_join()`).
> 3. Verify that no thread can dereference `t` or `t->context`.
> 4. Call `fw9369_hw_transport_destroy(t)`.

### 2.1 Why Thread Joining Is Required Before Destruction in C
In standard C (unlike garbage-collected languages or C++ with `std::shared_ptr`):
- A caller executing `t->transfer(t->context, tx, rx, len)` dereferences `t->transfer` and passes `t->context` as an argument before the transport function can even execute its first instruction or acquire `ctx->lock`.
- If another thread executes `free(t)` concurrently, the caller will dereference `t` after it has been freed (use-after-free).
- Therefore, no internal lock inside `struct hw_context` can ever protect the outer `struct fw9369_transport` wrapper from concurrent deallocation if callers do not synchronize before destroying the context.
- This contract is verified in **Suite 14 Test 9**.

---

## 3. Defense-in-Depth Within the Transport Layer

While the caller is contractually obligated to join worker threads, the transport implementation provides internal defense-in-depth:

```c
void fw9369_hw_transport_destroy(struct fw9369_transport *t)
{
    if (t) {
        if (t->context) {
            struct hw_context *ctx = t->context;
            pthread_mutex_lock(&ctx->lock);
            ctx->is_shutting_down = true;
            hw_close(ctx);
            pthread_mutex_unlock(&ctx->lock);
            pthread_mutex_destroy(&ctx->lock);
            free(ctx);
        }
        free(t);
    }
}
```

### 3.1 Role of Test 14.2 vs Test 14.9
- **Test 14.2 (In-Flight SPI Drain Stress Test):** Simulates the specific edge case where a worker thread has already entered `hw_transfer()` and is blocked inside the SPI ioctl when `destroy()` is called. Test 14.2 proves that `destroy()` acquires `ctx->lock`, waits for the SPI ioctl to finish completely, and drains the in-flight operation safely before memory deallocation proceeds.
- **Test 14.9 (Contract Compliance Verification):** Demonstrates the compliant operational lifecycle: multiple concurrent worker threads execute 100 total transactions, the owner thread cleanly joins all workers, and then destroys the transport without data races or leaks.

---

## 4. Contract Summary Table

| Scenario | Caller Obligation | Transport Guarantee | Test Verification |
|---|---|---|---|
| Normal Operation | Pass valid buffers and allowed opcodes | Mutual exclusion across all operations | Suite 14 Test 1 |
| Post-Shutdown Transfer | Avoid calling transfer after shutdown | Clean rejection with `-ESHUTDOWN`, no ioctl | Suite 14 Test 5, 8, 10 |
| Telemetry Readout | Provide allocated buffer with capacity | Atomic snapshot copy-out under lock | Suite 14 Test 6 |
| Operational Teardown | Join all worker threads before calling `destroy()` | Clean teardown, fd closed, memory freed | Suite 14 Test 9 |
| In-Flight ioctl at Teardown | Handled via caller shutdown coordination | Destructor blocks until in-flight ioctl drains | Suite 14 Test 2 |
