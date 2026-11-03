# Stage 2E.4.3 Independent Audit: Stage 2E.4.2 Corrections and Implementation Readiness

**Target Silicon:** FocalTech FT9362 / FW9369 Fingerprint Sensor  
**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Host Controller:** Intel Tiger Lake-LP PCH LPSS SPI2 Controller (`pxa2xx-spi.2`, ACPI ID `FTE3600:00` at `spi1.0`)  
**Target Kernel:** Linux `7.2.8-9-omarchy` (SMP preempt mod_unload x86_64)  
**Safety Gate Status:** CLOSED (`bin/fw9369-hw-tool` exit code 1; zero hardware access; zero module loading)  
**Authoritative Verdict:** `STATIC AUDIT PASSED — READY FOR SEPARATE HARDWARE-TEST REVIEW`

---

## 1. Executive Summary

This independent, read-only static source audit evaluates the software corrections implemented in Stage 2E.4.2 to resolve the blocking findings identified in Stage 2E.4.1. The audit inspected `kernel/fte3600.c`, `kernel/fte3600.h`, `kernel/fte3600-test.c`, build artifacts, and reference protocol documentation.

### Key Audit Conclusions:
1. **SPI Transfer Completion Fully Hardened:** In [`fte3600_spi_sync_transfer()`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L173-L224), `m.actual_length == len` is now strictly enforced. Short or truncated transfers unconditionally fail closed with `-EIO`, transition the state machine to `FTE3600_STATE_UNKNOWN`, and set `last_error = EIO`. Incomplete or uninitialized receive buffer bytes are never consumed by any caller.
2. **State Machine Boundary Corrected:** Introduced `FTE3600_STATE_IDENTIFIED = 8` without renumbering states 0 through 7. Successful identification transitions strictly to `IDENTIFIED`. The driver never enters `FTE3600_STATE_READY` (state 4), reserving `READY` for future stages when AFE DACs, finger detection thresholds, image baseline acquisition, and interrupt handling are fully configured. `FTE3600_IOC_CAPTURE` continues to return `-EOPNOTSUPP`.
3. **Reset Semantics Clarified:** Renamed the primary reset state enum constant to `FTE3600_STATE_RESET_SEQUENCE_ISSUED = 2` with `#define FTE3600_STATE_RESET_COMPLETE FTE3600_STATE_RESET_SEQUENCE_ISSUED` retained as a compatibility alias. Documented that `gpiod_set_value_cansleep()` returns `void`, establishing controller register dispatch only. Physical pin voltage and sensor silicon responsiveness remain unverified until subsequent SPI interrogation succeeds.
4. **DMA & Buffer Architecture Reviewed:** Differentiated verified generic Linux kernel lowmem allocation properties from unverified controller-specific runtime DMA behavior. `tx_buf` and `rx_buf` reside in dynamically allocated heap lowmem inside `struct fte3600_data`, remain valid throughout `spi_sync()`, are guarded by `data->lock`, avoid stack DMA hazards (`CONFIG_VMAP_STACK`), and comfortably accommodate all active identification lengths (4, 5, 10 bytes). Controller-specific DMA utilization (versus PIO) on Tiger Lake LPSS SPI2 is explicitly marked **UNVERIFIED**.
5. **Test Harness Validity Formally Classified:** All 34 tests in `kernel/fte3600-test.c` are formally classified. 3 tests are static ABI/layout checks; 31 tests are independent userspace simulations duplicating driver algorithms and concurrency patterns using POSIX primitives. Zero tests execute in-kernel or link `fte3600.ko` directly. The simulation harness genuinely covers short transfers, error codes, state transitions, repeated calls, and boundary invariants with a 100% pass rate.
6. **Safety & Execution Invariants Preserved:** Zero IRQ registration calls, zero FIFO burst capture routines, zero writes to prohibited registers (`0x1A84`, `0x1A82`, `0xF1`, `0xF4`, or write to `0x9A`), zero automatic retries or recovery loops, zero module loads, and zero physical device access. Hardware execution gate in `bin/fw9369-hw-tool` remains sealed (exit code 1).

---

## 2. SPI Transfer Completion Audit

