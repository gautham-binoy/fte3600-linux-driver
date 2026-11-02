/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fw9369_state.h"
#include "fw9369_transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  --mock               Run in simulated mock mode (default)\n");
    printf("  --dry-run            Simulate execution and print planned transactions\n");
    printf("  --hardware           Attempt real hardware backend (REJECTED by safety policy)\n");
    printf("  --inject-fail=<type> Inject simulated failure:\n");
    printf("                         transfer, short, chipid, crc, timeout, jitter, bounds, lock, fifo\n");
    printf("  --help               Display this help message\n");
}

int main(int argc, char **argv)
{
    struct fw9369_mock_config cfg;
    memset(&cfg, 0, sizeof(cfg));

    bool dry_run = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--mock") == 0) {
            /* Mock is default */
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        } else if (strcmp(argv[i], "--hardware") == 0) {
            printf("[SECURITY ALERT] Hardware mode is strictly prohibited in Stage 2C.4!\n");
            struct fw9369_transport *hw = fw9369_hardware_transport_create("/dev/spidev1.0");
            if (!hw) {
                printf("[SAFETY GATE PASS] Hardware backend was rejected without accessing any device node.\n");
                return 1;
            }
        } else if (strncmp(argv[i], "--inject-fail=", 14) == 0) {
            const char *type = argv[i] + 14;
            if (strcmp(type, "transfer") == 0) cfg.fail_transfers = true;
            else if (strcmp(type, "short") == 0) cfg.short_transfer_len = 4;
            else if (strcmp(type, "chipid") == 0) cfg.fail_chip_id = true;
            else if (strcmp(type, "crc") == 0) cfg.fail_crc = true;
            else if (strcmp(type, "timeout") == 0) cfg.timeout_adc_ready = true;
            else if (strcmp(type, "jitter") == 0) cfg.fdt_jitter = true;
            else if (strcmp(type, "bounds") == 0) cfg.fdt_out_of_bounds = true;
            else if (strcmp(type, "lock") == 0) cfg.fail_lock_write = true;
            else if (strcmp(type, "fifo") == 0) cfg.fail_fifo_read = true;
            else {
                fprintf(stderr, "Unknown failure type: %s\n", type);
                return 1;
            }
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
    printf(" FocalTech FW9369 Userspace Tool (Mock Engine v1.0)\n");
    printf(" Mode: %s\n", dry_run ? "DRY-RUN SIMULATION" : "MOCK IN-MEMORY EXECUTION");
    printf(" Safety Boundary: Physical hardware access is STRICTLY PROHIBITED\n");
    printf("===================================================================\n\n");

    struct fw9369_transport *t = fw9369_mock_transport_create(&cfg);
    if (!t) {
        fprintf(stderr, "Failed to create mock transport\n");
        return 1;
    }

    struct fw9369_pipeline_context ctx;
    int ret = fw9369_pipeline_run(t, &ctx);

    printf("Execution Summary:\n");
    printf("  Final Phase:         %s\n", ctx.phase == FW9369_PHASE_COMPLETED ? "COMPLETED" : "ABORTED");
    printf("  Identified Chip ID:  0x%04X (Expected: 0x9362)\n", ctx.identified_chip_id);
    printf("  Identified Wire CRC: 0x%04X (Expected: 0x1C53)\n", ctx.identified_chip_crc);
    printf("  MCU Status:          0x%02X (Expected: 0x50)\n", ctx.mcu_status);
    printf("  FDT Calibrated DAC:  %u\n", ctx.fdt.fdt_dac);
    printf("  FDT Minima Tracked:  [ %u, %u, %u, %u ]\n",
           ctx.fdt.minima[0], ctx.fdt.minima[1], ctx.fdt.minima[2], ctx.fdt.minima[3]);
    printf("  FDT Down Thresholds: [ %u, %u, %u, %u ]\n",
           ctx.fdt.fdt_down[0], ctx.fdt.fdt_down[1], ctx.fdt.fdt_down[2], ctx.fdt.fdt_down[3]);
    printf("  FDT Threshold CRC:   0x%04X\n", ctx.fdt.fdt_down_crc);
    printf("  Image Baseline CRC:  0x%04X (First 128 words signature)\n", ctx.baseline.baseline_crc);
    printf("  Protected 0x9A Lock: %s\n",
           ctx.lock_outcome == FW9369_LOCK_TRANSMITTED_CLEAN ? "TRANSMITTED CLEAN (0x00)" :
           ctx.lock_outcome == FW9369_LOCK_TRANSMISSION_FAILED ? "TRANSMISSION FAILED" : "UNKNOWN");
    printf("  Detection Armed:     %s\n", ctx.armed ? "YES" : "NO");
    printf("  Transactions Logged: %zu\n", fw9369_mock_get_tx_count(t));

    fw9369_mock_transport_destroy(t);

    if (ret != 0) {
        printf("\n>> Pipeline Execution: FAILED (code: %d)\n", ret);
        return 1;
    }

    printf("\n>> Pipeline Execution: SUCCESS\n");
    return 0;
}
