/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fw9369_protocol.h"
#include <string.h>

int fw9369_encode_byte_write(uint8_t addr, uint8_t val, uint8_t *pkt, size_t max_len)
{
    if (!pkt || max_len < 4)
        return -1;
    pkt[0] = FW9369_OP_BYTE_WRITE;
    pkt[1] = FW9369_OP_BYTE_WRITE_COMP;
    pkt[2] = addr;
    pkt[3] = val;
    return 4;
}

int fw9369_encode_byte_read(uint8_t addr, uint8_t *pkt, size_t max_len)
{
    if (!pkt || max_len < 5)
        return -1;
    pkt[0] = FW9369_OP_BYTE_READ;
    pkt[1] = FW9369_OP_BYTE_READ_COMP;
    pkt[2] = addr;
    pkt[3] = 0x00;
    pkt[4] = 0x00;
    return 5;
}

int fw9369_decode_byte_read(const uint8_t *rx, size_t len, uint8_t *val)
{
    if (!rx || !val || len < 5)
        return -1;
    *val = rx[4];
    return 0;
}

int fw9369_encode_word_write(uint16_t addr, uint16_t val, uint8_t *pkt, size_t max_len)
{
    if (!pkt || max_len < 8)
        return -1;
    pkt[0] = FW9369_OP_WORD_WRITE;
    pkt[1] = FW9369_OP_WORD_WRITE_COMP;
    pkt[2] = (uint8_t)(0x80 | (addr >> 8));
    pkt[3] = (uint8_t)(addr & 0xFF);
    pkt[4] = 0x00;
    pkt[5] = 0x01; /* 1 word */
    pkt[6] = (uint8_t)(val >> 8);
    pkt[7] = (uint8_t)(val & 0xFF);
    return 8;
}

int fw9369_encode_word_read(uint16_t addr, uint16_t count, uint8_t *pkt, size_t max_len)
{
    size_t req_len = 6 + (size_t)count * 2 + 2; /* 6 preamble + count*2 data + 2 CRC */
    if (!pkt || max_len < req_len)
        return -1;
    memset(pkt, 0, req_len);
    pkt[0] = FW9369_OP_WORD_READ;
    pkt[1] = FW9369_OP_WORD_READ_COMP;
    pkt[2] = (uint8_t)(0x80 | (addr >> 8));
    pkt[3] = (uint8_t)(addr & 0xFF);
    pkt[4] = (uint8_t)(count >> 8);
    pkt[5] = (uint8_t)(count & 0xFF);
    return (int)req_len;
}

int fw9369_decode_word_read(const uint8_t *rx, size_t len, uint16_t *words, size_t count, uint16_t *wire_crc)
{
    size_t req_len = 6 + (size_t)count * 2 + 2;
    if (!rx || !words || len < req_len)
        return -1;

    for (size_t i = 0; i < count; ++i) {
        words[i] = (uint16_t)(((uint16_t)rx[6 + i * 2] << 8) | (uint16_t)rx[7 + i * 2]);
    }
    if (wire_crc) {
        *wire_crc = (uint16_t)(((uint16_t)rx[6 + count * 2] << 8) | (uint16_t)rx[7 + count * 2]);
    }
    return 0;
}

int fw9369_encode_command(uint8_t c1, uint8_t c2, uint8_t *pkt, size_t max_len)
{
    if (!pkt || max_len < 3)
        return -1;
    pkt[0] = c1;
    pkt[1] = c2;
    pkt[2] = 0x00;
    return 3;
}

int fw9369_encode_fifo_read(uint8_t *pkt, size_t max_len)
{
    static const uint8_t prefix[6] = {
        FW9369_OP_FIFO_READ, FW9369_OP_FIFO_READ_COMP,
        0x9A, 0x05, 0x14, 0x00
    };
    if (!pkt || max_len < FW9369_FIFO_TRANSFER_LEN)
        return -1;
    memset(pkt, 0, FW9369_FIFO_TRANSFER_LEN);
    memcpy(pkt, prefix, sizeof(prefix));
    return FW9369_FIFO_TRANSFER_LEN;
}

bool fw9369_is_register_allowed(uint16_t reg, bool write)
{
    /* Allowlist based on FW9369_UNIFIED_INITIALIZATION_SPECIFICATION.md */
    switch (reg) {
    /* Read-Only Registers */
    case FW9369_REG_STATUS_80:
    case FW9369_REG_CHIP_ID_1A8B:
    case FW9369_REG_STATUS_1A82:
    case FW9369_REG_ADC_80B8:
        return !write;

    /* Write-Only Registers */
    case FW9369_REG_BURST_8E:
    case FW9369_REG_BURST_90:
    case FW9369_REG_BURST_91:
    case FW9369_REG_BURST_92:
    case FW9369_REG_PROTECT_9A:
    case FW9369_REG_FOUNDRY_F1:
    case FW9369_REG_FOUNDRY_F4:
    case FW9369_REG_SCAN_1885:
    case FW9369_REG_STROBE_1A83:
    case FW9369_REG_IRQ_1A84:
        return write;

    /* Read / Write Registers */
    case FW9369_REG_WAKEUP_C6:
    case FW9369_REG_AFE_1800:
    case FW9369_REG_DAC_1801:
    case FW9369_REG_SHIFT_1804:
    case FW9369_REG_PERIOD_1807:
    case FW9369_REG_ROW_1808:
    case FW9369_REG_TIMING_1881:
    case FW9369_REG_MODE_1887:
    case FW9369_REG_TRIM_00C0:
    case FW9369_REG_TRIM_00C1:
    case FW9369_REG_TRIM_00C2:
    case FW9369_REG_THRESH_80B0:
    case FW9369_REG_THRESH_80B1:
    case FW9369_REG_THRESH_80B2:
    case FW9369_REG_THRESH_80B3:
        return true;

    default:
        return false;
    }
}

bool fw9369_is_command_allowed(uint8_t c1, uint8_t c2)
{
    if (c1 == FW9369_CMD_WAKE_A && c2 == FW9369_CMD_WAKE_B)
        return true;
    if (c1 == FW9369_CMD_END_A && c2 == FW9369_CMD_END_B)
        return true;
    if (c1 == FW9369_CMD_EXCEPTION_A && c2 == FW9369_CMD_EXCEPTION_B)
        return true;
    if (c1 == FW9369_CMD_ARM_A && c2 == FW9369_CMD_ARM_B)
        return true;
    return false;
}
