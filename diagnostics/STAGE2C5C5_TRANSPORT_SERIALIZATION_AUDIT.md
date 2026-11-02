# Stage 2C.5C.5 — Transport Serialization and State Getter Audit

## Executive Summary

This document audits the serialization of operations and state inspection functions across concurrent callers in [`src/fw9369_transport_hw.c`](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c).

The audit confirms that all SPI transactions, register reads/writes, lock-state modifications, and telemetry updates are strictly serialized under `ctx->lock`. However, the audit identifies a specific data race vulnerability in `fw9369_hw_get_last_tx()` where a pointer into internal mutable context memory is returned without ongoing synchronization.

---

## 1. Serialization Matrix across Operations

All transport operations use `t->transfer()` as their common funnel, mapping to `hw_transfer()`:

| Operation Category | Synchronization Mechanism | Lock Scope | Race Hazard Eliminated |
| :--- | :--- | :--- | :--- |
| **Normal Register Reads** (Byte & Word) | `pthread_mutex_lock(&ctx->lock)` | Entire validation + `ioctl` + logging | No interleaved transactions on SPI bus |
| **Normal Register Writes** (Byte & Word) | `pthread_mutex_lock(&ctx->lock)` | Entire validation + `ioctl` + logging | Atomic register updates |
| **Lock/Unlock Commands** (`0x9A = 0x5A / 0x00`) | `pthread_mutex_lock(&ctx->lock)` | Entire validation + in-flight transition + `ioctl` + state resolution | In-flight states cannot be corrupted |
| **Protected Trim Writes** (`0x00C0..0x00C2`) | `pthread_mutex_lock(&ctx->lock)` | Entire validation + `ioctl` + logging | Blocked if another thread is unlocking |
| **FIFO Burst Reads** (10,246 bytes) | `pthread_mutex_lock(&ctx->lock)` | Entire validation + `ioctl` + logging | Burst transfer cannot be interrupted |
| **Telemetry Logging** (`ctx->log[]`) | `pthread_mutex_lock(&ctx->lock)` | Protected inside `hw_transfer()` | Zero log corruption or lost entries |

---

## 2. State Inspection Functions: Copy-by-Value vs. Internal Pointers

The audit evaluated all getter functions to determine whether they return thread-safe copied values or pointers to mutable internal data:

```c
/* 1. fw9369_hw_get_tx_count (L601) */
size_t count = ctx->log_count;
return count; /* Copied by value: THREAD-SAFE */

/* 2. fw9369_hw_get_fd (L629) */
int fd = ctx->fd;
return fd; /* Copied by value: THREAD-SAFE */

/* 3. fw9369_hw_is_protected_unlocked (L640) */
bool unlocked = (ctx->lock_state == FW9369_LOCK_STATE_ASSUMED_UNLOCKED);
return unlocked; /* Copied by value: THREAD-SAFE */

/* 4. fw9369_hw_get_lock_state (L651) */
enum fw9369_lock_state state = ctx->lock_state;
return state; /* Copied by value: THREAD-SAFE */
```

### Critical Vulnerability in `fw9369_hw_get_last_tx()`
In [src/fw9369_transport_hw.c:612-627](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L612-L627):
```c
const uint8_t *fw9369_hw_get_last_tx(struct fw9369_transport *t, size_t *out_len)
{
    if (!t || !t->context)
        return NULL;
    struct hw_context *ctx = t->context;
    pthread_mutex_lock(&ctx->lock);
    if (ctx->log_count == 0) {
        pthread_mutex_unlock(&ctx->lock);
        return NULL;
    }
    if (out_len)
        *out_len = ctx->log[ctx->log_count - 1].len;
    const uint8_t *ptr = ctx->log[ctx->log_count - 1].tx_prefix;
    pthread_mutex_unlock(&ctx->lock);
    return ptr;
}
```

#### Hazard Analysis:
1. `ptr` points directly into the internal array `ctx->log[...].tx_prefix`.
2. At line 625, `pthread_mutex_unlock(&ctx->lock)` releases the lock.
3. Once the caller receives `ptr`, the mutex is **no longer held**.
4. **Data Race / Invalidation Scenario:**
   - Thread A calls `fw9369_hw_get_last_tx()` and receives `ptr`.
   - Before Thread A finishes reading through `ptr`, Thread B submits new transfers.
   - If log entries are recycled, or if `destroy()` frees `ctx`, `ptr` becomes a **dangling pointer** or points to overwritten data.
5. **Architectural Recommendation:**
   - In production API design, `fw9369_hw_get_last_tx()` should accept a caller-supplied buffer and copy the prefix bytes under the lock (`memcpy(out_buf, entry->tx_prefix, len)`), rather than returning an internal raw pointer.
