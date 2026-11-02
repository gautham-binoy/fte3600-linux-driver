/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FW9369_CRC_H
#define FW9369_CRC_H

#include <stddef.h>
#include <stdint.h>

/**
 * fw9369_crc_wire - Calculate hardware wire CRC-CCITT (MSB-first, big-endian)
 * @bytes: Raw byte stream as transmitted/received on SPI MISO/MOSI
 * @len: Number of bytes
 *
 * Used for Chip ID verification (2 bytes) and FDT wire ADC telemetry (8 bytes).
 * Polynomial: 0x1021, Initial Seed: 0xFFFF, No reflection, Final XOR: 0x0000.
 *
 * Test vector: [ 0x93, 0x62 ] -> 0x1C53
 */
uint16_t fw9369_crc_wire(const uint8_t *bytes, size_t len);

/**
 * fw9369_crc_words_le - Calculate host memory CRC-CCITT across 16-bit words (little-endian per word)
 * @words: Host array of 16-bit words
 * @count: Number of 16-bit words
 *
 * Used for host FDT threshold vector (4 words) and Image Baseline signature (128 words).
 * Each 16-bit word is serialized little-endian: byte 0 = word & 0xFF, byte 1 = (word >> 8) & 0xFF.
 * Polynomial: 0x1021, Initial Seed: 0xFFFF, No reflection, Final XOR: 0x0000.
 *
 * Test vectors:
 *   [ 0, 0, 0, 0 ] -> 0x313E
 *   [ 512, 512, 512, 512 ] -> 0x5B7C
 *   [ 482, 482, 482, 482 ] -> 0xE4AA
 */
uint16_t fw9369_crc_words_le(const uint16_t *words, size_t count);

#endif /* FW9369_CRC_H */
