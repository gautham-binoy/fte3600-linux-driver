# Stage D6.6 — CAPTURE Errno Compatibility & Semantic Reconciliation

**Project:** `/home/titan/fte3600-linux-driver/`  
**Host Environment:** Linux `7.2.8-9-omarchy` (x86_64)  
**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`), FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Driver State:** Stage D6.6 Source-Only Remediation  

---

## 1. Executive Summary

Prior to Stage D6.6, there existed minor semantic discrepancies across documentation, test suites, and userspace tooling regarding the precise errno value returned when `FTE3600_IOC_CAPTURE` was issued under various driver conditions. Specifically, earlier tools expected `-EOPNOTSUPP` unconditionally, whereas kernel state gating introduced in Stage D6.4 correctly rejected capture in non-READY states with `-EPERM`, and offline devices with `-ENODEV`.

In Stage D6.6, the driver kernel code, simulation test suite, userspace utility (`tools/fte3600-tool.c`), and documentation have been reconciled into a strictly ordered, three-tier error classification model.

---

## 2. Three-Tier Error Classification Architecture

The kernel handler for `FTE3600_IOC_CAPTURE` in `kernel/fte3600.c` evaluates conditions in strict precedence order under `data->lock`:

```
   ┌────────────────────────────────────────────────────────┐
   │ 1. Device Offline / Removal Check                      │
   │    Condition: data->offline == true                    │
   │    Return: -ENODEV (0 hardware transfers)              │
   └────────────────────────┬───────────────────────────────┘
                            │ [Device is Online]
                            ▼
   ┌────────────────────────────────────────────────────────┐
   │ 2. State-Machine Readiness Gate                        │
   │    Condition: data->state != FTE3600_STATE_READY       │
   │    (e.g. UNINITIALIZED, RESET, IDENTIFIED,             │
   │          UNCALIBRATED, UNKNOWN, ERROR)                 │
   │    Return: -EPERM (0 hardware transfers)               │
   └────────────────────────┬───────────────────────────────┘
                            │ [State == READY]
                            ▼
   ┌────────────────────────────────────────────────────────┐
   │ 3. Feature Implementation Gate                         │
   │    Condition: Capture path not yet implemented         │
   │    Return: -EOPNOTSUPP (0 hardware transfers)          │
   └────────────────────────────────────────────────────────┘
```

### 2.1 Error Code Semantics and Rationale

| Errno | Standard Meaning | Driver Semantic Context | Hardware Access |
|---|---|---|---|
| **`-ENODEV`** | *No such device* | The hardware has been unbound, removed, or torn down via ACPI / SPI driver removal. The underlying `spi_device` is invalid. No future operations can succeed. | None (0 transfers) |
| **`-EPERM`** | *Operation not permitted* | The hardware is present and online, but the driver is in an incompatible lifecycle state. Specifically, the sensor has not yet undergone calibration and baseline acquisition to reach `READY`. | None (0 transfers) |
| **`-EOPNOTSUPP`** | *Operation not supported* | The sensor is online and has successfully achieved the `READY` state, but the driver build does not implement the image acquisition path (e.g. Stage D7/D8 feature deferral). | None (0 transfers) |

### 2.2 Distinguishing Operational vs Configuration Errors

This three-tier model prevents common userspace misinterpretations:
1. An application cannot confuse an uncalibrated sensor (`-EPERM`) with a disconnected device (`-ENODEV`).
2. An application can distinguish between missing kernel driver functionality (`-EOPNOTSUPP` in `READY`) and a caller protocol sequence violation (`-EPERM` in `IDENTIFIED` or `UNCALIBRATED`).
3. In all three cases, zero SPI transfers occur, completely eliminating hardware safety risks.

---

## 3. Tool and Harness Reconciliation

### 3.1 Userspace Tool Updates (`tools/fte3600-tool.c`)

The userspace diagnostics tool `tools/fte3600-tool.c` was updated to accurately interpret and report each errno:

```c
	} else if (strcmp(argv[1], "capture") == 0) {
		struct fte3600_frame_buffer fb = {
			.version = FTE3600_UAPI_VERSION,
			.length = FTE3600_IMAGE_BYTES
		};
		printf(">> Issuing FTE3600_IOC_CAPTURE...\n");
		if (ioctl(fd, FTE3600_IOC_CAPTURE, &fb) < 0) {
			if (errno == EPERM) {
				printf(">> Capture rejected as expected: device not capture-ready (%s, errno=%d, EPERM)\n",
				       strerror(errno), errno);
			} else if (errno == EOPNOTSUPP) {
				printf(">> Capture rejected as expected: capture feature deferred/unimplemented (%s, errno=%d, EOPNOTSUPP)\n",
				       strerror(errno), errno);
			} else if (errno == ENODEV) {
				printf(">> Capture rejected as expected: device offline or removed (%s, errno=%d, ENODEV)\n",
				       strerror(errno), errno);
			} else {
				fprintf(stderr, ">> Capture failed with unexpected errno: %s (errno=%d)\n",
				        strerror(errno), errno);
				close(fd);
				return 1;
			}
		}
		close(fd);
		return 0;
	}
```

### 3.2 Simulation Harness Tests (`kernel/fte3600-test.c`)

Test Suite 10 contains dedicated regression tests verifying this three-tier model:
- **Scenario 7 (`test_d6_6_capture_errno_non_ready_eperm`)**:
  Explicitly iterates through all non-READY states (`UNINITIALIZED`, `RESET`, `IDENTIFIED`, `UNCALIBRATED`, `UNKNOWN`, `ERROR`) and asserts that `ioctl(FTE3600_IOC_CAPTURE)` returns `-EPERM`.
- **Scenario 8 (`test_d6_6_capture_errno_offline_enodev`)**:
  Marks the device offline and asserts that `ioctl(FTE3600_IOC_CAPTURE)` returns `-ENODEV` with 0 hardware accesses.
- **Scenario 9 (`test_d6_6_capture_errno_ready_eopnotsupp`)**:
  Simulates advancing the internal state to `READY` and asserts that `ioctl(FTE3600_IOC_CAPTURE)` returns `-EOPNOTSUPP` with 0 hardware accesses.

---

## 4. Verification Verdict

All three tiers have been verified in the simulation test suite and compiled cleanly without warnings into the kernel module and userspace tools. Semantic compatibility across kernel, tests, and userspace is complete and consistent.
