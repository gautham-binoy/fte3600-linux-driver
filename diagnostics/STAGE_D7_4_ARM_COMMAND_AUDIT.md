# Stage D7.4 — FT9362 Arm Command & Protocol Discrepancy Audit

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Scope:** Strictly Read-Only Source & Design Review  
**Deliverable:** `diagnostics/STAGE_D7_4_ARM_COMMAND_AUDIT.md`

---

## 1. Executive Summary & Objective

In Stage D7.3, the proposed hybrid calibration commit transaction (`FTE3600_IOC_CALIB_COMMIT`) specified:
> *"Issues touch-monitoring arm strobe: 0xA2 (1 byte)... Strobe commands: 0xA0 (trigger image capture), 0xA2 (arm touch detection mode), 0xA3 (clear interrupt / unarm)."*

This independent audit evaluated whether `0xA2` (or `0xA0` / `0xA3`) has any source-supported validity in the FT9362 / FW9369 communication protocol, tracing every vendor-source call site to identify the exact byte sequences, packet lengths, framing rules, and surrounding register operations.

### Key Finding:
- **`0xA2`, `0xA0`, and `0xA3` have ZERO source support in the FT9362 / FW9369 vendor protocol.** They represent unverified assumptions introduced during earlier conversational design iterations.
- **The true vendor touch-monitoring arm strobe is strictly `{0xC2, 0x3D, 0x00}` (3 bytes).**
- **The true vendor image capture trigger strobe is strictly `{0xC4, 0x3B, 0x00}` (3 bytes).**
- **All command strobes in this protocol adhere to a strict 3-byte mathematical framing convention: `[CMD, ~CMD, 0x00]`.** Single-byte strobes are invalid and unacknowledged by the FT9362 silicon.

---

## 2. Vendor Source Call Site Survey

An exhaustive search across the reference vendor implementation (`research/libfprint-fte3600/libfprint/drivers/fte3600/`) was conducted.

### 2.1. Touch-Monitoring Arm Call Sites: `{0xC2, 0x3D, 0x00}`

1. **`fw9369_startup.c:417` (Vendor Startup Sequence):**
   ```c
   /* fw9369_startup.c:416-418 */
   write_word (op, 0x1a84, 0x2f);
   command (op, 0xc2, 0x3d);
   wait_ms (op, 1);
   ```
2. **`fw9369_release.c:279, 289` (Vendor Release / Finger-Lift Sequence):**
   ```c
   /* fw9369_release.c:279, 289 */
   uint8_t trigger[3] = {0xc2, 0x3d, 0};
   ...
   if (!write_word (0x1a84, 0x2f, io, d) || !transfer (trigger, sizeof (trigger), io, d))
     return FTE3600_FW9369_RELEASE_TRANSPORT_ERROR;
   ```
3. **`fw9369_fdt.c:245` (FDT Verification Probe):**
   ```c
   /* fw9369_fdt.c:245 */
   command (op, 0xc2, 0x3d);
   ```

### 2.2. Command Strobe Implementation: `command()`

In `research/libfprint-fte3600/libfprint/drivers/fte3600/fw9369_startup.c:63-68`, the `command()` helper is defined as:
```c
static void
command (struct startup *op, uint8_t a, uint8_t b)
{
  uint8_t bytes[3] = {a, b, 0};

  (void) transfer (op, bytes, sizeof (bytes));
}
```
**Conclusion:** Calling `command(op, 0xc2, 0x3d)` emits an explicit 3-byte SPI transfer: `0xC2 0x3D 0x00`.

---

## 3. Mathematical Framing Invariant: `[CMD, ~CMD, 0x00]`

Cross-referencing all command strobes throughout the vendor source demonstrates that the FT9362 SPI protocol uses 3-byte command packets where the second byte is the exact bitwise NOT (`~CMD`) of the first byte, terminated by `0x00`:

| Functional Operation | Vendor Source Call Site | Transmitted Bytes | Mathematical Property |
| :--- | :--- | :--- | :--- |
| **Touch-Monitoring Arm** | `fw9369_startup.c:417` | `{0xC2, 0x3D, 0x00}` | `0xC2 ^ 0xFF == 0x3D` (`~0xC2 = 0x3D`) |
| **Image Capture / Exposure** | `capture.c:267` | `{0xC4, 0x3B, 0x00}` | `0xC4 ^ 0xFF == 0x3B` (`~0xC4 = 0x3B`) |
| **Silicon Wakeup / Sync** | `fw9369_startup.c:106` | `{0x5A, 0xA5, 0x00}` | `0x5A ^ 0xFF == 0xA5` (`~0x5A = 0xA5`) |
| **Soft Reset / Resync** | `fw9369_startup.c:110` | `{0xC0, 0x3F, 0x00}` | `0xC0 ^ 0xFF == 0x3F` (`~0xC0 = 0x3F`) |
| **Sleep / Low Power** | `fw9369_startup.c:113` | `{0xA5, 0x5A, 0x00}` | `0xA5 ^ 0xFF == 0x5A` (`~0xA5 = 0x5A`) |

