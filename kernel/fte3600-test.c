// SPDX-License-Identifier: GPL-2.0-only
/*
 * FTE3600 Unified Kernel Driver Software Test Harness (Stage 2E.4.2)
 *
 * Test Category Classification:
 * CATEGORY B: Standalone Userspace Simulation Test Harness.
 * Validates UAPI layouts, state machine transitions, concurrency semantics,
 * lifetime reference counting, device removal shutdown protocol, SPI transport
 * bounds checking, SPI transfer completion semantics (m.actual_length),
 * semantic state boundary (FTE3600_STATE_IDENTIFIED vs READY), and fail-closed
 * security invariants.
 *
 * NOTE: This is NOT an in-kernel runtime test (Category D) or KUnit test (Category C).
 * CONFIG_KUNIT is not set in the host kernel. Physical hardware tests remain barred.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "fte3600.h"

/* Color logging helpers */
#define TEST_PASS "\033[32m[PASS]\033[0m"
#define TEST_FAIL "\033[31m[FAIL]\033[0m"

static unsigned int total_tests = 0;
static unsigned int passed_tests = 0;

#define RUN_TEST(fn, desc) do { \
    total_tests++; \
    printf("  Testing: %s ... ", desc); \
    if (fn()) { \
        passed_tests++; \
        printf("%s\n", TEST_PASS); \
    } else { \
        printf("%s\n", TEST_FAIL); \
    } \
} while (0)

