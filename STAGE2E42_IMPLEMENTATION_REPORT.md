# Stage 2E.4.2 Implementation Report: SPI Completion Verification and Identification-State Corrections

**Project:** FocalTech FT9362 / FW9369 Linux Kernel Driver (`fte3600.ko`)  
**Target Platform:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Host Subsystem:** Intel Tiger Lake-LP PCH LPSS SPI2 Controller (`pxa2xx-spi.2`, ACPI ID `FTE3600:00`)  
**Operating Environment:** Linux `7.2.8-9-omarchy` (SMP preempt mod_unload x86_64)  
**Execution Safety Policy:** ZERO physical hardware access. Module loading prohibited. Hardware gate sealed (`bin/fw9369-hw-tool` exit code 1).  
**Authoritative Verdict:** `SOFTWARE CORRECTIONS COMPLETE — STATIC VALIDATION PASSED`

---

## 1. Executive Summary

Stage 2E.4.2 addresses and resolves the two blocking defects identified during the Stage 2E.4.1 independent static audit, formalizes reset-state semantics, reviews kernel DMA and lowmem buffer properties, and expands the Category B userspace test suite:

1. **SPI Transfer Completion Semantics Corrected:** In `fte3600_spi_sync_transfer()`, the driver now explicitly verifies `m.actual_length == len`. Any transfer where fewer bytes than requested were transferred returns `-EIO`, unconditionally transitions the driver state machine to `FTE3600_STATE_UNKNOWN`, and sets `last_error = EIO`. Truncated or short transfers can never be consumed as valid sensor responses.
2. **Identification State Boundary Corrected:** Introduced `FTE3600_STATE_IDENTIFIED = 8` into `enum fte3600_state` without renumbering states 0–7. Transitioning to `FTE3600_STATE_IDENTIFIED` confirms bus responsiveness, transceiver clock un-gating (`0xC6=0x01`), silicon Chip ID verification (`0x1A8B=0x9362`), and wire CRC-16 validity (`0x1C53`). The driver explicitly refrains from entering `FTE3600_STATE_READY` (state 4), which remains strictly reserved for subsequent stages when analog front-end (AFE) DAC calibration, finger detection thresholds, image baseline acquisition, and interrupt handling are fully configured. `FTE3600_IOC_CAPTURE` continues to return `-EOPNOTSUPP`.
3. **Reset State Semantics Clarified:** Renamed the primary reset state enum constant to `FTE3600_STATE_RESET_SEQUENCE_ISSUED = 2` with `#define FTE3600_STATE_RESET_COMPLETE FTE3600_STATE_RESET_SEQUENCE_ISSUED` retained as a compatibility alias. Documented that `gpiod_set_value_cansleep()` returns `void`, proving LPSS GPIO controller register write dispatch only; physical pin voltage and sensor internal silicon state remain unproven until subsequent SPI interrogation succeeds.
4. **DMA & Buffer Architecture Validated:** Reviewed kernel DMA lowmem semantics. `struct fte3600_data` is allocated via `kzalloc(..., GFP_KERNEL)` in direct-mapped lowmem (`PAGE_OFFSET`), guaranteeing physical contiguity for Intel LPSS PCI DMA. `____cacheline_aligned` guarantees no cache-sharing hazards with adjacent fields. `GFP_DMA` is rejected as inappropriate for 64-bit PCI DMA engines.
5. **Validation & Verification:** Test harness `kernel/fte3600-test.c` was expanded to 34 Category B userspace simulation tests (100% pass rate). All 227 mock tests in `bin/fw9369-tests` continue to pass (100% pass rate). The driver compiles cleanly against `7.2.8-9-omarchy` headers with 0 warnings and 0 errors. The hardware execution gate remains sealed (exit code 1).

---

## 2. Detailed Technical Corrections

### 2.1 SPI Transfer Completion Verification (`m.actual_length`)