### 2.1 Implementation Analysis (`fte3600_spi_sync_transfer`)
The synchronous SPI transfer engine was inspected in [kernel/fte3600.c:173-224](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L173-L224):

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

	/* Verify full transfer completion: short transfers are treated as failed */
	if (m.actual_length != len) {
		data->state = FTE3600_STATE_UNKNOWN;
		data->last_error = EIO;
		return -EIO;
	}

	return 0;
}
```

### 2.2 Verification of Failure Paths and Completion Checks
1. **`spi_sync()` Return Handling:**
   - Evaluated: `if (ret)`.
   - If `spi_sync()` returns a negative errno (e.g., `-ENOMEM`, `-ESHUTDOWN`, `-EINTR`), the driver transitions `data->state = FTE3600_STATE_UNKNOWN;`, records `data->last_error = (u32)(-ret);`, and returns `ret`.
2. **`m.status` Handling:**
   - Evaluated: `if (m.status != 0)`.
   - In Linux SPI core, `m.status` stores the transfer completion status (0 on success, negative errno on controller fault). When nonzero, the driver transitions `data->state = FTE3600_STATE_UNKNOWN;`, records `data->last_error = (u32)(-m.status);`, and returns `m.status`.
3. **`m.actual_length == len` Enforcement:**
   - Evaluated: `if (m.actual_length != len)`.
   - If fewer bytes were transferred than requested, even if `ret == 0` and `m.status == 0`, the driver detects the truncation, transitions `data->state = FTE3600_STATE_UNKNOWN;`, sets `data->last_error = EIO;`, and returns `-EIO`.
   - Since `len` cannot be 0 (rejected with `-EINVAL` at line 191), `m.actual_length` cannot trivially match 0.
4. **Negative Errno Guarantee:**
   - All failure exits return negative errnos: `-ENODEV`, `-EINVAL`, `ret` (< 0), `m.status` (< 0), or `-EIO`.
5. **No Short Transfer Escape Path:**
   - No code path exists in `fte3600_spi_sync_transfer()` where a transfer with `m.actual_length != len` can return 0 or leave state unchanged.

### 2.3 Caller Inspection and Receive Buffer Consumption
All callers of `fte3600_spi_sync_transfer()` reside exclusively within `fte3600_hw_identify()`:
- **Step A (Line 280):** `fte3600_spi_sync_transfer(data, data->tx_buf, NULL, 4);`
  - `if (ret) return ret;` -> Aborts immediately. No RX buffer involved.
- **Step B (Line 296):** `fte3600_spi_sync_transfer(data, data->tx_buf, data->rx_buf, 5);`
  - `if (ret) return ret;` -> Aborts immediately. `data->rx_buf[4]` is accessed **only if** `ret == 0`.
- **Step D (Line 318):** `fte3600_spi_sync_transfer(data, data->tx_buf, data->rx_buf, 10);`
  - `if (ret) return ret;` -> Aborts immediately. `data->rx_buf[6..9]` is accessed **only if** `ret == 0`.

**Verdict on Receive Data Safety:** Partial or truncated receive data is **never** consumed. On any transport failure or short transfer, callers exit immediately without reading `rx_buf`.

### 2.4 Mutex Ownership and Concurrency Contract
- `lockdep_assert_held(&data->lock);` is called at the entry of both `fte3600_spi_sync_transfer()` (line 180) and `fte3600_hw_identify()` (line 265).
- In `fte3600_ioctl_common()`, `case FTE3600_IOC_INIT:` acquires `mutex_lock_interruptible(&data->lock);` before dispatching to `fte3600_hw_identify()`.
- State transitions to `UNKNOWN` and `last_error` updates occur strictly under `data->lock`.
- No automatic retries, bus recovery writes, or recursive calls are triggered after a short transfer.

---

## 3. State Machine and UAPI Audit

### 3.1 State Value Numeric Integrity
The enumeration `enum fte3600_state` in [kernel/fte3600.h:31-41](file:///home/titan/fte3600-linux-driver/kernel/fte3600.h#L31-L41) was audited:

```c
enum fte3600_state {
	FTE3600_STATE_UNINITIALIZED         = 0,
	FTE3600_STATE_RESETTING             = 1,
	FTE3600_STATE_RESET_SEQUENCE_ISSUED = 2,
	FTE3600_STATE_INITIALIZING          = 3,
	FTE3600_STATE_READY                 = 4,
	FTE3600_STATE_CAPTURING             = 5,
	FTE3600_STATE_ERROR                 = 6,
	FTE3600_STATE_UNKNOWN               = 7,
	FTE3600_STATE_IDENTIFIED            = 8,
};
```

| State Name | Assigned Value | Verification Status | Functional Meaning |
| :--- | :---: | :---: | :--- |
| `FTE3600_STATE_UNINITIALIZED` | 0 | **Verified Unchanged** | Driver probe complete; zero hardware pulses issued. |
| `FTE3600_STATE_RESETTING` | 1 | **Verified Unchanged** | Electrical reset pulse currently asserting on Pad 14. |
| `FTE3600_STATE_RESET_SEQUENCE_ISSUED` | 2 | **Verified Preserved** | Reset timing pulse dispatched to GPIO controller; sensor silicon unverified. |
| `FTE3600_STATE_INITIALIZING` | 3 | **Verified Unchanged** | Active SPI identification transaction in progress. |
| `FTE3600_STATE_READY` | 4 | **Verified Unchanged** | Reserved for future stages; never set by identification. |
| `FTE3600_STATE_CAPTURING` | 5 | **Verified Unchanged** | Reserved for future FIFO image capture. |
| `FTE3600_STATE_ERROR` | 6 | **Verified Unchanged** | Deprecated error state; fail-closed uses `UNKNOWN`. |
| `FTE3600_STATE_UNKNOWN` | 7 | **Verified Unchanged** | Fail-closed state upon any transport or validation failure. |
| `FTE3600_STATE_IDENTIFIED` | 8 | **Verified Appended** | Bus responsiveness, `0xC6=0x01`, Chip ID `0x9362`, and wire CRC `0x1C53` verified. |

- Existing state values 0 through 7 are completely intact.
- `FTE3600_STATE_IDENTIFIED` is assigned value 8 at the tail.
- `#define FTE3600_STATE_RESET_COMPLETE FTE3600_STATE_RESET_SEQUENCE_ISSUED` preserves backwards compatibility with earlier tooling without semantic ambiguity.