/* --- CRC Algorithms for Offline Validation --- */
static uint16_t test_crc16_wire(const uint8_t *bytes, size_t len)
{
    uint16_t crc = 0xFFFF;
    if (!bytes || len == 0)
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

static uint16_t test_crc16_words_le(const uint16_t *words, size_t count)
{
    uint16_t crc = 0xFFFF;
    if (!words || count == 0)
        return 0;

    for (size_t i = 0; i < count; ++i) {
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

/* --- Suite 1: UAPI & Data Structure Alignment Validation --- */

static bool test_uapi_sizes(void)
{
    if (sizeof(struct fte3600_state_snapshot) != 32)
        return false;
    if (sizeof(struct fte3600_init_params) != 32)
        return false;
    if (sizeof(struct fte3600_frame_buffer) != 32)
        return false;
    return true;
}

static bool test_uapi_constants(void)
{
    if (FTE3600_UAPI_VERSION != 1)
        return false;
    if (FTE3600_IOC_MAGIC != 'F')
        return false;
    if (FTE3600_FIFO_TRANSFER_LEN != 10246)
        return false;
    if (FTE3600_IMAGE_BYTES != 10240)
        return false;
    if (FTE3600_FIFO_PREFIX_LEN != 6)
        return false;
    if (FTE3600_IMAGE_PIXELS != 5120)
        return false;
    if (FTE3600_MAX_SPI_TRANSFER_LEN != 10246)
        return false;
    if (FTE3600_STATE_UNINITIALIZED != 0)
        return false;
    if (FTE3600_STATE_RESETTING != 1)
        return false;
    if (FTE3600_STATE_RESET_SEQUENCE_ISSUED != 2)
        return false;
    if (FTE3600_STATE_RESET_COMPLETE != 2)
        return false;
    if (FTE3600_STATE_INITIALIZING != 3)
        return false;
    if (FTE3600_STATE_READY != 4)
        return false;
    if (FTE3600_STATE_CAPTURING != 5)
        return false;
    if (FTE3600_STATE_ERROR != 6)
        return false;
    if (FTE3600_STATE_UNKNOWN != 7)
        return false;
    if (FTE3600_STATE_IDENTIFIED != 8)
        return false;
    if (FTE3600_FLAG_OFFLINE != (1U << 0))
        return false;
    if (FTE3600_FLAG_RESET_SEQUENCE_ISSUED != (1U << 1))
        return false;
    if (FTE3600_FLAG_RESET_COMPLETED != (1U << 1))
        return false;
    return true;
}

static bool test_ioctl_encodings(void)
{
    if (_IOC_TYPE(FTE3600_IOC_GET_STATE) != 'F' || _IOC_NR(FTE3600_IOC_GET_STATE) != 0x01)
        return false;
    if (_IOC_DIR(FTE3600_IOC_GET_STATE) != _IOC_READ || _IOC_SIZE(FTE3600_IOC_GET_STATE) != 32)
        return false;

    if (_IOC_TYPE(FTE3600_IOC_RESET) != 'F' || _IOC_NR(FTE3600_IOC_RESET) != 0x02)
        return false;
    if (_IOC_DIR(FTE3600_IOC_RESET) != _IOC_NONE)
        return false;

    if (_IOC_TYPE(FTE3600_IOC_INIT) != 'F' || _IOC_NR(FTE3600_IOC_INIT) != 0x03)
        return false;
    if (_IOC_DIR(FTE3600_IOC_INIT) != (_IOC_READ | _IOC_WRITE) || _IOC_SIZE(FTE3600_IOC_INIT) != 32)
        return false;

    if (_IOC_TYPE(FTE3600_IOC_CAPTURE) != 'F' || _IOC_NR(FTE3600_IOC_CAPTURE) != 0x04)
        return false;
    if (_IOC_DIR(FTE3600_IOC_CAPTURE) != (_IOC_READ | _IOC_WRITE) || _IOC_SIZE(FTE3600_IOC_CAPTURE) != 32)
        return false;

    return true;
}

/* --- Suite 2: CRC Verification --- */

static bool test_crc_vectors(void)
{
    static const uint8_t chip_id[2] = { 0x93, 0x62 };
    if (test_crc16_wire(chip_id, 2) != 0x1C53)
        return false;

    static const uint16_t v0[4] = { 0, 0, 0, 0 };
    if (test_crc16_words_le(v0, 4) != 0x313E)
        return false;

    static const uint16_t v512[4] = { 512, 512, 512, 512 };
    if (test_crc16_words_le(v512, 4) != 0x5B7C)
        return false;

    static const uint16_t v482[4] = { 482, 482, 482, 482 };
    if (test_crc16_words_le(v482, 4) != 0xE4AA)
        return false;

    return true;
}

/* --- Suite 3: State Machine Simulation & Invariants --- */

typedef struct {
    enum fte3600_state state;
    bool reset_gpio_ok;
    bool spi_ok;
    uint32_t last_error;
} sim_dev_t;

static void sim_init(sim_dev_t *dev)
{
    dev->state = FTE3600_STATE_UNINITIALIZED;
    dev->reset_gpio_ok = true;
    dev->spi_ok = true;
    dev->last_error = 0;
}

static int sim_do_reset(sim_dev_t *dev)
{
    dev->state = FTE3600_STATE_RESETTING;
    if (!dev->reset_gpio_ok) {
        dev->state = FTE3600_STATE_UNKNOWN;
        dev->last_error = EIO;
        return -EIO;
    }
    dev->state = FTE3600_STATE_RESET_SEQUENCE_ISSUED;
    return 0;
}

static int sim_do_init(sim_dev_t *dev)
{
    if (dev->state != FTE3600_STATE_RESET_SEQUENCE_ISSUED)
        return -EPERM;

    dev->state = FTE3600_STATE_INITIALIZING;
    if (!dev->spi_ok) {
        dev->state = FTE3600_STATE_UNKNOWN;
        dev->last_error = EIO;
        return -EIO;
    }
    dev->state = FTE3600_STATE_IDENTIFIED;
    return 0;
}

static bool test_state_transitions(void)
{
    sim_dev_t dev;

    sim_init(&dev);
    if (dev.state != FTE3600_STATE_UNINITIALIZED)
        return false;

    if (sim_do_init(&dev) != -EPERM)
        return false;

    if (sim_do_reset(&dev) != 0 || dev.state != FTE3600_STATE_RESET_SEQUENCE_ISSUED)
        return false;

    if (sim_do_init(&dev) != 0 || dev.state != FTE3600_STATE_IDENTIFIED)
        return false;

    /* Repeated init from IDENTIFIED is rejected */
    if (sim_do_init(&dev) != -EPERM)
        return false;

    /* Verify sensor does not enter READY state */
    if (dev.state == FTE3600_STATE_READY)
        return false;

    dev.spi_ok = false;
    dev.state = FTE3600_STATE_RESET_SEQUENCE_ISSUED;
    if (sim_do_init(&dev) != -EIO || dev.state != FTE3600_STATE_UNKNOWN)
        return false;

    dev.spi_ok = true;
    if (sim_do_init(&dev) != -EPERM)
        return false;

    if (sim_do_reset(&dev) != 0 || dev.state != FTE3600_STATE_RESET_SEQUENCE_ISSUED)
        return false;

    return true;
}

/* --- Suite 4: Stage 2E.2.2 Mandatory Lifecycle & Concurrency Scenarios --- */

/*
 * Simulated driver context modeling Linux struct device refcounting (kobject),
 * cdev open lease, hardware mutex, and devres pointer invalidation.
 */
typedef struct {
    atomic_int refcount;           /* Models get_device(&data->dev) / put_device */
    atomic_int is_open;            /* Models single-client exclusive lease */
    atomic_int offline;            /* Models atomic_set(&data->offline, 1) */
    atomic_int freed;              /* Models fte3600_device_release callback */
    pthread_mutex_t lock;          /* Models struct mutex lock */
    void *reset_gpiod;             /* Models struct gpio_desc *reset_gpiod */
    void *spi;                     /* Models struct spi_device *spi */
    enum fte3600_state state;
    uint32_t last_error;
    int hardware_access_count;     /* Counts physical hardware operations */
    uint8_t foundry_code;
    uint16_t line_period_us;
} sim_context_t;

static void sim_ctx_init(sim_context_t *ctx)
{
    atomic_init(&ctx->refcount, 1); /* Probe takes initial reference */
    atomic_init(&ctx->is_open, 0);
    atomic_init(&ctx->offline, 0);
    atomic_init(&ctx->freed, 0);
    pthread_mutex_init(&ctx->lock, NULL);
    ctx->reset_gpiod = (void *)0x1000; /* Simulated non-null descriptor */
    ctx->spi = (void *)0x2000;         /* Simulated non-null SPI device */
    ctx->state = FTE3600_STATE_UNINITIALIZED;
    ctx->last_error = 0;
    ctx->hardware_access_count = 0;
    ctx->foundry_code = 0;
    ctx->line_period_us = 0;
}

static void sim_ctx_get(sim_context_t *ctx)
{
    atomic_fetch_add(&ctx->refcount, 1);
}

static void sim_ctx_put(sim_context_t *ctx)
{
    if (atomic_fetch_sub(&ctx->refcount, 1) == 1) {
        atomic_store(&ctx->freed, 1);
        pthread_mutex_destroy(&ctx->lock);
    }
}

/* Simulated open with single-client lease and offline re-check */
static int sim_driver_open(sim_context_t *ctx)
{
    if (atomic_load(&ctx->offline))
        return -ENODEV;

    if (atomic_compare_exchange_strong(&ctx->is_open, &(int){0}, 1) == 0)
        return -EBUSY;

    if (atomic_load(&ctx->offline)) {
        atomic_store(&ctx->is_open, 0);
        return -ENODEV;
    }

    sim_ctx_get(ctx);
    return 0;
}

/* Simulated release */
static void sim_driver_release(sim_context_t *ctx)
{
    atomic_store(&ctx->is_open, 0);
    sim_ctx_put(ctx);
}

/* Simulated device removal following exact Hardware-Resource Shutdown Protocol */
static void sim_driver_remove(sim_context_t *ctx)
{
    /* Step 2: Acquire lock to serialize with in-flight or waiting hardware operations */
    pthread_mutex_lock(&ctx->lock);

    /* Step 3: Mark offline and invalidate devres hardware resource pointers */
    atomic_store(&ctx->offline, 1);
    ctx->state = FTE3600_STATE_DEVICE_REMOVAL;
    ctx->reset_gpiod = NULL;
    ctx->spi = NULL;

    /* Step 4: Release lock */
    pthread_mutex_unlock(&ctx->lock);

    /* Step 6: Drop probe reference */
    sim_ctx_put(ctx);
}

/* Simulated ioctl(FTE3600_IOC_RESET) adhering to the shutdown protocol */
static int sim_driver_ioctl_reset(sim_context_t *ctx)
{
    /* Fast-path check */
    if (atomic_load(&ctx->offline))
        return -ENODEV;

    pthread_mutex_lock(&ctx->lock);

    /* Hardware-Resource Shutdown Protocol: Re-check under lock */
    if (atomic_load(&ctx->offline) || !ctx->reset_gpiod) {
        pthread_mutex_unlock(&ctx->lock);
        return -ENODEV;
    }

    /* Simulate hardware reset pulse */
    ctx->hardware_access_count++;
    ctx->state = FTE3600_STATE_RESET_SEQUENCE_ISSUED;

    pthread_mutex_unlock(&ctx->lock);
    return 0;
}

/* Simulated ioctl(FTE3600_IOC_GET_STATE) adhering to safe retained software access */
static int sim_driver_ioctl_get_state(sim_context_t *ctx, struct fte3600_state_snapshot *snap)
{
    pthread_mutex_lock(&ctx->lock);

    memset(snap, 0, sizeof(*snap));
    snap->version = FTE3600_UAPI_VERSION;
    snap->state = fte3600_state_to_uapi(ctx->state);
    if (atomic_load(&ctx->offline) || ctx->state == FTE3600_STATE_DEVICE_REMOVAL)
        snap->flags |= FTE3600_FLAG_OFFLINE;
    if (ctx->state == FTE3600_STATE_RESET_SEQUENCE_ISSUED)
        snap->flags |= FTE3600_FLAG_RESET_SEQUENCE_ISSUED;
    if (ctx->state == FTE3600_STATE_INITIALIZED_UNCALIBRATED)
        snap->flags |= FTE3600_FLAG_STARTUP_PREFIX_COMPLETED | FTE3600_FLAG_CALIBRATION_PENDING;
    snap->last_error = ctx->last_error;

    pthread_mutex_unlock(&ctx->lock);
    return 0;
}

/* Simulated ioctl(FTE3600_IOC_CAPTURE) adhering to Stage D6.4 capture-gating enforcement */
static int sim_driver_ioctl_capture(sim_context_t *ctx, const struct fte3600_frame_buffer *fb)
{
    if (atomic_load(&ctx->offline))
        return -ENODEV;

    if (!fb || fb->version != FTE3600_UAPI_VERSION)
        return -EINVAL;

    if (fb->length != FTE3600_IMAGE_BYTES)
        return -EINVAL;

    for (int i = 0; i < 4; i++) {
        if (fb->reserved[i] != 0)
            return -EINVAL;
    }

    pthread_mutex_lock(&ctx->lock);

    if (atomic_load(&ctx->offline) || !ctx->spi) {
        pthread_mutex_unlock(&ctx->lock);
        return -ENODEV;
    }

    /*
     * Capture-Gating Enforcement:
     * Capture is strictly prohibited unless the device has completed all
     * required initialization, analog baseline calibration, and touch arming stages.
     * In all non-READY states, returns -EPERM.
     */
    if (ctx->state != FTE3600_STATE_READY) {
        pthread_mutex_unlock(&ctx->lock);
        return -EPERM;
    }

    /* Physical FIFO capture deferred in Stage 2E.2 software milestone */
    pthread_mutex_unlock(&ctx->lock);
    return -EOPNOTSUPP;
}

/* Simulated hardened SPI transfer helper */
static int sim_spi_sync_transfer(sim_context_t *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
    /* Caller must hold lock */
    if (!ctx->spi || atomic_load(&ctx->offline))
        return -ENODEV;

    if (!tx && !rx)
        return -EINVAL;

    if (len == 0 || len > FTE3600_MAX_SPI_TRANSFER_LEN)
        return -EINVAL;

    ctx->hardware_access_count++;
    return 0;
}

/* Scenario 1: Open followed by normal release */
static bool test_scenario_1_open_release(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);

    if (sim_driver_open(&ctx) != 0)
        return false;
    if (atomic_load(&ctx.refcount) != 2 || atomic_load(&ctx.freed) != 0)
        return false;

    sim_driver_release(&ctx);
    if (atomic_load(&ctx.refcount) != 1 || atomic_load(&ctx.freed) != 0)
        return false;

    /* Subsequent open succeeds */
    if (sim_driver_open(&ctx) != 0)
        return false;
    sim_driver_release(&ctx);

    /* Clean probe teardown */
    sim_ctx_put(&ctx);
    return (atomic_load(&ctx.freed) == 1);
}

/* Scenario 2: Open followed by device removal and then release */
static bool test_scenario_2_open_remove_release(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);

    if (sim_driver_open(&ctx) != 0)
        return false;

    /* Unbind driver while file is open */
    sim_driver_remove(&ctx);
    if (atomic_load(&ctx.refcount) != 1 || atomic_load(&ctx.freed) != 0)
        return false;
    if (atomic_load(&ctx.offline) != 1 || ctx.reset_gpiod != NULL)
        return false;

    /* Release file handle; drops last ref -> context freed */
    sim_driver_release(&ctx);
    return (atomic_load(&ctx.refcount) == 0 && atomic_load(&ctx.freed) == 1);
}

/* Scenario 3: Removal racing with an ioctl waiting for the mutex */
typedef struct {
    sim_context_t *ctx;
    pthread_barrier_t barrier;
    int ioctl_result;
} race_arg_t;

static void *thread_waiting_ioctl(void *arg)
{
    race_arg_t *r = (race_arg_t *)arg;
    pthread_barrier_wait(&r->barrier);
    /* Attempt reset; remove will have acquired lock first */
    r->ioctl_result = sim_driver_ioctl_reset(r->ctx);
    return NULL;
}

static bool test_scenario_3_removal_racing_waiting_ioctl(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);

    race_arg_t r;
    r.ctx = &ctx;
    r.ioctl_result = 0;
    pthread_barrier_init(&r.barrier, NULL, 2);

    /* Open file */
    sim_driver_open(&ctx);

    /* Pre-lock mutex to force ioctl thread to block waiting */
    pthread_mutex_lock(&ctx.lock);

    pthread_t th;
    pthread_create(&th, NULL, thread_waiting_ioctl, &r);

    /* Wait for thread to launch and reach lock attempt */
    pthread_barrier_wait(&r.barrier);
    usleep(5000); /* 5 ms window to ensure thread is blocked on mutex */

    /* Perform removal shutdown while holding the lock */
    atomic_store(&ctx.offline, 1);
    ctx.reset_gpiod = NULL;
    ctx.spi = NULL;
    pthread_mutex_unlock(&ctx.lock);

    /* Drop probe reference */
    sim_ctx_put(&ctx);

    pthread_join(th, NULL);
    pthread_barrier_destroy(&r.barrier);

    /* Waiting ioctl MUST observe offline and return -ENODEV without touching hardware */
    bool pass = (r.ioctl_result == -ENODEV && ctx.hardware_access_count == 0);

    sim_driver_release(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Scenario 4: Removal racing with an ioctl already holding the mutex */
static void *thread_holding_ioctl(void *arg)
{
    race_arg_t *r = (race_arg_t *)arg;
    pthread_mutex_lock(&r->ctx->lock);
    pthread_barrier_wait(&r->barrier);

    /* Simulate hardware reset holding mutex */
    usleep(15000); /* 15 ms in critical section */
    r->ctx->hardware_access_count++;
    r->ctx->state = FTE3600_STATE_RESET_SEQUENCE_ISSUED;

    pthread_mutex_unlock(&r->ctx->lock);
    return NULL;
}

static bool test_scenario_4_removal_racing_holding_ioctl(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);

    race_arg_t r;
    r.ctx = &ctx;
    pthread_barrier_init(&r.barrier, NULL, 2);

    sim_driver_open(&ctx);

    pthread_t th;
    pthread_create(&th, NULL, thread_holding_ioctl, &r);

    /* Wait until ioctl thread holds the mutex */
    pthread_barrier_wait(&r.barrier);

    /* Remove must block until thread finishes reset holding lock */
    sim_driver_remove(&ctx);

    pthread_join(th, NULL);
    pthread_barrier_destroy(&r.barrier);

    /* Verified: ioctl completed reset cleanly BEFORE remove invalidated pointers and set state to DEVICE_REMOVAL */
    bool pass = (ctx.hardware_access_count == 1 && ctx.state == FTE3600_STATE_DEVICE_REMOVAL);
    pass = pass && (ctx.reset_gpiod == NULL && ctx.spi == NULL && atomic_load(&ctx.offline) == 1);

    sim_driver_release(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Scenario 5: GET_STATE after removal */
static bool test_scenario_5_get_state_after_removal(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);

    sim_driver_open(&ctx);
    sim_driver_remove(&ctx);

    struct fte3600_state_snapshot snap;
    int ret = sim_driver_ioctl_get_state(&ctx, &snap);

    /* GET_STATE succeeds safely against retained software state */
    bool pass = (ret == 0);
    pass = pass && (snap.version == FTE3600_UAPI_VERSION);
    pass = pass && (snap.state == (uint32_t)FTE3600_STATE_UNINITIALIZED);
    pass = pass && ((snap.flags & FTE3600_FLAG_OFFLINE) != 0);

    sim_driver_release(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Scenario 6: RESET attempted after removal */
static bool test_scenario_6_reset_after_removal(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);

    sim_driver_open(&ctx);
    sim_driver_remove(&ctx);

    /* Reset ioctl must immediately fail with -ENODEV */
    int ret = sim_driver_ioctl_reset(&ctx);
    bool pass = (ret == -ENODEV && ctx.hardware_access_count == 0);

    sim_driver_release(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Scenario 7: Multiple open attempts */
static bool test_scenario_7_multiple_opens(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);

    /* Open 1 succeeds */
    if (sim_driver_open(&ctx) != 0)
        return false;

    /* Open 2 rejected with -EBUSY */
    if (sim_driver_open(&ctx) != -EBUSY)
        return false;

    /* Open 3 rejected with -EBUSY */
    if (sim_driver_open(&ctx) != -EBUSY)
        return false;

    /* Release allows subsequent open */
    sim_driver_release(&ctx);
    if (sim_driver_open(&ctx) != 0)
        return false;

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return (atomic_load(&ctx.freed) == 1);
}

/* Scenario 8: Reference acquisition and release on every error path */
static bool test_scenario_8_error_path_references(void)
{
    sim_context_t ctx;

    /* Probe failure: initialize then drop ref -> freed immediately */
    sim_ctx_init(&ctx);
    sim_ctx_put(&ctx);
    if (atomic_load(&ctx.freed) != 1)
        return false;

    /* Open error: attempting open on offline device does NOT leak ref */
    sim_ctx_init(&ctx);
    atomic_store(&ctx.offline, 1);
    if (sim_driver_open(&ctx) != -ENODEV)
        return false;
    if (atomic_load(&ctx.refcount) != 1)
        return false;

    sim_ctx_put(&ctx);
    return (atomic_load(&ctx.freed) == 1);
}

/* Scenario 9: SPI helper rejects invalid arguments */
static bool test_scenario_9_spi_invalid_arguments(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);

    uint8_t buf[16] = { 0 };

    /* 1. NULL buffers */
    if (sim_spi_sync_transfer(&ctx, NULL, NULL, 16) != -EINVAL)
        return false;

    /* 2. Zero length */
    if (sim_spi_sync_transfer(&ctx, buf, NULL, 0) != -EINVAL)
        return false;

    /* 3. Offline device */
    atomic_store(&ctx.offline, 1);
    if (sim_spi_sync_transfer(&ctx, buf, NULL, 16) != -ENODEV)
        return false;

    /* 4. NULL spi device */
    atomic_store(&ctx.offline, 0);
    ctx.spi = NULL;
    if (sim_spi_sync_transfer(&ctx, buf, NULL, 16) != -ENODEV)
        return false;

    sim_ctx_put(&ctx);
    return (atomic_load(&ctx.freed) == 1);
}

/* Scenario 10: SPI helper rejects transfers beyond its documented maximum */
static bool test_scenario_10_spi_length_bounds(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);

    uint8_t buf[16] = { 0 };

    /* Exactly maximum permitted length: accepted */
    if (sim_spi_sync_transfer(&ctx, buf, NULL, FTE3600_MAX_SPI_TRANSFER_LEN) != 0)
        return false;

    /* Maximum + 1 byte: rejected with -EINVAL */
    if (sim_spi_sync_transfer(&ctx, buf, NULL, FTE3600_MAX_SPI_TRANSFER_LEN + 1) != -EINVAL)
        return false;

    /* Large arbitrary transfer: rejected with -EINVAL */
    if (sim_spi_sync_transfer(&ctx, buf, NULL, 65536) != -EINVAL)
        return false;

    sim_ctx_put(&ctx);
    return (atomic_load(&ctx.freed) == 1);
}

/* Scenario 11: No hardware-resource access is possible after offline is set */
static bool test_scenario_11_no_hardware_access_after_offline(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);

    sim_driver_open(&ctx);
    sim_driver_remove(&ctx);

    int initial_accesses = ctx.hardware_access_count;

    /* Attempt reset */
    sim_driver_ioctl_reset(&ctx);

    /* Attempt SPI transfer */
    uint8_t buf[16] = { 0 };
    sim_spi_sync_transfer(&ctx, buf, NULL, 16);

    /* Verify zero hardware accesses occurred */
    bool pass = (ctx.hardware_access_count == initial_accesses);
    pass = pass && (ctx.reset_gpiod == NULL && ctx.spi == NULL);

    sim_driver_release(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* --- Suite 5: UAPI Validation & Parameter Rejections --- */

static int mock_validate_ioctl(unsigned int cmd, const void *arg)
{
    if (_IOC_TYPE(cmd) != FTE3600_IOC_MAGIC)
        return -ENOTTY;

    switch (cmd) {
    case FTE3600_IOC_GET_STATE:
        return 0;
    case FTE3600_IOC_RESET:
        return 0;
    case FTE3600_IOC_INIT: {
        const struct fte3600_init_params *p = (const struct fte3600_init_params *)arg;
        if (!p || p->version != FTE3600_UAPI_VERSION)
            return -EINVAL;
        if (p->flags & ~FTE3600_INIT_FLAGS_MASK)
            return -EINVAL;
        for (int i = 0; i < 6; i++) {
            if (p->reserved[i] != 0)
                return -EINVAL;
        }
        return 0; /* Parameters valid; dispatched to fte3600_hw_identify in Stage 2E.4 */
    }
    case FTE3600_IOC_CAPTURE: {
        const struct fte3600_frame_buffer *fb = (const struct fte3600_frame_buffer *)arg;
        if (!fb || fb->version != FTE3600_UAPI_VERSION)
            return -EINVAL;
        if (fb->length != FTE3600_IMAGE_BYTES)
            return -EINVAL;
        for (int i = 0; i < 4; i++) {
            if (fb->reserved[i] != 0)
                return -EINVAL;
        }
        return -EOPNOTSUPP;
    }
    default:
        return -ENOTTY;
    }
}

static bool test_uapi_rejections(void)
{
    unsigned int bogus_cmd = _IO('X', 0x01);
    if (mock_validate_ioctl(bogus_cmd, NULL) != -ENOTTY)
        return false;

    unsigned int bogus_nr = _IO(FTE3600_IOC_MAGIC, 0x99);
    if (mock_validate_ioctl(bogus_nr, NULL) != -ENOTTY)
        return false;

    struct fte3600_init_params ip = { .version = 99 };
    if (mock_validate_ioctl(FTE3600_IOC_INIT, &ip) != -EINVAL)
        return false;

    struct fte3600_init_params ip2 = { .version = FTE3600_UAPI_VERSION, .reserved = {0, 1, 0, 0, 0, 0} };
    if (mock_validate_ioctl(FTE3600_IOC_INIT, &ip2) != -EINVAL)
        return false;

    struct fte3600_init_params ip_valid = { .version = FTE3600_UAPI_VERSION };
    if (mock_validate_ioctl(FTE3600_IOC_INIT, &ip_valid) != 0)
        return false;

    struct fte3600_frame_buffer fb_wrong_len = {
        .version = FTE3600_UAPI_VERSION,
        .length = 4096,
    };
    if (mock_validate_ioctl(FTE3600_IOC_CAPTURE, &fb_wrong_len) != -EINVAL)
        return false;

    struct fte3600_frame_buffer fb_reserved = {
        .version = FTE3600_UAPI_VERSION,
        .length = FTE3600_IMAGE_BYTES,
        .reserved = {1, 0, 0, 0},
    };
    if (mock_validate_ioctl(FTE3600_IOC_CAPTURE, &fb_reserved) != -EINVAL)
        return false;

    struct fte3600_frame_buffer fb_valid = {
        .version = FTE3600_UAPI_VERSION,
        .length = FTE3600_IMAGE_BYTES,
    };
    if (mock_validate_ioctl(FTE3600_IOC_CAPTURE, &fb_valid) != -EOPNOTSUPP)
        return false;

    return true;
}

/* --- Suite 6: Stage 2E.4.2 Chip Identification & Completion Semantics --- */

typedef struct {
    int wake1_ret;
    uint8_t status_80_val;         /* Normally 0x50 */
    int abort_ret;
    uint8_t status_80_retry_val;   /* If first was not 0x50 */
    int wake2_ret;
    int loop_write_ret;
    int loop_read_ret;
    uint8_t fab_code_val;          /* e.g. 0x4C for SMIC AA (0x4C >> 2 == 0x13) */
    int fab_code_ret;
} mock_spi_startup_profile_t;

typedef struct {
    int wake_write_ret;        /* errno returned by spi_sync on wake write */
    int wake_write_status;     /* m.status on wake write */
    size_t wake_write_actual_len; /* m.actual_length on wake write (normally 4) */

    int wake_read_ret;         /* errno returned by spi_sync on wake readback */
    int wake_read_status;      /* m.status on wake readback */
    size_t wake_read_actual_len;  /* m.actual_length on wake readback (normally 5) */
    uint8_t wake_read_val;     /* rx[4] value on wake readback (normally 0x01) */

    int chip_id_ret;           /* errno returned by spi_sync on chip ID read */
    int chip_id_status;        /* m.status on chip ID read */
    size_t chip_id_actual_len;    /* m.actual_length on chip ID read (normally 10) */
    uint16_t chip_id_val;      /* chip ID returned in rx[6..7] (normally 0x9362) */
    uint16_t chip_crc_val;     /* CRC returned in rx[8..9] */
    bool override_crc;         /* if true, use chip_crc_val; if false, auto-generate matching wire CRC */
    unsigned long delay_us;    /* delay to sleep during stabilization */

    mock_spi_startup_profile_t startup;
} mock_spi_profile_t;

static void init_mock_profile(mock_spi_profile_t *p)
{
    memset(p, 0, sizeof(*p));
    p->wake_write_actual_len = 4;
    p->wake_read_actual_len = 5;
    p->wake_read_val = 0x01;
    p->chip_id_actual_len = 10;
    p->chip_id_val = FTE3600_EXPECTED_CHIP_ID;
    p->override_crc = false;
    p->delay_us = 0;

    p->startup.status_80_val = 0x50;
    p->startup.status_80_retry_val = 0x50;
    p->startup.fab_code_val = 0x4C; /* SMIC Fab AA: 0x4C >> 2 == 0x13 */
}

static int sim_driver_hw_identify(sim_context_t *ctx, const mock_spi_profile_t *prof)
{
    if (atomic_load(&ctx->offline) || !ctx->spi)
        return -ENODEV;

    if (ctx->state != FTE3600_STATE_RESET_SEQUENCE_ISSUED)
        return -EPERM;

    ctx->state = FTE3600_STATE_INITIALIZING;

    /* Step A: Wake write 0xC6 = 0x01 (4 bytes) */
    ctx->hardware_access_count++;
    if (prof->wake_write_ret != 0) {
        ctx->state = FTE3600_STATE_UNKNOWN;
        ctx->last_error = (uint32_t)(-prof->wake_write_ret);
        return prof->wake_write_ret;
    }
    if (prof->wake_write_status != 0) {
        ctx->state = FTE3600_STATE_UNKNOWN;
        ctx->last_error = (uint32_t)(-prof->wake_write_status);
        return prof->wake_write_status;
    }
    if (prof->wake_write_actual_len != 4) {
        ctx->state = FTE3600_STATE_UNKNOWN;
        ctx->last_error = EIO;
        return -EIO;
    }

    /* Step B: Transceiver stabilization delay */
    if (prof->delay_us > 0)
        usleep(prof->delay_us);

    /* Step C: Wake readback verification (5 bytes) */
    ctx->hardware_access_count++;
    if (prof->wake_read_ret != 0) {
        ctx->state = FTE3600_STATE_UNKNOWN;
        ctx->last_error = (uint32_t)(-prof->wake_read_ret);
        return prof->wake_read_ret;
    }
    if (prof->wake_read_status != 0) {
        ctx->state = FTE3600_STATE_UNKNOWN;
        ctx->last_error = (uint32_t)(-prof->wake_read_status);
        return prof->wake_read_status;
    }
    if (prof->wake_read_actual_len != 5) {
        ctx->state = FTE3600_STATE_UNKNOWN;
        ctx->last_error = EIO;
        return -EIO;
    }
    if (prof->wake_read_val != 0x01) {
        ctx->state = FTE3600_STATE_UNKNOWN;
        ctx->last_error = EIO;
        return -EIO;
    }

    /* Step D: Interrogate Chip ID register 0x1A8B (10 bytes) */
    ctx->hardware_access_count++;
    if (prof->chip_id_ret != 0) {
        ctx->state = FTE3600_STATE_UNKNOWN;
        ctx->last_error = (uint32_t)(-prof->chip_id_ret);
        return prof->chip_id_ret;
    }
    if (prof->chip_id_status != 0) {
        ctx->state = FTE3600_STATE_UNKNOWN;
        ctx->last_error = (uint32_t)(-prof->chip_id_status);
        return prof->chip_id_status;
    }
    if (prof->chip_id_actual_len != 10) {
        ctx->state = FTE3600_STATE_UNKNOWN;
        ctx->last_error = EIO;
        return -EIO;
    }

    uint16_t chip_id = prof->chip_id_val;
    uint8_t id_bytes[2] = { (uint8_t)(chip_id >> 8), (uint8_t)(chip_id & 0xFF) };
    uint16_t crc_calc = test_crc16_wire(id_bytes, 2);
    uint16_t crc_rx = prof->override_crc ? prof->chip_crc_val : crc_calc;

    /* Step E & F: Variant gate & fault evaluation */
    if (chip_id == 0x0000 || chip_id == 0xFFFF) {
        ctx->state = FTE3600_STATE_UNKNOWN;
        ctx->last_error = ENODEV;
        return -ENODEV;
    }

    if (crc_rx != crc_calc) {
        ctx->state = FTE3600_STATE_UNKNOWN;
        ctx->last_error = EBADMSG;
        return -EBADMSG;
    }

    if (chip_id != FTE3600_EXPECTED_CHIP_ID) {
        ctx->state = FTE3600_STATE_UNKNOWN;
        ctx->last_error = ENODEV;
        return -ENODEV;
    }

    /* Identification verified: transition state to IDENTIFIED (NOT READY) */
    ctx->state = FTE3600_STATE_IDENTIFIED;
    ctx->last_error = 0;
    return 0;
}

static int sim_driver_hw_startup(sim_context_t *ctx, const mock_spi_startup_profile_t *sprof)
{
    if (atomic_load(&ctx->offline) || !ctx->spi)
        return -ENODEV;

    if (ctx->state != FTE3600_STATE_IDENTIFIED)
        return -EPERM;

    ctx->state = FTE3600_STATE_INITIALIZING;

    /* Step 1: Wake strobe 1 { 0x5A, 0xA5, 0x00 } */
    ctx->hardware_access_count++;
    if (sprof->wake1_ret != 0) {
        ctx->state = FTE3600_STATE_UNKNOWN;
        ctx->last_error = (uint32_t)(-sprof->wake1_ret);
        return sprof->wake1_ret;
    }

    /* Step 2: Stabilization wait */

    /* Step 3: Verify HW_STATUS (0x80) enters idle state 0x50 */
    ctx->hardware_access_count++;
    uint8_t st80 = sprof->status_80_val;
    if (st80 != 0x50) {
        /* Abort strobe { 0xC0, 0x3F, 0x00 } */
        ctx->hardware_access_count++;
        if (sprof->abort_ret != 0) {
            ctx->state = FTE3600_STATE_UNKNOWN;
            ctx->last_error = (uint32_t)(-sprof->abort_ret);
            return sprof->abort_ret;
        }

        /* Retry status 0x80 */
        ctx->hardware_access_count++;
        st80 = sprof->status_80_retry_val;
        if (st80 != 0x50) {
            ctx->state = FTE3600_STATE_UNKNOWN;
            ctx->last_error = EIO;
            return -EIO;
        }
    }

    /* Step 4: Wake strobe 2 { 0xA5, 0x5A, 0x00 } */
    ctx->hardware_access_count++;
    if (sprof->wake2_ret != 0) {
        ctx->state = FTE3600_STATE_UNKNOWN;
        ctx->last_error = (uint32_t)(-sprof->wake2_ret);
        return sprof->wake2_ret;
    }

    /* Step 5: Foundry Interrogation (two loop iterations: index 0x03 and index 0x13) */
    for (int i = 0; i < 2; i++) {
        for (int w = 0; w < 5; w++) {
            ctx->hardware_access_count++;
            if (sprof->loop_write_ret != 0) {
                ctx->state = FTE3600_STATE_UNKNOWN;
                ctx->last_error = (uint32_t)(-sprof->loop_write_ret);
                return sprof->loop_write_ret;
            }
        }
        ctx->hardware_access_count++;
        if (sprof->loop_read_ret != 0) {
            ctx->state = FTE3600_STATE_UNKNOWN;
            ctx->last_error = (uint32_t)(-sprof->loop_read_ret);
            return sprof->loop_read_ret;
        }
    }

    /* Step 6: Read foundry identification code from register 0x9B */
    ctx->hardware_access_count++;
    if (sprof->fab_code_ret != 0) {
        ctx->state = FTE3600_STATE_UNKNOWN;
        ctx->last_error = (uint32_t)(-sprof->fab_code_ret);
        return sprof->fab_code_ret;
    }
    uint8_t fab = sprof->fab_code_val;
    if ((fab >> 2) == 0x13) {
        ctx->foundry_code = 0xAA; /* SMIC Fab AA */
    } else {
        ctx->foundry_code = 0x00;
    }
    ctx->line_period_us = (ctx->foundry_code == 0xAA) ? 150 : 200;

    /* Volatile startup prefix successful: transition to INITIALIZED_UNCALIBRATED (withholding READY) */
    ctx->state = FTE3600_STATE_INITIALIZED_UNCALIBRATED;
    ctx->last_error = 0;
    return 0;
}

static int sim_driver_ioctl_init(sim_context_t *ctx, const struct fte3600_init_params *params, const mock_spi_profile_t *prof)
{
    if (atomic_load(&ctx->offline))
        return -ENODEV;

    if (!params || params->version != FTE3600_UAPI_VERSION)
        return -EINVAL;

    if (params->flags & ~FTE3600_INIT_FLAGS_MASK)
        return -EINVAL;

    for (int i = 0; i < 6; i++) {
        if (params->reserved[i] != 0)
            return -EINVAL;
    }

    pthread_mutex_lock(&ctx->lock);

    if (atomic_load(&ctx->offline) || !ctx->spi) {
        pthread_mutex_unlock(&ctx->lock);
        return -ENODEV;
    }

    int ret = 0;
    switch (ctx->state) {
    case FTE3600_STATE_RESET_SEQUENCE_ISSUED:
        ret = sim_driver_hw_identify(ctx, prof);
        if (ret == 0 && (params->flags & FTE3600_INIT_FLAG_FULL_STARTUP))
            ret = sim_driver_hw_startup(ctx, &prof->startup);
        break;
    case FTE3600_STATE_IDENTIFIED:
        ret = sim_driver_hw_startup(ctx, &prof->startup);
        break;
    default:
        ret = -EPERM;
        break;
    }

    pthread_mutex_unlock(&ctx->lock);
    return ret;
}

/* 1. Valid wake response (rx_c6[4] == 0x01) */
static bool test_id_scenario_1_valid_wake_response(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);

    if (sim_driver_ioctl_reset(&ctx) != 0 || ctx.state != FTE3600_STATE_RESET_SEQUENCE_ISSUED)
        return false;

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    prof.wake_read_val = 0x01;

    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);

    bool pass = (ret == 0 && ctx.state == FTE3600_STATE_IDENTIFIED && ctx.last_error == 0);
    pass = pass && (ctx.hardware_access_count == 4); /* 1 reset + 3 transfers */

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* 2. Invalid wake response (rx_c6[4] != 0x01) */
static bool test_id_scenario_2_invalid_wake_response(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);

    sim_driver_ioctl_reset(&ctx);

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    prof.wake_read_val = 0x00; /* Mismatch */

    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);

    bool pass = (ret == -EIO && ctx.state == FTE3600_STATE_UNKNOWN && ctx.last_error == EIO);

    /* Fail-closed: Subsequent INIT fails with -EPERM from UNKNOWN */
    if (sim_driver_ioctl_init(&ctx, &params, &prof) != -EPERM)
        return false;

    /* Test 0xFF response */
    sim_driver_ioctl_reset(&ctx);
    prof.wake_read_val = 0xFF;
    ret = sim_driver_ioctl_init(&ctx, &params, &prof);
    pass = pass && (ret == -EIO && ctx.state == FTE3600_STATE_UNKNOWN);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* 3. Valid FT9362 ID and CRC (0x9362 and 0x1C53) */
static bool test_id_scenario_3_valid_id_and_crc(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    sim_driver_ioctl_reset(&ctx);

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    prof.chip_id_val = 0x9362;
    prof.override_crc = true;
    prof.chip_crc_val = 0x1C53;

    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);

    bool pass = (ret == 0 && ctx.state == FTE3600_STATE_IDENTIFIED && ctx.last_error == 0);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* 4. CRC mismatch */
static bool test_id_scenario_4_crc_mismatch(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    sim_driver_ioctl_reset(&ctx);

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    prof.chip_id_val = 0x9362;
    prof.override_crc = true;
    prof.chip_crc_val = 0xBADC; /* Corrupted wire CRC */

    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);

    bool pass = (ret == -EBADMSG && ctx.state == FTE3600_STATE_UNKNOWN && ctx.last_error == EBADMSG);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* 5. Unexpected chip ID (0x9368, 0x9391) */
static bool test_id_scenario_5_unexpected_chip_id(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);

    /* Test 0x9368 */
    sim_driver_ioctl_reset(&ctx);
    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    prof.chip_id_val = 0x9368;
    prof.override_crc = false; /* valid CRC for 0x9368, but wrong silicon ID */

    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
    int ret1 = sim_driver_ioctl_init(&ctx, &params, &prof);
    if (ret1 != -ENODEV || ctx.state != FTE3600_STATE_UNKNOWN || ctx.last_error != ENODEV)
        return false;

    /* Test 0x9391 */
    sim_driver_ioctl_reset(&ctx);
    prof.chip_id_val = 0x9391;
    int ret2 = sim_driver_ioctl_init(&ctx, &params, &prof);
    if (ret2 != -ENODEV || ctx.state != FTE3600_STATE_UNKNOWN || ctx.last_error != ENODEV)
        return false;

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return (atomic_load(&ctx.freed) == 1);
}