#### 2.1.1 Problem Statement (Stage 2E.4.1 Finding 1)
In Linux SPI subsystem architecture, `spi_sync()` dispatches a `struct spi_message` to the host controller driver (`pxa2xx-spi.2`). While `spi_sync()` return code indicates whether the message was accepted and queued, and `m.status` indicates the controller-level completion status, short transfers can occur if a bus transaction is aborted, truncated by FIFO underrun, or terminated early by hardware flow conditions. In Stage 2E.4, `fte3600_spi_sync_transfer()` checked only `ret != 0` and `m.status != 0`. If `m.actual_length < len` while `m.status == 0`, uninitialized or stale bytes in `rx_buf` would be parsed as valid register payloads.

#### 2.1.2 Implementation in `kernel/fte3600.c`
In `fte3600_spi_sync_transfer()` ([kernel/fte3600.c](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L202-L224)):

```c
static int fte3600_spi_sync_transfer(struct fte3600_data *data,
				     const u8 *tx, u8 *rx, size_t len)
{
	struct spi_transfer t;
	struct spi_message m;
	int ret;

	if (!data->spi || atomic_read(&data->offline))
		return -ENODEV;

	if (!tx && !rx)
		return -EINVAL;

	if (len == 0 || len > FTE3600_MAX_SPI_TRANSFER_LEN)
		return -EINVAL;

	memset(&t, 0, sizeof(t));
	t.tx_buf = tx;
	t.rx_buf = rx;
	t.len = len;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);

	ret = spi_sync(data->spi, &m);
	if (ret < 0) {
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

#### 2.1.3 Failure Semantics
If `m.actual_length != len`:
- Returns `-EIO`.
- Transitions `data->state` unconditionally to `FTE3600_STATE_UNKNOWN`.
- Sets `data->last_error = EIO`.
- Disallows further commands without an explicit hardware reset via `FTE3600_IOC_RESET`.

---

### 2.2 Semantic State Machine: Introduction of `FTE3600_STATE_IDENTIFIED`

#### 2.2.1 Problem Statement (Stage 2E.4.1 Finding 2)
In Stage 2E.4, successful chip ID readback transitioned `data->state` to `FTE3600_STATE_READY`. This violated domain semantics:
- Identifying `0x9362` and validating CRC `0x1C53` confirms only SPI bus responsiveness and silicon presence.
- It does **not** configure the analog front end (AFE), program finger detection DACs (`0x1816`), compute/load baseline calibration frames, or enable interrupt handling on Pad 229.
- Telemetry reporting `READY` misled userspace clients into attempting image captures, which unconditionally failed with `-EOPNOTSUPP`.

#### 2.2.2 State Machine Definition in `kernel/fte3600.h`
In [kernel/fte3600.h](file:///home/titan/fte3600-linux-driver/kernel/fte3600.h#L30-L50):

```c
/* Explicit 9-State State Machine */
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

/* Compatibility alias: RESET_COMPLETE indicates strictly that the electrical reset pulse sequence was issued */
#define FTE3600_STATE_RESET_COMPLETE FTE3600_STATE_RESET_SEQUENCE_ISSUED

/* Status Flags */
#define FTE3600_FLAG_OFFLINE                (1U << 0)
#define FTE3600_FLAG_RESET_SEQUENCE_ISSUED  (1U << 1)
#define FTE3600_FLAG_RESET_COMPLETED        FTE3600_FLAG_RESET_SEQUENCE_ISSUED
```

- Values 0 through 7 preserve exact numeric backwards compatibility.
- `FTE3600_STATE_IDENTIFIED` is assigned value 8.
- `FTE3600_STATE_READY` (value 4) remains unused by `fte3600_hw_identify()`, reserved for full capture readiness in Stage 2E.5+.

#### 2.2.3 Transition Logic in `kernel/fte3600.c`
In `fte3600_hw_identify()` ([kernel/fte3600.c](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L244-L365)):

```c
	/* Identification verified: transition state to IDENTIFIED (NOT READY) */
	data->state = FTE3600_STATE_IDENTIFIED;
	data->last_error = 0;
	dev_info(&data->spi->dev,
		 "FocalTech FT9362 identified (ID 0x%04x, CRC 0x%04x, state IDENTIFIED)\n",
		 chip_id, crc_rx);
	return 0;
