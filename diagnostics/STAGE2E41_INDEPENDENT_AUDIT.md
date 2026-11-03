# Stage 2E.4.1 — Independent Static Audit of FT9362 Identification Implementation Report

**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 (Chip ID `0x9362`, Wire CRC `0x1C53`)  
**Host Kernel:** `7.2.8-9-omarchy` (SMP preempt mod_unload x86_64)  
**Host Compiler:** `gcc (GCC) 16.2.1 20260810`  
**Document ID:** `STAGE2E41_INDEPENDENT_AUDIT`  
**Audit Scope:** Independent, read-only static source audit of the Stage 2E.4 minimal FT9362 identification implementation in `kernel/fte3600.c`, `kernel/fte3600.h`, and `kernel/fte3600-test.c`  
**Hardware Safety Gate:** **CLOSED (`bin/fw9369-hw-tool` exits with code 1 unconditionally)**  
**Audit Mode:** **STRICTLY READ-ONLY (Zero source modifications, zero commits, zero hardware execution)**  

---

## 1. Executive Summary & Authoritative Stage Verdict

This independent static audit evaluates the software implementation delivered in Stage 2E.4. Every claim regarding SPI wire protocol framing, transfer completion semantics, DMA buffer safety, state machine transitions, UNKNOWN recovery, concurrency, test harness validity, and security boundaries was audited directly against the source code in [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c), [`kernel/fte3600.h`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.h), and [`kernel/fte3600-test.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600-test.c).

### Key Audit Findings

1. **Wire Protocol Fully Verified Against Evidence:** The identification packet bytes, transfer lengths, 4 ms transceiver stabilization delay, response offsets, CRC-16 wire calculation, and variant gate implemented in [`fte3600_hw_identify()`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L247-L352) match [`tools/stage2a-identify.c`](file:///home/titan/fte3600-linux-driver/tools/stage2a-identify.c) byte-for-byte in executable code.
2. **DEFECT IDENTIFIED — Missing Transfer Length Completion Check:** In [`fte3600_spi_sync_transfer()`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L172-L216), `spi_sync()` return code and `m.status != 0` are checked, but **`m.actual_length` is never inspected**. A short or truncated transfer where `m.status == 0` but `m.actual_length < len` is not detected and would be treated as success.
3. **DMA Buffer Safety Verified:** Transfer buffers `data->tx_buf[32]` and `data->rx_buf[32]` are embedded within the kmalloc-allocated [`struct fte3600_data`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L34-L48). They reside in lowmem, are cacheline-aligned (`____cacheline_aligned`), remain valid throughout `spi_sync()`, are accessed strictly under `data->lock`, and completely avoid stack DMA (`CONFIG_VMAP_STACK`) and module `.rodata` vmalloc address hazards.
4. **SEMANTIC DEFECT IDENTIFIED — Premature `READY` State Transition:** The driver transitions to [`FTE3600_STATE_READY`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L346) immediately upon chip ID identification. Chip identification proves only bus responsiveness, chip ID `0x9362`, and wire CRC `0x1C53`. It does not initialize the Analog Front End (AFE), arm the FIFO controller, configure capture thresholds, or service interrupts. Userspace seeing `READY` would expect capture to function, yet `FTE3600_IOC_CAPTURE` unconditionally returns `-EOPNOTSUPP`. A distinct state [`FTE3600_STATE_IDENTIFIED`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.h#L31-L40) is architecturally required.
5. **Reset Recovery Limitations Documented:** `fte3600_hw_reset_pulse()` drives active-low GPIO timing via `gpiod_set_value_cansleep()`. Because this API returns `void`, it confirms only register writes to the host Intel PCH LPSS GPIO controller, not physical pin voltage on Pad 14 or sensor silicon reset. Recovery claims must not assume sensor-internal state is guaranteed until subsequent SPI interrogation succeeds.
6. **Concurrency & Teardown Verified:** All SPI transactions, reset pulses, and offline checks are strictly serialized under `data->lock`. The Hardware-Resource Shutdown Protocol in `fte3600_remove()` nullifies `reset_gpiod` and `spi` under the lock before returning, preventing post-unbind use-after-free.
7. **Test Suite Classification:** All 32 tests in [`kernel/fte3600-test.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600-test.c) are **Category B userspace simulations**. They thoroughly validate UAPI layouts, state transitions, concurrency models, and failure paths, but do not link or execute the static kernel functions inside `kernel/fte3600.c` directly.
8. **Security Boundary Verified:** FIFO capture remains disabled (`-EOPNOTSUPP`), IRQ handling is absent (0 handlers), prohibited registers (`0x1A84`, `0x1A82`, `0xF1`, `0xF4`, and writes to `0x9A`) are absent, and the hardware gate in `bin/fw9369-hw-tool` remains sealed (exit code 1).