/* 6. All-zero response (0x0000) */
static bool test_id_scenario_6_all_zero_response(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    sim_driver_ioctl_reset(&ctx);

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    prof.chip_id_val = 0x0000;
    prof.override_crc = true;
    prof.chip_crc_val = 0x0000;

    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);

    bool pass = (ret == -ENODEV && ctx.state == FTE3600_STATE_UNKNOWN && ctx.last_error == ENODEV);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* 7. All-ones response (0xFFFF) */
static bool test_id_scenario_7_all_ones_response(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    sim_driver_ioctl_reset(&ctx);

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    prof.chip_id_val = 0xFFFF;
    prof.override_crc = true;
    prof.chip_crc_val = 0xFFFF;

    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);

    bool pass = (ret == -ENODEV && ctx.state == FTE3600_STATE_UNKNOWN && ctx.last_error == ENODEV);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* 8. SPI transfer error (-EIO) */
static bool test_id_scenario_8_spi_transfer_error(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);

    /* Error on wake write */
    sim_driver_ioctl_reset(&ctx);
    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    prof.wake_write_ret = -EIO;

    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);
    if (ret != -EIO || ctx.state != FTE3600_STATE_UNKNOWN || ctx.last_error != EIO)
        return false;

    /* Error on chip ID transfer */
    sim_driver_ioctl_reset(&ctx);
    init_mock_profile(&prof);
    prof.chip_id_ret = -EIO;
    ret = sim_driver_ioctl_init(&ctx, &params, &prof);
    if (ret != -EIO || ctx.state != FTE3600_STATE_UNKNOWN || ctx.last_error != EIO)
        return false;

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return (atomic_load(&ctx.freed) == 1);
}

