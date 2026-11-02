# Stage 2B — FT9362 / FW9369 Initialization & Architecture Research

**Document ID:** `STAGE2B_FW9369_RESEARCH`  
**Date:** 2026-11-02  
**Target Hardware:** Dynabook Satellite Pro C40-J-109 (`PYS46G-06J02D`)  
**Host Platform:** Intel Tiger Lake-UP3 PCH-LP (`pxa2xx-spi.2`), Linux `7.2.8-9-omarchy` x86_64  
**Hardware Verification Status:** Stage 2A Verified (Silicon ID `0x9362`, Sensed CRC `0x1C53`, SPI Mode 0, 1 MHz)  
**Execution Gate:** Strictly static analysis. No hardware execution.

---

## 1. Executive Summary

During Stage 2A hardware identification, the physical sensor was probed using non-destructive register reads over the native Intel LPSS SPI bus (`spi-FTE3600:00`). The hardware reported:
- **Raw Silicon ID:** `0x9362` (read from 16-bit register `0x1A8B`).
- **Hardware CRC-CCITT:** `0x1C53` (mathematically verified against polynomial `0x1021`, init `0xFFFF`).
- **Sensor Silicon Classification:** FocalTech FT9362 sensor.
- **Driver / Firmware Pathway:** FW9369 architecture.

Stage 2B investigates the complete initialization, calibration, finger detection, image acquisition, and host integration requirements for this specific sensor. 

The primary breakthrough from this research is that **the FT9362 / FW9369 pathway does NOT require an external binary firmware upload (no `.bin` or `.pram` blob)**. Unlike earlier FocalTech sensors (such as FT9338, FT9348, FT9361, and FT9368) which require external RAM/PRAM boot binaries loaded into volatile memory at boot, the FT9362 features internal hardwired state machines and factory ROM/OTP. Initialization is accomplished entirely through SPI register configuration, Analog Front-End (AFE) tuning, Finger Detection Threshold (FDT) calibration, and image baseline acquisition.

---

## 2. Silicon Identity vs. Firmware Model Mapping

### 2.1 The 0x9362 vs 0x9369 Relationship
A crucial finding in the reference code is the distinction between raw silicon identity (`0x9362`) and firmware/model architecture designation (`0x9369` / `FW9369`):

1. **Physical Chip ID (`0x9362`):**
   - Read directly from 16-bit register `0x1A8B` using SPI frame:
     `[ 04 FB 9A 8B 00 01 00 00 00 00 00 00 ]` -> MISO: `[ ... 93 62 1C 53 ]`.
   - In `research/libfprint-fte3600/libfprint/drivers/fte3600/factory_integration.c:432`:
     ```c
     return id == 0x9362 ? FTE3600_MODEL_FW9369 : ...
     ```
   - In `device_session.c:343-344`:
     ```c
     s->status.raw_id = 0x9362;
     s->status.chip_id = 0x9369;
     ```
   - In `fw9369_startup.c:340`:
     ```c
     if (!read_word(op, 0x1a8b, &id))
         return FTE3600_FW9369_STARTUP_WORD_READ_ERROR;
     if (id != 0x9362)
         op->d->warnings |= FTE3600_FW9369_UNRECOGNIZED_ID;
     ```

2. **Windows Official Driver Confirmation:**
   - In `research/dynabook-windows-driver/strings/ftWbioUmdfDriverV2.dll.ascii.txt:2689`:
     ```text
     [Driver] I %s[%d]: Sensor type: %d(1=FT9338W, 2=FT9348W, 3=FT9361W, 4=FT9368W, 5=FT9371W, 6=FT9536W, 7=FT9362W)
     ```
     FT9362W is officially cataloged as Sensor Type 7.
   - The C++ implementation class in the Windows UMDF driver is `clsFT9369` / `clsFT9369Base` (`chips\fw9369\fw9369.cpp`, `fw9369_afe.cpp`, `fw9369_fdt.cpp`, `fw9369_image.cpp`, `fw9369_spidrv.cpp`).
   - Logging in Windows driver (`ftWbioUmdfDriverV2.dll:2380-2383`):
     ```text
     [Driver] Clear 9369 int flag
     [Driver] try to get 9369 chip id:0x%x
     [Driver] got device id fw9369 !!!.
     ```

3. **Field Evidence (Kvadra NAU LE14U):**
   - In `research/fte3600-kvadra-notes/README_EN.md`, laptops with FTE3600 sensors exhibit identical kernel/driver messages:
     ```text
     fw9362_Update_Base err,ret=-1
     ```
   - Proving that across OEM platforms (Dynabook C40-J, Kvadra NAU LE14U), ACPI hardware ID `FTE3600` hosts the FT9362 sensor executing the FW9369 driver protocol.

---

## 3. Firmware Requirements and Availability