### Authoritative Stage Verdict

$$\mathbf{STATIC\;AUDIT\;PASSED\;WITH\;BLOCKING\;LIMITATIONS}$$

*The wire protocol, DMA buffer allocation, and concurrency protections are statically verified against evidence. However, two software limitations block hardware testing: the absence of an `actual_length == requested_length` completion check in the SPI transfer engine, and the premature semantic transition to `READY` prior to AFE/sensor initialization.*

---

## 2. Verification of Exact SPI Wire Protocol

A line-by-line comparison between [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c) and reference [`tools/stage2a-identify.c`](file:///home/titan/fte3600-linux-driver/tools/stage2a-identify.c) was performed:

| Protocol Operation | Parameter / Field | Reference (`stage2a-identify.c`) | Executable Driver (`kernel/fte3600.c`) | Audit Verdict |
| :--- | :--- | :--- | :--- | :---: |
| **Step A: Wake Write** | MOSI Bytes | `[ 0x09, 0xF6, 0xC6, 0x01 ]` (L158) | `tx_buf[0..3] = {0x09, 0xF6, 0xC6, 0x01}` (L263-266) | **EXACT MATCH** |
| | Transfer Length | 4 bytes (L164) | `fte3600_spi_sync_transfer(..., 4)` (L267) | **EXACT MATCH** |
| | RX Buffer | NULL / 0 (L163) | `NULL` (L267) | **EXACT MATCH** |
| **Step B: Transceiver Delay** | Duration | 4 ms (4,000 µs) (L184) | `usleep_range(4000, 5000)` (L274) | **EXACT MATCH** |
| | Bus CS State | Deasserted HIGH (L178) | CS deasserted high between messages | **EXACT MATCH** |
| **Step C: Wake Readback** | MOSI Bytes | `[ 0x08, 0xF7, 0xC6, 0x00, 0x00 ]` (L193) | `tx_buf[0..4] = {0x08, 0xF7, 0xC6, 0x00, 0x00}` (L278-282) | **EXACT MATCH** |
| | Transfer Length | 5 bytes (L200) | `fte3600_spi_sync_transfer(..., 5)` (L283) | **EXACT MATCH** |
| | Verification Byte | `rx_c6_rd[4] == 0x01` (L216, 218) | `data->rx_buf[4] != 0x01` (L289) | **EXACT MATCH** |
| | Failure Error | `EXIT_ERR_VERIFY_MISMATCH` (L222) | `return -EIO; state = UNKNOWN` (L294-295) | **VERIFIED** |
| **Step D: Chip ID Read** | MOSI Bytes | `[ 0x04, 0xFB, 0x9A, 0x8B, 0..0 ]` (L233) | `tx_buf[0..9] = {0x04, 0xFB, 0x9A, 0x8B, ...}` (L300-304) | **EXACT MATCH** |
| | Transfer Length | 10 bytes (L240) | `fte3600_spi_sync_transfer(..., 10)` (L305) | **EXACT MATCH** |
| | Chip ID Offset | `((u16)rx[6] << 8) \| rx[7]` (L256) | `((u16)data->rx_buf[6] << 8) \| data->rx_buf[7]` (L311) | **EXACT MATCH** |
| | Wire CRC Offset | `((u16)rx[8] << 8) \| rx[9]` (L257) | `((u16)data->rx_buf[8] << 8) \| data->rx_buf[9]` (L312) | **EXACT MATCH** |
| **Step E: Wire CRC-16** | Input Slice | `&rx_id_rd[6]`, length 2 (L258) | `&data->rx_buf[6]`, length 2 (L315) | **EXACT MATCH** |
| | Polynomial & Seed | CCITT `0x1021`, seed `0xFFFF` (L52) | CCITT `0x1021`, seed `0xFFFF` (L85-98) | **EXACT MATCH** |
| | Bit Order | MSB-first bit-by-bit (L56-59) | MSB-first bit-by-bit (L93-97) | **EXACT MATCH** |
| **Step F: Variant Gate** | Bus Fault (Ground) | `chip_id == 0x0000` (L265) | `chip_id == 0x0000` (L318) $\to$ `-ENODEV` | **EXACT MATCH** |
| | Bus Fault (Float) | `chip_id == 0xFFFF` (L270) | `chip_id == 0xFFFF` (L318) $\to$ `-ENODEV` | **EXACT MATCH** |
| | CRC Check | `crc_rx != crc_calc` (L278) | `crc_rx != crc_calc` (L327) $\to$ `-EBADMSG` | **EXACT MATCH** |
| | Expected Chip ID | `0x9362` (L293) | `FTE3600_EXPECTED_CHIP_ID` (`0x9362`) (L336) | **EXACT MATCH** |
| | Register 0x1816 Read | Strictly if `chip_id == 0x9391` (L314) | **NOT performed** (`0x9391` rejected $\to$ `-ENODEV`) | **VERIFIED** |

**Conclusion:** The wire protocol in executable C code is identical to the verified Stage 2A reference implementation.

---

## 3. Audit of SPI Transfer Completion Semantics

Inspection of [`fte3600_spi_sync_transfer()`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L172-L216):

```c
static int fte3600_spi_sync_transfer(struct fte3600_data *data,
				     const u8 *tx, u8 *rx, size_t len)
{
	struct spi_transfer t;
	struct spi_message m;
	int ret;

	lockdep_assert_held(&data->lock);

	/* Check device validity and shutdown state */
	if (!data->spi || atomic_read(&data->offline))
		return -ENODEV;

	/* Validate buffer pointers: at least one must be non-NULL */
	if (!tx && !rx)
		return -EINVAL;

	/* Validate transfer length: reject zero-length and oversized transfers */
	if (len == 0 || len > FTE3600_MAX_SPI_TRANSFER_LEN)
		return -EINVAL;

	memset(&t, 0, sizeof(t));
	t.tx_buf = tx;
	t.rx_buf = rx;
	t.len = len;
	t.cs_change = 0;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);

	ret = spi_sync(data->spi, &m);
	if (ret) {
		data->state = FTE3600_STATE_UNKNOWN;
		data->last_error = (u32)(-ret);
		return ret;
	}

	if (m.status != 0) {
		data->state = FTE3600_STATE_UNKNOWN;
		data->last_error = (u32)(-m.status);
		return m.status;
	}

	return 0;
}
```

### Analysis of Detection Checks

1. **Negative `spi_sync()` Errors:** Detected at lines 202-207 (`if (ret) { ... return ret; }`).
2. **Nonzero Message Status:** Detected at lines 209-214 (`if (m.status != 0) { ... return m.status; }`).
3. **Invalid Pointers:** Detected at line 186 (`if (!tx && !rx) return -EINVAL;`).
4. **Invalid Lengths:** Detected at line 190 (`if (len == 0 || len > FTE3600_MAX_SPI_TRANSFER_LEN) return -EINVAL;`).
5. **Device Offline / Unbound:** Detected at line 182 (`if (!data->spi || atomic_read(&data->offline)) return -ENODEV;`).
6. **Buffer Overlap / Reuse:** `tx_buf` and `rx_buf` are separate struct members protected by `data->lock`. No concurrent overlap exists.
7. **Short or Incomplete Transfers (`actual_length` Check):**
   - **NOT IMPLEMENTED.**
   - In the Linux SPI subsystem (`include/linux/spi/spi.h`), `struct spi_message` provides `unsigned actual_length`, representing the cumulative bytes transferred by the controller driver.
   - Lines 202-215 inspect `ret` and `m.status`, but **never read or compare `m.actual_length` against requested `len`**.
   - If an SPI controller encounters an internal FIFO underflow or DMA burst truncation where `spi_sync()` returns 0 and `m.status` remains 0, `fte3600_spi_sync_transfer()` returns 0 (success). The caller then reads partial or uninitialized buffer data.
   - **Finding:** A successful `spi_sync()` return and zero message status do NOT prove full transfer completion unless `m.actual_length == len` is verified.

---

## 4. Audit of DMA Buffer Safety

Inspection of [`struct fte3600_data`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L34-L48) and probe allocation:

### 4.1 Structure Allocation & Memory Domain
- Allocated via `data = kzalloc(sizeof(*data), GFP_KERNEL);` in [`fte3600_probe()`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L552).
- `GFP_KERNEL` allocations are serviced by the kernel slab allocator (`kmalloc`), which draws from the kernel direct-mapped lowmem address space (`PAGE_OFFSET`).
- This memory is physically contiguous and direct-mapped; physical addresses are resolvable via `virt_to_phys()`, and `dma_map_single()` executes without page-table translation faults.

### 4.2 Cache-Line Alignment vs. DMA Alignment
- Declared as:
  ```c
  u8 tx_buf[32] ____cacheline_aligned;
  u8 rx_buf[32] ____cacheline_aligned;
  ```
- `____cacheline_aligned` aligns each buffer offset to the CPU L1 cache line boundary (64 bytes on x86_64).
- **Distinction:**
  - *Cache-line alignment* prevents cacheline sharing hazards: on non-coherent architectures, DMA writeback to one member cannot corrupt adjacent struct members sharing the same cache line. On x86_64, host LPSS SPI DMA is hardware cache-coherent, so cache invalidation hazards are mitigated by hardware.
  - *DMA alignment* represents the address divisibility required by the host SPI controller DMA engine (e.g. 4-byte or 8-byte boundary for 32-bit or 64-bit PCI burst transfers).
  - While aligning to 64 bytes satisfies 4-byte and 8-byte DMA alignment, cache-line alignment alone does NOT make memory DMA-safe: if buffers were on the stack, aligning them to a cache line would still cause a kernel panic under `CONFIG_VMAP_STACK`. It is the combination of **slab allocation (`kzalloc`)** and **cacheline alignment** that guarantees DMA safety.

### 4.3 Buffer Lifetime, Locking, and Memory Domain Verification
- **Buffer Lifetime:** Both buffers reside in `struct fte3600_data` and remain allocated from `probe()` until the final `put_device(&data->dev)` drops the refcount to 0 in `fte3600_device_release()`. They remain fully valid during synchronous `spi_sync()` execution.
- **Mutual Exclusion:** `data->tx_buf` and `data->rx_buf` are accessed exclusively within `fte3600_hw_identify()`, which is serialized under `data->lock`. No two transfers can access or overwrite the buffers concurrently.
- **Zero Stack / `.rodata` Usage:** An audit of all calls to `fte3600_spi_sync_transfer()` in `kernel/fte3600.c` confirmed that:
  - Line 267 passes `data->tx_buf` and `NULL`.
  - Line 283 passes `data->tx_buf` and `data->rx_buf`.
  - Line 305 passes `data->tx_buf` and `data->rx_buf`.
  Zero stack variables and zero module `.rodata` symbols are passed to `spi_sync()`.
- **Buffer Capacity:** Buffer size is 32 bytes each. Active identification transactions use 4 bytes (wake write), 5 bytes (wake readback), and 10 bytes (chip ID read). 32 bytes is sufficient for all identification operations.

---

## 5. Audit of State Machine Semantics & `READY` State

### 5.1 The `FTE3600_STATE_READY` Semantic Gap
In [`kernel/fte3600.c:346`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L346):
```c
	/* Identification verified: transition state to READY */
	data->state = FTE3600_STATE_READY;
	data->last_error = 0;
```

### Analysis of Semantic Accuracy

Chip identification alone proves strictly that:
1. Volatile register `0xC6` accepted write `0x01`.
2. 4 ms later, register `0xC6` read back `0x01`.
3. Register `0x1A8B` returned `0x9362` and wire CRC `0x1C53`.

Chip identification **DOES NOT PROVE**:
- Analog Front End (AFE) calibration or centering (FDT calibration).
- Image baseline capture.
- Image sensor matrix power-up.
- SPI FIFO controller configuration (10,246-byte frame buffer burst).
- Interrupt line (Pad 229) configuration, routing, or servicing.
- Fingerprint enrollment or matching capability.

Furthermore, if a userspace client queries `FTE3600_IOC_GET_STATE` and observes `FTE3600_STATE_READY`, it naturally attempts image acquisition via `ioctl(fd, FTE3600_IOC_CAPTURE)`. However, `FTE3600_IOC_CAPTURE` unconditionally returns `-EOPNOTSUPP` (line 515).

**Conclusion:** Setting `FTE3600_STATE_READY` after chip identification alone is semantically inaccurate. It overclaims sensor operational readiness. An intermediate state, such as `FTE3600_STATE_IDENTIFIED` (or `FTE3600_STATE_CHIP_IDENTIFIED`), must be added to [`enum fte3600_state`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.h#L31-L40). Transition to `READY` must be reserved for full sensor/AFE initialization.

---

## 6. Audit of UNKNOWN-State Recovery & Reset Limitations

### 6.1 RESET from UNKNOWN
In [`kernel/fte3600.c:423-444`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L423-L444), `case FTE3600_IOC_RESET:` does not restrict the initial state. It allows execution of `fte3600_hw_reset_pulse(data)` from any non-offline state, including `FTE3600_STATE_UNKNOWN`.

### 6.2 Sensor-Internal State vs. GPIO Pulse Reality
In [`fte3600_hw_reset_pulse()`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L130-L156):
- Calls `gpiod_set_value_cansleep()`:
  - 10 ms settle at Logical 0 (physical HIGH).
  - 20 ms assert at Logical 1 (physical LOW).
  - 10 ms settle at Logical 0 (physical HIGH).
- State transitions to `FTE3600_STATE_RESET_COMPLETE` (aliased as `FTE3600_STATE_RESET_SEQUENCE_ISSUED`).
- **Limitation:** `gpiod_set_value_cansleep()` returns `void`. Calling it proves solely that host register writes were dispatched to the Intel PCH LPSS GPIO controller. It does NOT verify physical pin voltage on Pad 14, and does NOT verify whether the sensor silicon responded.
- A reset pulse does not guarantee that the sensor has reached a known state; only subsequent SPI interrogation can establish this.

### 6.3 Protocol State Invariant on Failed SPI Transactions
- If an SPI transaction fails midway (e.g. after wake write `0xC6=0x01` ungates the transceiver clock), the sensor's internal SPI state machine is left active and potentially waiting for remaining packet bytes.
- The reference driver relies strictly on a hardware reset pulse on Pad 14 to recover the sensor silicon. (Software teardown `0xC6=0x00` was rejected in Stage 2D.3 as undocumented).
- Until the hardware reset pulse is proven on physical hardware, claiming that a reset pulse *guarantees* sensor recovery exceeds empirical evidence.

### 6.4 Safety of Repeated Operations
- **Repeated RESET:** Issuing multiple `FTE3600_IOC_RESET` calls is serialized under `data->lock` and benign: each pulse drives the active-low reset pin low for 20 ms.
- **Repeated INIT:** Calling `FTE3600_IOC_INIT` from `READY` or `UNKNOWN` is rejected with `-EPERM` by the state check `data->state != FTE3600_STATE_RESET_COMPLETE` in `fte3600_hw_identify()`. The driver strictly prevents repeated blind SPI writes to a failing sensor.

---

## 7. Audit of Concurrency & Removal Safety

### 7.1 Mutex Serialization
- `data->lock` serializes all hardware operations:
  - `FTE3600_IOC_GET_STATE` (lines 400-417).
  - `FTE3600_IOC_RESET` (lines 424-442).
  - `FTE3600_IOC_INIT` (lines 460-488).
  - `fte3600_remove()` (lines 642-658).
- Reset and INIT cannot overlap or interleave.
- Buffer reuse across concurrent threads is impossible.

### 7.2 Hardware-Resource Shutdown Protocol
In [`fte3600_remove()`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L624-L674):
1. `cdev_device_del(&data->cdev, &data->dev)` removes character device lookup.
2. `mutex_lock(&data->lock)` blocks until any in-flight `RESET` or `INIT` finishes.
3. `atomic_set(&data->offline, 1)` asserts the offline flag under the lock.
4. `data->reset_gpiod = NULL; data->spi = NULL;` invalidates hardware pointers while holding the lock.
5. `mutex_unlock(&data->lock)` releases lock.
6. `wake_up_all(&data->event_wq)` wakes sleeping pollers.
7. `put_device(&data->dev)` drops the probe reference.

### 7.3 Stale Pointer & Post-Unbind Protection
- Fast-path check at entry of `fte3600_ioctl_common()`:
  `if (atomic_read(&data->offline) && cmd != FTE3600_IOC_GET_STATE) return -ENODEV;`
- Re-check under `data->lock` in `FTE3600_IOC_RESET` (line 433) and `FTE3600_IOC_INIT` (line 481):
  If `remove()` ran while an ioctl waited for the lock, the ioctl wakes, detects `offline == 1` and NULL pointers, and aborts with `-ENODEV` without touching hardware.
- Post-lease re-check in `fte3600_cdev_open()` (line 368) prevents racing unbind during open.

---

## 8. Audit of Test Validity (`kernel/fte3600-test.c`)

Inspection of [`kernel/fte3600-test.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600-test.c):

### 8.1 Test Classification Matrix

| Suite | Name | Test Count | Mechanism | Classification |
| :---: | :--- | :---: | :--- | :--- |
| **Suite 1** | UAPI & Data Structure Alignment | 3 | Checks `sizeof` and ioctl macros against `fte3600.h` | **Shared UAPI Layout Verification** |
| **Suite 2** | Protocol CRC Vector Validation | 1 | Re-implements CRC-16 CCITT in test harness | **Pure Algorithmic Test Vector Verification** |
| **Suite 3** | State Machine Transitions | 1 | Uses userspace `sim_dev_t` structure | **Independent Userspace Simulation** |
| **Suite 4** | Mandatory Lifecycle & Concurrency | 11 | Uses `sim_context_t` with POSIX threads & atomics | **Independent Userspace Concurrency Simulation** |
| **Suite 5** | UAPI Parameter Rejections | 1 | Tests `mock_validate_ioctl()` in userspace | **Independent Userspace Parameter Simulation** |
| **Suite 6** | Minimal FT9362 Chip Identification | 15 | Uses `sim_driver_hw_identify()` and mock profiles | **Independent Userspace Protocol Simulation** |
| **Total** | | **32** | | **Category B Userspace Harness (100%)** |

### 8.2 Critical Audit Finding on Test Coverage
- **Zero Kernel Code Linkage:** None of the 32 tests link or execute the static functions compiled inside `kernel/fte3600.c` (`fte3600_hw_identify`, `fte3600_spi_sync_transfer`, `fte3600_crc16_wire`).
- Because `fte3600.c` contains kernel-internal symbols (`lockdep_assert_held`, `gpiod_set_value_cansleep`, `spi_sync`, `cdev_device_add`), it cannot be compiled as a host userspace binary without KUnit or UML.
- The host kernel `7.2.8-9-omarchy` has `CONFIG_KUNIT=n`.
- The tests prove that the algorithms, state transitions, concurrency contracts, and UAPI layouts are mathematically sound in userspace simulation.
- However, **runtime execution of the compiled kernel module `fte3600.ko` remains untested**.

---

## 9. Security & Safety Regression Scan

| Security / Safety Item | Required Boundary | Executable Status in `kernel/` | Audit Verdict |
| :--- | :--- | :--- | :---: |
| **FIFO Capture** | Disabled | `FTE3600_IOC_CAPTURE` returns `-EOPNOTSUPP` (L515) | **PASSED** |
| **Interrupt Servicing** | Disabled | 0 occurrences of `request_irq` or `request_threaded_irq` | **PASSED** |
| **Prohibited Reg `0x1A84`** | Barred | 0 occurrences in `kernel/` | **PASSED** |
| **Prohibited Reg `0x1A82`** | Barred | 0 occurrences in `kernel/` | **PASSED** |
| **Prohibited Reg `0xF1`** | Barred | 0 occurrences in `kernel/` | **PASSED** |
| **Prohibited Reg `0xF4`** | Barred | 0 occurrences in `kernel/` | **PASSED** |
| **Trim Lock Reg `0x9A` Write** | Barred | 0 write occurrences (`0x9A` appears only as addr byte for `0x1A8B`) | **PASSED** |
| **EFUSE / Flash Access** | Barred | 0 occurrences of opcodes `0xF0`-`0xFF` | **PASSED** |
| **Raw Arbitrary SPI IOCTL** | Barred | No raw SPI dispatch exists in character device | **PASSED** |
| **Automatic Retries** | Barred | Zero retry loops in `fte3600_hw_identify()` | **PASSED** |
| **Undocumented Teardown** | Barred | `0xC6=0x00` write is completely absent | **PASSED** |
| **Hardware Safety Gate** | Sealed | `bin/fw9369-hw-tool` exits with code 1 unconditionally | **PASSED** |

---

## 10. Audit Findings & Required Corrections

### Finding 1: Missing SPI Transfer Length Verification (`m.actual_length`)
- **Severity:** **HIGH**
- **File & Line:** [`kernel/fte3600.c:202-215`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L202-L215)
- **Evidence:** `spi_sync(data->spi, &m)` returns 0 and `m.status` is checked, but `m.actual_length` is never read or compared against `len`.
- **Consequence:** If an SPI controller transfer terminates prematurely or returns short with status 0, the driver treats incomplete data as valid, leading to uninitialized buffer access and unpredictable protocol behavior.
- **Recommended Correction:** Add explicit verification in `fte3600_spi_sync_transfer()`:
  ```c
  if (m.actual_length != len) {
      data->state = FTE3600_STATE_UNKNOWN;
      data->last_error = EIO;
      return -EIO;
  }
  ```

### Finding 2: Premature Transition to `FTE3600_STATE_READY`
- **Severity:** **MEDIUM**
- **File & Line:** [`kernel/fte3600.c:346`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L346), [`kernel/fte3600.h:36`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.h#L36)
- **Evidence:** `fte3600_hw_identify()` sets `data->state = FTE3600_STATE_READY;` upon chip ID verification alone, while AFE calibration, baseline capture, and FIFO capture remain uninitialized and disabled (`-EOPNOTSUPP`).
- **Consequence:** Misleads userspace clients querying telemetry into assuming the fingerprint sensor is operationally ready for frame capture.
- **Recommended Correction:** Define `FTE3600_STATE_IDENTIFIED` in `kernel/fte3600.h`. Transition to `FTE3600_STATE_IDENTIFIED` upon identification success. Reserve `FTE3600_STATE_READY` for subsequent stages when AFE and sensor capture are fully armed.

### Finding 3: Reset Pulse Confirms LPSS Register Dispatch Only
- **Severity:** **LOW / INFORMATIONAL**
- **File & Line:** [`kernel/fte3600.c:130-156`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L130-L156)
- **Evidence:** `gpiod_set_value_cansleep()` returns `void`. Calling it confirms only host GPIO controller register dispatch, not physical voltage on Pad 14 or sensor reset.
- **Consequence:** `FTE3600_STATE_RESET_COMPLETE` represents strictly that the electrical timing sequence was dispatched to the controller, not that the sensor silicon is verified.
- **Recommended Correction:** Maintain semantic alias `FTE3600_STATE_RESET_SEQUENCE_ISSUED` and document that only subsequent SPI interrogation proves sensor responsiveness.

### Finding 4: Test Suite is Strictly Category B Userspace Simulation
- **Severity:** **LOW / INFORMATIONAL**
- **File & Line:** [`kernel/fte3600-test.c:1-1130`](file:///home/titan/fte3600-linux-driver/kernel/fte3600-test.c#L1-L1130)
- **Evidence:** `fte3600-test.c` does not include `fte3600.c` or call kernel module functions.
- **Consequence:** Runtime execution of the compiled kernel module `fte3600.ko` remains untested offline due to `CONFIG_KUNIT=n`.
- **Recommended Correction:** Explicitly document test classification in all reports. Do not claim kernel unit test execution.

---

## 11. Exact File & Line References

- [`kernel/fte3600.c#L34-L48`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L34-L48): `struct fte3600_data` context definition with cacheline-aligned transfer buffers.
- [`kernel/fte3600.c#L83-L101`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L83-L101): `fte3600_crc16_wire()` wire CRC calculator.
- [`kernel/fte3600.c#L130-L156`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L130-L156): `fte3600_hw_reset_pulse()` active-low reset sequence.
- [`kernel/fte3600.c#L172-L216`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L172-L216): `fte3600_spi_sync_transfer()` transfer engine (omits `m.actual_length` check).
- [`kernel/fte3600.c#L247-L352`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L247-L352): `fte3600_hw_identify()` identification engine.
- [`kernel/fte3600.c#L346`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L346): Transition to `FTE3600_STATE_READY` (premature state).
- [`kernel/fte3600.c#L423-L444`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L423-L444): `case FTE3600_IOC_RESET:` reset ioctl dispatch.
- [`kernel/fte3600.c#L445-L490`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L445-L490): `case FTE3600_IOC_INIT:` init ioctl dispatch under lock.
- [`kernel/fte3600.c#L492-L516`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L492-L516): `case FTE3600_IOC_CAPTURE:` returns `-EOPNOTSUPP`.
- [`kernel/fte3600.c#L624-L674`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L624-L674): `fte3600_remove()` Hardware-Resource Shutdown Protocol.
- [`kernel/fte3600.h#L18-L19`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.h#L18-L19): Expected Chip ID `0x9362` and CRC `0x1C53`.
- [`kernel/fte3600.h#L31-L40`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.h#L31-L40): `enum fte3600_state` 8-state model.
- [`kernel/fte3600-test.c#L798-L1130`](file:///home/titan/fte3600-linux-driver/kernel/fte3600-test.c#L798-L1130): Suite 6 simulation tests (15 scenarios).
- [`tools/stage2a-identify.c#L158-L260`](file:///home/titan/fte3600-linux-driver/tools/stage2a-identify.c#L158-L260): Reference packet encodings for Stage 2A identification.
