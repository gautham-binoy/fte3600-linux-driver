/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fw9369_transport_hw.h"
#include "fw9369_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("Hardware Preflight Utility (Stage 2C.5B Offline Build)\n");
    printf("Options:\n");
    printf("  --validate-config    Validate hardware configuration offline (no I/O)\n");
    printf("  --device=<path>      Set explicit SPI device path (default: /dev/spidev1.0)\n");
    printf("  --speed=<hz>         Set bus speed in Hz (default: 1000000, max: 1000000)\n");
    printf("  --help               Display this help message\n");
    printf("\nNote: Physical hardware execution is strictly blocked in Stage 2C.5B.\n");
}

int main(int argc, char **argv)
{
    struct fw9369_hw_config cfg = {
        .device_path = FW9369_HW_DEFAULT_DEV_PATH,
        .speed_hz = FW9369_HW_DEFAULT_SPEED_HZ,
        .mode = FW9369_HW_DEFAULT_MODE,
        .bits_per_word = FW9369_HW_DEFAULT_BITS,
        .lsb_first = 0,
        .max_transfer_len = FW9369_HW_MAX_TRANSFER_LEN,
        .enable_logging = true,
    };

    bool validate_only = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--validate-config") == 0) {
            validate_only = true;
        } else if (strncmp(argv[i], "--device=", 9) == 0) {
            cfg.device_path = argv[i] + 9;
        } else if (strncmp(argv[i], "--speed=", 8) == 0) {
            cfg.speed_hz = (uint32_t)strtoul(argv[i] + 8, NULL, 10);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    printf("===================================================================\n");
    printf(" FocalTech FW9369 Hardware Transport Tool (Stage 2C.5B)\n");
    printf(" Safety Boundary: Physical hardware execution is STRICTLY PROHIBITED\n");
    printf(" Target Device:   %s\n", cfg.device_path ? cfg.device_path : "(null)");
    printf(" Target Speed:    %u Hz\n", cfg.speed_hz);
    printf("===================================================================\n\n");

    int val_ret = fw9369_hw_validate_config(&cfg);
    if (val_ret != 0) {
        fprintf(stderr, "[CONFIG ERROR] Invalid hardware configuration: code %d\n", val_ret);
        return 1;
    }
    printf("[PASS] Offline configuration validation: OK (Mode 0, 8-bit, MSB-first, <= 1 MHz)\n");

    if (validate_only) {
        printf("[INFO] Offline validation complete. Exiting cleanly without device access.\n");
        return 0;
    }

    /*
     * FAIL-CLOSED HARDWARE GATE:
     * In Stage 2C.5B, physical hardware communication is strictly prohibited.
     * The hardware tool will NEVER open device nodes or issue SPI transactions.
     */
    printf("\n[SECURITY ALERT] Stage 2C.5B is static preparation only.\n");
    printf("[SAFETY GATE PASS] Physical device access is blocked by policy.\n");
    printf("To verify hardware transport mechanics offline, execute bin/fw9369-tests.\n");
    return 1;
}