### 3.1 Exhaustive Analysis of Firmware Dependencies
One of the most critical questions before Stage 2B was whether an external proprietary firmware blob (e.g. from `/lib/firmware/` or Windows driver extraction) was needed.

#### Evidence from Reference Driver (`linux_firmware.c`)
In `research/libfprint-fte3600/libfprint/drivers/fte3600/linux_firmware.c:30-41`:
```c
static const struct firmware_identity firmware[] = {
  {FTE3600_MODEL_FT9338, "ft9338.bin", 14184, "ca44..."},
  {FTE3600_MODEL_FT9348, "ft9348.bin", 10312, "48d6..."},
  {FTE3600_MODEL_FT9361, "ft9361.bin", 10396, "027d..."},
  {FTE3600_MODEL_FT9536, "ft9536.bin", 11934, "4a62..."},
  {FTE3600_MODEL_FT9368, "ft9368-app.bin", 27120, "9997..."},
  {FTE3600_MODEL_FT9368, "ft9368-pramboot.bin", 6096, "c93a..."}
};
```
`FTE3600_MODEL_FW9369` is **explicitly omitted** from the external firmware list.

#### Evidence from Reference Driver Runtime (`linux_runtime.c` & `factory_integration.c`)
- In `linux_runtime.c:298-315`:
  `fte3600_linux_firmware_load()` is only called if `fte3600_device_start()` returns `FTE3600_DEVICE_MISSING_FIRMWARE`.
- In `factory_integration.c:395-415`:
  When `model == FTE3600_MODEL_FW9369`, startup calls `fte3600_fw9369_startup_linux()`. It does **not** check for or pass any firmware buffer (`op->in->firmware`).
- In `fw9369_startup.c`:
  The startup sequence runs directly against hardware registers without any SRAM/PRAM memory load.

#### Evidence from Windows Driver (`ftWbioUmdfDriverV2.dll`)
In `research/dynabook-windows-driver/strings/ftWbioUmdfDriverV2.dll.ascii.txt:3017-3036`:
```text
[Driver] I %s[%d]: 9338 bin code size %d
[Driver] I %s[%d]: 9348 bin code size %d
[Driver] I %s[%d]: 9361 bin code size %d
[Driver] I %s[%d]: 9368 bin code size %d
[Driver] I %s[%d]: 9368 pramboot bin code size %d
[Driver] I %s[%d]: 9536 bin code size %d
```
Sensors 9338, 9348, 9361, 9368, and 9536 query bin code sizes and download firmware via `clsFT9338Base::ft_sensor_sensorbase_DownloadSensorFirmware`.
In contrast:
- `clsFT9369Base` does **not** inherit or call `DownloadSensorFirmware`.
- `clsFT9369Base` only implements `ft_sensor_sensorbase_DevD0Entry` and `ft_sensor_sensorbase_CaptureData`.

### 3.2 Conclusion on Firmware Availability
- **External Firmware Blob Needed:** **NO**.
- **Firmware Status:** Resident in on-chip ROM/hardwired logic.
- **Host Responsibility:** Pure parameter initialization, analog front-end (AFE) calibration, threshold calculation, and baseline frame storage in host RAM.

---

## 4. Hardware Architecture & Interfaces

### 4.1 Bus Architecture: Intel LPSS SPI (`pxa2xx-spi.2`)
- **Controller:** Intel Low Power Subsystem (LPSS) SPI controller #2 (`pci0000:00/0000:00:1e.2/pxa2xx-spi.2`).
- **Physical SPI Device:** `spi-FTE3600:00`.
- **SPI Mode:** Mode 0 (`CPOL=0, CPHA=0`).
  - Active-low Chip Select (`CS_N`).
  - MSB first.
  - Bits per word: 8.
- **Operating Frequency:**
  - Identification & Wakeup: 1 MHz (`1,000,000 Hz`). Verified in Stage 2A.
  - Fast scan / FIFO image transfer: ACPI specifies up to 4 MHz; however, 1 MHz is completely stable and supported by all reference drivers.

### 4.2 Reset Line Architecture: PCH Pad 14 (`GPP_B14`)
- **ACPI Declaration:** `GpioIo(Exclusive, PullDefault, 0, 0, IoRestrictionOutput, "\\_SB.PCI0.GPI0", 0, ResourceConsumer)` at pin offset `0x000E` (14).
- **Physical Hardware Register:** Pad configuration register `0x000E` in PCH Community 0 (`GPP_B14`).
- **Observed Hardware Baseline:** `0x84000201` (GPIO Output, TX value 1, buffer active).
- **Electrical Polarity:** Active Low (`RST_N`).
  - High (`1`, 3.3V/1.8V): Normal operation (run).
  - Low (`0`, 0V): Hardware reset asserted.
