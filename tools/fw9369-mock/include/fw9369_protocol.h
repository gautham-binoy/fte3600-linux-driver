/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FW9369_PROTOCOL_H
#define FW9369_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* SPI Opcodes and their hardware complements */
#define FW9369_OP_BYTE_WRITE      0x09
#define FW9369_OP_BYTE_WRITE_COMP 0xF6
#define FW9369_OP_BYTE_READ       0x08
#define FW9369_OP_BYTE_READ_COMP  0xF7
#define FW9369_OP_WORD_WRITE      0x05
#define FW9369_OP_WORD_WRITE_COMP 0xFA
#define FW9369_OP_WORD_READ       0x04
#define FW9369_OP_WORD_READ_COMP  0xFB
#define FW9369_OP_FIFO_READ       0x06
#define FW9369_OP_FIFO_READ_COMP  0xF9

/* Key Sensor Registers */
#define FW9369_REG_WAKEUP_C6      0xC6
#define FW9369_REG_STATUS_80      0x80
#define FW9369_REG_BURST_8E       0x8E
#define FW9369_REG_BURST_90       0x90
#define FW9369_REG_BURST_91       0x91
#define FW9369_REG_BURST_92       0x92
#define FW9369_REG_PROTECT_9A     0x9A
#define FW9369_REG_FOUNDRY_F1     0xF1
#define FW9369_REG_FOUNDRY_F4     0xF4

/* Word Registers */
#define FW9369_REG_AFE_1800       0x1800
#define FW9369_REG_DAC_1801       0x1801
#define FW9369_REG_SHIFT_1804     0x1804
#define FW9369_REG_PERIOD_1807    0x1807
#define FW9369_REG_ROW_1808       0x1808
#define FW9369_REG_TIMING_1881    0x1881
#define FW9369_REG_SCAN_1885      0x1885
#define FW9369_REG_MODE_1887      0x1887
#define FW9369_REG_STATUS_1A82    0x1A82
#define FW9369_REG_STROBE_1A83    0x1A83
#define FW9369_REG_IRQ_1A84       0x1A84
#define FW9369_REG_CHIP_ID_1A8B   0x1A8B

/* Protected Analog Trim Bank */
#define FW9369_REG_TRIM_00C0      0x00C0
#define FW9369_REG_TRIM_00C1      0x00C1
#define FW9369_REG_TRIM_00C2      0x00C2

/* Detection Threshold Vector */
#define FW9369_REG_THRESH_80B0    0x80B0
#define FW9369_REG_THRESH_80B1    0x80B1
#define FW9369_REG_THRESH_80B2    0x80B2
#define FW9369_REG_THRESH_80B3    0x80B3
#define FW9369_REG_ADC_80B8       0x80B8

/* Commands */
#define FW9369_CMD_WAKE_A         0x5A
#define FW9369_CMD_WAKE_B         0xA5
#define FW9369_CMD_END_A          0xA5
#define FW9369_CMD_END_B          0x5A
#define FW9369_CMD_EXCEPTION_A    0xC0
#define FW9369_CMD_EXCEPTION_B    0x3F
#define FW9369_CMD_ARM_A          0xC2
#define FW9369_CMD_ARM_B          0x3D

/* Protection Keys */
#define FW9369_KEY_UNLOCK         0x5A
#define FW9369_KEY_LOCK           0x00

/* Constants */
#define FW9369_EXPECTED_CHIP_ID   0x9362
#define FW9369_EXPECTED_CHIP_CRC  0x1C53
#define FW9369_STATUS_IDLE        0x50

#define FW9369_IMAGE_WIDTH        64
#define FW9369_IMAGE_HEIGHT       80
#define FW9369_IMAGE_PIXELS       (FW9369_IMAGE_WIDTH * FW9369_IMAGE_HEIGHT) /* 5120 */
#define FW9369_IMAGE_BYTES        (FW9369_IMAGE_PIXELS * 2)                  /* 10240 */
#define FW9369_FIFO_PREFIX_LEN    6
#define FW9369_FIFO_TRANSFER_LEN  (FW9369_FIFO_PREFIX_LEN + FW9369_IMAGE_BYTES) /* 10246 */

/* Packet Encoding Helpers */
int fw9369_encode_byte_write(uint8_t addr, uint8_t val, uint8_t *pkt, size_t max_len);
int fw9369_encode_byte_read(uint8_t addr, uint8_t *pkt, size_t max_len);
int fw9369_decode_byte_read(const uint8_t *rx, size_t len, uint8_t *val);

int fw9369_encode_word_write(uint16_t addr, uint16_t val, uint8_t *pkt, size_t max_len);
int fw9369_encode_word_read(uint16_t addr, uint16_t count, uint8_t *pkt, size_t max_len);
int fw9369_decode_word_read(const uint8_t *rx, size_t len, uint16_t *words, size_t count, uint16_t *wire_crc);

int fw9369_encode_command(uint8_t c1, uint8_t c2, uint8_t *pkt, size_t max_len);
int fw9369_encode_fifo_read(uint8_t *pkt, size_t max_len);

/* Allowlist Validation */
bool fw9369_is_register_allowed(uint16_t reg, bool write);
bool fw9369_is_command_allowed(uint8_t c1, uint8_t c2);

#endif /* FW9369_PROTOCOL_H */
