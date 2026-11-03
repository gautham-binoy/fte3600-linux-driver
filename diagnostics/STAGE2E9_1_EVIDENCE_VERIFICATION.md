# Stage 2E.9.1 Final Evidence Verification Report

**Target System:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Target Silicon:** FocalTech FT9362 / FW9369 SPI Fingerprint Sensor  
**Target SPI Device:** `spi-FTE3600:00` on Intel LPSS SPI2 (`pxa2xx-spi.2`, CS0)  
**Host Kernel:** Linux `7.2.8-9-omarchy` (x86_64)  
**Task Type:** Static-Only Evidence Verification and Provenance Audit  
**Status:** **STAGE B HARDWARE EXECUTION ON HOLD — STRICTLY STATIC VERIFICATION**

---

## 1. SHA-256 Baseline Provenance Verification

In Stage 2E.9, cryptographic SHA-256 digests were computed for all relevant repository components. This section rigorously investigates the provenance of those hashes by examining earlier audit reports (Stages 2E.8.1 through 2E.8.5) and git history.

### 1.1 Provenance Audit Finding
- **Historical Report Inspection:** Reports `STAGE2E8_1_INDEPENDENT_IMPLEMENTATION_AUDIT.md` through `STAGE2E8_5_INFLIGHT_OPEN_REMOVE_RACE_AUDIT.md` recorded file line counts and performed line-by-line source audits, but **did not record explicit SHA-256 cryptographic digests** in their report bodies.
- **Git Commit Baseline:** The kernel driver source files (`kernel/fte3600.c` and `kernel/fte3600.h`) were committed at git commit `c8553816802199b3912c64f69a172e05b0a192b9` (Milestone 3). `git diff HEAD -- kernel/` confirms 0 modifications since that commit.
- **Policy Classification:** In accordance with the prompt's instructions ("If an earlier audit did not record a baseline hash, mark it: `BASELINE HASH NOT AVAILABLE — MATCH CLAIM NOT INDEPENDENTLY PROVEN`"), all components whose hashes were first generated in Stage 2E.9 are formally classified below.

### 1.2 Hash Provenance Table

| Component File | Size (Bytes) | Stage 2E.9 Hash | Earlier Audit Documented Hash | Current Hash (Stage 2E.9.1) | Provenance Status |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `kernel/fte3600.c` | 24,015 | `29d2726c9a63...` | None recorded in 2E.8.1–2E.8.5 | `29d2726c9a63772f0981bf65321f48934882eeb6702fe4a02629794b410805e8` | `BASELINE HASH NOT AVAILABLE — MATCH CLAIM NOT INDEPENDENTLY PROVEN` *(Identical to git commit c855381)* |
| `kernel/fte3600.h` | 3,171 | `1ffca1a4e04f...` | None recorded in 2E.8.1–2E.8.5 | `1ffca1a4e04fad026efbfde409ed0fdb67508ac57b3944aaf1cc57f3dbc35754` | `BASELINE HASH NOT AVAILABLE — MATCH CLAIM NOT INDEPENDENTLY PROVEN` *(Identical to git commit c855381)* |
| `tools/fte3600-tool.c` | 2,746 | `1b021a4d7f47...` | None recorded in 2E.8.1–2E.8.5 | `1b021a4d7f4770e8a627d6cd29116859dae908e2e5bf10739c0d05253d31d4f7` | `BASELINE HASH NOT AVAILABLE — MATCH CLAIM NOT INDEPENDENTLY PROVEN` *(Line count 103 matched)* |
| `bin/fte3600-tool` | 16,920 | `b91d7dc3cb0c...` | None recorded in 2E.8.1–2E.8.5 | `b91d7dc3cb0ca172d7020613af884b649c451be79a55e1836d31dc6f66217d12` | `BASELINE HASH NOT AVAILABLE — MATCH CLAIM NOT INDEPENDENTLY PROVEN` *(ELF binary verified)* |
| `tools/fw9369-mock/src/fw9369_main_hw.c` | 2,568 | `0ef7e497c6ab...` | None recorded in 2E.8.1–2E.8.5 | `0ef7e497c6ab20adbda11a5d3e435cb51f14e185d6247ab6c56279c55bf38a83` | `BASELINE HASH NOT AVAILABLE — MATCH CLAIM NOT INDEPENDENTLY PROVEN` *(Line count 80 matched)* |
| `bin/fw9369-hw-tool` | 16,776 | `576874d860c3...` | None recorded in 2E.8.1–2E.8.5 | `576874d860c3ee27fa5fcf2472d7ec4633df3c68c2eaffecce5ea1b930ba119b` | `BASELINE HASH NOT AVAILABLE — MATCH CLAIM NOT INDEPENDENTLY PROVEN` *(Sealed status 1 verified)* |
| `scripts/stage-b-test.sh` | 51,757 | `d7b08c4508b1...` | None recorded in 2E.8.1–2E.8.5 | `d7b08c4508b1acf1554b382eb7523fa96989bfba3e8fb190c1d72fb4976256f5` | `BASELINE HASH NOT AVAILABLE — MATCH CLAIM NOT INDEPENDENTLY PROVEN` *(Line count 1,394 matched)* |
| `scripts/run-stage-b-in-terminal.sh` | 1,489 | `0d45f9445912...` | None recorded in 2E.8.1–2E.8.5 | `0d45f94459124430c2b64c9bef9d820c22c984032a959a0d20b88248ed9b4e27` | `BASELINE HASH NOT AVAILABLE — MATCH CLAIM NOT INDEPENDENTLY PROVEN` *(Line count 39 matched)* |
| `scratch/test_synthetic_suite.py` | 39,268 | `49bc015b383f...` | None recorded in 2E.8.1–2E.8.5 | `49bc015b383fb930f6fad094bd98a9f0b567356cbafaffa26e4d06d800474398` | `BASELINE HASH NOT AVAILABLE — MATCH CLAIM NOT INDEPENDENTLY PROVEN` *(Line count 1,044 matched)* |

