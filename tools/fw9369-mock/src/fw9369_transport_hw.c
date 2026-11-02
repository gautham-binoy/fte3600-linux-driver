/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "fw9369_transport_hw.h"
#include "fw9369_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/spi/spidev.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define HW_MAX_LOG_PACKETS 512
#define HW_MAX_LOG_PAYLOAD 64

struct hw_log_entry {
    uint8_t tx_prefix[HW_MAX_LOG_PAYLOAD];
    uint8_t rx_prefix[HW_MAX_LOG_PAYLOAD];
    size_t  len;
    int     status;
};

struct hw_context {
    struct fw9369_hw_config config;
    struct fw9369_hw_ops    ops;
    int                     fd;
    enum fw9369_lock_state  lock_state; /* Tracks 0x9A protected register software knowledge */
    struct hw_log_entry     log[HW_MAX_LOG_PACKETS];
    size_t                  log_count;
    pthread_mutex_t         lock;             /* Serializes operations, state, and telemetry */
    bool                    is_shutting_down; /* Teardown gate flag */
};

/* Default system call implementations */
static int default_open(const char *pathname, int flags)
{
    return open(pathname, flags);
}

static int default_ioctl(int fd, unsigned long request, void *arg)
{
    return ioctl(fd, request, arg);
}

static int default_close(int fd)
{
    return close(fd);
}

static const struct fw9369_hw_ops default_ops = {
    .open_fn = default_open,
    .ioctl_fn = default_ioctl,
    .close_fn = default_close,
};

int fw9369_hw_validate_device_path(const char *path)
{
    if (!path)
        return -EINVAL;

    /*
     * Strict device path validation:
     * Only the dedicated fingerprint SPI controller node is permitted.
     * No fallback to /dev/spidev0.0 or any other node is allowed.
     */
    if (strcmp(path, FW9369_HW_DEFAULT_DEV_PATH) != 0)
        return -ENODEV;

    return 0;
}

int fw9369_hw_verify_device_identity(const char *spidev_path, const char *sysfs_root)
{
    if (!spidev_path)
        return -EINVAL;

    if (strcmp(spidev_path, FW9369_HW_DEFAULT_DEV_PATH) != 0)
        return -ENODEV;

    const char *root = (sysfs_root && sysfs_root[0] != '\0') ? sysfs_root : "/sys";

    /*
     * Resolve /sys/class/spidev/spidev1.0/device symlink.
     * Expected backing topology:
     *   Controller: pxa2xx-spi.2
     *   SPI Master: spi1
     *   Device:     spi-FTE3600:00 (or spi1.0)
     */
    char link_path[PATH_MAX];
    int n = snprintf(link_path, sizeof(link_path), "%s/class/spidev/spidev1.0/device", root);
    if (n < 0 || (size_t)n >= sizeof(link_path))
        return -ENAMETOOLONG;

    char canonical[PATH_MAX];
    if (!realpath(link_path, canonical)) {
        return -ENODEV;
    }

    /* Verify controller parentage */
    if (!strstr(canonical, "pxa2xx-spi.2")) {
        return -EILSEQ;
    }

    /* Verify SPI master bus */
    if (!strstr(canonical, "spi1")) {
        return -EILSEQ;
    }

    /* Verify device identifier */
    if (!strstr(canonical, "spi-FTE3600:00") && !strstr(canonical, "spi1.0")) {
        return -EILSEQ;
    }

    /*
     * Verify ACPI device identity:
     * Inspect modalias under the resolved device node or under the sysfs bus tree.
     */
    char modalias_path[PATH_MAX];
    n = snprintf(modalias_path, sizeof(modalias_path), "%s/modalias", canonical);
    if (n < 0 || (size_t)n >= sizeof(modalias_path))
        return -ENAMETOOLONG;

    FILE *f = fopen(modalias_path, "r");
    if (!f) {
        n = snprintf(modalias_path, sizeof(modalias_path), "%s/bus/spi/devices/spi-FTE3600:00/modalias", root);
        if (n >= 0 && (size_t)n < sizeof(modalias_path))
            f = fopen(modalias_path, "r");
    }

    if (!f) {
        return -ENODEV;
    }

    char line[128] = {0};
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -ENODATA;
    }
    fclose(f);

    if (!strstr(line, "FTE3600")) {
        return -EKEYREJECTED;
    }

    return 0;
}