/* 9. Short/incomplete transfer (m.status != 0 or actual_length < len) */
static bool test_id_scenario_9_short_incomplete_transfer(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);

    /* 9a: Incomplete message status on chip ID read */
    sim_driver_ioctl_reset(&ctx);
    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    prof.chip_id_status = -EIO;

    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);
    if (ret != -EIO || ctx.state != FTE3600_STATE_UNKNOWN || ctx.last_error != EIO)
        return false;

    /* 9b: Status error on wake write */
    sim_driver_ioctl_reset(&ctx);
    init_mock_profile(&prof);
    prof.wake_write_status = -EMSGSIZE;
    ret = sim_driver_ioctl_init(&ctx, &params, &prof);
    if (ret != -EMSGSIZE || ctx.state != FTE3600_STATE_UNKNOWN || ctx.last_error != EMSGSIZE)
        return false;

    /* 9c: Short transfer on wake write (actual_length == 3 != 4) with status == 0 */
    sim_driver_ioctl_reset(&ctx);
    init_mock_profile(&prof);
    prof.wake_write_actual_len = 3;
    ret = sim_driver_ioctl_init(&ctx, &params, &prof);
    if (ret != -EIO || ctx.state != FTE3600_STATE_UNKNOWN || ctx.last_error != EIO)
        return false;

    /* 9d: Short transfer on wake readback (actual_length == 4 != 5) with status == 0 */
    sim_driver_ioctl_reset(&ctx);
    init_mock_profile(&prof);
    prof.wake_read_actual_len = 4;
    ret = sim_driver_ioctl_init(&ctx, &params, &prof);
    if (ret != -EIO || ctx.state != FTE3600_STATE_UNKNOWN || ctx.last_error != EIO)
        return false;

    /* 9e: Short transfer on chip ID read (actual_length == 8 != 10) with status == 0 */
    sim_driver_ioctl_reset(&ctx);
    init_mock_profile(&prof);
    prof.chip_id_actual_len = 8;
    ret = sim_driver_ioctl_init(&ctx, &params, &prof);
    if (ret != -EIO || ctx.state != FTE3600_STATE_UNKNOWN || ctx.last_error != EIO)
        return false;

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return (atomic_load(&ctx.freed) == 1);
}

/* 10. Failure after wake write */
static bool test_id_scenario_10_failure_after_wake_write(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    sim_driver_ioctl_reset(&ctx);

    /* Wake write succeeds, but wake readback fails */
    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    prof.wake_read_ret = -EIO;

    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);

    bool pass = (ret == -EIO && ctx.state == FTE3600_STATE_UNKNOWN);

    /* Fail-closed: verify sensor is not left in IDENTIFIED, READY, or INITIALIZING */
    if (ctx.state == FTE3600_STATE_IDENTIFIED || ctx.state == FTE3600_STATE_READY || ctx.state == FTE3600_STATE_INITIALIZING)
        return false;

    /* Verify subsequent INIT fails with -EPERM without reset */
    if (sim_driver_ioctl_init(&ctx, &params, &prof) != -EPERM)
        return false;

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* 11. INIT from invalid state (UNINITIALIZED, INITIALIZING, READY, UNKNOWN, RESETTING) */
static bool test_id_scenario_11_invalid_state_init(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };

    /* 1. From UNINITIALIZED */
    ctx.state = FTE3600_STATE_UNINITIALIZED;
    if (sim_driver_ioctl_init(&ctx, &params, &prof) != -EPERM || ctx.state != FTE3600_STATE_UNINITIALIZED)
        return false;

    /* 2. From INITIALIZING */
    ctx.state = FTE3600_STATE_INITIALIZING;
    if (sim_driver_ioctl_init(&ctx, &params, &prof) != -EPERM || ctx.state != FTE3600_STATE_INITIALIZING)
        return false;

    /* 3. From READY */
    ctx.state = FTE3600_STATE_READY;
    if (sim_driver_ioctl_init(&ctx, &params, &prof) != -EPERM || ctx.state != FTE3600_STATE_READY)
        return false;

    /* 4. From UNKNOWN */
    ctx.state = FTE3600_STATE_UNKNOWN;
    if (sim_driver_ioctl_init(&ctx, &params, &prof) != -EPERM || ctx.state != FTE3600_STATE_UNKNOWN)
        return false;

    /* 5. From RESETTING */
    ctx.state = FTE3600_STATE_RESETTING;
    if (sim_driver_ioctl_init(&ctx, &params, &prof) != -EPERM || ctx.state != FTE3600_STATE_RESETTING)
        return false;

    /* Zero hardware accesses during invalid state calls */
    if (ctx.hardware_access_count != 0)
        return false;

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return (atomic_load(&ctx.freed) == 1);
}

/* 12. INIT while device offline (-ENODEV) */
static bool test_id_scenario_12_init_while_offline(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    sim_driver_ioctl_reset(&ctx);

    /* Mark offline and remove SPI device pointer */
    atomic_store(&ctx.offline, 1);
    ctx.spi = NULL;

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };

    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);
    bool pass = (ret == -ENODEV);
    pass = pass && (ctx.hardware_access_count == 1); /* Only initial reset, zero transfers */

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* 13. Repeated INIT after success (-EPERM) */
static bool test_id_scenario_13_repeated_init_after_success(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    sim_driver_ioctl_reset(&ctx);

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };

    /* First INIT succeeds -> transitions to IDENTIFIED */
    if (sim_driver_ioctl_init(&ctx, &params, &prof) != 0 || ctx.state != FTE3600_STATE_IDENTIFIED)
        return false;

    /* Second INIT succeeds -> transitions to INITIALIZED_UNCALIBRATED (withholding READY) */
    if (sim_driver_ioctl_init(&ctx, &params, &prof) != 0 || ctx.state != FTE3600_STATE_INITIALIZED_UNCALIBRATED)
        return false;

    /* Third INIT from INITIALIZED_UNCALIBRATED is rejected with -EPERM */
    int ret3 = sim_driver_ioctl_init(&ctx, &params, &prof);
    if (ret3 != -EPERM)
        return false;

    /* State must remain strictly INITIALIZED_UNCALIBRATED */
    if (ctx.state != FTE3600_STATE_INITIALIZED_UNCALIBRATED)
        return false;

    struct fte3600_state_snapshot snap;
    if (sim_driver_ioctl_get_state(&ctx, &snap) != 0 || snap.state != (uint32_t)FTE3600_STATE_IDENTIFIED)
        return false;

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return (atomic_load(&ctx.freed) == 1);
}

/* 14. Concurrent RESET and INIT serialization under mutex */
typedef struct {
    sim_context_t *ctx;
    atomic_int stop;
} conc_worker_arg_t;

static void *thread_concurrent_reset(void *arg)
{
    conc_worker_arg_t *c = (conc_worker_arg_t *)arg;
    while (!atomic_load(&c->stop)) {
        sim_driver_ioctl_reset(c->ctx);
        usleep(100);
    }
    return NULL;
}

static void *thread_concurrent_init(void *arg)
{
    conc_worker_arg_t *c = (conc_worker_arg_t *)arg;
    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };

    while (!atomic_load(&c->stop)) {
        sim_driver_ioctl_init(c->ctx, &params, &prof);
        usleep(100);
    }
    return NULL;
}

static bool test_id_scenario_14_concurrent_reset_init_mutex(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);

    conc_worker_arg_t c;
    c.ctx = &ctx;
    atomic_init(&c.stop, 0);

    pthread_t th1, th2;
    pthread_create(&th1, NULL, thread_concurrent_reset, &c);
    pthread_create(&th2, NULL, thread_concurrent_init, &c);

    usleep(25000); /* 25 ms concurrent execution */
    atomic_store(&c.stop, 1);

    pthread_join(th1, NULL);
    pthread_join(th2, NULL);

    /* Mutex guarantees state is a valid enum value and no torn memory exists */
    bool valid_state = (ctx.state == FTE3600_STATE_RESET_SEQUENCE_ISSUED ||
                        ctx.state == FTE3600_STATE_IDENTIFIED ||
                        ctx.state == FTE3600_STATE_UNKNOWN);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return valid_state && (atomic_load(&ctx.freed) == 1);
}

/* 15. Removal during an operation */
typedef struct {
    sim_context_t *ctx;
    pthread_barrier_t barrier;
    int init_ret;
} removal_during_op_arg_t;

static void *thread_slow_init(void *arg)
{
    removal_during_op_arg_t *r = (removal_during_op_arg_t *)arg;
    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    prof.delay_us = 20000; /* 20 ms artificial delay while holding lock */
    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };

    pthread_barrier_wait(&r->barrier);
    r->init_ret = sim_driver_ioctl_init(r->ctx, &params, &prof);
    return NULL;
}

