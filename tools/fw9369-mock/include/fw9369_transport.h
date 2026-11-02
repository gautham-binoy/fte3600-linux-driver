/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FW9369_TRANSPORT_H
#define FW9369_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * struct fw9369_transport - Abstract SPI transport interface
 */
struct fw9369_transport {
    void *context;
    int (*transfer)(void *context, const uint8_t *tx, uint8_t *rx, size_t len);
    void (*delay_ms)(void *context, uint32_t ms);
    void (*delay_us)(void *context, uint32_t us);
    void (*close)(void *context);
};

/**
 * Failure injection flags for mock backend testing
 */
struct fw9369_mock_config {
    bool fail_transfers;         /* Return -EIO on all transfers */
    size_t short_transfer_len;   /* If > 0, transfer only this many bytes */
    bool fail_chip_id;           /* Return 0x9368 instead of 0x9362 */
    bool fail_crc;               /* Corrupt hardware wire CRC */
    bool timeout_adc_ready;      /* Register 0x1A82 bit 3 never sets */
    bool fdt_jitter;             /* FDT readings fluctuate by >5 to test early exit failure */
    bool fdt_out_of_bounds;      /* FDT minima < 300 to test range validation */
    bool fail_lock_write;        /* Fail write_byte(0x9A, 0x00) to test lock error reporting */
    bool interrupt_in_trim;      /* Simulate process kill / interrupt while 0x9A is 0x5A */
    bool fail_fifo_read;         /* Fail 10,246-byte burst FIFO read */
};

/* Mock Transport Creation & Inspection */
struct fw9369_transport *fw9369_mock_transport_create(const struct fw9369_mock_config *cfg);
void fw9369_mock_transport_destroy(struct fw9369_transport *t);

size_t fw9369_mock_get_tx_count(struct fw9369_transport *t);
int fw9369_mock_get_last_tx(struct fw9369_transport *t, uint8_t *out_buf, size_t buf_cap, size_t *out_len);
uint8_t fw9369_mock_get_reg_byte(struct fw9369_transport *t, uint8_t addr);
uint16_t fw9369_mock_get_reg_word(struct fw9369_transport *t, uint16_t addr);
bool fw9369_mock_is_protected_locked(struct fw9369_transport *t);

/**
 * Hardware Backend Stub:
 * Deliberately rejects hardware selection and NEVER discovers or opens any device node.
 */
struct fw9369_transport *fw9369_hardware_transport_create(const char *device_path);

#endif /* FW9369_TRANSPORT_H */