```

#### 2.2.4 Telemetry and IOCTL Gate
- `FTE3600_IOC_GET_STATE` returns `snap.state = FTE3600_STATE_IDENTIFIED` (8).
- `FTE3600_IOC_INIT` requires `data->state == FTE3600_STATE_RESET_SEQUENCE_ISSUED`. Calling `INIT` from `IDENTIFIED` returns `-EPERM`, preventing repeated bus sweeps.
- `FTE3600_IOC_CAPTURE` continues to return `-EOPNOTSUPP`.

---

### 2.3 Reset State Semantics & Hardware Verification Limits

#### 2.3.1 Controller Dispatch vs Physical Pin State
In Linux kernel GPIO subsystem:
- `gpiod_set_value_cansleep(data->reset_gpiod, value)` returns `void`.
- A successful return indicates only that the GPIO driver (`pinctrl-tigerlake`) dispatched MMIO register writes to the Intel LPSS GPIO host controller.
- It does **not** read Pad 14 electrical voltage or verify sensor silicon reset acceptance.
- Physical sensor recovery is verified exclusively by subsequent SPI bus transactions (register 0xC6 write/readback, register 0x1A8B read, CRC validation).

#### 2.3.2 Naming and Documentation Alignment
To prevent misinterpreting the reset pulse as sensor readiness:
- The primary state is named `FTE3600_STATE_RESET_SEQUENCE_ISSUED = 2`.
- `#define FTE3600_STATE_RESET_COMPLETE FTE3600_STATE_RESET_SEQUENCE_ISSUED` is preserved for API compatibility.
- Comprehensive inline documentation added to `fte3600_hw_reset_pulse()` in [kernel/fte3600.c](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L115-L135).

---

### 2.4 DMA and Buffer Lowmem Architecture Review

1. **Lowmem Direct Mapping (`PAGE_OFFSET`):**
   `struct fte3600_data` is allocated in `fte3600_probe()` using `kzalloc(sizeof(*data), GFP_KERNEL)`. On 64-bit x86_64 Linux kernels, `GFP_KERNEL` allocations below 4 GB are mapped into the direct kernel lowmem region (`PAGE_OFFSET`). They are physically contiguous and directly accessible by PCI DMA engines.
2. **Rejection of `GFP_DMA`:**
   `GFP_DMA` restricts allocations to the ancient 16 MB ISA DMA zone, which is unnecessary and detrimental on modern 64-bit PCI Express devices.
3. **Prevention of Stack DMA Faults:**
   Under `CONFIG_VMAP_STACK` (standard in modern kernels), stack pages are virtually contiguous but physically fragmented, causing silent memory corruption or kernel panics if passed to `spi_sync()`. `fte3600.ko` embeds `tx_buf[32]` and `rx_buf[32]` inside `struct fte3600_data`, ensuring zero stack variables and zero `.rodata` symbols are ever passed to `spi_sync()`.
4. **Cacheline Separation:**
   `tx_buf` and `rx_buf` are decorated with `____cacheline_aligned`. This places them on dedicated L1/L2 cacheline boundaries (64 bytes), eliminating cacheline bouncing or false-sharing hazards with adjacent fields (`state`, `lock`, `offline`, `cdev`).
5. **Lifetime and Serialization:**
   `tx_buf` and `rx_buf` remain allocated for the lifetime of `struct fte3600_data` and are accessed strictly under `mutex_lock(&data->lock)`.

---

## 3. Test Harness Expansion & Offline Verification

