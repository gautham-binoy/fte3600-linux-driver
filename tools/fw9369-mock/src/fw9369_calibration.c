/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fw9369_calibration.h"
#include "fw9369_protocol.h"
#include "fw9369_crc.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define CALIBRATION_MAX_TRIALS 32
#define ADC_READY_MAX_POLLS    5

static int send_reg_write_word(struct fw9369_transport *t, uint16_t addr, uint16_t val)
{
    uint8_t tx[8];
    int len = fw9369_encode_word_write(addr, val, tx, sizeof(tx));
    if (len < 0)
        return -EINVAL;
    return t->transfer(t->context, tx, NULL, (size_t)len);
}

static int send_reg_read_word(struct fw9369_transport *t, uint16_t addr, uint16_t *val)
{
    uint8_t tx[10];
    uint8_t rx[10];
    int len = fw9369_encode_word_read(addr, 1, tx, sizeof(tx));
    if (len < 0)
        return -EINVAL;
    int ret = t->transfer(t->context, tx, rx, (size_t)len);
    if (ret < 0)
        return ret;
    uint16_t wire_crc;
    return fw9369_decode_word_read(rx, (size_t)len, val, 1, &wire_crc);
}

static int sample_fdt_channels(struct fw9369_transport *t, uint8_t dac, uint16_t *channels)
{
    /* Set DAC in 0x1801 */
    int ret = send_reg_write_word(t, FW9369_REG_DAC_1801, (uint16_t)(0xFC80 | (dac & 0x7F)));
    if (ret < 0)
        return ret;

    /* Set timing 0x1881 |= 2 */
    ret = send_reg_write_word(t, FW9369_REG_TIMING_1881, 0x0082);
    if (ret < 0)
        return ret;

    /* Trigger scan: 0x1885 = 1 */
    ret = send_reg_write_word(t, FW9369_REG_SCAN_1885, 0x0001);
    if (ret < 0)
        return ret;

    /* Poll ADC ready bit in 0x1A82 (bit 3) */
    bool ready = false;
    for (int poll = 0; poll < ADC_READY_MAX_POLLS; ++poll) {
        uint16_t status = 0;
        ret = send_reg_read_word(t, FW9369_REG_STATUS_1A82, &status);
        if (ret < 0)
            return ret;
        if (status & 0x0008) {
            ready = true;
            break;
        }
        t->delay_ms(t->context, 1);
    }
    if (!ready)
        return -ETIMEDOUT;

    /* Read 4 channels from 0x80B8 */
    uint8_t tx[16];
    uint8_t rx[16];
    int len = fw9369_encode_word_read(FW9369_REG_ADC_80B8, 4, tx, sizeof(tx));
    if (len < 0)
        return -EINVAL;
    ret = t->transfer(t->context, tx, rx, (size_t)len);
    if (ret < 0)
        return ret;

    uint16_t wire_crc = 0;
    ret = fw9369_decode_word_read(rx, (size_t)len, channels, 4, &wire_crc);
    if (ret < 0)
        return ret;

    /* Verify hardware wire CRC */
    uint16_t expected_crc = fw9369_crc_wire(&rx[6], 8);
    if (wire_crc != expected_crc)
        return -EBADMSG;

    return 0;
}

