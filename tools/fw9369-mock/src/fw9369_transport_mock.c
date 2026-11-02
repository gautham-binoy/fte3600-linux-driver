/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fw9369_transport.h"
#include "fw9369_protocol.h"
#include "fw9369_crc.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LOG_PACKETS 512
#define MAX_PACKET_LEN  FW9369_FIFO_TRANSFER_LEN

struct log_entry {
    uint8_t tx[MAX_PACKET_LEN];
    uint8_t rx[MAX_PACKET_LEN];
    size_t  len;
};

struct mock_context {
    struct fw9369_mock_config config;

    /* Simulated Sensor State */
    uint8_t  reg_byte[256];
    uint16_t reg_word_18xx[256];
    uint16_t reg_word_1axx[256];
    uint16_t reg_trim[16];   /* 0x00C0 - 0x00CF */
    uint16_t reg_thresh[16]; /* 0x80B0 - 0x80BF */

    uint8_t  mcu_state;
    bool     protected_unlocked;
    uint32_t fdt_sample_counter;

    /* Transaction Log */
    struct log_entry log[MAX_LOG_PACKETS];
    size_t log_count;
};

static uint16_t decode_address(uint8_t hi, uint8_t lo)
{
    if (hi == 0x98) {
        return (uint16_t)(0x1800 | lo);
    } else if (hi == 0x9A) {
        return (uint16_t)(0x1A00 | lo);
    } else if (hi == 0x80) {
        if (lo >= 0xB0 && lo <= 0xBF)
            return (uint16_t)(0x8000 | lo);
        if (lo >= 0xC0 && lo <= 0xCF)
            return (uint16_t)(0x0000 | lo);
        return (uint16_t)(0x8000 | lo);
    }
    return (uint16_t)(((uint16_t)(hi & 0x7F) << 8) | lo);
}

static void mock_delay_ms(void *context, uint32_t ms)
{
    (void)context;
    (void)ms;
}

static void mock_delay_us(void *context, uint32_t us)
{
    (void)context;
    (void)us;
}