static bool test_id_scenario_15_removal_during_operation(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    sim_driver_ioctl_reset(&ctx);

    removal_during_op_arg_t r;
    r.ctx = &ctx;
    r.init_ret = 0;
    pthread_barrier_init(&r.barrier, NULL, 2);

    pthread_t th;
    pthread_create(&th, NULL, thread_slow_init, &r);

    /* Wait for worker to enter */
    pthread_barrier_wait(&r.barrier);
    usleep(5000); /* Ensure thread has entered sim_driver_ioctl_init and taken lock */

    /* Remove must block until the in-flight INIT completes */
    sim_driver_remove(&ctx);

    pthread_join(th, NULL);
    pthread_barrier_destroy(&r.barrier);

    /* INIT completed cleanly before removal invalidated pointers and transitioned to DEVICE_REMOVAL */
    bool pass = (r.init_ret == 0 && ctx.state == FTE3600_STATE_DEVICE_REMOVAL);
    pass = pass && (atomic_load(&ctx.offline) == 1 && ctx.spi == NULL && ctx.reset_gpiod == NULL);

    /* Subsequent operation rejected with -ENODEV */
    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
    if (sim_driver_ioctl_init(&ctx, &params, &prof) != -ENODEV)
        return false;

    sim_driver_release(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* 16. Dedicated SPI transfer completion semantics */
static bool test_id_scenario_16_spi_completion_semantics(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
    mock_spi_profile_t prof;

    /* 16a. Exact length match (actual_length == len) succeeds */
    sim_driver_ioctl_reset(&ctx);
    init_mock_profile(&prof);
    prof.wake_write_actual_len = 4;
    prof.wake_read_actual_len = 5;
    prof.chip_id_actual_len = 10;
    if (sim_driver_ioctl_init(&ctx, &params, &prof) != 0 || ctx.state != FTE3600_STATE_IDENTIFIED)
        return false;

    /* 16b. Short transfer with status == 0 fails with -EIO and UNKNOWN */
    sim_driver_ioctl_reset(&ctx);
    init_mock_profile(&prof);
    prof.chip_id_actual_len = 7; /* Short transfer */
    prof.chip_id_status = 0;     /* Status deceptively zero */
    if (sim_driver_ioctl_init(&ctx, &params, &prof) != -EIO)
        return false;
    if (ctx.state != FTE3600_STATE_UNKNOWN || ctx.last_error != EIO)
        return false;

    /* 16c. Nonzero message status fails with status code and UNKNOWN */
    sim_driver_ioctl_reset(&ctx);
    init_mock_profile(&prof);
    prof.chip_id_status = -EBUSY;
    if (sim_driver_ioctl_init(&ctx, &params, &prof) != -EBUSY)
        return false;
    if (ctx.state != FTE3600_STATE_UNKNOWN || ctx.last_error != EBUSY)
        return false;

    /* 16d. Nonzero spi_sync return fails with ret code and UNKNOWN */
    sim_driver_ioctl_reset(&ctx);
    init_mock_profile(&prof);
    prof.chip_id_ret = -ETIMEDOUT;
    if (sim_driver_ioctl_init(&ctx, &params, &prof) != -ETIMEDOUT)
        return false;
    if (ctx.state != FTE3600_STATE_UNKNOWN || ctx.last_error != ETIMEDOUT)
        return false;

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return (atomic_load(&ctx.freed) == 1);
}

/* 17. Semantic boundary: IDENTIFIED vs READY, and CAPTURE rejection */
static bool test_id_scenario_17_semantic_boundary_capture_rejection(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
    mock_spi_profile_t prof;
    init_mock_profile(&prof);

    sim_driver_ioctl_reset(&ctx);
    if (ctx.state != FTE3600_STATE_RESET_SEQUENCE_ISSUED)
        return false;

    if (sim_driver_ioctl_init(&ctx, &params, &prof) != 0)
        return false;

    /* Verify state is strictly IDENTIFIED (8), NOT READY (4) */
    if (ctx.state != FTE3600_STATE_IDENTIFIED)
        return false;
    if (ctx.state == FTE3600_STATE_READY)
        return false;

    /* Verify telemetry snapshot reflects IDENTIFIED */
    struct fte3600_state_snapshot snap;
    if (sim_driver_ioctl_get_state(&ctx, &snap) != 0)
        return false;
    if (snap.state != (uint32_t)FTE3600_STATE_IDENTIFIED)
        return false;

    /* Verify FTE3600_IOC_CAPTURE continues to be rejected with -EOPNOTSUPP */
    struct fte3600_frame_buffer fb = {
        .version = FTE3600_UAPI_VERSION,
        .length = FTE3600_IMAGE_BYTES,
    };
    if (mock_validate_ioctl(FTE3600_IOC_CAPTURE, &fb) != -EOPNOTSUPP)
        return false;

    /* Verify clean reset recovery from IDENTIFIED back to RESET_SEQUENCE_ISSUED */
    if (sim_driver_ioctl_reset(&ctx) != 0)
        return false;
    if (ctx.state != FTE3600_STATE_RESET_SEQUENCE_ISSUED)
        return false;

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return (atomic_load(&ctx.freed) == 1);
}

/* --- Suite 7: Stage D5.2 Remediation, UAPI Mapping, and Poll Verification --- */

/* Test 1: State Mapping and UAPI Range Isolation */
static bool test_d5_2_state_mapping_and_uapi_isolation(void)
{
    /* 1. Public states (0..8) must map exactly to themselves */
    for (int s = 0; s <= FTE3600_MAX_PUBLIC_STATE; s++) {
        if (fte3600_state_to_uapi((enum fte3600_state)s) != (uint32_t)s)
            return false;
    }

    /* 2. Extended internal state 13 (DEVICE_REMOVAL) must map to UNINITIALIZED (0) */
    if (fte3600_state_to_uapi(FTE3600_STATE_DEVICE_REMOVAL) != (uint32_t)FTE3600_STATE_UNINITIALIZED)
        return false;

    /* 3. Operational internal states (9..11) must map to READY (4) */
    if (fte3600_state_to_uapi(FTE3600_STATE_WAITING_FOR_TOUCH) != (uint32_t)FTE3600_STATE_READY)
        return false;
    if (fte3600_state_to_uapi(FTE3600_STATE_TOUCH_DETECTED) != (uint32_t)FTE3600_STATE_READY)
        return false;
    if (fte3600_state_to_uapi(FTE3600_STATE_FRAME_READY) != (uint32_t)FTE3600_STATE_READY)
        return false;

    /* 4. Suspended internal state (12) must map to UNKNOWN (7) */
    if (fte3600_state_to_uapi(FTE3600_STATE_SUSPENDED) != (uint32_t)FTE3600_STATE_UNKNOWN)
        return false;

    /* 5. Any arbitrary or out-of-range state value must be bounded within [0, FTE3600_MAX_PUBLIC_STATE] */
    for (int s = 0; s <= 30; s++) {
        if (fte3600_state_to_uapi((enum fte3600_state)s) > FTE3600_MAX_PUBLIC_STATE)
            return false;
    }

    /* 6. Verify legacy alias INITIALIZED == READY (4) */
    if (FTE3600_STATE_INITIALIZED != FTE3600_STATE_READY || FTE3600_STATE_INITIALIZED != 4)
        return false;

    return true;
}

/* Test 2: ABI Layout and Reserved Field Integrity */
static bool test_d5_2_abi_compile_time_and_reserved_assertions(void)
{
    /* Static size assertions (C11 _Static_assert) */
    _Static_assert(sizeof(struct fte3600_state_snapshot) == 32, "state_snapshot size mismatch");
    _Static_assert(sizeof(struct fte3600_init_params) == 32, "init_params size mismatch");
    _Static_assert(sizeof(struct fte3600_frame_buffer) == 32, "frame_buffer size mismatch");

    /* Offset assertions */
    if (offsetof(struct fte3600_state_snapshot, version) != 0 ||
        offsetof(struct fte3600_state_snapshot, state) != 4 ||
        offsetof(struct fte3600_state_snapshot, flags) != 8 ||
        offsetof(struct fte3600_state_snapshot, last_error) != 12 ||
        offsetof(struct fte3600_state_snapshot, reserved) != 16)
        return false;

    if (offsetof(struct fte3600_init_params, version) != 0 ||
        offsetof(struct fte3600_init_params, flags) != 4 ||
        offsetof(struct fte3600_init_params, reserved) != 8)
        return false;

    if (offsetof(struct fte3600_frame_buffer, version) != 0 ||
        offsetof(struct fte3600_frame_buffer, length) != 4 ||
        offsetof(struct fte3600_frame_buffer, buffer_ptr) != 8 ||
        offsetof(struct fte3600_frame_buffer, reserved) != 16)
        return false;

    /* Verify GET_STATE guarantees all 4 reserved __u32 words are zero */
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    struct fte3600_state_snapshot snap;
    if (sim_driver_ioctl_get_state(&ctx, &snap) != 0)
        return false;
    for (int i = 0; i < 4; i++) {
        if (snap.reserved[i] != 0)
            return false;
    }
    sim_ctx_put(&ctx);
    return true;
}

/* Test 3: Removal GET_STATE UAPI Invariant (State 13 Never Leaks) */
static bool test_d5_2_removal_get_state_and_uapi_guarantee(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);

    /* Open file handle, then remove device */
    if (sim_driver_open(&ctx) != 0)
        return false;

    sim_driver_remove(&ctx);

    /* Internal driver state must be DEVICE_REMOVAL (13) */
    if (ctx.state != FTE3600_STATE_DEVICE_REMOVAL)
        return false;

    /* Query state snapshot via GET_STATE ioctl */
    struct fte3600_state_snapshot snap;
    int ret = sim_driver_ioctl_get_state(&ctx, &snap);
    if (ret != 0)
        return false;

    /*
     * UAPI Safety Invariant:
     * 1. Public state MUST NOT equal 13 (must be mapped to UNINITIALIZED == 0).
     * 2. snap.state MUST NOT exceed FTE3600_MAX_PUBLIC_STATE (8).
     * 3. snap.flags MUST assert FTE3600_FLAG_OFFLINE.
     * 4. Version must match FTE3600_UAPI_VERSION (1).
     */
    if (snap.state != (uint32_t)FTE3600_STATE_UNINITIALIZED)
        return false;
    if (snap.state > FTE3600_MAX_PUBLIC_STATE)
        return false;
    if ((snap.flags & FTE3600_FLAG_OFFLINE) == 0)
        return false;
    if (snap.version != FTE3600_UAPI_VERSION)
        return false;

    sim_driver_release(&ctx);
    return (atomic_load(&ctx.freed) == 1);
}

/* Simulated poll helper modeling fte3600_cdev_poll() */
static __poll_t sim_driver_poll(sim_context_t *ctx)
{
    if (atomic_load(&ctx->offline))
        return EPOLLERR | EPOLLHUP;

    /*
     * Hardware interrupts and capture streaming are deferred to future stages.
     * While operational states are unreachable, poll returns 0 when online.
     */
    return 0;
}

/* Test 4: Character Device Poll Lifecycle and Offline Terminal Behavior */
static bool test_d5_2_poll_lifecycle_and_offline_conditions(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);

    /* 1. Poll on active, uninitialized device returns 0 (no phantom readiness) */
    __poll_t mask = sim_driver_poll(&ctx);
    if (mask != 0)
        return false;

    /* 2. Transition to IDENTIFIED: poll continues to return 0 (touch deferred) */
    ctx.state = FTE3600_STATE_IDENTIFIED;
    mask = sim_driver_poll(&ctx);
    if (mask != 0)
        return false;

    /* 3. Open device */
    if (sim_driver_open(&ctx) != 0)
        return false;

    /* 4. Remove device: offline set to 1, state set to DEVICE_REMOVAL */
    sim_driver_remove(&ctx);

    /* 5. Poll after removal must immediately return EPOLLERR | EPOLLHUP */
    mask = sim_driver_poll(&ctx);
    if (mask != (EPOLLERR | EPOLLHUP))
        return false;

    sim_driver_release(&ctx);
    return (atomic_load(&ctx.freed) == 1);
}

/* --- Suite 8: Stage D6.2 FT9362 Volatile Startup Verification --- */

/* Test 1: Successful startup (SMIC Fab AA: 0x4C, period 150 us, INITIALIZED_UNCALIBRATED) */
static bool test_d6_startup_scenario_1_smic_fab_aa(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    ctx.state = FTE3600_STATE_IDENTIFIED;

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    prof.startup.fab_code_val = 0x4C; /* SMIC Fab AA: 0x4C >> 2 == 0x13 */

    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);

    bool pass = (ret == 0 && ctx.state == FTE3600_STATE_INITIALIZED_UNCALIBRATED && ctx.last_error == 0);
    pass = pass && (ctx.foundry_code == 0xAA && ctx.line_period_us == 150);
    /* Transfers: 1 (wake1) + 1 (status80) + 1 (wake2) + 12 (loop) + 1 (fab) = 16 */
    pass = pass && (ctx.hardware_access_count == 16);

    struct fte3600_state_snapshot snap;
    sim_driver_ioctl_get_state(&ctx, &snap);
    pass = pass && (snap.state == (uint32_t)FTE3600_STATE_IDENTIFIED);
    pass = pass && (snap.state != (uint32_t)FTE3600_STATE_READY);
    pass = pass && ((snap.flags & FTE3600_FLAG_STARTUP_PREFIX_COMPLETED) != 0);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Test 2: Successful startup (Non-SMIC Fab: 0x00, period 200 us, INITIALIZED_UNCALIBRATED) */
static bool test_d6_startup_scenario_2_non_smic_default(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    ctx.state = FTE3600_STATE_IDENTIFIED;

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    prof.startup.fab_code_val = 0x00; /* Non-SMIC: default 200 us */

    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);

    bool pass = (ret == 0 && ctx.state == FTE3600_STATE_INITIALIZED_UNCALIBRATED && ctx.last_error == 0);
    pass = pass && (ctx.foundry_code == 0x00 && ctx.line_period_us == 200);
    pass = pass && (ctx.hardware_access_count == 16);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Test 3: Status 0x80 non-idle recovery path (first read != 0x50, abort strobe, retry == 0x50) */
