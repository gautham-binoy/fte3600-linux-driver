/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fw9369_state.h"
#include "fw9369_protocol.h"
#include "fw9369_crc.h"

#include <errno.h>
#include <string.h>

static int write_byte(struct fw9369_transport *t, uint8_t addr, uint8_t val)
{
    uint8_t tx[4];
    int len = fw9369_encode_byte_write(addr, val, tx, sizeof(tx));
    if (len < 0)
        return -EINVAL;
    return t->transfer(t->context, tx, NULL, (size_t)len);
}

static int read_byte(struct fw9369_transport *t, uint8_t addr, uint8_t *val)
{
    uint8_t tx[5];
    uint8_t rx[5];
    int len = fw9369_encode_byte_read(addr, tx, sizeof(tx));
    if (len < 0)
        return -EINVAL;
    int ret = t->transfer(t->context, tx, rx, (size_t)len);
    if (ret < 0)
        return ret;
    return fw9369_decode_byte_read(rx, (size_t)len, val);
}

static int write_word(struct fw9369_transport *t, uint16_t addr, uint16_t val)
{
    uint8_t tx[8];
    int len = fw9369_encode_word_write(addr, val, tx, sizeof(tx));
    if (len < 0)
        return -EINVAL;
    return t->transfer(t->context, tx, NULL, (size_t)len);
}

static int read_word(struct fw9369_transport *t, uint16_t addr, uint16_t *val, uint16_t *wire_crc)
{
    uint8_t tx[10];
    uint8_t rx[10];
    int len = fw9369_encode_word_read(addr, 1, tx, sizeof(tx));
    if (len < 0)
        return -EINVAL;
    int ret = t->transfer(t->context, tx, rx, (size_t)len);
    if (ret < 0)
        return ret;
    return fw9369_decode_word_read(rx, (size_t)len, val, 1, wire_crc);
}

static int send_command(struct fw9369_transport *t, uint8_t c1, uint8_t c2)
{
    uint8_t tx[3];
    int len = fw9369_encode_command(c1, c2, tx, sizeof(tx));
    if (len < 0)
        return -EINVAL;
    return t->transfer(t->context, tx, NULL, (size_t)len);
}

void fw9369_pipeline_abort_cleanup(struct fw9369_transport *t, struct fw9369_pipeline_context *ctx)
{
    if (!t || !ctx)
        return;

    /* Structured cleanup: always attempt to lock 0x9A on abort */
    int ret = write_byte(t, FW9369_REG_PROTECT_9A, FW9369_KEY_LOCK);
    if (ret < 0) {
        ctx->lock_outcome = FW9369_LOCK_TRANSMISSION_FAILED;
    } else {
        ctx->lock_outcome = FW9369_LOCK_TRANSMITTED_CLEAN;
    }
    ctx->phase = FW9369_PHASE_ABORTED;
}

