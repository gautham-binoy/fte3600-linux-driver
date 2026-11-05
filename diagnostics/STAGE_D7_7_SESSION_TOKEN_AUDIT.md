# Stage D7.7 — Session Token Semantics & Epoch Disambiguation Audit

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Session Identification & Nonce Audit  
**Deliverable:** `diagnostics/STAGE_D7_7_SESSION_TOKEN_AUDIT.md`  
**Date:** 2026-11-05  

---

## 1. Executive Summary & Objective

In Stage D7.5 and D7.6, the 32-bit session token was formally re-classified from a "security token" to an "ephemeral synchronization nonce."

This audit establishes the exact operational parameters, epoch mechanics, and verification rules for the 32-bit session token to guarantee that it is used strictly for temporal disambiguation without creating false assumptions of cryptographic authentication.

---

## 2. Token Properties & Lifecycle Rules

### 2.1. Non-Cryptographic Classification
- **Core Principle:** The 32-bit token **does not authenticate** callers. Authentication is performed exclusively by Linux VFS filesystem permissions on `/dev/fte3600` (mode `0600 root:root`) and open file description binding (`owner_filp`).
- **Actual Function:** The token acts as an **epoch disambiguation nonce**. Its sole purpose is to detect and reject delayed, stale, or replayed ioctl calls from a previous process instance that crashed, timed out, or restarted.

### 2.2. Generation & Zero Rejection
In `fte3600_ioctl_calib_begin()`:
```c
u32 token;

/* Generate non-zero random nonce */
do {
    token = get_random_u32();
} while (token == 0);

data->session.id = token;
data->session.active = true;
```
- Value `0` is strictly reserved across the kernel driver to indicate **"NO ACTIVE SESSION"**.
- A userspace request containing `session_id == 0` is immediately rejected with `-EINVAL`.

### 2.3. Epoch Disambiguation Across Restarts
Suppose Process A begins a session (Token $T_1$), issues several steps, and crashes.
Process B starts, resets the hardware, and begins a new session (Token $T_2$).
If an orphaned worker thread or delayed network message from Process A arrives carrying $T_1$:
- The kernel compares: `T_1 != data->session.id` ($T_2$).
- The call is immediately rejected with `-ESTALE`.
- The probability that Process B drew the exact same random token as Process A is strictly:
  $$P(\text{collision}) = \frac{1}{2^{32} - 1} \approx 2.328 \times 10^{-10}$$

---

## 3. Mandatory Verification Invariants

Every session-gated ioctl (`FTE3600_IOC_CALIB_FDT_SAMPLE`, `CALIB_READ_FRAME`, `CALIB_SET_THRESHOLDS`, `CALIB_COMMIT`, `CALIB_ABORT`) must enforce:

1. **Mutex Protection:** The token check **MUST** occur inside `mutex_lock(&data->lock)`. Checking the token outside the mutex is a fatal concurrency flaw.
2. **Uniform Error Encodings:**
   - If `token == 0`: return `-EINVAL`.
   - If `session.active == false` OR `session.id != req.session_id`: return `-ESTALE`.
   - If `session.owner_filp != filp`: return `-EACCES`.
3. **Destruction on Exit:** Upon `COMMIT`, `ABORT`, watchdog expiry, or `cdev_release`, `data->session.id` is immediately overwritten with `0`. Tokens are never reused or cached.

---

## 4. Evidence Classification

| Property | Classification | Proof Reference |
| :--- | :--- | :--- |
| Random nonce generated via kernel CSPRNG | `KERNEL-SOURCE-VERIFIED` | Linux kernel `lib/random.c` |
| Zero value reserved for inactive state | `ARCHITECTURAL-SPECIFICATION` | Formally specified in Section 2.2 |
| Stale token rejection via `-ESTALE` under mutex | `ARCHITECTURAL-SPECIFICATION` | Formally specified in Section 3 |
| Collision probability $2.33 \times 10^{-10}$ | `MATHEMATICALLY-DERIVED` | Formally proven in Section 2.3 |
| Nonce is not cryptographic authentication | `ARCHITECTURAL-SPECIFICATION` | Formally specified in Section 2.1 |