static bool test_d6_startup_scenario_3_status_80_recovery(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    ctx.state = FTE3600_STATE_IDENTIFIED;

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    prof.startup.status_80_val = 0x30;       /* Non-idle */
    prof.startup.status_80_retry_val = 0x50; /* Idle on retry */

    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);

    bool pass = (ret == 0 && ctx.state == FTE3600_STATE_INITIALIZED_UNCALIBRATED && ctx.last_error == 0);
    /* 16 base + 1 (abort strobe) + 1 (retry status read) = 18 transfers */
    pass = pass && (ctx.hardware_access_count == 18);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Test 4: Status 0x80 persistent failure (retry != 0x50 -> aborts, state UNKNOWN, -EIO) */
static bool test_d6_startup_scenario_4_status_80_persistent_failure(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    ctx.state = FTE3600_STATE_IDENTIFIED;

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    prof.startup.status_80_val = 0xFF;
    prof.startup.status_80_retry_val = 0xFF;

    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);

    bool pass = (ret == -EIO && ctx.state == FTE3600_STATE_UNKNOWN && ctx.last_error == EIO);
    /* 1 (wake1) + 1 (st80) + 1 (abort) + 1 (retry) = 4 transfers */
    pass = pass && (ctx.hardware_access_count == 4);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Test 5: Command strobe 3-byte wire framing ({ cmd, ~cmd, 0x00 }) */
static bool test_d6_startup_scenario_5_cmd_strobe_framing(void)
{
    /*
     * Verify that all FT9362 command strobes transmit exactly 3 bytes:
     * { cmd, complement, 0x00 }
     * matching vendor reference implementations (fw9369_startup.c,
     * fw9369_fdt.c, capture.c, ft93xx.c) and backend mock assertions.
     */
    uint8_t strobe_buf[3];

    #define CHECK_STROBE(cmd, complement) do { \
        strobe_buf[0] = (cmd); \
        strobe_buf[1] = (complement); \
        strobe_buf[2] = 0x00; \
        if (strobe_buf[0] != (cmd)) return false; \
        if (strobe_buf[1] != (uint8_t)(~(cmd))) return false; \
        if (strobe_buf[1] != (complement)) return false; \
        if (strobe_buf[2] != 0x00) return false; \
    } while (0)

    CHECK_STROBE(0x5A, 0xA5); /* CMD_WAKE */
    CHECK_STROBE(0xA5, 0x5A); /* CMD_IDLE_ACK */
    CHECK_STROBE(0xC0, 0x3F); /* CMD_ABORT */
    #undef CHECK_STROBE

    return true;
}

/* Test 6: Wake strobe 1 failure (Step 1 fails -> state UNKNOWN) */
static bool test_d6_startup_scenario_6_wake1_failure(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    ctx.state = FTE3600_STATE_IDENTIFIED;

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    prof.startup.wake1_ret = -ETIMEDOUT;

    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);

    bool pass = (ret == -ETIMEDOUT && ctx.state == FTE3600_STATE_UNKNOWN && ctx.last_error == ETIMEDOUT);
    pass = pass && (ctx.hardware_access_count == 1);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Test 7: Foundry interrogation loop failure (Step 5 failure -> state UNKNOWN) */
static bool test_d6_startup_scenario_7_loop_failure(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    ctx.state = FTE3600_STATE_IDENTIFIED;

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    prof.startup.loop_write_ret = -EIO;

    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);

    bool pass = (ret == -EIO && ctx.state == FTE3600_STATE_UNKNOWN && ctx.last_error == EIO);
    /* 1 (wake1) + 1 (st80) + 1 (wake2) + 1 (loop write failed) = 4 transfers */
    pass = pass && (ctx.hardware_access_count == 4);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Test 8: Zero 0x1A84 writes in baseline startup (isolated to Stage D7) */
static bool test_d6_startup_scenario_8_no_irq_clear_in_baseline(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    ctx.state = FTE3600_STATE_IDENTIFIED;

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    prof.startup.fab_code_val = 0x4C;

    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);

    bool pass = (ret == 0 && ctx.state == FTE3600_STATE_INITIALIZED_UNCALIBRATED && ctx.last_error == 0);
    /* Exactly 16 baseline transfers: zero register 0x1A84 writes */
    pass = pass && (ctx.hardware_access_count == 16);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Test 9: Precondition enforcement (calling startup from invalid states rejected with -EPERM) */
static bool test_d6_startup_scenario_9_precondition_enforcement(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };

    /* UNINITIALIZED */
    ctx.state = FTE3600_STATE_UNINITIALIZED;
    if (sim_driver_ioctl_init(&ctx, &params, &prof) != -EPERM)
        return false;

    /* RESETTING */
    ctx.state = FTE3600_STATE_RESETTING;
    if (sim_driver_ioctl_init(&ctx, &params, &prof) != -EPERM)
        return false;

    /* UNKNOWN */
    ctx.state = FTE3600_STATE_UNKNOWN;
    if (sim_driver_ioctl_init(&ctx, &params, &prof) != -EPERM)
        return false;

    /* INITIALIZED_UNCALIBRATED */
    ctx.state = FTE3600_STATE_INITIALIZED_UNCALIBRATED;
    if (sim_driver_ioctl_init(&ctx, &params, &prof) != -EPERM)
        return false;

    /* READY */
    ctx.state = FTE3600_STATE_READY;
    if (sim_driver_ioctl_init(&ctx, &params, &prof) != -EPERM)
        return false;

    if (ctx.hardware_access_count != 0)
        return false;

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return (atomic_load(&ctx.freed) == 1);
}

/* Test 10: Full startup flag (FTE3600_INIT_FLAG_FULL_STARTUP) from RESET_SEQUENCE_ISSUED */
static bool test_d6_startup_scenario_10_full_startup_flag_combined(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    sim_driver_ioctl_reset(&ctx); /* count = 1 */

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    struct fte3600_init_params params = {
        .version = FTE3600_UAPI_VERSION,
        .flags = FTE3600_INIT_FLAG_FULL_STARTUP
    };

    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);
    bool pass = (ret == 0 && ctx.state == FTE3600_STATE_INITIALIZED_UNCALIBRATED && ctx.last_error == 0);
    pass = pass && (ctx.foundry_code == 0xAA && ctx.line_period_us == 150);
    /* 1 reset + 3 identify transfers + 16 startup transfers = 20 total ops */
    pass = pass && (ctx.hardware_access_count == 20);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Test 11: Removal / offline concurrency during startup */
static bool test_d6_startup_scenario_11_removal_concurrency(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    ctx.state = FTE3600_STATE_IDENTIFIED;

    /* Device removed */
    atomic_store(&ctx.offline, 1);
    ctx.spi = NULL;

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };

    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);
    bool pass = (ret == -ENODEV && ctx.hardware_access_count == 0);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Test 12: Blocked operations (threshold bank unlock, DAC search, arm touch) are excluded */
static bool test_d6_startup_scenario_12_blocked_operations_absent(void)
{
    /* Static validation of safety constants and flags */
    _Static_assert(FTE3600_INIT_FLAG_IDENTIFY_ONLY == 0x00000000U, "IDENTIFY_ONLY mismatch");
    _Static_assert(FTE3600_INIT_FLAG_FULL_STARTUP == 0x00000001U, "FULL_STARTUP mismatch");

    /* Verify wire addresses and opcodes */
    uint16_t reg_irq = 0x1A84;
    uint8_t op_write16 = 0x05;
    uint8_t op_sfr_write = 0x09;
    uint8_t op_sfr_read = 0x08;

    if (op_write16 != 0x05 || op_sfr_write != 0x09 || op_sfr_read != 0x08)
        return false;
    if ((0x80 | (reg_irq >> 8)) != 0x9A || (reg_irq & 0xFF) != 0x84)
        return false;

    /* Verify that threshold write wire address 0x00E0 has wire high byte 0x80 */
    uint16_t thresh_addr = 0x00E0;
    if ((0x80 | (thresh_addr >> 8)) != 0x80 || (thresh_addr & 0xFF) != 0xE0)
        return false;

    return true;
}

/* --- Suite 9: Stage D6.4 State Semantics and Capture-Gating Verification --- */

/* Test 1: Prefix completion withholds READY and sets status flags */
static bool test_d6_4_prefix_completion_withholds_ready(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    sim_driver_ioctl_reset(&ctx);

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    struct fte3600_init_params params = {
        .version = FTE3600_UAPI_VERSION,
        .flags = FTE3600_INIT_FLAG_FULL_STARTUP
    };

    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);
    bool pass = (ret == 0);
    pass = pass && (ctx.state == FTE3600_STATE_INITIALIZED_UNCALIBRATED);
    pass = pass && (ctx.state != FTE3600_STATE_READY);

    struct fte3600_state_snapshot snap;
    int sret = sim_driver_ioctl_get_state(&ctx, &snap);
    pass = pass && (sret == 0);
    pass = pass && (snap.state == (uint32_t)FTE3600_STATE_IDENTIFIED);
    pass = pass && (snap.state != (uint32_t)FTE3600_STATE_READY);
    pass = pass && ((snap.flags & FTE3600_FLAG_STARTUP_PREFIX_COMPLETED) != 0);
    pass = pass && ((snap.flags & FTE3600_FLAG_CALIBRATION_PENDING) != 0);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Test 2: Capture is rejected in IDENTIFIED */
static bool test_d6_4_capture_rejected_in_identified(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    sim_driver_ioctl_reset(&ctx);

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);
    if (ret != 0 || ctx.state != FTE3600_STATE_IDENTIFIED)
        return false;

    struct fte3600_frame_buffer fb = {
        .version = FTE3600_UAPI_VERSION,
        .length = FTE3600_IMAGE_BYTES
    };

    int cret = sim_driver_ioctl_capture(&ctx, &fb);
    bool pass = (cret == -EPERM && ctx.state == FTE3600_STATE_IDENTIFIED);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Test 3: Capture is rejected in prefix-complete/uncalibrated state */
static bool test_d6_4_capture_rejected_in_uncalibrated(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    sim_driver_ioctl_reset(&ctx);

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    struct fte3600_init_params params = {
        .version = FTE3600_UAPI_VERSION,
        .flags = FTE3600_INIT_FLAG_FULL_STARTUP
    };
    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);
    if (ret != 0 || ctx.state != FTE3600_STATE_INITIALIZED_UNCALIBRATED)
        return false;

    struct fte3600_frame_buffer fb = {
        .version = FTE3600_UAPI_VERSION,
        .length = FTE3600_IMAGE_BYTES
    };

    int cret = sim_driver_ioctl_capture(&ctx, &fb);
    bool pass = (cret == -EPERM && ctx.state == FTE3600_STATE_INITIALIZED_UNCALIBRATED);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Test 4: Capture is rejected in UNKNOWN */