*Integrity Conclusion:* The SHA-256 hashes generated in Stage 2E.9 are confirmed identical to the current files in Stage 2E.9.1. However, because earlier audit reports (2E.8.1–2E.8.5) relied on structural line-by-line inspection rather than cryptographic hashes, earlier hash match claims cannot be independently proven from prior report text alone.

---

## 2. Direct Source Evidence for Critical Safety Claims

### 2.1 Every Reachable Call to `FTE3600_IOC_RESET`
1. **Userspace Invocation in [`tools/fte3600-tool.c:55-63`](file:///home/titan/fte3600-linux-driver/tools/fte3600-tool.c#L55-L63):**
   ```c
   55: 	} else if (strcmp(argv[1], "reset") == 0) {
   56: 		printf(">> Issuing FTE3600_IOC_RESET...\n");
   57: 		if (ioctl(fd, FTE3600_IOC_RESET) < 0) {
   58: 			/* libc ioctl() returns -1 on error and sets errno */
   59: 			perror("ioctl(FTE3600_IOC_RESET)");
   60: 			close(fd);
   61: 			return 1;
   62: 		}
   63: 		printf(">> Reset ioctl returned 0 (Success).\n");
   ```
2. **Script Invocation in [`scripts/stage-b-test.sh:1313-1322`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L1313-L1322):**
   ```bash
   1313: printf "\n%s\n" "--- [PHASE 4] Executing Exactly ONE Controlled Reset IOCTL ---"
   1314: printf "Invoking: %s reset\n" "$TOOL_BIN"
   1315: RESET_RC=0
   1316: was_errexit_reset=0
   1317: if shopt -o -q errexit; then was_errexit_reset=1; fi
   1318: set +e
   1319: "$TOOL_BIN" reset
   1320: RESET_RC=$?
   1321: if [ "$was_errexit_reset" -eq 1 ]; then set -e; fi
   1322: printf "Reset ioctl process return code: %d\n" "$RESET_RC"
   ```
3. **Kernel IOCTL Dispatcher in [`kernel/fte3600.c:436-457`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L436-L457):**
   ```c
   436: 	case FTE3600_IOC_RESET: {
   437: 		if (mutex_lock_interruptible(&data->lock))
   438: 			return -ERESTARTSYS;
   439: 
   440: 		/*
   441: 		 * Hardware-Resource Shutdown Protocol:
   442: 		 * Re-check offline status and GPIO descriptor validity under data->lock!
   443: 		 * If fte3600_remove() executed while we waited for the lock, offline
   444: 		 * is now 1 and reset_gpiod is NULL. We abort without touching hardware.
   445: 		 */
   446: 		if (atomic_read(&data->offline) || !data->reset_gpiod) {
   447: 			mutex_unlock(&data->lock);
   448: 			return -ENODEV;
   449: 		}
   450: 
   451: 		ret = fte3600_hw_reset_pulse(data);
   452: 		if (ret)
   453: 			data->last_error = (u32)(-ret);
   454: 
   455: 		mutex_unlock(&data->lock);
   456: 		break;
   457: 	}
   ```

### 2.2 Reset Helper Implementation in [`kernel/fte3600.c:131-158`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c#L131-L158)
```c
131: static int fte3600_hw_reset_pulse(struct fte3600_data *data)
132: {
133: 	lockdep_assert_held(&data->lock);
134: 
135: 	if (atomic_read(&data->offline) || !data->reset_gpiod)
136: 		return -ENODEV;
137: 
138: 	data->state = FTE3600_STATE_RESETTING;
139: 
140: 	/* Pre-reset settle interval (Logical 0 = physical HIGH / inactive) */
141: 	gpiod_set_value_cansleep(data->reset_gpiod, 0);
142: 	msleep(10);
143: 
144: 	/* Assert reset: Logical 1 = physical LOW / asserted */
145: 	gpiod_set_value_cansleep(data->reset_gpiod, 1);
146: 	msleep(20); /* Reference hold duration (recovery.c:19) */
147: 
148: 	/* Deassert reset: Logical 0 = physical HIGH / inactive */
149: 	gpiod_set_value_cansleep(data->reset_gpiod, 0);
150: 	msleep(10); /* Reference settle latency (factory_startup.c:341) */
151: 
152: 	/*
153: 	 * Electrical reset pulse sequence completed on GPIO controller.
154: 	 * State is RESET_SEQUENCE_ISSUED. Sensor silicon responsiveness is unverified.
155: 	 */
156: 	data->state = FTE3600_STATE_RESET_SEQUENCE_ISSUED;
157: 	return 0;
158: }
```

### 2.3 Every Module Insertion Path in [`scripts/stage-b-test.sh:1210-1250`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L1210-L1250)
```bash
1214:     trap 'defer_signal "SIGINT"' INT
1215:     trap 'defer_signal "SIGTERM"' TERM
1216:     trap 'defer_signal "SIGHUP"' HUP
...
1228:     set +e
1229:     insmod "$MODULE_PATH"
1230:     insmod_rc=$?
1231:     if [ "$was_errexit_insmod" -eq 1 ]; then set -e; fi
...
1238:     trap 'cleanup_handler 130 "SIGINT"' INT
1239:     trap 'cleanup_handler 143 "SIGTERM"' TERM
1240:     trap 'cleanup_handler 129 "SIGHUP"' HUP
...
1247:     if [ "$insmod_rc" -eq 0 ] && [ "$post_insmod_loaded" -eq 1 ]; then
1248:         INSERTION_STATE="DEFINITELY_SUCCEEDED"
1249:         THIS_INVOCATION_INSERTED=1
1250:         MODULE_INSERTED=1
```

### 2.4 Signal Traps Registration in [`scripts/stage-b-test.sh:1029-1032`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L1029-L1032)
```bash
1029: trap 'cleanup_handler 130 "SIGINT"' INT
1030: trap 'cleanup_handler 143 "SIGTERM"' TERM
1031: trap 'cleanup_handler 129 "SIGHUP"' HUP
1032: trap 'cleanup_handler $? "EXIT"' EXIT
```

### 2.5 Cooperative Lock Release in [`scripts/stage-b-test.sh:1003-1007`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L1003-L1007)
```bash
1003:     if { true >&200; } 2>/dev/null; then
1004:         flock -u 200 2>/dev/null || true
1005:         exec 200>&- 2>/dev/null || true
1006:         printf "%s\n" "[INFO] Script-owned cooperative lock released (FD 200 closed)."
1007:     fi
```

### 2.6 Strict Device-Node Identity Verification in [`scripts/stage-b-test.sh:1288-1296`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L1288-L1296)
```bash
1288:     if [ "$DEVNODE_RDEV" -ne "$DEVNODE_SYSFS_RDEV" ] || \
1289:        [ "$DEVNODE_MAJOR" -ne "$DEVNODE_SYSFS_MAJOR" ] || \
1290:        [ "$DEVNODE_MINOR" -ne "$DEVNODE_SYSFS_MINOR" ]; then
1291:         printf "%s\n" "[FAIL] Device node identity mismatch between stat and sysfs!"
1292:         printf "stat rdev: %d (maj=%d, min=%d) vs sysfs rdev: %d (maj=%d, min=%d)\n" \
1293:             "$DEVNODE_RDEV" "$DEVNODE_MAJOR" "$DEVNODE_MINOR" \
1294:             "$DEVNODE_SYSFS_RDEV" "$DEVNODE_SYSFS_MAJOR" "$DEVNODE_SYSFS_MINOR"
1295:         exit 1
1296:     fi
```

### 2.7 Pre-Existing Module Audit in [`scripts/stage-b-test.sh:1183-1198`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L1183-L1198)
```bash
1183: query_module_state "fte3600"
1184: init_mod_rc=$?
1185: if [ "$init_mod_rc" -eq 0 ]; then
1186:     INITIAL_FTE3600_LOADED=1
1187:     printf "%s\n" "[INFO] Pre-existing fte3600 module is currently loaded."
1188:     printf "%s\n" "[POLICY] Script will NOT attempt insertion and will NOT own or unload this module."
...
1198:     THIS_INVOCATION_INSERTED=0
```

### 2.8 GET_STATE Calls Before and After RESET
- Pre-Reset (Snapshot 1) in [`scripts/stage-b-test.sh:1309-1310`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L1309-L1310):
  ```bash
  1309: printf "\n%s\n" "--- [PHASE 3] Telemetry Snapshot 1: Pre-Reset State ---"
  1310: "$TOOL_BIN" get-state
  ```
- Post-Reset (Snapshot 2) in [`scripts/stage-b-test.sh:1329-1334`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L1329-L1334):
  ```bash
  1329: printf "\n%s\n" "--- [PHASE 5] Telemetry Snapshot 2: Post-Reset State ---"
  ...
  1334: "$TOOL_BIN" get-state
  ```

---

## 3. Demonstration of At-Most-Once Reset Invocation Across All Paths

| Execution Scenario | Path Traversed | Point of Abort or IOCTL Invocation | Total RESET IOCTL Count |
| :--- | :--- | :--- | :--- |
| **Normal Execution** | Phases 1 $\rightarrow$ 2 $\rightarrow$ 3 $\rightarrow$ 4 $\rightarrow$ 5 $\rightarrow$ Cleanup | Line 1319 dispatches `$TOOL_BIN reset`; runs to completion | **Exactly 1** |
| **Reset IOCTL Fails (Non-Zero Return)** | Phases 1 $\rightarrow$ 2 $\rightarrow$ 3 $\rightarrow$ 4 $\rightarrow$ 5 $\rightarrow$ Cleanup | Line 1319 executed; returns error; line 1324 logs warning; NO retry; proceeds to Phase 5 and exits | **Exactly 1** |
| **SIGINT / SIGTERM / SIGHUP before Phase 4** | Aborts during Phase 1, 2, or 3 | Trapped to `cleanup_handler`; Phase 4 is never reached | **0** |
| **SIGINT during `$TOOL_BIN reset`** | Aborts during Phase 4 | `$TOOL_BIN` killed or returns $rc \ge 128$; line 1320 logs return code; no retry; enters cleanup | **At most 1** (Outcome Uncertain) |
| **SIGINT after Phase 4** | Aborts during Phase 5 or Diagnostics | Trapped to `cleanup_handler`; cleanup contains 0 reset calls | **Exactly 1** |
| **Interrupted Module Insertion** | Phase 2 `insmod` interrupted | Trapped to `defer_signal`, dispatched to `cleanup_handler` at line 1244; exits before Phase 4 | **0** |
| **Missing Device Node** | Phase 2 wait loop times out | Line 1275 calls `exit 1`; traps to `cleanup_handler`; Phase 4 never reached | **0** |
| **Failed Pre-Reset Snapshot 1** | Phase 3 `$TOOL_BIN get-state` fails | Under `set -e`, aborts immediately; traps to `cleanup_handler`; Phase 4 never reached | **0** |
| **Failed Post-Reset Snapshot 2** | Phase 5 `$TOOL_BIN get-state` fails | Under `set +e`, captures code; preserves primary failure; no retry; enters cleanup | **Exactly 1** |
| **Cleanup Handler Re-Entry** | Multiple signals during cleanup | Line 900 `if [ "$CLEANUP_IN_PROGRESS" -eq 1 ]; then return 0; fi`; zero ioctl calls in cleanup | **0 additional** |

---

## 4. Complete Verbatim Source of `cleanup_handler()`

Excerpted in full directly from [`scripts/stage-b-test.sh:895-1026`](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L895-L1026):

```bash
895: cleanup_handler() {
896:     local incoming_exit="${1:-0}"
897:     local signal_name="${2:-EXIT}"
898: 
899:     # Prevent recursive invocation
900:     if [ "$CLEANUP_IN_PROGRESS" -eq 1 ]; then
901:         return 0
902:     fi
903:     CLEANUP_IN_PROGRESS=1
904:     CLEANUP_ATTEMPTED=1
905: 
906:     # Disable set -e so failures in individual reporting steps
907:     # do not abort the cleanup sequence midway
908:     set +e
909: 
910:     printf "\n%s\n" "--- [CLEANUP] Post-Test Diagnostic & Resource Release Handler ---"
911:     printf "Invoked via: %s (incoming exit status: %d)\n" "$signal_name" "$incoming_exit"
912: 
913:     # --------------------------------------------------------------------------
914:     # Read-Only Diagnostic Inspection: Kernel Module Status
915:     # --------------------------------------------------------------------------
916:     printf "\n%s\n" "--- Module & Device State Audit ---"
917:     local mod_state=0
918:     query_module_state "fte3600"
919:     mod_state=$?
920: 
921:     if [ "$mod_state" -eq 0 ]; then
922:         printf "%s\n" "[INFO] fte3600 module is currently loaded in kernel."
923:     elif [ "$mod_state" -eq 1 ]; then
924:         printf "%s\n" "[INFO] fte3600 module is confirmed absent from kernel."
925:     else
926:         printf "%s\n" "[WARN] Unable to determine fte3600 module state in kernel (inspection inconclusive)."
927:     fi
928: 
929:     # --------------------------------------------------------------------------
930:     # Read-Only Diagnostic Inspection: Character Device Node Status
931:     # --------------------------------------------------------------------------
932:     local target_devnode="${MOCK_TARGET_NODE:-/dev/fte3600}"
933:     printf "Checking character device node %s ... " "$target_devnode"
934:     if [ -e "$target_devnode" ]; then
935:         printf "%s\n" "[INFO] $target_devnode is PRESENT"
936:     else
937:         printf "%s\n" "[INFO] $target_devnode is ABSENT"
938:     fi
939: 
940:     # --------------------------------------------------------------------------
941:     # Read-Only Diagnostic Inspection: SPI Driver Binding Status (Never unbind)
942:     # --------------------------------------------------------------------------
943:     printf "Checking SPI driver binding link ... "
944:     if [ -d "$SPI_PATH/driver" ]; then
945:         local bound_driver
946:         bound_driver=$(readlink "$SPI_PATH/driver" 2>/dev/null || true)
947:         printf "[INFO] SPI device is BOUND to: %s\n" "$bound_driver"
948:     else
949:         printf "%s\n" "[INFO] SPI device is UNBOUND"
950:     fi
951: 
952:     # --------------------------------------------------------------------------
953:     # Read-Only Informative Descriptor Audit
954:     # --------------------------------------------------------------------------
955:     if [ -e "$target_devnode" ]; then
956:         printf "\n%s\n" "--- Informative Open-Descriptor Audit ---"
957:         check_open_descriptors || true
958:     fi
959: 
960:     # --------------------------------------------------------------------------
961:     # Read-Only Diagnostic Inspection: Hardware & GPIO State
962:     # --------------------------------------------------------------------------
963:     printf "\n%s\n" "--- Post-Test Hardware State ---"
964:     if [ -r "$PINCTRL_PINS" ]; then
965:         printf "PCH Pad 14 post-test: %s\n" "$(grep "pin 14 " "$PINCTRL_PINS" || true)"
966:         printf "PCH Pad 229 post-test: %s\n" "$(grep "pin 229 " "$PINCTRL_PINS" || true)"
967:     fi
968: 
969:     if [ -r "$DEBUGFS_GPIO" ]; then
970:         printf "GPIO lines claimed by fte3600 post-test: %s\n" \
971:             "$(grep "fte3600" "$DEBUGFS_GPIO" || echo "None")"
972:     fi
973: 
974:     printf "\n%s\n" "--- Kernel Ring Buffer Messages ---"
975:     dmesg | tail -n 15 || true
976: 
977:     # --------------------------------------------------------------------------
978:     # Option 1 Mandatory Policy Banners
979:     # --------------------------------------------------------------------------
980:     printf "\n%s\n" "==============================================================================="
981:     printf "%s\n" " OPTION 1 POLICY: AUTOMATIC MODULE UNLOAD IS PERMANENTLY DISABLED"
982:     printf "%s\n" "==============================================================================="
983: 
984:     if [ "$INITIAL_FTE3600_LOADED" -eq 1 ]; then
985:         printf "%s\n" "[POLICY] Module was pre-existing before script started. Script does not own module."
986:         printf "%s\n" "[POLICY] PRE-EXISTING MODULE PRESERVED IN KERNEL — NO UNLOAD PERFORMED"
987:     elif [ "$THIS_INVOCATION_INSERTED" -eq 1 ] && [ "${INSERTION_STATE:-NOT_ATTEMPTED}" = "DEFINITELY_SUCCEEDED" ]; then
988:         printf "%s\n" "[POLICY] MODULE LEFT LOADED BY POLICY — MANUAL CLEANUP REQUIRED"
989:         printf "%s\n" "[POLICY] To manually unload after inspection: sudo rmmod fte3600"
990:     elif [ "${INSERTION_STATE:-NOT_ATTEMPTED}" = "UNCERTAIN" ]; then
991:         printf "%s\n" "[POLICY] MODULE STATE MAY BE PRESENT — INSERTION OUTCOME UNCERTAIN; DO NOT AUTOMATICALLY REMOVE"
992:         printf "%s\n" "[POLICY] Manual inspection required to verify kernel module status."
993:     elif [ "${INSERTION_STATE:-NOT_ATTEMPTED}" = "DEFINITELY_FAILED" ]; then
994:         printf "%s\n" "[POLICY] Module insertion definitely failed. Script does not own any loaded module."
995:     else
996:         printf "[POLICY] Insertion state: %s. No automatic unload performed.\n" "${INSERTION_STATE:-NOT_ATTEMPTED}"
997:     fi
998:     printf "%s\n\n" "==============================================================================="
999: 
1000:     # --------------------------------------------------------------------------
1001:     # Release Script-Owned Cooperative Lock (FD 200) Only After Diagnostics
1002:     # --------------------------------------------------------------------------
1003:     if { true >&200; } 2>/dev/null; then
1004:         flock -u 200 2>/dev/null || true
1005:         exec 200>&- 2>/dev/null || true
1006:         printf "%s\n" "[INFO] Script-owned cooperative lock released (FD 200 closed)."
1007:     fi
1008: 
1009:     # --------------------------------------------------------------------------
1010:     # Determine final exit status (Preserves original test exit status)
1011:     # --------------------------------------------------------------------------
1012:     # 1. If invoked via signal (INT=130, TERM=143, HUP=129), retain signal status.
1013:     # 2. If test operations failed (incoming_exit != 0), retain primary test failure code.
1014:     # 3. If test operations passed (0), return 0.
1015:     local final_exit="$incoming_exit"
1016:     if [ "$final_exit" -eq 0 ]; then
1017:         printf "\n%s\n" "[PASS] Stage B test execution completed successfully (status 0)."
1018:     else
1019:         printf "\n[INFO] Script terminated with exit status %d (trigger: %s).\n" \
1020:             "$incoming_exit" "$signal_name"
1021:     fi
1022: 
1023:     # Unhook all traps before final exit to prevent recursion
1024:     trap - EXIT INT TERM HUP
1025:     exit "$final_exit"
1026: }
```

### 4.1 Verification of Seven Invariant Properties in `cleanup_handler()`
1. **Never unloads or unbinds:** 0 occurrences of `rmmod`, `modprobe -r`, `delete_module`, or `unbind`.
2. **Never terminates another process:** `check_open_descriptors` only reads `/proc/*/fd`; zero `kill` calls.
3. **Never issues another ioctl:** Zero invocations of `$TOOL_BIN` or `ioctl`.
4. **Never retries RESET:** Zero reset commands.
5. **Closes only invocation-owned descriptors:** Closes strictly FD 200 ([lines 1003–1007](file:///home/titan/fte3600-linux-driver/scripts/stage-b-test.sh#L1003-L1007)).
6. **Releases cooperative lock after diagnostics:** Releasing FD 200 is located at line 1003, strictly after all module, devnode, SPI, descriptor, GPIO, and dmesg audits.
7. **Preserves original exit status:** Line 1015 initializes `final_exit="$incoming_exit"`, maintaining primary test failure codes or signal codes without overriding them.

---

## 5. Kernel Driver Lifetime Assumptions: Verification Boundary Analysis

To ensure complete scientific integrity, verified facts are explicitly decoupled from standard architectural inferences:

### 5.1 Directly Verified in Actual Local Environment
- **Driver Source Code:** [`kernel/fte3600.c`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.c) and [`kernel/fte3600.h`](file:///home/titan/fte3600-linux-driver/kernel/fte3600.h) inspected in full.
- **Hardware-Resource Shutdown Protocol:** Verified that `fte3600_remove()` acquires `data->lock`, sets `data->offline = 1`, and nullifies `data->reset_gpiod = NULL` under mutex before returning.
- **Pre-Pulse Check:** Verified that `case FTE3600_IOC_RESET:` evaluates `atomic_read(&data->offline) || !data->reset_gpiod` under `data->lock`.
- **Installed Kernel Headers:** Verified `/lib/modules/7.2.8-9-omarchy/build/include/linux/cdev.h` declares `cdev_device_add()` and `cdev_set_parent()`.

### 5.2 Inferred from Installed Headers and Documented Linux API Semantics
- **`cdev_device_add()` Parenting:** In standard Linux kernel source (`fs/char_dev.c`), `cdev_device_add()` calls `cdev_set_parent(cdev, &dev->kobj)` and `cdev_add(cdev, ...)`, which links `cdev->kobj.parent = &dev->kobj` and increments parent kref.
- **`chrdev_open()` Pinning:** In standard Linux VFS (`fs/char_dev.c`), `chrdev_open()` calls `cdev_get(p)` which increments `kobject_get(&p->kobj)` during `.open` execution.
- **Devres Timing:** In standard Linux driver core (`drivers/base/core.c`), `devres_release_all(&spi->dev)` is invoked after `spi_driver.remove` returns.

### 5.3 What Remains Unverified Locally Due to Source Unavailability
- The exact `.c` source files of `fs/char_dev.c` and `drivers/base/core.c` for the running kernel (`7.2.8-9-omarchy`) are not installed on the system (only C headers are present under `/lib/modules/7.2.8-9-omarchy/build/include/`).
- While `cdev_device_add()` and `cdev_get()` behavior has been standard upstream Linux architecture since kernel 4.12, local line-by-line verification of the host kernel's compiled VFS binary implementation cannot be performed directly from disk without the full kernel source package installed.

---

## 6. Final Result and Status

### **Selected Result: A. Evidence supports all critical safety claims.**

**Justification:**
1. Direct source excerpts prove that `FTE3600_IOC_RESET` can be invoked at most once across all possible execution paths (normal, failure, signal, timeout, or error).
2. Direct source excerpts prove that `cleanup_handler()` contains zero unloads, zero unbinds, zero process terminations, and zero secondary ioctls.
3. Provenance of all hashes was honestly classified, distinguishing newly computed Stage 2E.9 baseline hashes from prior un-hashed audit checkpoints.
4. The exact verification boundaries of local driver code versus kernel core VFS architecture were transparently documented.

---

STAGE B HARDWARE EXECUTION: NOT PERFORMED  
AUTOMATIC MODULE UNLOAD: DISABLED  
AUTOMATIC SPI UNBIND: DISABLED
