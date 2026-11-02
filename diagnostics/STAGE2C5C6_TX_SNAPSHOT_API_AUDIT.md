# Stage 2C.5C.6 — Telemetry Snapshot API and Public Interface Audit

**Target Silicon:** FocalTech FT9362 / FW9369  
**Host Architecture:** Linux x86_64 / Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Component:** `tools/fw9369-mock/src/fw9369_transport_hw.c`, `fw9369_transport_mock.c`  
**Evaluation Mode:** Offline Static Analysis & Unit Test Verification (Zero Hardware Access)

---

## 1. Vulnerability Analysis of Pointer-Returning Getter

In Stage 2C.5C.4 and Stage 2C.5C.5, `fw9369_hw_get_last_tx()` was implemented as:

```c
const uint8_t *fw9369_hw_get_last_tx(struct fw9369_transport *t, size_t *out_len)
{
    if (!t || !t->context) return NULL;
    struct hw_context *ctx = t->context;
    pthread_mutex_lock(&ctx->lock);
    if (ctx->log_count == 0) {
        pthread_mutex_unlock(&ctx->lock);
        return NULL;
    }
    const struct hw_log_entry *entry = &ctx->log[ctx->log_count - 1];
    if (out_len) *out_len = entry->len;
    const uint8_t *ptr = entry->tx_prefix;
    pthread_mutex_unlock(&ctx->lock);
    return ptr; /* HAZARD: Returning internal pointer after unlocking mutex */
}
```

### 1.1 Concurrency and Lifetime Hazards
1. **Dangling Pointer Hazard:** If another thread calls `fw9369_hw_transport_destroy(t)` immediately after `fw9369_hw_get_last_tx()` releases `ctx->lock`, `ctx` is freed. Any subsequent read by the caller through the returned pointer produces an immediate heap-use-after-free (`SIGSEGV` or reading garbage).
2. **Data Race Hazard:** The telemetry log is circular / bounded (`ctx->log[ctx->log_count++]`). If another thread initiates a new SPI transfer while the caller is inspecting the memory at `ptr`, the log slot can be overwritten mid-inspection, causing torn reads and corrupted telemetry assertions.
3. **Synchronization Inversion:** The internal mutex provided false safety: it protected only the reading of `entry->len` and index computation, but completely abandoned protection for the actual memory buffer.

---

## 2. Safe Copy-Out API Specification

To eliminate all memory lifetime and race hazards, the interface was refactored into a safe copy-out pattern:

```c
int fw9369_hw_get_last_tx(struct fw9369_transport *t, uint8_t *out_buf, size_t buf_cap, size_t *out_len);
int fw9369_mock_get_last_tx(struct fw9369_transport *t, uint8_t *out_buf, size_t buf_cap, size_t *out_len);
```

### 2.1 Formal Semantics & Error Codes
- **`t == NULL` or `t->context == NULL`:** Returns `-EINVAL`.
- **`out_buf == NULL` or `buf_cap == 0`:** Returns `-EINVAL`.
- **Empty Log (`ctx->log_count == 0`):** Returns `-ENODATA`.
- **Insufficient Buffer Capacity (`buf_cap < payload_len`):**
  - If `out_len != NULL`, `*out_len` is populated with `entry->len` so the caller knows the required allocation size.
  - Returns `-ENOBUFS`.
  - Mutex is released without modifying `out_buf`.
- **Success:**
  - Copies `min(entry->len, HW_MAX_LOG_PAYLOAD)` bytes directly into caller's buffer `out_buf` while holding `ctx->lock`.
  - If `out_len != NULL`, `*out_len` is set to `entry->len`.
  - Releases `ctx->lock`.
  - Returns the exact number of bytes copied (positive integer).

---

## 3. Public API Audit for Pointer-Returning Getters

A comprehensive audit of all functions exposed in `tools/fw9369-mock/include/` was conducted to verify that no other internal context pointers or un-synchronized references escape:

| Header | Function | Return Type | Thread-Safety / Mechanism | Audit Verdict |
|---|---|---|---|---|
| `fw9369_transport_hw.h` | `fw9369_hw_get_tx_count` | `size_t` | Passed by value under `ctx->lock` | Safe |
| `fw9369_transport_hw.h` | `fw9369_hw_get_last_tx` | `int` | Safe copy-out under `ctx->lock` | Safe |
| `fw9369_transport_hw.h` | `fw9369_hw_get_fd` | `int` | Scalar value under `ctx->lock` | Safe |
| `fw9369_transport_hw.h` | `fw9369_hw_is_protected_unlocked` | `bool` | Scalar boolean under `ctx->lock` | Safe |
| `fw9369_transport_hw.h` | `fw9369_hw_get_lock_state` | `enum fw9369_lock_state` | Scalar enum under `ctx->lock` | Safe |
| `fw9369_transport_hw.h` | `fw9369_lock_state_name` | `const char *` | Read-only static string literal | Safe |
| `fw9369_transport.h` | `fw9369_mock_get_tx_count` | `size_t` | Passed by value | Safe |
| `fw9369_transport.h` | `fw9369_mock_get_last_tx` | `int` | Safe copy-out into caller buffer | Safe |
| `fw9369_transport.h` | `fw9369_mock_get_reg_byte` | `uint8_t` | Scalar register value | Safe |
| `fw9369_transport.h` | `fw9369_mock_get_reg_word` | `uint16_t` | Scalar register value | Safe |
| `fw9369_transport.h` | `fw9369_mock_is_protected_locked` | `bool` | Scalar boolean | Safe |

**Audit Conclusion:** Zero raw internal pointers or unsubscribed references are returned by any public getter in the FW9369 transport interfaces.

---

## 4. Test Verification Summary

The safe copy-out implementation was verified in Suite 14 Test 6:
1. **Test 6a:** Calling `fw9369_hw_get_last_tx` on fresh transport returns `-ENODATA`.
2. **Test 6b:** Rejection of `NULL` transport, `NULL` destination buffer, and 0 capacity with `-EINVAL`.
3. **Test 6c:** Calling with `buf_cap < payload_len` returns `-ENOBUFS` and accurately populates `out_len`.
4. **Test 6d:** Accurate payload copy matching transmitted bytes byte-for-byte.
5. **Test 6e (Snapshot Immutability):** Caller snapshot remains 100% immutable and uncorrupted after subsequent SPI transfers update internal telemetry logs.
6. **Test 6f:** Mock backend `fw9369_mock_get_last_tx` achieves 1:1 functional parity across all error and success conditions.