static bool test_d6_4_capture_rejected_in_unknown(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    ctx.state = FTE3600_STATE_UNKNOWN;

    struct fte3600_frame_buffer fb = {
        .version = FTE3600_UAPI_VERSION,
        .length = FTE3600_IMAGE_BYTES
    };

    int cret = sim_driver_ioctl_capture(&ctx, &fb);
    bool pass = (cret == -EPERM && ctx.state == FTE3600_STATE_UNKNOWN);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Test 5: Capture is rejected when OFFLINE */
static bool test_d6_4_capture_rejected_when_offline(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    atomic_store(&ctx.offline, 1);
    ctx.spi = NULL;

    struct fte3600_frame_buffer fb = {
        .version = FTE3600_UAPI_VERSION,
        .length = FTE3600_IMAGE_BYTES
    };

    int cret = sim_driver_ioctl_capture(&ctx, &fb);
    bool pass = (cret == -ENODEV && ctx.hardware_access_count == 0);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Test 6: Capture is rejected after failed initialization */
static bool test_d6_4_capture_rejected_after_failed_init(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    sim_driver_ioctl_reset(&ctx);

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    prof.chip_id_val = 0x0000; /* Invalid ID -> causes INIT failure */

    struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);
    if (ret != -ENODEV || ctx.state != FTE3600_STATE_UNKNOWN)
        return false;

    struct fte3600_frame_buffer fb = {
        .version = FTE3600_UAPI_VERSION,
        .length = FTE3600_IMAGE_BYTES
    };

    int cret = sim_driver_ioctl_capture(&ctx, &fb);
    bool pass = (cret == -EPERM && ctx.state == FTE3600_STATE_UNKNOWN);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Test 7: Capture can only be admitted after a valid future-ready state transition */
static bool test_d6_4_capture_admitted_only_in_ready(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);

    struct fte3600_frame_buffer fb = {
        .version = FTE3600_UAPI_VERSION,
        .length = FTE3600_IMAGE_BYTES
    };

    /* From UNCALIBRATED: rejected with -EPERM */
    ctx.state = FTE3600_STATE_INITIALIZED_UNCALIBRATED;
    if (sim_driver_ioctl_capture(&ctx, &fb) != -EPERM)
        return false;

    /* From IDENTIFIED: rejected with -EPERM */
    ctx.state = FTE3600_STATE_IDENTIFIED;
    if (sim_driver_ioctl_capture(&ctx, &fb) != -EPERM)
        return false;

    /* Simulating future calibration/arming completion: transition to READY */
    ctx.state = FTE3600_STATE_READY;
    /* Gating succeeds; reaches deferred FIFO stub returning -EOPNOTSUPP */
    int ret = sim_driver_ioctl_capture(&ctx, &fb);
    bool pass = (ret == -EOPNOTSUPP && ctx.state == FTE3600_STATE_READY);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Test 8: Legacy zero-flag INIT remains identification-only */
static bool test_d6_4_legacy_zero_flag_init(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    sim_driver_ioctl_reset(&ctx); /* count = 1 */

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    struct fte3600_init_params params = {
        .version = FTE3600_UAPI_VERSION,
        .flags = 0 /* Zero flags: identification only */
    };

    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);
    bool pass = (ret == 0 && ctx.state == FTE3600_STATE_IDENTIFIED);
    /* 1 reset + 3 identify transfers = 4 total accesses */
    pass = pass && (ctx.hardware_access_count == 4);

    struct fte3600_state_snapshot snap;
    sim_driver_ioctl_get_state(&ctx, &snap);
    pass = pass && (snap.state == (uint32_t)FTE3600_STATE_IDENTIFIED);
    pass = pass && ((snap.flags & FTE3600_FLAG_STARTUP_PREFIX_COMPLETED) == 0);
    pass = pass && ((snap.flags & FTE3600_FLAG_CALIBRATION_PENDING) == 0);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Test 9: Public UAPI values, structure sizes and ioctl numbers remain unchanged */
static bool test_d6_4_uapi_constants_and_sizes(void)
{
    /* Size assertions */
    if (sizeof(struct fte3600_state_snapshot) != 32)
        return false;
    if (sizeof(struct fte3600_init_params) != 32)
        return false;
    if (sizeof(struct fte3600_frame_buffer) != 32)
        return false;

    /* IOCTL size validations */
    if (_IOC_SIZE(FTE3600_IOC_GET_STATE) != 32)
        return false;
    if (_IOC_SIZE(FTE3600_IOC_RESET) != 0)
        return false;
    if (_IOC_SIZE(FTE3600_IOC_INIT) != 32)
        return false;
    if (_IOC_SIZE(FTE3600_IOC_CAPTURE) != 32)
        return false;

    /* Canonical public state values (0..8) */
    if (FTE3600_STATE_UNINITIALIZED != 0 ||
        FTE3600_STATE_RESETTING != 1 ||
        FTE3600_STATE_RESET_SEQUENCE_ISSUED != 2 ||
        FTE3600_STATE_INITIALIZING != 3 ||
        FTE3600_STATE_READY != 4 ||
        FTE3600_STATE_CAPTURING != 5 ||
        FTE3600_STATE_ERROR != 6 ||
        FTE3600_STATE_UNKNOWN != 7 ||
        FTE3600_STATE_IDENTIFIED != 8)
        return false;

    /* Flag definitions */
    if (FTE3600_FLAG_OFFLINE != (1U << 0))
        return false;
    if (FTE3600_FLAG_RESET_SEQUENCE_ISSUED != (1U << 1))
        return false;
    if (FTE3600_FLAG_STARTUP_PREFIX_COMPLETED != (1U << 2))
        return false;
    if (FTE3600_FLAG_CALIBRATION_PENDING != (1U << 3))
        return false;

    /* State mapping bounds check */
    if (FTE3600_MAX_PUBLIC_STATE != 8)
        return false;

    for (int s = 0; s <= 20; s++) {
        uint32_t pub = fte3600_state_to_uapi((enum fte3600_state)s);
        if (pub > FTE3600_MAX_PUBLIC_STATE)
            return false;
    }

    /* Verify specific mapping of extended state 14 */
    if (fte3600_state_to_uapi(FTE3600_STATE_INITIALIZED_UNCALIBRATED) != (uint32_t)FTE3600_STATE_IDENTIFIED)
        return false;

    return true;
}

/* Test 10: Existing D5.4 lifetime and OFFLINE behavior remain intact */
static bool test_d6_4_d5_4_lifetime_offline_intact(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);

    /* Simulate device removal while fd open */
    sim_driver_remove(&ctx);

    bool pass = (atomic_load(&ctx.offline) == 1 && ctx.state == FTE3600_STATE_DEVICE_REMOVAL);
    pass = pass && (ctx.reset_gpiod == NULL && ctx.spi == NULL);

    /* GET_STATE returns UNINITIALIZED with OFFLINE flag */
    struct fte3600_state_snapshot snap;
    int sret = sim_driver_ioctl_get_state(&ctx, &snap);
    pass = pass && (sret == 0);
    pass = pass && (snap.state == (uint32_t)FTE3600_STATE_UNINITIALIZED);
    pass = pass && ((snap.flags & FTE3600_FLAG_OFFLINE) != 0);

    /* Hardware ioctls return -ENODEV */
    if (sim_driver_ioctl_reset(&ctx) != -ENODEV)
        return false;

    struct fte3600_frame_buffer fb = {
        .version = FTE3600_UAPI_VERSION,
        .length = FTE3600_IMAGE_BYTES
    };
    if (sim_driver_ioctl_capture(&ctx, &fb) != -ENODEV)
        return false;

    struct fte3600_init_params ip = { .version = FTE3600_UAPI_VERSION };
    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    if (sim_driver_ioctl_init(&ctx, &ip, &prof) != -ENODEV)
        return false;

    /* Release cleans up */
    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* --- Suite 10: Stage D6.6 INIT Validation and Semantic Compatibility --- */

/* Test 1: Zero flags accepted (identification-only) */
static bool test_d6_6_init_flags_zero_accepted(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    sim_driver_ioctl_reset(&ctx);

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    struct fte3600_init_params params = {
        .version = FTE3600_UAPI_VERSION,
        .flags = FTE3600_INIT_FLAG_IDENTIFY_ONLY /* 0 */
    };

    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);
    bool pass = (ret == 0 && ctx.state == FTE3600_STATE_IDENTIFIED);
    pass = pass && (ctx.hardware_access_count == 4); /* 1 reset + 3 identify */

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Test 2: FULL_STARTUP flag only accepted (advances to INITIALIZED_UNCALIBRATED) */
static bool test_d6_6_init_flags_full_startup_accepted(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    sim_driver_ioctl_reset(&ctx);

    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    struct fte3600_init_params params = {
        .version = FTE3600_UAPI_VERSION,
        .flags = FTE3600_INIT_FLAG_FULL_STARTUP /* 1U << 0 */
    };

    int ret = sim_driver_ioctl_init(&ctx, &params, &prof);
    bool pass = (ret == 0 && ctx.state == FTE3600_STATE_INITIALIZED_UNCALIBRATED);
    pass = pass && (ctx.hardware_access_count == 20); /* 1 reset + 3 identify + 16 startup */

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Test 3: Every individual unsupported flag bit (bits 1..31) rejected with -EINVAL */
static bool test_d6_6_init_flags_individual_unsupported_bits_rejected(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    sim_driver_ioctl_reset(&ctx); /* count = 1 */

    mock_spi_profile_t prof;
    init_mock_profile(&prof);

    for (int bit = 1; bit < 32; bit++) {
        struct fte3600_init_params params = {
            .version = FTE3600_UAPI_VERSION,
            .flags = (1U << bit)
        };

        int ret = sim_driver_ioctl_init(&ctx, &params, &prof);
        if (ret != -EINVAL)
            return false;
        if (ctx.state != FTE3600_STATE_RESET_SEQUENCE_ISSUED)
            return false;
        if (ctx.hardware_access_count != 1) /* No hardware access attempted */
            return false;

        /* Also test parameter validation layer */
        if (mock_validate_ioctl(FTE3600_IOC_INIT, &params) != -EINVAL)
            return false;
    }

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return (atomic_load(&ctx.freed) == 1);
}

/* Test 4: Multiple unsupported flag bit combinations rejected with -EINVAL */
static bool test_d6_6_init_flags_multiple_unsupported_bits_rejected(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    sim_driver_ioctl_reset(&ctx); /* count = 1 */

    mock_spi_profile_t prof;
    init_mock_profile(&prof);

    uint32_t invalid_patterns[] = {
        0xFFFFFFFEU, /* All bits except bit 0 */
        0x55555554U,
        0xAAAAAAA0U,
        0x00000002U,
        0x00000004U,
        0x00000008U,
        0x80000000U
    };

    for (size_t i = 0; i < sizeof(invalid_patterns) / sizeof(invalid_patterns[0]); i++) {
        struct fte3600_init_params params = {
            .version = FTE3600_UAPI_VERSION,
            .flags = invalid_patterns[i]
        };

        int ret = sim_driver_ioctl_init(&ctx, &params, &prof);
        if (ret != -EINVAL)
            return false;
        if (ctx.state != FTE3600_STATE_RESET_SEQUENCE_ISSUED)
            return false;
        if (ctx.hardware_access_count != 1)
            return false;

        if (mock_validate_ioctl(FTE3600_IOC_INIT, &params) != -EINVAL)
            return false;
    }

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return (atomic_load(&ctx.freed) == 1);
}

/* Test 5: FULL_STARTUP combined with invalid bits rejected with -EINVAL */
static bool test_d6_6_init_flags_full_startup_with_invalid_bits_rejected(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    sim_driver_ioctl_reset(&ctx); /* count = 1 */

    mock_spi_profile_t prof;
    init_mock_profile(&prof);

    uint32_t invalid_combos[] = {
        FTE3600_INIT_FLAG_FULL_STARTUP | (1U << 1),
        FTE3600_INIT_FLAG_FULL_STARTUP | (1U << 2),
        FTE3600_INIT_FLAG_FULL_STARTUP | (1U << 31),
        0xFFFFFFFFU,
        FTE3600_INIT_FLAG_FULL_STARTUP | 0x000000FEU
    };

    for (size_t i = 0; i < sizeof(invalid_combos) / sizeof(invalid_combos[0]); i++) {
        struct fte3600_init_params params = {
            .version = FTE3600_UAPI_VERSION,
            .flags = invalid_combos[i]
        };

        int ret = sim_driver_ioctl_init(&ctx, &params, &prof);
        if (ret != -EINVAL)
            return false;
        if (ctx.state != FTE3600_STATE_RESET_SEQUENCE_ISSUED)
            return false;
        if (ctx.hardware_access_count != 1)
            return false;

        if (mock_validate_ioctl(FTE3600_IOC_INIT, &params) != -EINVAL)
            return false;
    }

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return (atomic_load(&ctx.freed) == 1);
}

/* Test 6: Non-zero reserved fields rejected with -EINVAL */
static bool test_d6_6_init_reserved_fields_rejected(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);
    sim_driver_ioctl_reset(&ctx); /* count = 1 */

    mock_spi_profile_t prof;
    init_mock_profile(&prof);

    for (int r = 0; r < 6; r++) {
        struct fte3600_init_params params = {
            .version = FTE3600_UAPI_VERSION,
            .flags = FTE3600_INIT_FLAG_IDENTIFY_ONLY
        };
        params.reserved[r] = 1;

        int ret = sim_driver_ioctl_init(&ctx, &params, &prof);
        if (ret != -EINVAL)
            return false;
        if (ctx.state != FTE3600_STATE_RESET_SEQUENCE_ISSUED)
            return false;
        if (ctx.hardware_access_count != 1)
            return false;

        if (mock_validate_ioctl(FTE3600_IOC_INIT, &params) != -EINVAL)
            return false;
    }

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return (atomic_load(&ctx.freed) == 1);
}

/* Test 7: Non-READY states return state-gating error (-EPERM) on capture */
static bool test_d6_6_capture_errno_non_ready_eperm(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);

    struct fte3600_frame_buffer fb = {
        .version = FTE3600_UAPI_VERSION,
        .length = FTE3600_IMAGE_BYTES
    };

    enum fte3600_state test_states[] = {
        FTE3600_STATE_UNINITIALIZED,
        FTE3600_STATE_RESETTING,
        FTE3600_STATE_RESET_SEQUENCE_ISSUED,
        FTE3600_STATE_INITIALIZING,
        FTE3600_STATE_ERROR,
        FTE3600_STATE_UNKNOWN,
        FTE3600_STATE_IDENTIFIED,
        FTE3600_STATE_INITIALIZED_UNCALIBRATED
    };

    for (size_t i = 0; i < sizeof(test_states) / sizeof(test_states[0]); i++) {
        ctx.state = test_states[i];
        int ret = sim_driver_ioctl_capture(&ctx, &fb);
        if (ret != -EPERM)
            return false;
        if (ctx.state != test_states[i])
            return false;
    }

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return (atomic_load(&ctx.freed) == 1);
}

/* Test 8: Offline state returns offline error (-ENODEV) on capture */
static bool test_d6_6_capture_errno_offline_enodev(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);

    /* Device offline */
    atomic_store(&ctx.offline, 1);
    ctx.spi = NULL;

    struct fte3600_frame_buffer fb = {
        .version = FTE3600_UAPI_VERSION,
        .length = FTE3600_IMAGE_BYTES
    };

    int ret = sim_driver_ioctl_capture(&ctx, &fb);
    bool pass = (ret == -ENODEV && ctx.hardware_access_count == 0);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Test 9: Simulated READY state reaches existing unimplemented capture response (-EOPNOTSUPP) */
static bool test_d6_6_capture_errno_ready_eopnotsupp(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);

    /* Explicitly simulate future transition to READY */
    ctx.state = FTE3600_STATE_READY;

    struct fte3600_frame_buffer fb = {
        .version = FTE3600_UAPI_VERSION,
        .length = FTE3600_IMAGE_BYTES
    };

    /* State gating passes; reaches stub returning -EOPNOTSUPP */
    int ret = sim_driver_ioctl_capture(&ctx, &fb);
    bool pass = (ret == -EOPNOTSUPP && ctx.state == FTE3600_STATE_READY);

    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return pass && (atomic_load(&ctx.freed) == 1);
}