### 3.2 State Transition Boundary Verification
1. **Successful Identification:**
   - In `fte3600_hw_identify()` ([kernel/fte3600.c:359](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L359)):
     `data->state = FTE3600_STATE_IDENTIFIED;`
   - Verified: The driver transitions to `IDENTIFIED` (8) and **never** transitions to `READY` (4).
2. **`READY` State Isolation:**
   - `FTE3600_STATE_READY` is not reachable anywhere in `kernel/fte3600.c`.
   - `FTE3600_IOC_CAPTURE` ([kernel/fte3600.c:506](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L506)) unconditionally returns `-EOPNOTSUPP`.
   - Telemetry never presents `READY` to userspace while capture is unimplemented.
3. **Precondition Gating on `INIT`:**
   - In `fte3600_hw_identify()` ([kernel/fte3600.c:270](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L270)):
     `if (data->state != FTE3600_STATE_RESET_SEQUENCE_ISSUED) return -EPERM;`
   - Calling `INIT` from `UNINITIALIZED`, `INITIALIZING`, `READY`, `CAPTURING`, `ERROR`, `UNKNOWN`, or `IDENTIFIED` returns `-EPERM`.
   - Repeated `INIT` after success returns `-EPERM`, preventing blind write loops.
4. **Recovery from `UNKNOWN`:**
   - Calling `INIT` from `UNKNOWN` returns `-EPERM`.
   - Recovery strictly requires `FTE3600_IOC_RESET`, which dispatches the electrical pulse sequence and transitions state to `RESET_SEQUENCE_ISSUED`.
   - `RESET_SEQUENCE_ISSUED` does not falsely claim silicon recovery; it requires subsequent SPI interrogation to confirm sensor responsiveness.