### 3.1 Test Suite Structure
The test harness [kernel/fte3600-test.c](file:///home/titan/fte3600-linux-driver/kernel/fte3600-test.c) was expanded from 32 to 34 tests:
- **Suite 1:** UAPI & Data Structure Alignment (3 tests).
- **Suite 2:** Protocol CRC Vector Validation (1 test).
- **Suite 3:** State Machine Transitions & Invariants (1 test) — updated to validate the 9-state machine, `IDENTIFIED`, and rejection of repeated INIT.
- **Suite 4:** Stage 2E.2.2 Mandatory Lifecycle & Concurrency Scenarios (11 tests).
- **Suite 5:** UAPI Validation & Parameter Rejections (1 test).
- **Suite 6:** Stage 2E.4.2 Chip Identification & Completion Semantics (17 tests) — updated Scenarios 1, 3, 9, 10, 11, 13, 14, 15, plus dedicated Scenarios 16 and 17.

### 3.2 Category B Classification
As documented in the test harness header:
- **Test Category:** **Category B: Standalone Userspace Simulation Test Harness**.
- Uses POSIX threads, atomics, and mock SPI profiles in userspace.
- Validates UAPI definitions, memory alignments, fail-closed state transitions, and concurrency boundaries.
- Does **not** link `fte3600.ko` into the running kernel (Category D) or require `CONFIG_KUNIT` (Category C).
- Fully maintains the physical hardware safety boundary.

### 3.3 New Dedicated Test Scenarios

#### Scenario 16: SPI Completion Semantics
- **16a:** Exact length match (`actual_length == len`) with `status == 0` succeeds.
- **16b:** Short transfer (`actual_length < len`) with `status == 0` fails with `-EIO`, sets state `UNKNOWN`, sets `last_error = EIO`.
- **16c:** Nonzero message status (`m.status != 0`) fails with status code and sets state `UNKNOWN`.
- **16d:** Nonzero `spi_sync()` return (`ret < 0`) fails with ret code and sets state `UNKNOWN`.

#### Scenario 17: Semantic Boundary & CAPTURE Rejection
- Validates that state after identification is strictly `FTE3600_STATE_IDENTIFIED` (8), NOT `FTE3600_STATE_READY` (4).
- Validates that `FTE3600_IOC_CAPTURE` continues to be rejected with `-EOPNOTSUPP`.
- Validates that `FTE3600_IOC_RESET` from `IDENTIFIED` safely returns to `RESET_SEQUENCE_ISSUED`.

### 3.4 Test Execution Results

```text
===================================================================
 FTE3600 Unified Kernel Driver Test Suite (Stage 2E.4.2 Hardened)
 Mode: Standalone Userspace Simulation (Zero Hardware Access)
===================================================================

--- Test Suite 1: UAPI & Data Structure Alignment ---
  Testing: UAPI structures exact 32-byte size and alignment ... [PASS]
  Testing: UAPI protocol constants (10246 B wire, 10240 B image, v1) ... [PASS]
  Testing: IOCTL command encodings (magic 'F', sizes, directions) ... [PASS]

--- Test Suite 2: Protocol CRC Vector Validation ---
  Testing: Wire CRC (0x1C53) and Word LE CRC (0x313E, 0x5B7C, 0xE4AA) ... [PASS]

--- Test Suite 3: State Machine Transitions & Invariants ---
  Testing: 9-state model, UNKNOWN fail-closed invariant, reset recovery ... [PASS]

--- Test Suite 4: Stage 2E.2.2 Mandatory Lifecycle & Concurrency Scenarios ---
  Testing: Scenario 1: Open followed by normal release ... [PASS]
  Testing: Scenario 2: Open followed by device removal and release ... [PASS]
  Testing: Scenario 3: Removal racing with ioctl waiting for mutex ... [PASS]
  Testing: Scenario 4: Removal racing with ioctl holding mutex ... [PASS]
  Testing: Scenario 5: GET_STATE after removal ... [PASS]
  Testing: Scenario 6: RESET attempted after removal ... [PASS]
  Testing: Scenario 7: Multiple open attempts (single-client exclusive lease) ... [PASS]
  Testing: Scenario 8: Reference balance on probe/open error paths ... [PASS]
  Testing: Scenario 9: SPI helper rejects invalid arguments (NULL, 0-len) ... [PASS]
  Testing: Scenario 10: SPI helper rejects transfers beyond max limit ... [PASS]
  Testing: Scenario 11: No hardware access possible after offline ... [PASS]

--- Test Suite 5: UAPI Validation & Parameter Rejections ---
  Testing: IOCTL validation, version check, reserved fields, and -EOPNOTSUPP ... [PASS]

--- Test Suite 6: Stage 2E.4.2 Chip Identification & Completion Semantics ---
  Testing: Scenario 1: Valid wake response (rx_c6[4] == 0x01) ... [PASS]
  Testing: Scenario 2: Invalid wake response (rx_c6[4] != 0x01) ... [PASS]
  Testing: Scenario 3: Valid FT9362 ID and CRC (0x9362 and 0x1C53) ... [PASS]
  Testing: Scenario 4: CRC mismatch ... [PASS]
  Testing: Scenario 5: Unexpected chip ID (0x9368, 0x9391) ... [PASS]
  Testing: Scenario 6: All-zero response (0x0000) ... [PASS]
  Testing: Scenario 7: All-ones response (0xFFFF) ... [PASS]
  Testing: Scenario 8: SPI transfer error (-EIO) ... [PASS]
  Testing: Scenario 9: Short/incomplete transfer (m.status != 0, actual_length < len) ... [PASS]
  Testing: Scenario 10: Failure after wake write ... [PASS]
  Testing: Scenario 11: INIT from invalid state (UNINIT, INITING, READY, IDENTIFIED, UNKNOWN) ... [PASS]
  Testing: Scenario 12: INIT while device offline (-ENODEV) ... [PASS]
  Testing: Scenario 13: Repeated INIT after success (-EPERM) ... [PASS]
  Testing: Scenario 14: Concurrent RESET and INIT serialization under mutex ... [PASS]
  Testing: Scenario 15: Removal during an operation ... [PASS]
  Testing: Scenario 16: SPI completion semantics (actual_length == len, status, ret) ... [PASS]
  Testing: Scenario 17: Semantic boundary (IDENTIFIED vs READY, CAPTURE rejection) ... [PASS]

===================================================================
 Stage 2E.4.2 Test Results: 34 / 34 Passed (100.0%)
===================================================================
./fte3600-policy-test
FTE3600 glue policy: 9 groups passed (GPIO/ACPI IRQ, CS capability, polarity, lease, PM, removal)
```

---

## 4. Kernel Module Compilation Verification

`fte3600.ko` was recompiled using the target kernel build tree (`/usr/lib/modules/7.2.8-9-omarchy/build`):
- **Compiler Warnings:** 0
- **Compiler Errors:** 0
- **BTF Generation:** Completed successfully (`.BTF` section generated by `pahole`).
- **Module Verification:**
  - `vermagic`: `7.2.8-9-omarchy SMP preempt mod_unload`
  - `license`: `GPL`
  - `alias`: `acpi*:FTE3600:*`
- **Module Execution:** The compiled module was **not** loaded (`insmod`/`modprobe`), maintaining strict adherence to zero-execution safety boundaries.

---

## 5. Security and Operational Invariants

| Security Invariant | Verification Status | Implementation Proof |
| :--- | :--- | :--- |
| **Short Transfer Protection** | **VERIFIED** | `m.actual_length != len` returns `-EIO`, sets state `UNKNOWN` ([kernel/fte3600.c](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L217-L221)) |
| **State Machine Boundary** | **VERIFIED** | Identification transitions to `IDENTIFIED` (8), NOT `READY` (4) ([kernel/fte3600.c](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L359)) |
| **Capture Disablement** | **VERIFIED** | `FTE3600_IOC_CAPTURE` unconditionally returns `-EOPNOTSUPP` ([kernel/fte3600.c](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L441)) |
| **IRQ Disablement** | **VERIFIED** | Zero IRQ registration calls in driver probe or init paths |
| **Prohibited Registers** | **VERIFIED** | Zero writes to `0x9A`, `0xF1`, `0xF4`, `0x1A84`, `0x1A82` |
| **Fail-Closed Gate** | **VERIFIED** | Any mismatch/error transitions to `UNKNOWN`; recovery requires `RESET` |
| **Hardware Safety Gate** | **VERIFIED** | `bin/fw9369-hw-tool` unconditionally exits with code 1 |
| **Git Working Tree** | **VERIFIED** | Zero git commits created; uncommitted working tree edits |

---

## 6. Authoritative Verdict

```
SOFTWARE CORRECTIONS COMPLETE — STATIC VALIDATION PASSED
```

All defects from Stage 2E.4.1 have been corrected and verified via offline static compilation and 34 Category B simulation tests. The codebase is prepared for a subsequent hardware-readiness evaluation when authorized.