int fw9369_calibrate_fdt(struct fw9369_transport *t, struct fw9369_fdt_calibration *cal)
{
    if (!t || !cal)
        return -EINVAL;

    memset(cal, 0, sizeof(*cal));

    /* Operation 1: DAC Centering Search */
    uint8_t dac = 27; /* Default initial seed for 4-channel mode */
    uint8_t lower = 0;
    uint8_t upper = 127;
    uint8_t prev_dac = 0;
    unsigned int prev_meas = 0;
    bool centered = false;

    for (int trial = 0; trial < CALIBRATION_MAX_TRIALS; ++trial) {
        uint16_t ch[4];
        int ret = sample_fdt_channels(t, dac, ch);
        if (ret < 0)
            return ret;

        unsigned int sum = (unsigned int)ch[0] + ch[1] + ch[2] + ch[3];
        unsigned int meas = sum / 4;

        if ((dac == prev_dac + 1 && prev_meas <= 512 && meas > 512) ||
            (prev_dac == dac + 1 && meas <= 512 && prev_meas > 512)) {
            dac = (dac > prev_dac) ? dac : prev_dac;
            centered = true;
            break;
        }

        if (dac < 1 || dac > 125)
            break;

        prev_dac = dac;
        prev_meas = meas;

        if (meas < 20) {
            if (dac == upper)
                break;
            lower = dac;
            dac = (uint8_t)((dac + upper) / 2);
        } else if (meas > 1000) {
            if (dac == lower)
                break;
            upper = dac;
            dac = (uint8_t)((lower + dac) / 2);
        } else {
            dac = (uint8_t)(meas <= 512 ? dac + 1 : dac - 1);
        }
    }

    if (!centered)
        return -EPROTO;

    cal->fdt_dac = dac;

    /* Operations 2 & 3: Up to 10 Stability Samples & Minimum Tracking */
    for (size_t c = 0; c < 4; ++c) {
        cal->minima[c] = 1023;
    }

    uint16_t prev_channels[4] = {0};
    uint8_t stable = 0;
    uint8_t sample_count = 0;

    for (unsigned int s = 0; s < 10; ++s) {
        uint16_t ch[4];
        sample_count++;
        int ret = sample_fdt_channels(t, cal->fdt_dac, ch);
        if (ret < 0)
            return ret;

        stable++;
        for (size_t c = 0; c < 4; ++c) {
            /* Operation 3: Absolute Minimum Tracking */
            if (ch[c] < cal->minima[c])
                cal->minima[c] = ch[c];

            /* Operation 2: Stability window check */
            if (s > 0) {
                int delta = (int)ch[c] - (int)prev_channels[c];
                if (delta > 5 || delta < -5)
                    stable = 1;
            }
            prev_channels[c] = ch[c];
        }

        /* Early exit if 3 consecutive samples stable within +-5 counts */
        if (s > 0 && stable >= 3)
            break;
    }

    cal->stability_samples = sample_count;

    /* Validation: Bounds check [300, 700] on all channels */
    for (size_t c = 0; c < 4; ++c) {
        if (cal->minima[c] < 300 || cal->minima[c] > 700)
            return -ERANGE;
    }

    /* Operation 4: Threshold Vector Calculation: max(0, minima[c] - 30) */
    for (size_t c = 0; c < 4; ++c) {
        cal->fdt_down[c] = (cal->minima[c] < 31) ? 0 : (uint16_t)(cal->minima[c] - 30);
    }

    /* 4-word little-endian CRC calculation */
    cal->fdt_down_crc = fw9369_crc_words_le(cal->fdt_down, 4);
    cal->valid = true;

    return 0;
}

int fw9369_acquire_image_baseline(struct fw9369_transport *t, struct fw9369_image_baseline *base)
{
    if (!t || !base)
        return -EINVAL;

    memset(base, 0, sizeof(*base));

    /* Set image DAC setting */
    base->image_dac = 54; /* Tuned nominal value */
    base->median = 512;

    /* Prepare FIFO read packet (10,246 bytes) */
    uint8_t *fifo_buf = calloc(FW9369_FIFO_TRANSFER_LEN, 1);
    if (!fifo_buf)
        return -ENOMEM;

    int pkt_len = fw9369_encode_fifo_read(fifo_buf, FW9369_FIFO_TRANSFER_LEN);
    if (pkt_len != FW9369_FIFO_TRANSFER_LEN) {
        free(fifo_buf);
        return -EINVAL;
    }

    uint8_t *rx_buf = calloc(FW9369_FIFO_TRANSFER_LEN, 1);
    if (!rx_buf) {
        free(fifo_buf);
        return -ENOMEM;
    }

    int ret = t->transfer(t->context, fifo_buf, rx_buf, (size_t)pkt_len);
    free(fifo_buf);

    if (ret < 0) {
        free(rx_buf);
        return ret;
    }

    /* Unpack 5,120 16-bit big-endian words into work image */
    const uint8_t *pixels_wire = rx_buf + FW9369_FIFO_PREFIX_LEN;
    uint16_t work_image[FW9369_IMAGE_PIXELS];

    for (size_t i = 0; i < FW9369_IMAGE_PIXELS; ++i) {
        work_image[i] = (uint16_t)(((uint16_t)pixels_wire[i * 2] << 8) | pixels_wire[i * 2 + 1]);
    }
    free(rx_buf);

    /*
     * Operation 5: Cold Image Baseline Copy
     * Bit-exact copy of single frame: zero averaging, zero temporal filtering.
     */
    memcpy(base->baseline, work_image, sizeof(base->baseline));

    /* Compute 128-word little-endian CRC signature over first 2 rows */
    base->baseline_crc = fw9369_crc_words_le(base->baseline, 128);
    base->valid = true;

    return 0;
}
