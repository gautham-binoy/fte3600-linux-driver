# Stage D7.1 — Adversarial Audit of Sensor Arming Lifecycle & Event State Transitions

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source and Documentation Audit  
**Audit Classification:** Adversarial Challenge & Evidence Qualification  

---

## 1. Executive Summary & Challenge to Stage D7 Findings

In Stage D7, the report observed that repeated arming is standard across finger touch/release cycles and concluded: *"Repeated arming is standard and completely safe."*

This Stage D7.1 audit independently challenges the scope of that safety claim:
1. **Context-Dependent Safety:**  
   Repeated arming is safe **only within an unbroken, fully calibrated operational session**. Calling `arm()` or `arm_detection()` outside this strict lifecycle—such as after a failed calibration, after an electrical reset pulse, or across power transitions—is severely unsafe.
2. **Missing Prerequisite Enforcements:**  
   If `arm()` is invoked when baselines are uninitialized (`fdt_down == 0`), the driver writes zero thresholds to the hardware comparator bank. In that state, ambient thermal noise triggers continuous spurious interrupts, flooding the host CPU.
3. **Reset Invalidation:**  
   Because the comparator threshold bank (`0x80E0`/`0x80B0`) is volatile, a hardware reset pulse via Pad 14 erases the thresholds. Re-arming without re-running FDT and image baseline will fail silently.

---

## 2. Reconstructed Touch Lifecycle & Event State Machine

Vendor source evidence ([`fw9369_release.c`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_release.c), [`device_session.c`](file:///home/titan/fte3600-linux-driver/research/libfprint-fte3600/libfprint/drivers/fte3600/device_session.c)) establishes an authentic 6-stage operational cycle:

```
                      ┌────────────────────────────────────────┐
                      │ 1. Down Arm (FTE3600_FW9369_DOWN)      │
                      │    - Threshold bank = fdt_down         │
                      │    - 0x1881 mode = 0x81                │
                      │    - Strobe 0xC2 0x3D issued           │
                      └──────────────────┬─────────────────────┘
                                         │
                        [User touches finger to sensor]
                                         │
                                         ▼
                      ┌────────────────────────────────────────┐
                      │ 2. Touch Down Event (IRQ Pad 229)      │
                      │    - 0x1A82 bit 0 asserted             │
                      │    - Host reads & ACKs via 0x1A84      │
                      └──────────────────┬─────────────────────┘
                                         │
                                         ▼
                      ┌────────────────────────────────────────┐
                      │ 3. Image Acquisition                   │
                      │    - Exposure strobe 0xC4 0x3B         │
                      │    - Poll 0x80 == 0x54                 │
                      │    - Stream 10,246 bytes from 0x9A05   │
                      │    - ACK FIFO via 0x1A84 = 0x0020      │
                      └──────────────────┬─────────────────────┘
                                         │
                                         ▼
                      ┌────────────────────────────────────────┐
                      │ 4. Up Arm (FTE3600_FW9369_UP)          │
                      │    - Threshold bank = fdt_up           │
                      │    - 0x1881 mode = 0x00                │
                      │    - Strobe 0xC2 0x3D issued           │
                      └──────────────────┬─────────────────────┘
                                         │
                        [User lifts finger from sensor]
                                         │
                                         ▼
                      ┌────────────────────────────────────────┐
                      │ 5. Touch Up Event (IRQ Pad 229)        │
                      │    - 0x1A82 bit 1 asserted             │
                      │    - Host reads & ACKs via 0x1A84      │
                      └──────────────────┬─────────────────────┘
                                         │
                                         ▼
                      ┌────────────────────────────────────────┐
                      │ 6. Baseline Maintenance & Re-Arm       │
                      │    - Optional warm baseline update     │
                      │    - Re-issue Down Arm (Return to 1)   │
                      └────────────────────────────────────────┘
```

---

## 3. Detailed Audit of Call Sites & Safety Gates

### 3.1 Initial Arm Call (`fw9369_startup.c:386–421`)
- Called at the end of the full startup sequence.
- Writes `op->s->fdt_down` to the comparator bank, sets `0x1881 = 0x81`, clears interrupts with `0x002F`, and fires strobe `0xC2 0x3D`.
- **Precondition Gap in `run()`:**  
  As discovered in Stage D7.1, if `fdt_baseline()` fails, `run()` still attempts `arm()` unless `result > STARTUP_FAILED`! Only the outer caller in `device_session.c:700` checks `completed == 7` and refuses to accept the session.

### 3.2 Runtime Release Arming (`fw9369_release.c:243–293`)
- Implements `arm_detection()`, supporting explicit direction parameter: `FTE3600_FW9369_DOWN` vs `FTE3600_FW9369_UP`.
- Checks:
  ```c
  if (!valid_state (s) || !valid_io (io) ||
      (selection != FTE3600_FW9369_DOWN && selection != FTE3600_FW9369_UP))
      return FTE3600_FW9369_RELEASE_INVALID_ARGUMENT;
  ```
- **Safety Vulnerability Identified:**  
  `valid_state()` validates pointer non-nullity and channel mode, but **does NOT verify that `s->baseline_flag_1` or `s->baseline_flag_2` is non-zero!**  
  Calling `arm_detection()` on an uncalibrated structure is permitted by `fw9369_release.c`, pushing uncalibrated data directly into hardware registers!

### 3.3 Linux-Specific Event Masking (`fw9369_release.c:282–288`)
- When `linux_detection == 1`:
  ```c
  if (!word (0x1a83, &value, io, d))
      return FTE3600_FW9369_RELEASE_WORD_READ_ERROR;
  if (!write_word (0x1a83, (uint16_t) ((value & 0xff80) | 0x16), io, d))
      return FTE3600_FW9369_RELEASE_TRANSPORT_ERROR;
  ```
- Sets bits 4, 2, 1 (`0x0016`) in `0x1A83`.
- This ensures that Linux event routing persists across every Down and Up arm cycle.

---

## 4. Failure Modes & Safety Evaluation

| Invocation Scenario | Vendor Code Behavior | Hardware Blast Radius | Safety Verdict |
|---|---|---|---|
| **Normal Touch Cycle (Down -> Capture -> Up -> Down)** | Alternates threshold banks cleanly | Expected low-power monitoring | **SAFE** |
| **Arm called when uncalibrated (`fdt_down == 0`)** | Permitted by `arm_detection()`; writes 0 to bank | Floods Pad 229 with continuous noise interrupts | **UNSAFE / BUG** |
| **Arm called after hardware reset pulse** | Volatile bank lost; writes unverified state | Sensor comparator stalls or fails to trigger | **UNSAFE** |
| **Arm called after SPI transport error** | Bypasses strobe; leaves partial register writes | Comparator engine left in indeterminate state | **UNSAFE** |
| **Arm called during active finger touch** | Overwrites threshold bank while finger is down | Can deadlock Up/Down state machine | **UNSAFE** |

---

## 5. Architectural Rule for Future Implementation

To prevent the vulnerabilities discovered in vendor userspace code:

1. **State-Gated Arming:**  
   The kernel driver must refuse any arming request unless internal state is strictly `FDT_CALIBRATED` and `IMAGE_BASELINE_ACQUIRED`.
2. **Baseline Non-Zero Assertion:**  
   The driver must verify that `fdt_down_crc` and `fdt_up_crc` match the stored thresholds, rejecting all-zero or corrupted threshold banks with `-EINVAL`.
3. **Reset Invalidates Arming:**  
   Issuing `FTE3600_IOC_RESET` must immediately demote driver state to `RESET` (1), completely invalidating armed status and requiring a full re-calibration before `arm()` can be re-executed.
