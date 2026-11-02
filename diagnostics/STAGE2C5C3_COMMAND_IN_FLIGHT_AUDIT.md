# Stage 2C.5C.3 — Command-in-Flight States Audit

## Executive Summary

This audit evaluates the correctness, temporal semantics, and safety invariants of the ephemeral in-flight states introduced in Stage 2C.5C.2:
- `FW9369_LOCK_STATE_UNLOCK_COMMAND_SENT`
- `FW9369_LOCK_STATE_LOCK_COMMAND_SENT`

The audit verifies that these states prevent premature access to protected registers while an SPI transaction is active in the kernel, and evaluates whether any code paths could leave the transport in an inconsistent or orphaned state.

---

## 1. Timing and Source Verification

### Pre-Ioctl Assignment
In [fw9369_transport_hw.c:340-352](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L340-L352):

```c
enum fw9369_lock_state prev_lock_state = ctx->lock_state;
(void)prev_lock_state;
bool is_unlock_cmd = (len == 4 && tx[0] == FW9369_OP_BYTE_WRITE &&
                      tx[2] == FW9369_REG_PROTECT_9A && tx[3] == FW9369_KEY_UNLOCK);
bool is_lock_cmd   = (len == 4 && tx[0] == FW9369_OP_BYTE_WRITE &&
                      tx[2] == FW9369_REG_PROTECT_9A && tx[3] == FW9369_KEY_LOCK);

if (is_unlock_cmd) {
    ctx->lock_state = FW9369_LOCK_STATE_UNLOCK_COMMAND_SENT;
} else if (is_lock_cmd) {
    ctx->lock_state = FW9369_LOCK_STATE_LOCK_COMMAND_SENT;
}
```

The system call invocation occurs at [fw9369_transport_hw.c:367](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L367):
```c
int ret = ctx->ops.ioctl_fn(ctx->fd, SPI_IOC_MESSAGE(1), &tr);
```

### Temporal Safety Proof
- **Verified from source code:** The assignment of `ctx->lock_state` occurs **strictly prior** to the execution of `ioctl_fn()`.
- **Zero Window Vulnerability:** There is no execution window where an unlock or lock packet is being shifted over SPI while software still reports the previous state.

---

## 2. Control Flow and Early Return Audit

The code block between in-flight assignment (line 351) and state resolution (line 390) was audited for early exits:

```c
357:    struct spi_ioc_transfer tr;
358:    memset(&tr, 0, sizeof(tr));
359:    tr.tx_buf = (unsigned long)tx;
360:    tr.rx_buf = (unsigned long)rx;
361:    tr.len = (uint32_t)len;
362:    tr.speed_hz = ctx->config.speed_hz;
363:    tr.delay_usecs = 0;
364:    tr.bits_per_word = ctx->config.bits_per_word;
365:    tr.cs_change = 0;
366:
367:    int ret = ctx->ops.ioctl_fn(ctx->fd, SPI_IOC_MESSAGE(1), &tr);
368:    int status = 0;
369:
370:    if (ret < 0) {
371:        status = -errno;
372:    } else if ((size_t)ret < len) {
373:        status = -EMSGSIZE;
374:    } else if ((size_t)ret > len) {
375:        status = -EPROTO;
376:    } else {
377:        status = 0;
378:    }
379:
380:    /* Outcome handling begins at line 390 */
```

### Audit Findings
1. **No Early Returns:** There are zero `return`, `goto`, or `break` statements between line 351 and line 390.
2. **Deterministic Resolution:** Regardless of whether `ioctl_fn` returns success, negative errno, short count, or overflow count, execution unconditionally flows into lines 390-402 to resolve the state.
3. **Signal Interruption Handling:** If `ioctl_fn` returns `-1` with `errno == EINTR`, `status` is set to `-EINTR`, and lines 394 / 400 safely transition `ctx->lock_state` to `FW9369_LOCK_STATE_UNKNOWN`.

---

## 3. Protected Trim Access Rejection During In-Flight Window

A critical safety invariant is that while an unlock command is being shifted across the bus, software must NOT allow protected trim writes (`0x00C0..0x00C2`) to proceed prematurely.

### Allowlist Evaluation
In [fw9369_transport_hw.c:294-298](file:///home/titan/fte3600-linux-driver/tools/fw9369-mock/src/fw9369_transport_hw.c#L294-L298):
```c
if (reg >= 0x00C0 && reg <= 0x00C2) {
    if (ctx->lock_state == FW9369_LOCK_STATE_ASSUMED_UNLOCKED)
        packet_valid = true;
} else {
    packet_valid = true;
}
```

- When `ctx->lock_state == FW9369_LOCK_STATE_UNLOCK_COMMAND_SENT`, `packet_valid` remains `false`.
- The protected write is immediately rejected with `-EPERM` at line 334.
- Similarly, when `ctx->lock_state == FW9369_LOCK_STATE_LOCK_COMMAND_SENT`, `packet_valid` remains `false`, rejecting protected writes.

### Offline Test Verification
In `tests/test_runner.c` (Test Suite 12, Tests 6 & 7):
- An ioctl hook intercepted the transfer during `UNLOCK_COMMAND_SENT` and attempted a write to `0x00C0`. The transfer returned `-EPERM` without submitting an ioctl (`hook_in_flight_unlock_trim_rejected == true`).
- The same hook intercepted the transfer during `LOCK_COMMAND_SENT` and attempted a write to `0x00C0`. The transfer returned `-EPERM` (`hook_in_flight_lock_trim_rejected == true`).

---

## 4. Limitations of In-Flight States

1. **Single-Threaded Assumption:** The in-flight states are stored in plain memory (`ctx->lock_state`) without atomic primitives. They protect against re-entrancy, but do NOT provide multithreaded synchronization.
2. **Abrupt Process Termination:** If a process is terminated by the kernel (`SIGKILL`, OOM killer, power failure) while blocked inside `ioctl_fn()`, the userspace memory disappears. On subsequent startup, a fresh context starts in `UNKNOWN`, maintaining fail-closed safety.
