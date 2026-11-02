/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FW9369_TRANSPORT_HW_H
#define FW9369_TRANSPORT_HW_H

#include "fw9369_transport.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FW9369_HW_DEFAULT_DEV_PATH    "/dev/spidev1.0"
#define FW9369_HW_DEFAULT_SPEED_HZ    1000000u
#define FW9369_HW_DEFAULT_BITS        8
#define FW9369_HW_DEFAULT_MODE        0 /* SPI_MODE_0 */
#define FW9369_HW_MAX_TRANSFER_LEN    10246 /* FW9369_FIFO_TRANSFER_LEN */

/**
 * struct fw9369_hw_config - Hardware SPI backend configuration
 */
struct fw9369_hw_config {
    const char *device_path;      /* Must be explicitly set to "/dev/spidev1.0" */
    uint32_t   speed_hz;         /* Maximum bus frequency (<= 1000000) */
    uint8_t    mode;             /* SPI mode: must be 0 (CPOL=0, CPHA=0) */
    uint8_t    bits_per_word;    /* Word length: must be 8 */
    uint8_t    lsb_first;        /* Bit order: must be 0 (MSB first) */
    size_t     max_transfer_len; /* Max transfer length (up to 10246) */
    bool       enable_logging;   /* Capture transaction telemetry */
};

/**
 * enum fw9369_lock_state - Software knowledge model of sensor protection register 0x9A
 *
 * NOTE: These states represent userspace software knowledge based on transmitted
 * commands and kernel ioctl return values. They DO NOT represent physical silicon
 * readback confirmations (the FT9362 hardware provides no readable lock status bit).
 */
enum fw9369_lock_state {
    FW9369_LOCK_STATE_UNKNOWN = 0,         /* Initial state; physical silicon state is completely unknown */
    FW9369_LOCK_STATE_ASSUMED_LOCKED,      /* Lock command transmitted and verified shifted; assumed locked */
    FW9369_LOCK_STATE_UNLOCK_COMMAND_SENT, /* Unlock command currently in flight / transmission attempted */
    FW9369_LOCK_STATE_ASSUMED_UNLOCKED,    /* Unlock command transmitted and verified shifted; assumed unlocked */
    FW9369_LOCK_STATE_LOCK_COMMAND_SENT,   /* Lock command currently in flight / transmission attempted */
};

/**
 * struct fw9369_hw_ops - System call interface hooks for offline testing
 */
struct fw9369_hw_ops {
    int (*open_fn)(const char *pathname, int flags);
    int (*ioctl_fn)(int fd, unsigned long request, void *arg);
    int (*close_fn)(int fd);
};

/* Path and Configuration Validation (Offline, no device node access) */
int fw9369_hw_validate_device_path(const char *path);
int fw9369_hw_validate_config(const struct fw9369_hw_config *cfg);

/* Physical Device Identity & Sysfs Hierarchy Verification */
int fw9369_hw_verify_device_identity(const char *spidev_path, const char *sysfs_root);

/* Hardware Transport Creation & Teardown */
struct fw9369_transport *fw9369_hw_transport_create(const struct fw9369_hw_config *cfg);
struct fw9369_transport *fw9369_hw_transport_create_with_ops(const struct fw9369_hw_config *cfg,
                                                             const struct fw9369_hw_ops *ops);
void fw9369_hw_transport_destroy(struct fw9369_transport *t);

/* Telemetry & State Inspection */
size_t fw9369_hw_get_tx_count(struct fw9369_transport *t);
int fw9369_hw_get_last_tx(struct fw9369_transport *t, uint8_t *out_buf, size_t buf_cap, size_t *out_len);
int fw9369_hw_get_fd(struct fw9369_transport *t);
bool fw9369_hw_is_protected_unlocked(struct fw9369_transport *t);
enum fw9369_lock_state fw9369_hw_get_lock_state(struct fw9369_transport *t);
const char *fw9369_lock_state_name(enum fw9369_lock_state state);

/* Test Fixture Support (Strictly offline mock unit testing only) */
void fw9369_hw_set_lock_state_for_test(struct fw9369_transport *t, enum fw9369_lock_state state);
enum fw9369_lock_state fw9369_hw_get_lock_state_unlocked_for_test(struct fw9369_transport *t);
bool fw9369_hw_is_shutting_down_for_test(struct fw9369_transport *t);
void fw9369_hw_set_shutting_down_for_test(struct fw9369_transport *t, bool down);
void fw9369_hw_set_shutting_down_unlocked_for_test(struct fw9369_transport *t, bool down);

#endif /* FW9369_TRANSPORT_HW_H */