static int mock_transfer(void *context, const uint8_t *tx, uint8_t *rx, size_t len)
{
    struct mock_context *ctx = context;

    if (!ctx || !tx || len == 0)
        return -EINVAL;

    /* Failure Injection: Transfer Failure */
    if (ctx->config.fail_transfers)
        return -EIO;

    /* Failure Injection: Short Transfer */
    if (ctx->config.short_transfer_len > 0 && len > ctx->config.short_transfer_len) {
        return -EMSGSIZE;
    }

    uint8_t simulated_rx[MAX_PACKET_LEN];
    memset(simulated_rx, 0, sizeof(simulated_rx));

    /* Failure Injection: Lock write failure */
    if (ctx->config.fail_lock_write && len == 4 &&
        tx[0] == FW9369_OP_BYTE_WRITE && tx[2] == FW9369_REG_PROTECT_9A && tx[3] == FW9369_KEY_LOCK) {
        return -EIO;
    }

    /* Command Packets */
    if (len == 3 && tx[2] == 0x00) {
        uint8_t c1 = tx[0];
        uint8_t c2 = tx[1];
        if (c1 == FW9369_CMD_WAKE_A && c2 == FW9369_CMD_WAKE_B) {
            ctx->mcu_state = FW9369_STATUS_IDLE;
        } else if (c1 == FW9369_CMD_END_A && c2 == FW9369_CMD_END_B) {
            /* Delimiter received */
        } else if (c1 == FW9369_CMD_ARM_A && c2 == FW9369_CMD_ARM_B) {
            /* Arm autonomous detection */
        }
    }
    /* Byte Write: [ 0x09, 0xF6, addr, val ] */
    else if (len == 4 && tx[0] == FW9369_OP_BYTE_WRITE && tx[1] == FW9369_OP_BYTE_WRITE_COMP) {
        uint8_t addr = tx[2];
        uint8_t val = tx[3];
        ctx->reg_byte[addr] = val;
        if (addr == FW9369_REG_PROTECT_9A) {
            ctx->protected_unlocked = (val == FW9369_KEY_UNLOCK);
        }
    }
    /* Byte Read: [ 0x08, 0xF7, addr, 0, 0 ] */
    else if (len == 5 && tx[0] == FW9369_OP_BYTE_READ && tx[1] == FW9369_OP_BYTE_READ_COMP) {
        uint8_t addr = tx[2];
        simulated_rx[0] = 0x00;
        simulated_rx[1] = 0x00;
        simulated_rx[2] = 0x00;
        simulated_rx[3] = 0x00;
        if (addr == FW9369_REG_STATUS_80) {
            simulated_rx[4] = ctx->mcu_state;
        } else {
            simulated_rx[4] = ctx->reg_byte[addr];
        }
    }
    /* Word Write: [ 0x05, 0xFA, addr_hi, addr_lo, count_hi, count_lo, data_hi, data_lo ] */
    else if (len == 8 && tx[0] == FW9369_OP_WORD_WRITE && tx[1] == FW9369_OP_WORD_WRITE_COMP) {
        uint16_t addr = decode_address(tx[2], tx[3]);
        uint16_t val = (uint16_t)(((uint16_t)tx[6] << 8) | tx[7]);

        if (addr >= 0x00C0 && addr <= 0x00CF) {
            if (ctx->config.interrupt_in_trim) {
                return -EINTR;
            }
            if (ctx->protected_unlocked) {
                ctx->reg_trim[addr - 0x00C0] = val;
            }
        } else if (addr >= 0x1800 && addr <= 0x18FF) {
            ctx->reg_word_18xx[addr - 0x1800] = val;
        } else if (addr >= 0x1A80 && addr <= 0x1AFF) {
            ctx->reg_word_1axx[addr - 0x1A80] = val;
        } else if (addr >= 0x80B0 && addr <= 0x80BF) {
            ctx->reg_thresh[addr - 0x80B0] = val;
        }
    }
    /* Word Read: [ 0x04, 0xFB, addr_hi, addr_lo, count_hi, count_lo, ... ] */
    else if (len >= 8 && tx[0] == FW9369_OP_WORD_READ && tx[1] == FW9369_OP_WORD_READ_COMP) {
        uint16_t addr = decode_address(tx[2], tx[3]);
        uint16_t count = (uint16_t)(((uint16_t)tx[4] << 8) | tx[5]);

        /* Chip ID interrogation at 0x1A8B */
        if (addr == FW9369_REG_CHIP_ID_1A8B && count == 1) {
            uint16_t id = ctx->config.fail_chip_id ? 0x9368 : FW9369_EXPECTED_CHIP_ID;
            simulated_rx[6] = (uint8_t)(id >> 8);
            simulated_rx[7] = (uint8_t)(id & 0xFF);
            uint16_t wire_crc = fw9369_crc_wire(&simulated_rx[6], 2);
            if (ctx->config.fail_crc) {
                wire_crc ^= 0xFFFF;
            }
            simulated_rx[8] = (uint8_t)(wire_crc >> 8);
            simulated_rx[9] = (uint8_t)(wire_crc & 0xFF);
        }
        /* ADC status read at 0x1A82 */
        else if (addr == FW9369_REG_STATUS_1A82 && count == 1) {
            uint16_t status = ctx->config.timeout_adc_ready ? 0x0000 : 0x0008; /* bit 3 is ready */
            simulated_rx[6] = (uint8_t)(status >> 8);
            simulated_rx[7] = (uint8_t)(status & 0xFF);
            uint16_t wire_crc = fw9369_crc_wire(&simulated_rx[6], 2);
            simulated_rx[8] = (uint8_t)(wire_crc >> 8);
            simulated_rx[9] = (uint8_t)(wire_crc & 0xFF);
        }
        /* 4-channel FDT ADC readings at 0x80B8 */
        else if (addr == FW9369_REG_ADC_80B8 && count == 4) {
            ctx->fdt_sample_counter++;

            /* Model realistic AFE behavior: measurement responds to DAC setting */
            uint8_t current_dac = (uint8_t)(ctx->reg_word_18xx[0x01] & 0x7F);
            int dac_offset = ((int)current_dac - 27) * 6;
            int simulated_base = 512 + dac_offset;
            if (simulated_base < 0) simulated_base = 0;
            if (simulated_base > 1023) simulated_base = 1023;

            uint16_t base_adc = (uint16_t)simulated_base;
            if (ctx->config.fdt_out_of_bounds) {
                base_adc = 250; /* Out of bounds < 300 */
            }

            for (size_t c = 0; c < 4; ++c) {
                uint16_t val = base_adc;
                if (ctx->config.fdt_jitter) {
                    val = (uint16_t)(base_adc + ((ctx->fdt_sample_counter % 2) ? 15 : -15));
                } else {
                    /* Low noise within +-1 count */
                    val = (uint16_t)(base_adc + (c % 2));
                }
                simulated_rx[6 + c * 2] = (uint8_t)(val >> 8);
                simulated_rx[7 + c * 2] = (uint8_t)(val & 0xFF);
            }
            uint16_t wire_crc = fw9369_crc_wire(&simulated_rx[6], 8);
            if (ctx->config.fail_crc) {
                wire_crc ^= 0xFFFF;
            }
            simulated_rx[14] = (uint8_t)(wire_crc >> 8);
            simulated_rx[15] = (uint8_t)(wire_crc & 0xFF);
        }
        /* Trim Bank Read 0x00C0 */
        else if (addr >= 0x00C0 && addr <= 0x00CF && count == 1) {
            uint16_t val = ctx->protected_unlocked ? ctx->reg_trim[addr - 0x00C0] : 0x0000;
            simulated_rx[6] = (uint8_t)(val >> 8);
            simulated_rx[7] = (uint8_t)(val & 0xFF);
        }
        /* Other word registers */
        else if (addr >= 0x1800 && addr <= 0x18FF && count == 1) {
            uint16_t val = ctx->reg_word_18xx[addr - 0x1800];
            simulated_rx[6] = (uint8_t)(val >> 8);
            simulated_rx[7] = (uint8_t)(val & 0xFF);
        }
    }
    /* Burst FIFO Read: [ 0x06, 0xF9, 0x9A, 0x05, 0x14, 0x00, ... ] */
    else if (len == FW9369_FIFO_TRANSFER_LEN &&
             tx[0] == FW9369_OP_FIFO_READ && tx[1] == FW9369_OP_FIFO_READ_COMP) {
        if (ctx->config.fail_fifo_read) {
            return -EIO;
        }
        /* Populate simulated frame with deterministic pixel gradient (median 512) */
        for (size_t p = 0; p < FW9369_IMAGE_PIXELS; ++p) {
            uint16_t pixel = (uint16_t)(500 + (p % 25));
            simulated_rx[FW9369_FIFO_PREFIX_LEN + p * 2]     = (uint8_t)(pixel >> 8);
            simulated_rx[FW9369_FIFO_PREFIX_LEN + p * 2 + 1] = (uint8_t)(pixel & 0xFF);
        }
    }

    if (rx) {
        memcpy(rx, simulated_rx, len);
    }

    /* Record into transaction log */
    if (ctx->log_count < MAX_LOG_PACKETS) {
        struct log_entry *entry = &ctx->log[ctx->log_count++];
        size_t copy_len = len < MAX_PACKET_LEN ? len : MAX_PACKET_LEN;
        memcpy(entry->tx, tx, copy_len);
        if (rx)
            memcpy(entry->rx, rx, copy_len);
        entry->len = len;
    }

    return 0;
}