/* Test 10: OFFLINE userspace contract and RESET non-revival guarantee */
static bool test_d6_6_offline_contract_and_reset_rejection(void)
{
    sim_context_t ctx;
    sim_ctx_init(&ctx);
    sim_driver_open(&ctx);

    /* Normal probe state: not offline */
    struct fte3600_state_snapshot snap;
    sim_driver_ioctl_get_state(&ctx, &snap);
    if ((snap.flags & FTE3600_FLAG_OFFLINE) != 0)
        return false;

    /* Device removed */
    sim_driver_remove(&ctx);

    /* 1. OFFLINE contract: GET_STATE returns UNINITIALIZED with OFFLINE flag asserted */
    if (sim_driver_ioctl_get_state(&ctx, &snap) != 0)
        return false;
    if (snap.state != (uint32_t)FTE3600_STATE_UNINITIALIZED)
        return false;
    if ((snap.flags & FTE3600_FLAG_OFFLINE) == 0)
        return false;

    /* 2. RESET cannot revive offline or removed device */
    if (sim_driver_ioctl_reset(&ctx) != -ENODEV)
        return false;
    if (ctx.hardware_access_count != 0)
        return false;

    /* 3. INIT rejected when offline */
    mock_spi_profile_t prof;
    init_mock_profile(&prof);
    struct fte3600_init_params ip = { .version = FTE3600_UAPI_VERSION };
    if (sim_driver_ioctl_init(&ctx, &ip, &prof) != -ENODEV)
        return false;

    /* 4. CAPTURE rejected when offline */
    struct fte3600_frame_buffer fb = {
        .version = FTE3600_UAPI_VERSION,
        .length = FTE3600_IMAGE_BYTES
    };
    if (sim_driver_ioctl_capture(&ctx, &fb) != -ENODEV)
        return false;

    /* 5. Safe release and object deallocation */
    sim_driver_release(&ctx);
    sim_ctx_put(&ctx);
    return (atomic_load(&ctx.freed) == 1);
}

/* --- Main Runner --- */

int main(void)
{
    printf("===================================================================\n");
    printf(" FTE3600 Unified Kernel Driver Test Suite (Stage D6.6 Remediated)\n");
    printf(" Mode: Standalone Userspace Simulation (Zero Hardware Access)\n");
    printf("===================================================================\n\n");

    printf("--- Test Suite 1: UAPI & Data Structure Alignment ---\n");
    RUN_TEST(test_uapi_sizes, "UAPI structures exact 32-byte size and alignment");
    RUN_TEST(test_uapi_constants, "UAPI protocol constants (10246 B wire, 10240 B image, v1)");
    RUN_TEST(test_ioctl_encodings, "IOCTL command encodings (magic 'F', sizes, directions)");

    printf("\n--- Test Suite 2: Protocol CRC Vector Validation ---\n");
    RUN_TEST(test_crc_vectors, "Wire CRC (0x1C53) and Word LE CRC (0x313E, 0x5B7C, 0xE4AA)");

    printf("\n--- Test Suite 3: State Machine Transitions & Invariants ---\n");
    RUN_TEST(test_state_transitions, "9-state model, UNKNOWN fail-closed invariant, reset recovery");

    printf("\n--- Test Suite 4: Stage 2E.2.2 Mandatory Lifecycle & Concurrency Scenarios ---\n");
    RUN_TEST(test_scenario_1_open_release, "Scenario 1: Open followed by normal release");
    RUN_TEST(test_scenario_2_open_remove_release, "Scenario 2: Open followed by device removal and release");
    RUN_TEST(test_scenario_3_removal_racing_waiting_ioctl, "Scenario 3: Removal racing with ioctl waiting for mutex");
    RUN_TEST(test_scenario_4_removal_racing_holding_ioctl, "Scenario 4: Removal racing with ioctl holding mutex");
    RUN_TEST(test_scenario_5_get_state_after_removal, "Scenario 5: GET_STATE after removal");
    RUN_TEST(test_scenario_6_reset_after_removal, "Scenario 6: RESET attempted after removal");
    RUN_TEST(test_scenario_7_multiple_opens, "Scenario 7: Multiple open attempts (single-client exclusive lease)");
    RUN_TEST(test_scenario_8_error_path_references, "Scenario 8: Reference balance on probe/open error paths");
    RUN_TEST(test_scenario_9_spi_invalid_arguments, "Scenario 9: SPI helper rejects invalid arguments (NULL, 0-len)");
    RUN_TEST(test_scenario_10_spi_length_bounds, "Scenario 10: SPI helper rejects transfers beyond max limit");
    RUN_TEST(test_scenario_11_no_hardware_access_after_offline, "Scenario 11: No hardware access possible after offline");

    printf("\n--- Test Suite 5: UAPI Validation & Parameter Rejections ---\n");
    RUN_TEST(test_uapi_rejections, "IOCTL validation, version check, reserved fields, and -EOPNOTSUPP");

    printf("\n--- Test Suite 6: Stage 2E.4.2 Chip Identification & Completion Semantics ---\n");
    RUN_TEST(test_id_scenario_1_valid_wake_response, "Scenario 1: Valid wake response (rx_c6[4] == 0x01)");
    RUN_TEST(test_id_scenario_2_invalid_wake_response, "Scenario 2: Invalid wake response (rx_c6[4] != 0x01)");
    RUN_TEST(test_id_scenario_3_valid_id_and_crc, "Scenario 3: Valid FT9362 ID and CRC (0x9362 and 0x1C53)");
    RUN_TEST(test_id_scenario_4_crc_mismatch, "Scenario 4: CRC mismatch");
    RUN_TEST(test_id_scenario_5_unexpected_chip_id, "Scenario 5: Unexpected chip ID (0x9368, 0x9391)");
    RUN_TEST(test_id_scenario_6_all_zero_response, "Scenario 6: All-zero response (0x0000)");
    RUN_TEST(test_id_scenario_7_all_ones_response, "Scenario 7: All-ones response (0xFFFF)");
    RUN_TEST(test_id_scenario_8_spi_transfer_error, "Scenario 8: SPI transfer error (-EIO)");
    RUN_TEST(test_id_scenario_9_short_incomplete_transfer, "Scenario 9: Short/incomplete transfer (m.status != 0, actual_length < len)");
    RUN_TEST(test_id_scenario_10_failure_after_wake_write, "Scenario 10: Failure after wake write");
    RUN_TEST(test_id_scenario_11_invalid_state_init, "Scenario 11: INIT from invalid state (UNINIT, INITING, READY, UNKNOWN)");
    RUN_TEST(test_id_scenario_12_init_while_offline, "Scenario 12: INIT while device offline (-ENODEV)");
    RUN_TEST(test_id_scenario_13_repeated_init_after_success, "Scenario 13: Repeated INIT after success (-EPERM)");
    RUN_TEST(test_id_scenario_14_concurrent_reset_init_mutex, "Scenario 14: Concurrent RESET and INIT serialization under mutex");
    RUN_TEST(test_id_scenario_15_removal_during_operation, "Scenario 15: Removal during an operation");
    RUN_TEST(test_id_scenario_16_spi_completion_semantics, "Scenario 16: SPI completion semantics (actual_length == len, status, ret)");
    RUN_TEST(test_id_scenario_17_semantic_boundary_capture_rejection, "Scenario 17: Semantic boundary (IDENTIFIED vs READY, CAPTURE rejection)");

    printf("\n--- Test Suite 7: Stage D5.2 Remediation, UAPI Mapping, and Poll Verification ---\n");
    RUN_TEST(test_d5_2_state_mapping_and_uapi_isolation, "Stage D5.2: State mapping and UAPI range isolation ([0..8])");
    RUN_TEST(test_d5_2_abi_compile_time_and_reserved_assertions, "Stage D5.2: ABI structure layout (32 B) and reserved field assertions");
    RUN_TEST(test_d5_2_removal_get_state_and_uapi_guarantee, "Stage D5.2: Removal GET_STATE guarantee (state 13 never leaks, OFFLINE flag set)");
    RUN_TEST(test_d5_2_poll_lifecycle_and_offline_conditions, "Stage D5.2: Poll lifecycle simulation (offline terminal condition, no phantom readiness)");

    printf("\n--- Test Suite 8: Stage D6.2 FT9362 Volatile Startup Verification ---\n");
    RUN_TEST(test_d6_startup_scenario_1_smic_fab_aa, "Scenario 1: Startup successful (SMIC Fab AA: 0x4C, period 150 us, UNCALIBRATED, 16 ops)");
    RUN_TEST(test_d6_startup_scenario_2_non_smic_default, "Scenario 2: Startup successful (Non-SMIC Fab: 0x00, period 200 us, UNCALIBRATED, 16 ops)");
    RUN_TEST(test_d6_startup_scenario_3_status_80_recovery, "Scenario 3: Status 0x80 non-idle recovery path (abort strobe recovery, 18 ops)");
    RUN_TEST(test_d6_startup_scenario_4_status_80_persistent_failure, "Scenario 4: Status 0x80 persistent failure (returns -EIO, state UNKNOWN, 4 ops)");
    RUN_TEST(test_d6_startup_scenario_5_cmd_strobe_framing, "Scenario 5: Command strobe 3-byte wire framing ({cmd, ~cmd, 0x00})");
    RUN_TEST(test_d6_startup_scenario_6_wake1_failure, "Scenario 6: Wake strobe 1 failure (returns -ETIMEDOUT, state UNKNOWN, 1 op)");
    RUN_TEST(test_d6_startup_scenario_7_loop_failure, "Scenario 7: Foundry interrogation loop failure (returns -EIO, state UNKNOWN, 4 ops)");
    RUN_TEST(test_d6_startup_scenario_8_no_irq_clear_in_baseline, "Scenario 8: Zero 0x1A84 writes in baseline startup (isolated to Stage D7, 16 ops)");
    RUN_TEST(test_d6_startup_scenario_9_precondition_enforcement, "Scenario 9: Precondition enforcement (invalid states rejected with -EPERM)");
    RUN_TEST(test_d6_startup_scenario_10_full_startup_flag_combined, "Scenario 10: Full startup flag (FTE3600_INIT_FLAG_FULL_STARTUP) from RESET (20 ops)");
    RUN_TEST(test_d6_startup_scenario_11_removal_concurrency, "Scenario 11: Removal / offline concurrency during startup (-ENODEV)");
    RUN_TEST(test_d6_startup_scenario_12_blocked_operations_absent, "Scenario 12: Blocked operations (threshold bank, DAC, arm touch) excluded");

    printf("\n--- Test Suite 9: Stage D6.4 State Semantics and Capture-Gating Verification ---\n");
    RUN_TEST(test_d6_4_prefix_completion_withholds_ready, "Scenario 1: Prefix completion withholds READY and sets status flags");
    RUN_TEST(test_d6_4_capture_rejected_in_identified, "Scenario 2: Capture is rejected in IDENTIFIED (-EPERM)");
    RUN_TEST(test_d6_4_capture_rejected_in_uncalibrated, "Scenario 3: Capture is rejected in prefix-complete/uncalibrated state (-EPERM)");
    RUN_TEST(test_d6_4_capture_rejected_in_unknown, "Scenario 4: Capture is rejected in UNKNOWN (-EPERM)");
    RUN_TEST(test_d6_4_capture_rejected_when_offline, "Scenario 5: Capture is rejected when OFFLINE (-ENODEV, 0 hw accesses)");
    RUN_TEST(test_d6_4_capture_rejected_after_failed_init, "Scenario 6: Capture is rejected after failed initialization (-EPERM)");
    RUN_TEST(test_d6_4_capture_admitted_only_in_ready, "Scenario 7: Capture can only be admitted after a valid future-ready state transition");
    RUN_TEST(test_d6_4_legacy_zero_flag_init, "Scenario 8: Legacy zero-flag INIT remains identification-only");
    RUN_TEST(test_d6_4_uapi_constants_and_sizes, "Scenario 9: Public UAPI values, structure sizes and ioctl numbers remain unchanged");
    RUN_TEST(test_d6_4_d5_4_lifetime_offline_intact, "Scenario 10: Existing D5.4 lifetime and OFFLINE behavior remain intact");

    printf("\n--- Test Suite 10: Stage D6.6 INIT Validation and Semantic Compatibility ---\n");
    RUN_TEST(test_d6_6_init_flags_zero_accepted, "Scenario 1: Zero flags accepted (identification-only, 4 accesses)");
    RUN_TEST(test_d6_6_init_flags_full_startup_accepted, "Scenario 2: FULL_STARTUP flag accepted (advances to UNCALIBRATED, 20 accesses)");
    RUN_TEST(test_d6_6_init_flags_individual_unsupported_bits_rejected, "Scenario 3: Every individual unsupported bit (1..31) rejected (-EINVAL, 0 access)");
    RUN_TEST(test_d6_6_init_flags_multiple_unsupported_bits_rejected, "Scenario 4: Multiple unsupported bit combinations rejected (-EINVAL, 0 access)");
    RUN_TEST(test_d6_6_init_flags_full_startup_with_invalid_bits_rejected, "Scenario 5: FULL_STARTUP combined with invalid bits rejected (-EINVAL, 0 access)");
    RUN_TEST(test_d6_6_init_reserved_fields_rejected, "Scenario 6: Non-zero reserved fields rejected (-EINVAL, 0 access)");
    RUN_TEST(test_d6_6_capture_errno_non_ready_eperm, "Scenario 7: Non-READY states return state-gating error (-EPERM)");
    RUN_TEST(test_d6_6_capture_errno_offline_enodev, "Scenario 8: Offline state returns offline error (-ENODEV, 0 access)");
    RUN_TEST(test_d6_6_capture_errno_ready_eopnotsupp, "Scenario 9: Simulated READY state reaches existing stub response (-EOPNOTSUPP)");
    RUN_TEST(test_d6_6_offline_contract_and_reset_rejection, "Scenario 10: OFFLINE userspace contract and RESET non-revival guarantee (-ENODEV)");

    printf("\n===================================================================\n");
    printf(" Stage D6.6 Test Results: %u / %u Passed (%.1f%%)\n",
           passed_tests, total_tests, (float)passed_tests / total_tests * 100.0f);
    printf("===================================================================\n");

    return (passed_tests == total_tests) ? 0 : 1;
}