int fw9369_hw_validate_config(const struct fw9369_hw_config *cfg)
{
    if (!cfg)
        return -EINVAL;

    int ret = fw9369_hw_validate_device_path(cfg->device_path);
    if (ret < 0)
        return ret;

    /* Speed must be positive and not exceed 1,000,000 Hz */
    if (cfg->speed_hz == 0 || cfg->speed_hz > FW9369_HW_DEFAULT_SPEED_HZ)
        return -EINVAL;

    /* Mode must be strictly Mode 0 (CPOL=0, CPHA=0) */
    if (cfg->mode != FW9369_HW_DEFAULT_MODE)
        return -EINVAL;

    /* Bits per word must be strictly 8 */
    if (cfg->bits_per_word != FW9369_HW_DEFAULT_BITS)
        return -EINVAL;

    /* LSB first must be 0 (MSB first) */
    if (cfg->lsb_first != 0)
        return -EINVAL;

    /* Transfer size limit must be positive and bounded by FIFO limit */
    if (cfg->max_transfer_len == 0 || cfg->max_transfer_len > FW9369_HW_MAX_TRANSFER_LEN)
        return -EINVAL;

    return 0;
}

static void hw_delay_ms(void *context, uint32_t ms)
{
    (void)context;
    struct timespec req = {
        .tv_sec = ms / 1000,
        .tv_nsec = (long)(ms % 1000) * 1000000L,
    };
    nanosleep(&req, NULL);
}

static void hw_delay_us(void *context, uint32_t us)
{
    (void)context;
    struct timespec req = {
        .tv_sec = us / 1000000,
        .tv_nsec = (long)(us % 1000000) * 1000L,
    };
    nanosleep(&req, NULL);
}

static void hw_close(void *context)
{
    struct hw_context *ctx = context;
    if (ctx && ctx->fd >= 0) {
        ctx->ops.close_fn(ctx->fd);
        ctx->fd = -1;
    }
}

static uint16_t decode_register_addr(const uint8_t *tx, size_t len)
{
    if (len == 4 || len == 5) {
        /* Byte access: address is byte 2 */
        return tx[2];
    } else if (len >= 8) {
        /* Word access: address high byte in tx[2], low in tx[3] */
        uint8_t hi = tx[2];
        uint8_t lo = tx[3];
        if (hi == 0x98) return (uint16_t)(0x1800 | lo);
        if (hi == 0x9A) return (uint16_t)(0x1A00 | lo);
        if (hi == 0x80) {
            if (lo >= 0xB0 && lo <= 0xBF) return (uint16_t)(0x8000 | lo);
            if (lo >= 0xC0 && lo <= 0xCF) return (uint16_t)(0x0000 | lo);
            return (uint16_t)(0x8000 | lo);
        }
        return (uint16_t)(((uint16_t)(hi & 0x7F) << 8) | lo);
    }
    return 0xFFFF;
}

