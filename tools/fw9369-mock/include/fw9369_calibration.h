/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FW9369_CALIBRATION_H
#define FW9369_CALIBRATION_H

#include "fw9369_transport.h"
#include "fw9369_protocol.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * struct fw9369_fdt_calibration - Calibrated FDT channel parameters
 */
struct fw9369_fdt_calibration {
    uint8_t  fdt_dac;              /* Tuned setting 0..127 in register 0x1801 */
    uint16_t minima[4];            /* Tracked per-channel minimum ADC count (300..700) */
    uint16_t fdt_down[4];          /* Derived thresholds: max(0, minima[c] - 30) */
    uint16_t fdt_down_crc;         /* Little-endian 4-word CRC */
    uint8_t  stability_samples;    /* Actual sample count executed (3..10) */
    bool     valid;                /* Calibration completion flag */
};

/**
 * struct fw9369_image_baseline - Calibrated image baseline frame
 */
struct fw9369_image_baseline {
    uint8_t  image_dac;            /* Tuned pixel DAC (~54) */
    uint16_t median;               /* Target 512 */
    uint16_t baseline[FW9369_IMAGE_PIXELS]; /* Raw frame 64x80 words (10,240 bytes) */
    uint16_t baseline_crc;         /* 128-word CRC signature over first 2 rows */
    bool     valid;                /* Baseline ready flag */
};

/**
 * Calibrates FDT channels:
 * - Executes DAC centering search on 0x1801
 * - Samples stability window (up to 10 samples, early exit if 3 consecutive stable)
 * - Tracks per-channel minima
 * - Validates bounds [300, 700]
 * - Calculates threshold vector and 4-word CRC
 */
int fw9369_calibrate_fdt(struct fw9369_transport *t, struct fw9369_fdt_calibration *cal);

/**
 * Acquires cold image baseline:
 * - Tunes image_dac targeting median 512
 * - Reads 10,246-byte single-burst FIFO
 * - Copies frame bit-exact into baseline (single frame, no averaging)
 * - Computes 128-word CRC signature
 */
int fw9369_acquire_image_baseline(struct fw9369_transport *t, struct fw9369_image_baseline *base);

#endif /* FW9369_CALIBRATION_H */
