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
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
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
    snap->state = (uint32_t)ctx->state;
    if (atomic_load(&ctx->offline))
        snap->flags |= FTE3600_FLAG_OFFLINE;
    if (ctx->state == FTE3600_STATE_RESET_SEQUENCE_ISSUED)
        snap->flags |= FTE3600_FLAG_RESET_SEQUENCE_ISSUED;
    snap->last_error = ctx->last_error;

    pthread_mutex_unlock(&ctx->lock);
    return 0;
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

    /* Verified: ioctl completed reset cleanly BEFORE remove invalidated pointers */
    bool pass = (ctx.hardware_access_count == 1 && ctx.state == FTE3600_STATE_RESET_SEQUENCE_ISSUED);
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

static int sim_driver_ioctl_init(sim_context_t *ctx, const struct fte3600_init_params *params, const mock_spi_profile_t *prof)
{
    if (atomic_load(&ctx->offline))
        return -ENODEV;

    if (!params || params->version != FTE3600_UAPI_VERSION)
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

    int ret = sim_driver_hw_identify(ctx, prof);

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

/* 11. INIT from invalid state (UNINITIALIZED, INITIALIZING, READY, IDENTIFIED, UNKNOWN, RESETTING) */
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

    /* 4. From IDENTIFIED */
    ctx.state = FTE3600_STATE_IDENTIFIED;
    if (sim_driver_ioctl_init(&ctx, &params, &prof) != -EPERM || ctx.state != FTE3600_STATE_IDENTIFIED)
        return false;

    /* 5. From UNKNOWN */
    ctx.state = FTE3600_STATE_UNKNOWN;
    if (sim_driver_ioctl_init(&ctx, &params, &prof) != -EPERM || ctx.state != FTE3600_STATE_UNKNOWN)
        return false;

    /* 6. From RESETTING */
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

    /* Second INIT is rejected with -EPERM */
    int ret2 = sim_driver_ioctl_init(&ctx, &params, &prof);
    if (ret2 != -EPERM)
        return false;

    /* State must remain strictly IDENTIFIED (never transition to READY) */
    if (ctx.state != FTE3600_STATE_IDENTIFIED)
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

    /* INIT completed cleanly before removal invalidated pointers */
    bool pass = (r.init_ret == 0 && ctx.state == FTE3600_STATE_IDENTIFIED);
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

/* --- Main Runner --- */

int main(void)
{
    printf("===================================================================\n");
    printf(" FTE3600 Unified Kernel Driver Test Suite (Stage 2E.4.2 Hardened)\n");
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
    RUN_TEST(test_id_scenario_11_invalid_state_init, "Scenario 11: INIT from invalid state (UNINIT, INITING, READY, IDENTIFIED, UNKNOWN)");
    RUN_TEST(test_id_scenario_12_init_while_offline, "Scenario 12: INIT while device offline (-ENODEV)");
    RUN_TEST(test_id_scenario_13_repeated_init_after_success, "Scenario 13: Repeated INIT after success (-EPERM)");
    RUN_TEST(test_id_scenario_14_concurrent_reset_init_mutex, "Scenario 14: Concurrent RESET and INIT serialization under mutex");
    RUN_TEST(test_id_scenario_15_removal_during_operation, "Scenario 15: Removal during an operation");
    RUN_TEST(test_id_scenario_16_spi_completion_semantics, "Scenario 16: SPI completion semantics (actual_length == len, status, ret)");
    RUN_TEST(test_id_scenario_17_semantic_boundary_capture_rejection, "Scenario 17: Semantic boundary (IDENTIFIED vs READY, CAPTURE rejection)");

    printf("\n===================================================================\n");
    printf(" Stage 2E.4.2 Test Results: %u / %u Passed (%.1f%%)\n",
           passed_tests, total_tests, (float)passed_tests / total_tests * 100.0f);
    printf("===================================================================\n");

    return (passed_tests == total_tests) ? 0 : 1;
}