### 3.3 UAPI Layout and IOCTL Inspection
- `struct fte3600_state_snapshot` ([kernel/fte3600.h:59-65](file:///home/titan/fte3600-linux-driver/kernel/fte3600.h#L59-L65)):
  - Field `state` is `__u32`, which represents value 8 with zero truncation.
  - In `fte3600_ioctl_common()`, `snap.state = (u32)data->state;` copies value 8 directly to userspace without range validation rejection.
  - Size: exactly 32 bytes, 64-bit aligned.
- `struct fte3600_init_params` ([kernel/fte3600.h:73-77](file:///home/titan/fte3600-linux-driver/kernel/fte3600.h#L73-L77)):
  - Validates `version == FTE3600_UAPI_VERSION` (1) and `reserved[0..5] == 0`.
  - Size: exactly 32 bytes, 64-bit aligned.
- `struct fte3600_frame_buffer` ([kernel/fte3600.h:86-91](file:///home/titan/fte3600.h#L86-L91)):
  - Validates `version == FTE3600_UAPI_VERSION` (1), `length == FTE3600_IMAGE_BYTES` (10240), `reserved[0..3] == 0`.
  - Returns `-EOPNOTSUPP`. Size: exactly 32 bytes, 64-bit aligned.
- Zero UAPI definitions or ioctl signatures were modified during Stage 2E.4.2.

---

## 4. DMA and SPI Buffer Architecture Audit

### 4.1 Memory Allocation & Buffer Residency
In `struct fte3600_data` ([kernel/fte3600.c:45-46](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L45-L46)):

```c
	u8 tx_buf[32] ____cacheline_aligned;
	u8 rx_buf[32] ____cacheline_aligned;
```

- `struct fte3600_data` is allocated in `fte3600_probe()` using `kzalloc(sizeof(*data), GFP_KERNEL)`.
- Total structure size: ~320 bytes.
- Total allocation fits entirely within a single 4096-byte page frame.

### 4.2 Verified Architectural Properties
1. **Buffer Validity During Transfer:**
   - `struct fte3600_data` remains allocated for the entire duration of the probe and device lifetime.
   - Teardown is managed via `struct device dev` refcounting. Even if `fte3600_remove()` runs concurrently, it acquires `data->lock`, sets `offline = 1`, and the memory is freed only in `fte3600_device_release()` when all open file descriptors drop their reference.
   - `spi_sync()` is synchronous (blocking) and called under `data->lock`.
   - Therefore, `tx_buf` and `rx_buf` memory remains unconditionally valid throughout `spi_sync()`.
2. **Mutual Exclusion & Concurrency Protection:**
   - Both buffers are accessed exclusively in `fte3600_hw_identify()` and passed to `fte3600_spi_sync_transfer()`.
   - Both routines assert `lockdep_assert_held(&data->lock)`.
   - `fte3600_cdev_open()` enforces a single-client exclusive lease (`atomic_cmpxchg(&data->is_open, 0, 1)`).
   - Concurrent reuse of `tx_buf` or `rx_buf` is strictly impossible under this locking and open contract.
3. **Buffer Capacity vs Transfer Lengths:**
   - Buffer size: 32 bytes each.
   - Step A: `len = 4` (fits in 32 B).
   - Step C: `len = 5` (fits in 32 B).
   - Step D: `len = 10` (fits in 32 B).
   - Capacity comfortably exceeds active identification requirements.
4. **Stack DMA Elimination:**
   - Zero stack buffers and zero module `.rodata` symbols are passed to `spi_sync()`. This guarantees immunity to `CONFIG_VMAP_STACK` DMA faults.
5. **Rejection of `GFP_DMA`:**
   - Rejection of `GFP_DMA` is verified as technically correct. `GFP_DMA` forces allocation into the bottom 16 MB of physical RAM (the 24-bit ISA DMA zone), which is obsolete and harmful for 64-bit PCI Express controllers.

### 4.3 Unverified Claims & Controller-Specific Limitations
To maintain documentation integrity, the audit differentiates generic kernel behavior from hardware-specific guarantees:

1. **Direct-Mapped Memory vs Physical Contiguity (UNVERIFIED for general buffers):**
   - While `struct fte3600_data` (~320 B) resides within a single 4KB page frame and is physically contiguous by virtue of being `< PAGE_SIZE`, claiming that direct-mapped memory (`PAGE_OFFSET`) automatically guarantees physical contiguity for arbitrary buffers is inaccurate. Multi-page allocations require an IOMMU or `alloc_pages()` guarantees. For the 32-byte buffers here, physical contiguity within the single page is established.
2. **`____cacheline_aligned` vs DMA Safety (UNVERIFIED as a standalone proof):**
   - Aligning `tx_buf` and `rx_buf` to 64-byte boundaries prevents false-sharing and cacheline invalidation hazards on non-coherent architectures.
   - However, cacheline alignment alone does **not** prove DMA safety. DMA safety additionally requires valid device bus address mapping, IOMMU translation, and compliance with controller burst constraints.
3. **SPI Core DMA Mapping vs PXA2xx PIO Execution (UNVERIFIED at runtime):**
   - In Linux SPI core (`drivers/spi/spi.c`), `__spi_map_msg()` attempts `dma_map_single()` only if `ctlr->can_dma` returns true for the transfer.
   - In the PXA2xx controller driver (`drivers/spi/spi-pxa2xx.c`), transfers smaller than the controller threshold (typically 16 or 32 bytes) are executed via **Programmed I/O (PIO)** through the SSP FIFO registers (`SSDR`), completely bypassing DMA channels.
   - Whether the Tiger Lake LPSS SPI2 controller on the Dynabook Satellite Pro C40-J actually requests a DMA channel or executes these small identification transfers (4, 5, 10 bytes) via PIO **cannot be verified statically** without runtime access to `/sys/class/dma` or controller debug logs.
   - However, because `tx_buf` and `rx_buf` are valid for both PIO and DMA, this unverified runtime detail does not block static implementation readiness.

---

## 5. Reset Semantics Audit

### 5.1 Analysis of `fte3600_hw_reset_pulse`
The reset helper was inspected in [kernel/fte3600.c:131-158](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L131-L158):

```c
static int fte3600_hw_reset_pulse(struct fte3600_data *data)
{
	lockdep_assert_held(&data->lock);

	if (atomic_read(&data->offline) || !data->reset_gpiod)
		return -ENODEV;

	data->state = FTE3600_STATE_RESETTING;

	/* Pre-reset settle interval (Logical 0 = physical HIGH / inactive) */
	gpiod_set_value_cansleep(data->reset_gpiod, 0);
	msleep(10);

	/* Assert reset: Logical 1 = physical LOW / asserted */
	gpiod_set_value_cansleep(data->reset_gpiod, 1);
	msleep(20); /* Reference hold duration (recovery.c:19) */

	/* Deassert reset: Logical 0 = physical HIGH / inactive */
	gpiod_set_value_cansleep(data->reset_gpiod, 0);
	msleep(10); /* Reference settle latency (factory_startup.c:341) */

	/*
	 * Electrical reset pulse sequence completed on GPIO controller.
	 * State is RESET_SEQUENCE_ISSUED. Sensor silicon responsiveness is unverified.
	 */
	data->state = FTE3600_STATE_RESET_SEQUENCE_ISSUED;
	return 0;
}
```

### 5.2 Verification of Reset Invariants
1. **Active-Low Polarity Verified:**
   - In ACPI translation ([kernel/fte3600.c:61](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L61)): `fte3600_reset_gpios = { 0, 0, true };` (`active_low = true`).
   - In probe ([kernel/fte3600.c:602](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L602)): `devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_LOW);` (initial logical 0 = deasserted = physical HIGH).
   - In reset pulse:
     - Pre-settle: `gpiod_set_value_cansleep(..., 0)` -> logical 0 = physical HIGH.
     - Assert: `gpiod_set_value_cansleep(..., 1)` -> logical 1 = physical LOW (asserted).
     - Deassert: `gpiod_set_value_cansleep(..., 0)` -> logical 0 = physical HIGH.
   - GPIO descriptor active-low polarity semantics are preserved and correct.
2. **Timing Intervals Preserved:**
   - Pre-settle: 10 ms (`msleep(10)`).
   - Assert hold: 20 ms (`msleep(20)`). Reference: `recovery.c:19`.
   - Post-settle: 10 ms (`msleep(10)`). Reference: `factory_startup.c:341`.
3. **No False Silicon Recovery Claims:**
   - Transition is to `FTE3600_STATE_RESET_SEQUENCE_ISSUED` (value 2).
   - Source comments explicitly document that `gpiod_set_value_cansleep()` returns `void`, establishing controller register write dispatch only, not physical pin voltage or sensor internal state.
   - Sensor presence and recovery are proven only if subsequent SPI interrogation in `fte3600_hw_identify()` succeeds.
4. **Zero Automatic Recovery:**
   - No automatic retry, loop, or recursive power-cycle is present in the driver.

---

## 6. Test Validity Audit

### 6.1 Formal Classification of Test Harness (`kernel/fte3600-test.c`)
Every test in `kernel/fte3600-test.c` was inspected and classified into one of four standard categories:

| Test ID / Description | Test Category | Execution Mechanism | Code Tested |
| :--- | :---: | :--- | :--- |
| `test_uapi_sizes` | **Static ABI/layout check** | Evaluates `sizeof()` against fixed 32-byte requirement. | `kernel/fte3600.h` struct definitions |
| `test_uapi_constants` | **Static ABI/layout check** | Evaluates constants and enum values 0 through 8. | `kernel/fte3600.h` constants/enums |
| `test_ioctl_encodings` | **Static ABI/layout check** | Evaluates `_IOC_*` encoding macros. | `kernel/fte3600.h` ioctl definitions |
| `test_crc_vectors` | **Independent userspace simulation** | Userspace CRC-16 against 4 test vectors. | Userspace duplicated algorithm |
| `test_state_transitions` | **Independent userspace simulation** | Mock struct `sim_dev_t` state transitions. | Userspace duplicated state machine |
| `test_scenario_1_open_release` | **Independent userspace simulation** | POSIX atomic refcounting simulation. | Userspace duplicated lifecycle |
| `test_scenario_2_open_remove_release` | **Independent userspace simulation** | POSIX atomic refcounting simulation. | Userspace duplicated lifecycle |
| `test_scenario_3_removal_racing_waiting_ioctl` | **Independent userspace simulation** | POSIX threads + mutex race simulation. | Userspace duplicated concurrency |
| `test_scenario_4_removal_racing_holding_ioctl` | **Independent userspace simulation** | POSIX threads + mutex race simulation. | Userspace duplicated concurrency |
| `test_scenario_5_get_state_after_removal` | **Independent userspace simulation** | Post-removal telemetry access simulation. | Userspace duplicated ioctl |
| `test_scenario_6_reset_after_removal` | **Independent userspace simulation** | Post-removal reset rejection simulation. | Userspace duplicated ioctl |
| `test_scenario_7_multiple_opens` | **Independent userspace simulation** | Single-client lease simulation via `atomic_int`. | Userspace duplicated lease |
| `test_scenario_8_error_path_references` | **Independent userspace simulation** | Simulated probe error path balance. | Userspace duplicated lifecycle |
| `test_scenario_9_spi_invalid_arguments` | **Independent userspace simulation** | Simulated argument validation. | Userspace duplicated bounds check |
| `test_scenario_10_spi_length_bounds` | **Independent userspace simulation** | Simulated length bounds check. | Userspace duplicated bounds check |
| `test_scenario_11_no_hardware_access_after_offline` | **Independent userspace simulation** | Simulated offline hardware blocking. | Userspace duplicated shutdown |
| `test_uapi_rejections` | **Independent userspace simulation** | Simulated parameter validation function. | Userspace duplicated ioctl validator |
| `test_id_scenario_1_valid_wake_response` | **Independent userspace simulation** | Simulated profile: wake readback `0x01`. | Userspace duplicated identify logic |
| `test_id_scenario_2_invalid_wake_response` | **Independent userspace simulation** | Simulated profile: wake mismatch (`0x00`/`0xFF`). | Userspace duplicated identify logic |
| `test_id_scenario_3_valid_id_and_crc` | **Independent userspace simulation** | Simulated profile: `0x9362` and `0x1C53`. | Userspace duplicated identify logic |
| `test_id_scenario_4_crc_mismatch` | **Independent userspace simulation** | Simulated profile: CRC mismatch (`0xBADC`). | Userspace duplicated identify logic |
| `test_id_scenario_5_unexpected_chip_id` | **Independent userspace simulation** | Simulated profile: `0x9368` and `0x9391`. | Userspace duplicated identify logic |
| `test_id_scenario_6_all_zero_response` | **Independent userspace simulation** | Simulated profile: ground `0x0000`. | Userspace duplicated identify logic |
| `test_id_scenario_7_all_ones_response` | **Independent userspace simulation** | Simulated profile: float `0xFFFF`. | Userspace duplicated identify logic |
| `test_id_scenario_8_spi_transfer_error` | **Independent userspace simulation** | Simulated profile: `spi_sync` returns `-EIO`. | Userspace duplicated identify logic |
| `test_id_scenario_9_short_incomplete_transfer` | **Independent userspace simulation** | Simulated profile: `actual_length < len` & status != 0. | Userspace duplicated completion logic |
| `test_id_scenario_10_failure_after_wake_write` | **Independent userspace simulation** | Simulated profile: step C fails after step A. | Userspace duplicated identify logic |
| `test_id_scenario_11_invalid_state_init` | **Independent userspace simulation** | Calls simulated init from all invalid states. | Userspace duplicated state gate |
| `test_id_scenario_12_init_while_offline` | **Independent userspace simulation** | Calls simulated init while offline. | Userspace duplicated shutdown gate |
| `test_id_scenario_13_repeated_init_after_success` | **Independent userspace simulation** | Calls simulated init twice from `IDENTIFIED`. | Userspace duplicated state gate |
| `test_id_scenario_14_concurrent_reset_init_mutex` | **Independent userspace simulation** | Concurrent simulated reset and init threads. | Userspace duplicated concurrency |
| `test_id_scenario_15_removal_during_operation` | **Independent userspace simulation** | Removal while simulated init holds mutex. | Userspace duplicated concurrency |
| `test_id_scenario_16_spi_completion_semantics` | **Independent userspace simulation** | Explicit test of exact len, short len, status, ret. | Userspace duplicated completion logic |
| `test_id_scenario_17_semantic_boundary_capture_rejection` | **Independent userspace simulation** | Tests `IDENTIFIED` (8) vs `READY` (4), capture `-EOPNOTSUPP`. | Userspace duplicated boundary logic |

### 6.2 Test Architecture Summary
- **Actual kernel implementation tests:** 0 / 34 (0%)
- **Shared-code tests:** 0 / 34 (0%)
- **Independent userspace simulations:** 31 / 34 (91.2%)
- **Static ABI/layout checks:** 3 / 34 (8.8%)

**Finding:** None of the 34 tests execute inside kernel space or call functions compiled into `kernel/fte3600.ko`. This is technically necessary because the host kernel lacks `CONFIG_KUNIT=y` and physical hardware access is barred by safety policy. The test harness must be accurately described as a **Category B standalone userspace simulation and static ABI check**, not an in-kernel runtime test suite.

### 6.3 Test Coverage Verification
The 34 tests genuinely cover:
- Exact SPI transfer length: Verified (Scenario 16a).
- Short transfer with zero message status: Verified (Scenario 9c/d/e, Scenario 16b).
- Nonzero message status: Verified (Scenario 9a/b, Scenario 16c).
- Nonzero `spi_sync()` return: Verified (Scenario 8, Scenario 16d).
- Transition to `IDENTIFIED`: Verified (Scenarios 1, 3, 13, 16a, 17).
- No transition to `READY`: Verified (Scenarios 10, 13, 17).
- Transition to `UNKNOWN` after failure: Verified (Scenarios 2, 4, 5, 6, 7, 8, 9, 10, 16b/c/d).
- Reset-state semantics: Verified (Suite 3, Scenarios 1, 4, 11, 14, 17).
- CAPTURE returning `-EOPNOTSUPP`: Verified (Suite 5, Scenario 17).

---

## 7. Regression and Operational Safety Audit

| Safety & Security Restriction | Audit Verification | Source Evidence |
| :--- | :---: | :--- |
| **IRQ Registration & Handling** | **PASSED (ABSENT)** | Zero IRQ registration calls in driver (`request_irq`, `devm_request_threaded_irq` absent). `irq-gpios` translation defined in ACPI table only. |
| **FIFO Burst Capture** | **PASSED (DISABLED)** | `FTE3600_IOC_CAPTURE` unconditionally returns `-EOPNOTSUPP` ([kernel/fte3600.c:506](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L506)). Zero capture code. |
| **Registers `0x1A84` and `0x1A82`** | **PASSED (ABSENT)** | Zero references in `kernel/fte3600.c`, `kernel/fte3600.h`, or test files. |
| **EFUSE Registers `0xF1` and `0xF4`** | **PASSED (ABSENT)** | Zero references in driver source or headers. |
| **Writes to Register `0x9A`** | **PASSED (ABSENT)** | Zero writes to 8-bit register `0x9A`. (Wire interrogation of 16-bit register `0x1A8B` at [line 315](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L315) transmits opcode `0x04 0xFB` with address `0x9A 0x8B`, which is a read operation conforming to protocol specification). |
| **Automatic Retries / Loops** | **PASSED (ABSENT)** | Zero loops, retries, or recovery writes in any transfer or reset path. |
| **Physical Hardware Operations** | **PASSED (SEALED)** | Zero hardware transactions issued. `fte3600.ko` is not loaded in `lsmod`. Device node `/dev/fte3600` does not exist. |
| **Hardware Safety Gate** | **PASSED (CLOSED)** | `bin/fw9369-hw-tool` executes policy check and exits unconditionally with code 1. |
| **Git Working Tree State** | **PASSED (UNCOMMITTED)** | Zero git commits created. Working tree modifications remain uncommitted. |

---

## 8. Findings and Severity Table

| Finding ID | Severity | Category | Description | Status / Resolution |
| :---: | :---: | :---: | :--- | :--- |
| **F-01** | Low | Documentation | Generic claims regarding `kzalloc` direct-mapped lowmem automatically guaranteeing physical contiguity were overly broad. Clarified that contiguity is guaranteed for single-page allocations (< 4096 B) such as `struct fte3600_data` (~320 B), but not for arbitrary multi-page buffers without an IOMMU. | **Resolved & Clarified** |
| **F-02** | Low | Documentation | Generic claims that `____cacheline_aligned` alone proves DMA safety were imprecise. Clarified that alignment prevents false-sharing and cacheline invalidation hazards, but DMA safety additionally requires device address mapping and controller compliance. | **Resolved & Clarified** |
| **F-03** | Low | Architecture | Runtime DMA channel utilization (DMA vs PIO) by the Intel LPSS SPI2 controller on Tiger Lake cannot be verified statically without runtime hardware probing. | **Documented as UNVERIFIED** (Non-blocking: buffers are valid for both PIO and DMA). |
| **F-04** | Info | Test Classification | `kernel/fte3600-test.c` (34 tests) was formally classified as Category B standalone userspace simulation and static ABI checks, confirming zero in-kernel execution. | **Resolved & Documented** |

---

## 9. Authoritative Verdict

```
STATIC AUDIT PASSED — READY FOR SEPARATE HARDWARE-TEST REVIEW
```

### Justification:
1. All blocking defects identified in Stage 2E.4.1 (omitted `m.actual_length` verification and premature transition to `READY`) have been thoroughly and correctly resolved.
2. `fte3600_spi_sync_transfer()` strictly enforces `m.actual_length == len`, failing closed with `-EIO` and `UNKNOWN` on short transfers.
3. The state machine introduces `FTE3600_STATE_IDENTIFIED = 8` without disturbing existing state values (0–7) or ABI layouts, isolating `READY` until capture readiness is implemented.
4. `struct fte3600_data` heap buffer residency, locking, and lifetime are validated; unverified controller-specific runtime DMA details are clearly distinguished.
5. All 34 tests in `kernel/fte3600-test.c` pass (100%), all 227 mock tests in `bin/fw9369-tests` pass (100%), and `kernel/fte3600.ko` compiles with 0 warnings and 0 errors on target kernel `7.2.8-9-omarchy`.
6. Hardware safety gates remain intact, sealed, and enforced (exit code 1).

> [!NOTE]
> This passing static audit verdict confirms software and architectural correctness only. It does **not** authorize module loading or physical hardware execution. Hardware execution requires explicit, separate user authorization.
