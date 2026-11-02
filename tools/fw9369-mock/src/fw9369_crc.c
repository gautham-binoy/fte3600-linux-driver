/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fw9369_crc.h"

uint16_t fw9369_crc_wire(const uint8_t *bytes, size_t len)
{
    uint16_t crc = 0xFFFF;

    if (!bytes)
        return 0;

    for (size_t i = 0; i < len; ++i) {
        crc = (uint16_t)(crc ^ ((uint16_t)bytes[i] << 8));
        for (unsigned int bit = 0; bit < 8; ++bit) {
            crc = (uint16_t)(((uint16_t)crc << 1) ^
                             ((crc & 0x8000) ? 0x1021u : 0u));
        }
    }
    return crc;
}

uint16_t fw9369_crc_words_le(const uint16_t *words, size_t count)
{
    uint16_t crc = 0xFFFF;

    if (!words)
        return 0;

    for (size_t i = 0; i < count; ++i) {
        /* Little-endian serialization: byte 0 is LSB, byte 1 is MSB */
        for (unsigned int byte = 0; byte < 2; ++byte) {
            uint8_t b = (uint8_t)(words[i] >> (8 * byte));
            crc = (uint16_t)(crc ^ ((uint16_t)b << 8));
            for (unsigned int bit = 0; bit < 8; ++bit) {
                crc = (uint16_t)(((uint16_t)crc << 1) ^
                                 ((crc & 0x8000) ? 0x1021u : 0u));
            }
        }
    }
    return crc;
}