static void mock_close(void *context)
{
    (void)context;
}

struct fw9369_transport *fw9369_mock_transport_create(const struct fw9369_mock_config *cfg)
{
    struct fw9369_transport *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;

    struct mock_context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        free(t);
        return NULL;
    }

    if (cfg)
        ctx->config = *cfg;

    /* Initialize default power-on register state */
    ctx->reg_byte[FW9369_REG_WAKEUP_C6] = 0x00;
    ctx->reg_byte[FW9369_REG_PROTECT_9A] = 0x00;
    ctx->mcu_state = 0x00;
    ctx->protected_unlocked = false;

    t->context = ctx;
    t->transfer = mock_transfer;
    t->delay_ms = mock_delay_ms;
    t->delay_us = mock_delay_us;
    t->close = mock_close;

    return t;
}

void fw9369_mock_transport_destroy(struct fw9369_transport *t)
{
    if (t) {
        if (t->context)
            free(t->context);
        free(t);
    }
}

size_t fw9369_mock_get_tx_count(struct fw9369_transport *t)
{
    if (!t || !t->context)
        return 0;
    struct mock_context *ctx = t->context;
    return ctx->log_count;
}

int fw9369_mock_get_last_tx(struct fw9369_transport *t, uint8_t *out_buf, size_t buf_cap, size_t *out_len)
{
    if (out_len)
        *out_len = 0;

    if (!t || !t->context || !out_buf || buf_cap == 0)
        return -EINVAL;

    struct mock_context *ctx = t->context;
    if (ctx->log_count == 0)
        return -ENODATA;

    const struct log_entry *entry = &ctx->log[ctx->log_count - 1];
    size_t payload_len = entry->len;

    if (out_len)
        *out_len = entry->len;

    if (buf_cap < payload_len)
        return -ENOBUFS;

    memcpy(out_buf, entry->tx, payload_len);
    return (int)payload_len;
}

uint8_t fw9369_mock_get_reg_byte(struct fw9369_transport *t, uint8_t addr)
{
    if (!t || !t->context)
        return 0;
    struct mock_context *ctx = t->context;
    return ctx->reg_byte[addr];
}

uint16_t fw9369_mock_get_reg_word(struct fw9369_transport *t, uint16_t addr)
{
    if (!t || !t->context)
        return 0;
    struct mock_context *ctx = t->context;
    if (addr >= 0x1800 && addr <= 0x18FF)
        return ctx->reg_word_18xx[addr - 0x1800];
    if (addr >= 0x1A80 && addr <= 0x1AFF)
        return ctx->reg_word_1axx[addr - 0x1A80];
    if (addr >= 0x00C0 && addr <= 0x00CF)
        return ctx->reg_trim[addr - 0x00C0];
    return 0;
}

bool fw9369_mock_is_protected_locked(struct fw9369_transport *t)
{
    if (!t || !t->context)
        return true;
    struct mock_context *ctx = t->context;
    return !ctx->protected_unlocked;
}

struct fw9369_transport *fw9369_hardware_transport_create(const char *device_path)
{
    (void)device_path;
    /*
     * ABSOLUTE SAFETY GUARANTEE:
     * This stub unconditionally rejects hardware selection.
     * It NEVER calls open(), stat(), access(), or any I/O syscall.
     */
    errno = ENOTSUP;
    return NULL;
}