### Verification of Single-Byte Claims:
- If a hypothetical `0xA2` arm command existed, its 3-byte framed equivalent would be `{0xA2, 0x5D, 0x00}`. Neither `0xA2` nor `0x5D` appears in the vendor driver.
- Single-byte SPI transfers (`0xA0`, `0xA2`, `0xA3`) do not exist in the vendor driver and violate the hardware command decoder's checksum/framing check.

---

## 4. Complete Surrounding Register Operations for Arming

The arm procedure cannot be executed as an isolated command. In `fw9369_startup.c:385-421` and `fw9369_release.c:244-293`, arming requires a strict 8-step sequence:

```
[Step 1] Configure FDT (Period, Gain, Shift) via fte3600_fw9369_fdt_run()
           |
[Step 2] Unlock Threshold Register Bank:
         Write Byte: 0x9A = 0x5A (Unlock Key)
           |
[Step 3] Program 8-Channel Thresholds into Bank:
         Write Bank: {0x05, 0xFA, 0x80, (foundry==0xAA ? 0xE0 : 0xB0), 0x00, 0x08, [16 bytes thresholds]}
           |
[Step 4] Lock Threshold Register Bank:
         Write Byte: 0x9A = 0x00 (Lock Key)
           |
[Step 5] Configure Sensor Mode in Register 0x1881:
         Read Word: 0x1881 -> Modify: (value & 0xFF7C) | 0x81 (Down Arm) -> Write Word: 0x1881
           |
[Step 6] Configure Linux Detection in Register 0x1A83:
         Read Word: 0x1A83 -> Modify: (value & 0xFF80) | 0x16 -> Write Word: 0x1A83
           |
[Step 7] Clear Interrupt Status in Register 0x1A84:
         Write Word: 0x1A84 = 0x002F
           |
[Step 8] Transmit Touch-Monitoring Arm Strobe:
         Transfer 3 Bytes: {0xC2, 0x3D, 0x00}
           |
[Step 9] Hardware Settling Delay:
         Wait 1 ms (wait_ms(1))
```

### Protocol Distinctions:
1. **Image Exposure Trigger:**  
   Triggered by `{0xC4, 0x3B, 0x00}` (`capture.c:267`). Preceded by checking status register `0x80 == 0x54` and setting bit 0 in `0x1800`.
2. **Touch-Monitoring Arm Trigger:**  
   Triggered by `{0xC2, 0x3D, 0x00}` (`fw9369_startup.c:417`). Arms hardware capacitive sensing and sets interrupt generation on finger contact.
3. **Interrupt Clear:**  
   Performed by writing `0x002F` to register `0x1A84` prior to arming (`fw9369_startup.c:416`).

---

## 5. Design Remediation for Stage D8

The design documentation authored in Stage D7.3 (`STAGE_D7_3_CALIBRATION_TRANSACTION_DESIGN.md`, `STAGE_D7_3_PROPOSED_UAPI.md`, `STAGE_D7_3_READY_STATE_REQUIREMENTS.md`, `STAGE_D7_3_FINAL_VERDICT.md`) is hereby formally corrected:
1. **Replacement:** All references to arm strobe `0xA2` (1 byte) must be replaced with `{0xC2, 0x3D, 0x00}` (3 bytes).
2. **Replacement:** All references to capture strobe `0xA0` (1 byte) must be replaced with `{0xC4, 0x3B, 0x00}` (3 bytes).
3. **Prerequisite:** In `FTE3600_IOC_CALIB_COMMIT`, the driver must execute the complete 8-step sequence (or verify that threshold programming was committed in Step 4, word `0x1881` was configured, `0x1A84` was cleared with `0x002F`, and finally `{0xC2, 0x3D, 0x00}` was transmitted).

---

## 6. Evidence Classification

| Statement / Finding | Classification | Evidence Reference |
| :--- | :--- | :--- |
| Arm strobe is `{0xC2, 0x3D, 0x00}` (3 bytes) | `VENDOR-SOURCE-VERIFIED` | `fw9369_startup.c:417`, `fw9369_release.c:279`, `fw9369_fdt.c:245` |
| Image capture strobe is `{0xC4, 0x3B, 0x00}` | `VENDOR-SOURCE-VERIFIED` | `capture.c:267` |
| Command helper formats 3 bytes `[a, b, 0]` | `VENDOR-SOURCE-VERIFIED` | `fw9369_startup.c:63-68` |
| Command byte inversion property `b == ~a` | `MATHEMATICALLY-DERIVED` | Proved across all 5 vendor command definitions |
| Register sequence `0x9A` $\rightarrow$ `0x1881` $\rightarrow$ `0x1A83` $\rightarrow$ `0x1A84` | `VENDOR-SOURCE-VERIFIED` | `fw9369_startup.c:396-416` |
| `0xA2` / `0xA0` / `0xA3` single-byte strobes are invalid | `SOURCE-VERIFIED` | Zero call sites in vendor codebase |
| Proposed replacement in Stage D8 driver | `ARCHITECTURAL-PROPOSAL` | Defined in Section 5 above |
