# FTE3600 Linux Kernel Driver — Build, Installation, and Uninstallation Guide

**Target Hardware:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Fingerprint Silicon:** FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Host Controller:** Intel LPSS SPI2 (`pxa2xx-spi.2` / `idma64`, PCI `0000:00:1e.3`)  
**Reference Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Safety Status:** Source Implementation Complete (Hardware Validation Not Authorized / Not Performed)

---

## 1. Prerequisites and Toolchain

Ensure the kernel headers and standard C build tools are installed:

```bash
# On Arch Linux / Omarchy
sudo pacman -S --needed base-devel linux-headers clang pahole
```

Verify kernel version alignment:
```bash
uname -r
# Expected output: 7.2.8-9-omarchy (or compatible 6.x / 7.x kernel)
```

---

## 2. Build Instructions

### 2.1 Building the Kernel Module
The out-of-tree module builds against the installed kernel build tree:

```bash
cd /home/titan/fte3600-linux-driver
make -C /lib/modules/$(uname -r)/build M=$PWD/kernel modules
```

To build with strict kernel compiler warnings enabled (`W=1`):
```bash
make -C /lib/modules/$(uname -r)/build M=$PWD/kernel W=1 modules
```

This generates `kernel/fte3600.ko` with valid BTF (BPF Type Format) debug symbols.

### 2.2 Building the Userspace Diagnostic Utility
The userspace diagnostic client interacts with `/dev/fte3600` via UAPI ioctl calls:

```bash
gcc -Wall -Wextra -O2 tools/fte3600-tool.c -o tools/fte3600-tool
```

### 2.3 Running the Hardware-Independent Software Verification Test Suite
Before attempting any physical interaction, run the test suites in userspace simulation:

```bash
make -C kernel check
```

Output should report:
- `fte3600-test`: 160 / 160 Passed (100.0%) across 16 test suites.
- `fte3600-policy-test`: 9 / 9 Groups Passed (100.0%).

---

## 3. Installation Instructions

> [!CAUTION]
> **SAFETY HOLD WARNING:**
> Module installation and loading against physical hardware must only be executed after an explicit, authorized human validation grant. Do NOT load or test on physical hardware during source-only verification.

### 3.1 Out-of-Tree Manual Installation

1. Copy the compiled module to the kernel extra directory:
   ```bash
   sudo mkdir -p /lib/modules/$(uname -r)/extra/
   sudo cp kernel/fte3600.ko /lib/modules/$(uname -r)/extra/
   ```

2. Update kernel module dependencies:
   ```bash
   sudo depmod -a
   ```

3. Configure udev permissions for `/dev/fte3600`:
   ```bash
   sudo tee /etc/udev/rules.d/99-fte3600.rules << 'EOF'
   KERNEL=="fte3600", MODE="0660", GROUP="input"
   EOF
   sudo udevadm control --reload-rules
   sudo udevadm trigger
   ```

4. Load the kernel module:
   ```bash
   sudo modprobe fte3600
   ```

5. Verify ACPI match and probe in `dmesg`:
   ```bash
   dmesg | grep fte3600
   # Expected: fte3600 spi-FTE3600:00: FocalTech FT9362/FW9369 Fingerprint Sensor probed successfully
   ```

---

## 4. Uninstallation and Rollback Instructions

### 4.1 Unloading the Kernel Module
Ensure no userspace client (`fte3600-tool`) holds `/dev/fte3600` open:

```bash
sudo rmmod fte3600
```

> [!NOTE]
> If a hardware communication failure occurred and a transaction was latched into quarantine, the module pin (`try_module_get`) will prevent unloading, returning `-EBUSY`. This is by design to prevent use-after-free while unquiesced IDMA64 bus mastering is active. A system cold reboot is required to clear faulted hardware state.

### 4.2 Removing Installed Files
1. Remove module binary:
   ```bash
   sudo rm -f /lib/modules/$(uname -r)/extra/fte3600.ko
   sudo depmod -a
   ```

2. Remove udev rule:
   ```bash
   sudo rm -f /etc/udev/rules.d/99-fte3600.rules
   sudo udevadm control --reload-rules
   ```

---

## 5. Basic Operations via Userspace Client

Once loaded and bound to physical hardware:

1. **Query Telemetry Snapshot:**
   ```bash
   ./tools/fte3600-tool get-state
   ```

2. **Trigger Hardware Reset Pulse:**
   ```bash
   ./tools/fte3600-tool reset
   ```

3. **Identify & Startup Prefix:**
   ```bash
   ./tools/fte3600-tool init-full
   ```

4. **Calibrate Analog Baseline & Arm Touch Detector:**
   ```bash
   ./tools/fte3600-tool calibrate
   ```

5. **Capture Full Frame & Export PGM Image:**
   ```bash
   ./tools/fte3600-tool capture frame.pgm
   ```