static int hw_transfer(void *context, const uint8_t *tx, uint8_t *rx, size_t len)
{
    struct hw_context *ctx = context;

    if (!ctx || !tx || len == 0)
        return -EINVAL;

    pthread_mutex_lock(&ctx->lock);

    if (ctx->is_shutting_down) {
        pthread_mutex_unlock(&ctx->lock);
        return -ESHUTDOWN;
    }

    if (ctx->fd < 0) {
        pthread_mutex_unlock(&ctx->lock);
        return -EBADF;
    }

    /* Exact transfer-length upper bound enforcement */
    if (len > ctx->config.max_transfer_len) {
        pthread_mutex_unlock(&ctx->lock);
        return -EMSGSIZE;
    }

    /*
     * Fail-closed allowlist validation before submitting ioctl:
     * Rejects unauthorized opcodes, malformed complements, illegal lengths,
     * and non-allowlisted register/direction combinations at the transport boundary.
     */
    bool packet_valid = false;

    if (len == 3 && tx[2] == 0x00) {
        /* Command Packets: [ c1, c2, 0x00 ] */
        if (fw9369_is_command_allowed(tx[0], tx[1]))
            packet_valid = true;
    } else if (tx[0] == FW9369_OP_BYTE_WRITE) {
        /* Byte Write: [ 0x09, 0xF6, addr, val ] (exact len == 4) */
        if (len == 4 && tx[1] == FW9369_OP_BYTE_WRITE_COMP) {
            uint16_t reg = (uint16_t)tx[2];
            uint8_t val = tx[3];
            if (fw9369_is_register_allowed(reg, true)) {
                if (reg == FW9369_REG_PROTECT_9A) {
                    /*
                     * Protection Register 0x9A Hardening (Stage 2C.5C.4):
                     * Strict Fail-Closed Policy:
                     * - When state is FW9369_LOCK_STATE_UNKNOWN: REJECT BOTH unlock (0x5A) and lock (0x00).
                     * - Unlock (0x5A) is ONLY permitted when state is FW9369_LOCK_STATE_ASSUMED_LOCKED.
                     * - Lock (0x00) is ONLY permitted when state is FW9369_LOCK_STATE_ASSUMED_UNLOCKED.
                     * - In-flight states, redundant operations, or unverified states are rejected before ioctl.
                     */
                    if (val == FW9369_KEY_UNLOCK) {
                        if (ctx->lock_state == FW9369_LOCK_STATE_ASSUMED_LOCKED)
                            packet_valid = true;
                    } else if (val == FW9369_KEY_LOCK) {
                        if (ctx->lock_state == FW9369_LOCK_STATE_ASSUMED_UNLOCKED)
                            packet_valid = true;
                    }
                } else {
                    packet_valid = true;
                }
            }
        }
    } else if (tx[0] == FW9369_OP_BYTE_READ) {
        /* Byte Read: [ 0x08, 0xF7, addr, 0x00, 0x00 ] (exact len == 5) */
        if (len == 5 && tx[1] == FW9369_OP_BYTE_READ_COMP) {
            uint16_t reg = (uint16_t)tx[2];
            if (fw9369_is_register_allowed(reg, false))
                packet_valid = true;
        }
    } else if (tx[0] == FW9369_OP_WORD_WRITE) {
        /* Word Write: [ 0x05, 0xFA, addr_hi, addr_lo, count_hi, count_lo, data_hi, data_lo ] (exact len == 8) */
        if (len == 8 && tx[1] == FW9369_OP_WORD_WRITE_COMP) {
            uint16_t reg = decode_register_addr(tx, len);
            if (reg != 0xFFFF && fw9369_is_register_allowed(reg, true)) {
                /* Protected analog trim bank (0x00C0..0x00C2) requires assumed unlocked state */
                if (reg >= 0x00C0 && reg <= 0x00C2) {
                    if (ctx->lock_state == FW9369_LOCK_STATE_ASSUMED_UNLOCKED)
                        packet_valid = true;
                } else {
                    packet_valid = true;
                }
            }
        }
    } else if (tx[0] == FW9369_OP_WORD_READ) {
        /* Word Read: [ 0x04, 0xFB, addr_hi, addr_lo, count_hi, count_lo, ... ] */
        if (len >= 8 && tx[1] == FW9369_OP_WORD_READ_COMP) {
            uint16_t count = (uint16_t)(((uint16_t)tx[4] << 8) | tx[5]);
            /* Protect against count integer overflow and limit to max transfer capability */
            if (count <= (FW9369_HW_MAX_TRANSFER_LEN - 8) / 2) {
                size_t expected_len = 6 + (size_t)count * 2 + 2;
                if (len == expected_len) {
                    uint16_t reg = decode_register_addr(tx, len);
                    if (reg != 0xFFFF && fw9369_is_register_allowed(reg, false)) {
                        /* Protected analog trim bank (0x00C0..0x00C2) requires assumed unlocked state */
                        if (reg >= 0x00C0 && reg <= 0x00C2) {
                            if (ctx->lock_state == FW9369_LOCK_STATE_ASSUMED_UNLOCKED)
                                packet_valid = true;
                        } else {
                            packet_valid = true;
                        }
                    }
                }
            }
        }
    } else if (tx[0] == FW9369_OP_FIFO_READ) {
        /* FIFO Burst Read: exact 10,246 bytes, complement 0xF9, prefix [ 0x9A, 0x05, 0x14, 0x00 ] */
        static const uint8_t fifo_prefix[4] = { 0x9A, 0x05, 0x14, 0x00 };
        if (len == FW9369_FIFO_TRANSFER_LEN && tx[1] == FW9369_OP_FIFO_READ_COMP) {
            if (memcmp(&tx[2], fifo_prefix, sizeof(fifo_prefix)) == 0)
                packet_valid = true;
        }
    }

    if (!packet_valid) {
        pthread_mutex_unlock(&ctx->lock);
        return -EPERM;
    }

    /*
     * Track in-flight state transitions for 0x9A operations.
     * Before transmission, preserve previous state and record command in flight.
     */
    enum fw9369_lock_state prev_lock_state = ctx->lock_state;
    (void)prev_lock_state;
    bool is_unlock_cmd = (len == 4 && tx[0] == FW9369_OP_BYTE_WRITE &&
                          tx[2] == FW9369_REG_PROTECT_9A && tx[3] == FW9369_KEY_UNLOCK);
    bool is_lock_cmd   = (len == 4 && tx[0] == FW9369_OP_BYTE_WRITE &&
                          tx[2] == FW9369_REG_PROTECT_9A && tx[3] == FW9369_KEY_LOCK);

    if (is_unlock_cmd) {
        ctx->lock_state = FW9369_LOCK_STATE_UNLOCK_COMMAND_SENT;
    } else if (is_lock_cmd) {
        ctx->lock_state = FW9369_LOCK_STATE_LOCK_COMMAND_SENT;
    }

    /*
     * SPI_IOC_MESSAGE(1) Transfer Setup:
     * cs_change = 0: guarantees continuous chip-select assertion for the entire transfer.
     */
    struct spi_ioc_transfer tr;
    memset(&tr, 0, sizeof(tr));
    tr.tx_buf = (unsigned long)tx;
    tr.rx_buf = (unsigned long)rx;
    tr.len = (uint32_t)len;
    tr.speed_hz = ctx->config.speed_hz;
    tr.delay_usecs = 0;
    tr.bits_per_word = ctx->config.bits_per_word;
    tr.cs_change = 0;

    int ret = ctx->ops.ioctl_fn(ctx->fd, SPI_IOC_MESSAGE(1), &tr);
    int status = 0;

    if (ret < 0) {
        status = -errno;
    } else if ((size_t)ret < len) {
        /* Short transfer detection: incomplete byte shift */
        status = -EMSGSIZE;
    } else if ((size_t)ret > len) {
        /* Kernel returned more bytes than requested */
        status = -EPROTO;
    } else {
        status = 0;
    }

    /*
     * Handle uncertain transfer outcomes for 0x9A operations:
     * - On exact successful transfer (status == 0), record command transmitted.
     * - On any negative result, short transfer, unexpected count, or interruption,
     *   transition to UNKNOWN. Once bits shift onto MOSI, software cannot assume
     *   the command was unreceived, nor can it assume the device reached target state.
     * - A failed lock command must NEVER result in a confidently locked software state.
     */
    if (is_unlock_cmd) {
        if (status == 0) {
            ctx->lock_state = FW9369_LOCK_STATE_ASSUMED_UNLOCKED;
        } else {
            ctx->lock_state = FW9369_LOCK_STATE_UNKNOWN;
        }
    } else if (is_lock_cmd) {
        if (status == 0) {
            ctx->lock_state = FW9369_LOCK_STATE_ASSUMED_LOCKED;
        } else {
            ctx->lock_state = FW9369_LOCK_STATE_UNKNOWN;
        }
    }

    /* Record in-memory transaction log if enabled */
    if (ctx->config.enable_logging && ctx->log_count < HW_MAX_LOG_PACKETS) {
        struct hw_log_entry *entry = &ctx->log[ctx->log_count++];
        size_t copy_len = len < HW_MAX_LOG_PAYLOAD ? len : HW_MAX_LOG_PAYLOAD;
        memcpy(entry->tx_prefix, tx, copy_len);
        if (rx && status == 0)
            memcpy(entry->rx_prefix, rx, copy_len);
        else
            memset(entry->rx_prefix, 0, copy_len);
        entry->len = len;
        entry->status = status;
    }

    pthread_mutex_unlock(&ctx->lock);

    /* No automatic retries: return error status immediately */
    return status;
}