int fw9369_pipeline_run(struct fw9369_transport *t, struct fw9369_pipeline_context *ctx)
{
    if (!t || !ctx)
        return -EINVAL;

    memset(ctx, 0, sizeof(*ctx));

    /* =========================================================================
     * Phase 1: Bus Wakeup & Identity Verification
     * ========================================================================= */
    ctx->phase = FW9369_PHASE_1_WAKEUP;

    bool wakeup_ok = false;
    for (int retry = 0; retry < 31; ++retry) {
        int ret = write_byte(t, FW9369_REG_WAKEUP_C6, 0x01);
        if (ret < 0) {
            ctx->last_error = ret;
            goto fail;
        }
        t->delay_ms(t->context, 4);

        uint8_t val = 0;
        ret = read_byte(t, FW9369_REG_WAKEUP_C6, &val);
        if (ret < 0) {
            ctx->last_error = ret;
            goto fail;
        }
        if (val == 0x01) {
            wakeup_ok = true;
            break;
        }
    }
    if (!wakeup_ok) {
        ctx->last_error = -ETIMEDOUT;
        goto fail;
    }

    /* Interrogate Chip ID 0x1A8B */
    uint16_t id = 0;
    uint16_t wire_crc = 0;
    int ret = read_word(t, FW9369_REG_CHIP_ID_1A8B, &id, &wire_crc);
    if (ret < 0) {
        ctx->last_error = ret;
        goto fail;
    }

    ctx->identified_chip_id = id;
    ctx->identified_chip_crc = wire_crc;

    if (id != FW9369_EXPECTED_CHIP_ID) {
        ctx->last_error = -ENODEV;
        goto fail;
    }

    /* Verify Chip ID wire CRC against [ 0x93, 0x62 ] */
    uint8_t id_bytes[2] = { (uint8_t)(id >> 8), (uint8_t)(id & 0xFF) };
    uint16_t expected_crc = fw9369_crc_wire(id_bytes, 2);
    if (wire_crc != expected_crc) {
        ctx->last_error = -EBADMSG;
        goto fail;
    }

    /* Clear pending interrupts: 0x1A84 = 0xFFFF */
    ret = write_word(t, FW9369_REG_IRQ_1A84, 0xFFFF);
    if (ret < 0) {
        ctx->last_error = ret;
        goto fail;
    }

    /* =========================================================================
     * Phase 2: MCU Idle Handshake & Foundry Setup
     * ========================================================================= */
    ctx->phase = FW9369_PHASE_2_IDLE;

    /* Opening wake command: 0x5A 0xA5 */
    ret = send_command(t, FW9369_CMD_WAKE_A, FW9369_CMD_WAKE_B);
    if (ret < 0) {
        ctx->last_error = ret;
        goto fail;
    }
    t->delay_ms(t->context, 1);

    /* Poll status 0x80 expecting 0x50 */
    uint8_t status = 0;
    bool idle_ok = false;
    for (int i = 0; i < 5; ++i) {
        ret = read_byte(t, FW9369_REG_STATUS_80, &status);
        if (ret < 0) {
            ctx->last_error = ret;
            goto fail;
        }
        if (status == FW9369_STATUS_IDLE) {
            idle_ok = true;
            break;
        }
        /* Send exception strobe if needed */
        (void)send_command(t, FW9369_CMD_EXCEPTION_A, FW9369_CMD_EXCEPTION_B);
        t->delay_ms(t->context, 1);
    }

    /* Closing delimiter: 0xA5 0x5A (always sent) */
    (void)send_command(t, FW9369_CMD_END_A, FW9369_CMD_END_B);

    if (!idle_ok) {
        ctx->last_error = -EPROTO;
        goto fail;
    }
    ctx->mcu_status = status;

    /* Foundry test strobes */
    for (unsigned int i = 0; i < 2; ++i) {
        (void)write_byte(t, FW9369_REG_FOUNDRY_F1, i ? 0x13 : 0x03);
        (void)write_byte(t, FW9369_REG_FOUNDRY_F4, 0xC0);
        (void)write_word(t, FW9369_REG_STROBE_1A83, 0x0000);
        (void)write_word(t, FW9369_REG_STROBE_1A83, 0x0010);
        (void)write_word(t, FW9369_REG_STROBE_1A83, 0x0000);
    }

    /* =========================================================================
     * Phase 3: Capacitive AFE Tuning & FDT Calibration
     * ========================================================================= */
    ctx->phase = FW9369_PHASE_3_FDT;

    /* Configure AFE timing registers */
    ret = write_word(t, FW9369_REG_AFE_1800, 0x07FE);
    if (ret < 0) { ctx->last_error = ret; goto fail; }
    ret = write_word(t, FW9369_REG_SHIFT_1804, 0x27C8);
    if (ret < 0) { ctx->last_error = ret; goto fail; }
    ret = write_word(t, FW9369_REG_PERIOD_1807, 0x1671);
    if (ret < 0) { ctx->last_error = ret; goto fail; }
    ret = write_word(t, FW9369_REG_ROW_1808, 0x0801);
    if (ret < 0) { ctx->last_error = ret; goto fail; }
    ret = write_word(t, FW9369_REG_MODE_1887, 0x0005);
    if (ret < 0) { ctx->last_error = ret; goto fail; }

    /*
     * Structured RAII / Protected Block: 0x9A
     * Guarantees lock write 0x9A = 0x00 is submitted on both success and error.
     */
    ret = write_byte(t, FW9369_REG_PROTECT_9A, FW9369_KEY_UNLOCK);
    if (ret < 0) {
        ctx->last_error = ret;
        goto fail;
    }

    /* Trim bank configuration */
    int trim_err = 0;
    if (write_word(t, FW9369_REG_TRIM_00C0, 0x0444) < 0 ||
        write_word(t, FW9369_REG_TRIM_00C1, 0x0021) < 0 ||
        write_word(t, FW9369_REG_TRIM_00C2, 0x00C0) < 0) {
        trim_err = -EIO;
    }

    /* Unconditional lock cleanup */
    int lock_ret = write_byte(t, FW9369_REG_PROTECT_9A, FW9369_KEY_LOCK);
    if (lock_ret < 0) {
        ctx->lock_outcome = FW9369_LOCK_TRANSMISSION_FAILED;
    } else {
        ctx->lock_outcome = FW9369_LOCK_TRANSMITTED_CLEAN;
    }

    if (trim_err < 0) {
        ctx->last_error = trim_err;
        goto fail;
    }
    if (lock_ret < 0) {
        ctx->last_error = lock_ret;
        goto fail;
    }

    /* Execute FDT DAC search & minimum tracking */
    ret = fw9369_calibrate_fdt(t, &ctx->fdt);
    if (ret < 0) {
        ctx->last_error = ret;
        goto fail;
    }

    /* =========================================================================
     * Phase 4: Image Baseline Acquisition & DAC Tuning
     * ========================================================================= */
    ctx->phase = FW9369_PHASE_4_IMAGE;

    ret = fw9369_acquire_image_baseline(t, &ctx->baseline);
    if (ret < 0) {
        ctx->last_error = ret;
        goto fail;
    }

    /* =========================================================================
     * Phase 5: Autonomous Finger Detection Arming
     * ========================================================================= */
    ctx->phase = FW9369_PHASE_5_ARM;

    /* Program calibrated threshold vector */
    for (size_t c = 0; c < 4; ++c) {
        ret = write_word(t, (uint16_t)(FW9369_REG_THRESH_80B0 + c), ctx->fdt.fdt_down[c]);
        if (ret < 0) {
            ctx->last_error = ret;
            goto fail;
        }
    }

    /* Arm interrupt configuration */
    ret = write_word(t, FW9369_REG_STROBE_1A83, 0x0016);
    if (ret < 0) { ctx->last_error = ret; goto fail; }
    ret = write_word(t, FW9369_REG_IRQ_1A84, 0x002F);
    if (ret < 0) { ctx->last_error = ret; goto fail; }

    /* Arm command: 0xC2 0x3D */
    ret = send_command(t, FW9369_CMD_ARM_A, FW9369_CMD_ARM_B);
    if (ret < 0) { ctx->last_error = ret; goto fail; }
    t->delay_ms(t->context, 1);

    ctx->armed = true;
    ctx->phase = FW9369_PHASE_COMPLETED;
    return 0;

fail:
    fw9369_pipeline_abort_cleanup(t, ctx);
    return ctx->last_error ? ctx->last_error : -EIO;
}