- **Reset Requirement:**
  - Reference driver `alt/focal_spi.c:162-181`:
    Hardware reset pulse is generated by pulsing `RST_N` low for 5–10 ms, releasing high, and waiting 50 ms for internal boot before SPI communication:
    ```c
    gpiod_set_value_cansleep(data->gpiod_rst, 0); // High (inactive)
    usleep_range(5000, 6000);
    gpiod_set_value_cansleep(data->gpiod_rst, 1); // Low (assert reset)
    usleep_range(5000, 6000);
    gpiod_set_value_cansleep(data->gpiod_rst, 0); // High (deassert reset)
    msleep(50);
    ```
  - Stage 2A proved that on system boot, the BIOS/firmware leaves Pad 14 deasserted high (`0x84000201`), allowing immediate SPI identification without resetting.

### 4.3 Interrupt Line Architecture: PCH Pad 229 (`GPP_D13` / `CPU_GP_0`)
- **ACPI Declaration:** `GpioInt(Edge, ActiveLow, ExclusiveAndWake, PullUp, 0, "\\_SB.PCI0.GPI0", 0, ResourceConsumer)` at pin offset `0x00E5` (229).
- **Physical Hardware Register:** Pad configuration register `0x00E5` in PCH Community 1.
- **Observed Hardware Baseline:** `0x82000100` (GPIO Input, RX value 1, pull-up enabled).
- **Electrical Polarity:** Active Low (`INT_N`).
  - In normal idle: Line pulled high by internal/external pull-up.
  - On finger touch / event: Line driven low by sensor to generate a falling-edge interrupt.
- **Interrupt Routing:**
  - Exposed via ACPI interrupt or UIO interrupt (`FTE3600_RESOURCE_IRQ`).
  - Status register `0x1A82` on sensor clarifies whether interrupt was triggered by touch down (`0x0002`), release up (`0x0004`), or image ready (`0x0020`).

---

## 5. Reference Driver Implementations Comparison

| Feature | `research/libfprint-fte3600` | Windows `ftWbioUmdfDriverV2.dll` | DKMS `alt/focal_spi.c` |
| :--- | :--- | :--- | :--- |
| **Model Classification** | Maps `0x9362` to `FTE3600_MODEL_FW9369` | Sensor Type 7 (`FT9362W`), class `clsFT9369` | Generic FocalTech SPI miscdevice (`/dev/focal_moh_spi`) |
| **Firmware Download** | None for FW9369 (`linux_firmware.c`) | None (`ft_loadfirmware.cpp` skips Type 7) | None |
| **Kernel Interface** | Glue driver (`fte3600.ko`) exposing spidev + gpio + uio | UMDF direct hardware access via `WUDFRd.sys` | Character device (`/dev/focal_moh_spi`) with custom ioctls |
| **Reset Handling** | Active-low GPIO via Linux GPIO v2 ioctl | Handled by ACPI / kernel power policy | Active-low reset pulse (`GPIOD_OUT_LOW`, 10ms low, 50ms wait) |
| **Clock/Power Gating** | Register `0xC6` write `0x01` + 4ms wait | `fw9369_config_power_mode` | Command `0x5A` / `0xA5` |
| **Finger Detection** | 4-channel FDT calibration + autonomous arming | `fw9369_fdt_mode_init` + `fw9369_fdt_auto_start` | Userspace daemon polling via ioctl |
| **Image Resolution** | 64 columns × 80 rows = 5,120 pixels | 64 × 80 = 5,120 pixels | 64 × 80 = 5,120 pixels |
| **Image Frame Size** | 10,240 bytes (16-bit ADC samples) | 10,240 bytes (16-bit ADC samples) | 10,240 bytes (16-bit ADC samples) |
| **Image FIFO Read** | Opcode `0x06 0xF9 0x9A 0x05 0x14 0x00` (10246 B) | `fw9369_fifo_read` (10246 B) | Custom read buffer `focal_spi_read` |

---

## 6. Synthesis and Architectural Conclusion

1. **The FT9362 is a modern sensor requiring zero external firmware files.**
   All necessary program code is in on-chip hardware/ROM.
2. **The host driver is responsible for analog and threshold calibration.**
   Because silicon characteristics, surface cover glass, and temperature vary, the sensor requires host-directed calibration:
   - AFE DAC tuning to place raw pixel medians at ~512.
   - FDT threshold tuning to measure parasitic capacitance and set finger touch/release trip points.
   - Image baseline frame acquisition to subtract static sensor fixed-pattern noise (FPN).
3. **The SPI communication protocol uses standard FocalTech framing:**
   - 1-byte register read/write (`0x08`/`0x09`).
   - 2-byte word register read/write (`0x04`/`0x05`).
   - Command triggers (`0x5A 0xA5`, `0xA5 0x5A`, `0xC0 0x3F`, `0xC2 0x3D`, `0xC4 0x3B`).
   - Burst FIFO read (`0x06 0xF9 0x9A 0x05 0x14 0x00`).
4. **All communication can be performed via standard Linux subsystems (`spidev`, `gpiod`/GPIO character device, or a minimal kernel driver).**