struct fw9369_transport *fw9369_hw_transport_create_with_ops(const struct fw9369_hw_config *cfg,
                                                             const struct fw9369_hw_ops *ops)
{
    if (!cfg || !ops || !ops->open_fn || !ops->ioctl_fn || !ops->close_fn) {
        errno = EINVAL;
        return NULL;
    }

    int ret = fw9369_hw_validate_config(cfg);
    if (ret < 0) {
        errno = -ret;
        return NULL;
    }

    int fd = ops->open_fn(cfg->device_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }

    /* Verify & configure SPI mode 0 */
    uint8_t mode = cfg->mode;
    if (ops->ioctl_fn(fd, SPI_IOC_WR_MODE, &mode) < 0) {
        ops->close_fn(fd);
        return NULL;
    }
    uint8_t mode_read = 0xFF;
    if (ops->ioctl_fn(fd, SPI_IOC_RD_MODE, &mode_read) < 0 || mode_read != cfg->mode) {
        ops->close_fn(fd);
        errno = EPROTO;
        return NULL;
    }

    /* Verify & configure 8 bits per word */
    uint8_t bits = cfg->bits_per_word;
    if (ops->ioctl_fn(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) {
        ops->close_fn(fd);
        return NULL;
    }
    uint8_t bits_read = 0;
    if (ops->ioctl_fn(fd, SPI_IOC_RD_BITS_PER_WORD, &bits_read) < 0 || bits_read != 8) {
        ops->close_fn(fd);
        errno = EPROTO;
        return NULL;
    }

    /* Verify & configure speed (<= 1 MHz) */
    uint32_t speed = cfg->speed_hz;
    if (ops->ioctl_fn(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        ops->close_fn(fd);
        return NULL;
    }
    uint32_t speed_read = 0;
    if (ops->ioctl_fn(fd, SPI_IOC_RD_MAX_SPEED_HZ, &speed_read) < 0 || speed_read > cfg->speed_hz) {
        ops->close_fn(fd);
        errno = EPROTO;
        return NULL;
    }

    /* Verify & configure MSB first (lsb_first = 0) */
    uint8_t lsb = cfg->lsb_first;
    if (ops->ioctl_fn(fd, SPI_IOC_WR_LSB_FIRST, &lsb) < 0) {
        ops->close_fn(fd);
        return NULL;
    }

    struct fw9369_transport *t = calloc(1, sizeof(*t));
    if (!t) {
        ops->close_fn(fd);
        errno = ENOMEM;
        return NULL;
    }

    struct hw_context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        ops->close_fn(fd);
        free(t);
        errno = ENOMEM;
        return NULL;
    }

    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
        ops->close_fn(fd);
        free(ctx);
        free(t);
        errno = ENOMEM;
        return NULL;
    }
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
    if (pthread_mutex_init(&ctx->lock, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        ops->close_fn(fd);
        free(ctx);
        free(t);
        errno = ENOMEM;
        return NULL;
    }
    pthread_mutexattr_destroy(&attr);

    ctx->is_shutting_down = false;

    ctx->config = *cfg;
    ctx->ops = *ops;
    ctx->fd = fd;

    t->context = ctx;
    t->transfer = hw_transfer;
    t->delay_ms = hw_delay_ms;
    t->delay_us = hw_delay_us;
    t->close = hw_close;

    return t;
}

struct fw9369_transport *fw9369_hw_transport_create(const struct fw9369_hw_config *cfg)
{
    return fw9369_hw_transport_create_with_ops(cfg, &default_ops);
}

void fw9369_hw_transport_destroy(struct fw9369_transport *t)
{
    if (t) {
        if (t->context) {
            struct hw_context *ctx = t->context;
            pthread_mutex_lock(&ctx->lock);
            ctx->is_shutting_down = true;
            hw_close(ctx);
            pthread_mutex_unlock(&ctx->lock);
            pthread_mutex_destroy(&ctx->lock);
            free(ctx);
        }
        free(t);
    }
}

size_t fw9369_hw_get_tx_count(struct fw9369_transport *t)
{
    if (!t || !t->context)
        return 0;
    struct hw_context *ctx = t->context;
    pthread_mutex_lock(&ctx->lock);
    size_t count = ctx->log_count;
    pthread_mutex_unlock(&ctx->lock);
    return count;
}

int fw9369_hw_get_last_tx(struct fw9369_transport *t, uint8_t *out_buf, size_t buf_cap, size_t *out_len)
{
    if (out_len)
        *out_len = 0;

    if (!t || !t->context || !out_buf || buf_cap == 0)
        return -EINVAL;

    struct hw_context *ctx = t->context;
    pthread_mutex_lock(&ctx->lock);

    if (ctx->log_count == 0) {
        pthread_mutex_unlock(&ctx->lock);
        return -ENODATA;
    }

    const struct hw_log_entry *entry = &ctx->log[ctx->log_count - 1];
    size_t payload_len = entry->len < HW_MAX_LOG_PAYLOAD ? entry->len : HW_MAX_LOG_PAYLOAD;

    if (out_len)
        *out_len = entry->len;

    if (buf_cap < payload_len) {
        pthread_mutex_unlock(&ctx->lock);
        return -ENOBUFS;
    }

    memcpy(out_buf, entry->tx_prefix, payload_len);
    pthread_mutex_unlock(&ctx->lock);

    return (int)payload_len;
}

int fw9369_hw_get_fd(struct fw9369_transport *t)
{
    if (!t || !t->context)
        return -1;
    struct hw_context *ctx = t->context;
    pthread_mutex_lock(&ctx->lock);
    int fd = ctx->fd;
    pthread_mutex_unlock(&ctx->lock);
    return fd;
}

bool fw9369_hw_is_protected_unlocked(struct fw9369_transport *t)
{
    if (!t || !t->context)
        return false;
    struct hw_context *ctx = t->context;
    pthread_mutex_lock(&ctx->lock);
    bool unlocked = (ctx->lock_state == FW9369_LOCK_STATE_ASSUMED_UNLOCKED);
    pthread_mutex_unlock(&ctx->lock);
    return unlocked;
}

enum fw9369_lock_state fw9369_hw_get_lock_state(struct fw9369_transport *t)
{
    if (!t || !t->context)
        return FW9369_LOCK_STATE_UNKNOWN;
    struct hw_context *ctx = t->context;
    pthread_mutex_lock(&ctx->lock);
    enum fw9369_lock_state state = ctx->lock_state;
    pthread_mutex_unlock(&ctx->lock);
    return state;
}

void fw9369_hw_set_lock_state_for_test(struct fw9369_transport *t, enum fw9369_lock_state state)
{
    if (!t || !t->context)
        return;
    struct hw_context *ctx = t->context;
    pthread_mutex_lock(&ctx->lock);
    ctx->lock_state = state;
    pthread_mutex_unlock(&ctx->lock);
}

enum fw9369_lock_state fw9369_hw_get_lock_state_unlocked_for_test(struct fw9369_transport *t)
{
    if (!t || !t->context)
        return FW9369_LOCK_STATE_UNKNOWN;
    struct hw_context *ctx = t->context;
    return ctx->lock_state;
}

bool fw9369_hw_is_shutting_down_for_test(struct fw9369_transport *t)
{
    if (!t || !t->context)
        return false;
    struct hw_context *ctx = t->context;
    pthread_mutex_lock(&ctx->lock);
    bool down = ctx->is_shutting_down;
    pthread_mutex_unlock(&ctx->lock);
    return down;
}

void fw9369_hw_set_shutting_down_for_test(struct fw9369_transport *t, bool down)
{
    if (!t || !t->context)
        return;
    struct hw_context *ctx = t->context;
    pthread_mutex_lock(&ctx->lock);
    ctx->is_shutting_down = down;
    pthread_mutex_unlock(&ctx->lock);
}

void fw9369_hw_set_shutting_down_unlocked_for_test(struct fw9369_transport *t, bool down)
{
    if (!t || !t->context)
        return;
    struct hw_context *ctx = t->context;
    ctx->is_shutting_down = down;
}

const char *fw9369_lock_state_name(enum fw9369_lock_state state)
{
    switch (state) {
    case FW9369_LOCK_STATE_UNKNOWN:
        return "UNKNOWN";
    case FW9369_LOCK_STATE_ASSUMED_LOCKED:
        return "ASSUMED_LOCKED";
    case FW9369_LOCK_STATE_UNLOCK_COMMAND_SENT:
        return "UNLOCK_COMMAND_SENT";
    case FW9369_LOCK_STATE_ASSUMED_UNLOCKED:
        return "ASSUMED_UNLOCKED";
    case FW9369_LOCK_STATE_LOCK_COMMAND_SENT:
        return "LOCK_COMMAND_SENT";
    default:
        return "INVALID";
    }
}
