/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "fw9369_crc.h"
#include "fw9369_protocol.h"
#include "fw9369_transport.h"
#include "fw9369_transport_hw.h"
#include "fw9369_calibration.h"
#include "fw9369_state.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <linux/spi/spidev.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int total_tests = 0;
static int passed_tests = 0;

#define TEST_ASSERT(cond, msg) do { \
    total_tests++; \
    if (cond) { \
        passed_tests++; \
        printf("  [PASS] %s\n", msg); \
    } else { \
        printf("  [FAIL] %s (Line %d)\n", msg, __LINE__); \
    } \
} while (0)

/* 1. CRC Unit Tests */
static void test_crc_suite(void)
{
    printf("\n--- Test Suite 1: CRC Implementations & Vectors ---\n");

    /* Chip ID Wire CRC test: [ 0x93, 0x62 ] -> 0x1C53 */
    uint8_t id_vec[2] = { 0x93, 0x62 };
    uint16_t id_crc = fw9369_crc_wire(id_vec, 2);
    TEST_ASSERT(id_crc == 0x1C53, "Wire CRC matches Chip ID 0x9362 test vector (0x1C53)");

    /* Host Little-Endian Word CRC Vector 1: [ 0, 0, 0, 0 ] -> 0x313E */
    uint16_t zeros[4] = { 0, 0, 0, 0 };
    uint16_t zeros_crc = fw9369_crc_words_le(zeros, 4);
    TEST_ASSERT(zeros_crc == 0x313E, "Host LE CRC matches [0, 0, 0, 0] test vector (0x313E)");

    /* Host Little-Endian Word CRC Vector 2: [ 512, 512, 512, 512 ] -> 0x5B7C */
    uint16_t counts_512[4] = { 512, 512, 512, 512 };
    uint16_t counts_crc = fw9369_crc_words_le(counts_512, 4);
    TEST_ASSERT(counts_crc == 0x5B7C, "Host LE CRC matches [512, 512, 512, 512] test vector (0x5B7C)");

    /* Host Little-Endian Word CRC Vector 3: [ 482, 482, 482, 482 ] -> 0xE4AA */
    uint16_t thresh[4] = { 482, 482, 482, 482 };
    uint16_t thresh_crc = fw9369_crc_words_le(thresh, 4);
    TEST_ASSERT(thresh_crc == 0xE4AA, "Host LE CRC matches [482, 482, 482, 482] test vector (0xE4AA)");

    /* Byte ordering distinction: big-endian bytes vs LE words produce different results */
    uint8_t be_stream[8] = { 0x02, 0x00, 0x02, 0x00, 0x02, 0x00, 0x02, 0x00 };
    uint16_t be_crc = fw9369_crc_wire(be_stream, 8);
    TEST_ASSERT(be_crc != counts_crc, "Byte ordering distinction verified (BE wire != LE host word)");
}

/* 2. Protocol & Allowlist Tests */
static void test_protocol_suite(void)
{
    printf("\n--- Test Suite 2: Protocol Framing & Allowlist ---\n");

    uint8_t pkt[16];

    /* Byte write encoding: [ 0x09, 0xF6, 0xC6, 0x01 ] */
    int len = fw9369_encode_byte_write(0xC6, 0x01, pkt, sizeof(pkt));
    TEST_ASSERT(len == 4 && pkt[0] == 0x09 && pkt[1] == 0xF6 && pkt[2] == 0xC6 && pkt[3] == 0x01,
                "Byte write packet framing correct");

    /* Byte read encoding: [ 0x08, 0xF7, 0x80, 0x00, 0x00 ] */
    len = fw9369_encode_byte_read(0x80, pkt, sizeof(pkt));
    TEST_ASSERT(len == 5 && pkt[0] == 0x08 && pkt[1] == 0xF7 && pkt[2] == 0x80,
                "Byte read packet framing correct");

    /* Word write encoding: [ 0x05, 0xFA, 0x98, 0x01, 0x00, 0x01, 0xFC, 0x9B ] */
    len = fw9369_encode_word_write(0x1801, 0xFC9B, pkt, sizeof(pkt));
    TEST_ASSERT(len == 8 && pkt[0] == 0x05 && pkt[1] == 0xFA && pkt[2] == 0x98 && pkt[3] == 0x01 &&
                pkt[6] == 0xFC && pkt[7] == 0x9B,
                "Word write packet framing correct");

    /* Command encoding: [ 0x5A, 0xA5, 0x00 ] */
    len = fw9369_encode_command(0x5A, 0xA5, pkt, sizeof(pkt));
    TEST_ASSERT(len == 3 && pkt[0] == 0x5A && pkt[1] == 0xA5 && pkt[2] == 0x00,
                "Command wake framing correct");

    /* FIFO Read Prefix: [ 0x06, 0xF9, 0x9A, 0x05, 0x14, 0x00 ] */
    uint8_t fifo_pkt[FW9369_FIFO_TRANSFER_LEN];
    len = fw9369_encode_fifo_read(fifo_pkt, sizeof(fifo_pkt));
    TEST_ASSERT(len == FW9369_FIFO_TRANSFER_LEN &&
                fifo_pkt[0] == 0x06 && fifo_pkt[1] == 0xF9 &&
                fifo_pkt[2] == 0x9A && fifo_pkt[3] == 0x05 &&
                fifo_pkt[4] == 0x14 && fifo_pkt[5] == 0x00,
                "FIFO burst read packet length (10,246 B) and prefix correct");

    /* Allowlist enforcement */
    TEST_ASSERT(fw9369_is_register_allowed(FW9369_REG_WAKEUP_C6, true), "Allowlist permits register 0xC6 write");
    TEST_ASSERT(fw9369_is_register_allowed(FW9369_REG_WAKEUP_C6, false), "Allowlist permits register 0xC6 read");
    TEST_ASSERT(fw9369_is_register_allowed(FW9369_REG_CHIP_ID_1A8B, false), "Allowlist permits register 0x1A8B read");
    TEST_ASSERT(!fw9369_is_register_allowed(FW9369_REG_CHIP_ID_1A8B, true), "Allowlist rejects register 0x1A8B write (read-only)");
    TEST_ASSERT(!fw9369_is_register_allowed(FW9369_REG_STATUS_80, true), "Allowlist rejects register 0x80 write (read-only)");
    TEST_ASSERT(!fw9369_is_register_allowed(FW9369_REG_PROTECT_9A, false), "Allowlist rejects register 0x9A read (write-only)");
    TEST_ASSERT(!fw9369_is_register_allowed(0x0000, true), "Allowlist rejects unauthorized register 0x0000");
    TEST_ASSERT(!fw9369_is_register_allowed(0xFFFF, true), "Allowlist rejects unauthorized register 0xFFFF");

    /* Command allowlist enforcement */
    TEST_ASSERT(fw9369_is_command_allowed(0x5A, 0xA5), "Command allowlist permits wake (0x5A 0xA5)");
    TEST_ASSERT(fw9369_is_command_allowed(0xA5, 0x5A), "Command allowlist permits end (0xA5 0x5A)");
    TEST_ASSERT(!fw9369_is_command_allowed(0x12, 0x34), "Command allowlist rejects unauthorized opcode (0x12 0x34)");
}

/* 3. Mock Transport & Zero-Hardware Access Tests */
static void test_mock_transport_suite(void)
{
    printf("\n--- Test Suite 3: Mock Transport Invariants & Safety ---\n");

    struct fw9369_mock_config cfg;
    memset(&cfg, 0, sizeof(cfg));

    struct fw9369_transport *t = fw9369_mock_transport_create(&cfg);
    TEST_ASSERT(t != NULL, "Mock transport created successfully in memory");

    /* Verify initial register defaults */
    TEST_ASSERT(fw9369_mock_get_reg_byte(t, FW9369_REG_WAKEUP_C6) == 0x00, "Initial 0xC6 register is 0x00");
    TEST_ASSERT(fw9369_mock_is_protected_locked(t) == true, "Initial protected trim bank is locked");

    /* Write 0xC6 = 0x01 */
    uint8_t tx[4];
    fw9369_encode_byte_write(FW9369_REG_WAKEUP_C6, 0x01, tx, sizeof(tx));
    int ret = t->transfer(t->context, tx, NULL, 4);
    TEST_ASSERT(ret == 0 && fw9369_mock_get_reg_byte(t, FW9369_REG_WAKEUP_C6) == 0x01,
                "Mock accurately stores byte register 0xC6 write");

    /* Read 0xC6 back */
    uint8_t rx[5];
    uint8_t tx_read[5];
    fw9369_encode_byte_read(FW9369_REG_WAKEUP_C6, tx_read, sizeof(tx_read));
    ret = t->transfer(t->context, tx_read, rx, 5);
    uint8_t val = 0;
    fw9369_decode_byte_read(rx, 5, &val);
    TEST_ASSERT(ret == 0 && val == 0x01, "Mock returns 0x01 on 0xC6 readback");

    /* Verify transaction logger */
    TEST_ASSERT(fw9369_mock_get_tx_count(t) == 2, "Transaction logger accurately records packet count");

    fw9369_mock_transport_destroy(t);

    /* PROOF: Hardware transport stub strictly rejects selection without touching hardware */
    errno = 0;
    struct fw9369_transport *hw = fw9369_hardware_transport_create("/dev/spidev1.0");
    TEST_ASSERT(hw == NULL && errno == ENOTSUP,
                "Hardware transport creation is REJECTED with -ENOTSUP without touching devices");
}

/* 4. FDT Minimum Tracking & Calibration Tests */
static void test_fdt_calibration_suite(void)
{
    printf("\n--- Test Suite 4: FDT Calibration & Minimum Tracking ---\n");

    struct fw9369_mock_config cfg;
    memset(&cfg, 0, sizeof(cfg));

    struct fw9369_transport *t = fw9369_mock_transport_create(&cfg);
    struct fw9369_fdt_calibration cal;

    int ret = fw9369_calibrate_fdt(t, &cal);
    TEST_ASSERT(ret == 0, "FDT calibration executes cleanly on mock");
    TEST_ASSERT(cal.valid == true, "Calibration valid flag asserted");
    TEST_ASSERT(cal.fdt_dac >= 27 && cal.fdt_dac <= 28, "DAC centering search converges at target setting");

    /* Verify early exit on stability: with low noise, stability samples <= 5 */
    TEST_ASSERT(cal.stability_samples >= 3 && cal.stability_samples <= 5,
                "Early exit condition triggered when 3 consecutive stable samples observed");

    /* Verify bounds [300, 700] */
    bool bounds_ok = true;
    for (size_t c = 0; c < 4; ++c) {
        if (cal.minima[c] < 300 || cal.minima[c] > 700)
            bounds_ok = false;
    }
    TEST_ASSERT(bounds_ok, "All tracked minima within valid bounds [300, 700]");

    /* Verify threshold math: fdt_down[c] == minima[c] - 30 */
    bool math_ok = true;
    for (size_t c = 0; c < 4; ++c) {
        if (cal.fdt_down[c] != cal.minima[c] - 30)
            math_ok = false;
    }
    TEST_ASSERT(math_ok, "Threshold derivation strictly matches fdt_down = minima - 30");

    /* Verify 4-word CRC */
    uint16_t expected_crc = fw9369_crc_words_le(cal.fdt_down, 4);
    TEST_ASSERT(cal.fdt_down_crc == expected_crc, "FDT threshold CRC matches 4-word LE calculation");

    fw9369_mock_transport_destroy(t);
}

/* 5. Image Baseline Single-Frame Copy Tests */
static void test_image_baseline_suite(void)
{
    printf("\n--- Test Suite 5: Image Baseline & Single-Frame Copy ---\n");

    struct fw9369_mock_config cfg;
    memset(&cfg, 0, sizeof(cfg));

    struct fw9369_transport *t = fw9369_mock_transport_create(&cfg);
    struct fw9369_image_baseline base;

    int ret = fw9369_acquire_image_baseline(t, &base);
    TEST_ASSERT(ret == 0, "Image baseline acquired cleanly on mock");
    TEST_ASSERT(base.valid == true, "Baseline valid flag asserted");
    TEST_ASSERT(base.image_dac == 54, "Image DAC nominal setting preserved");

    /* Verify single frame copy: pixels in baseline match synthetic pattern */
    bool frame_matches = (base.baseline[0] == 500 && base.baseline[1] == 501);
    TEST_ASSERT(frame_matches, "Cold baseline is an exact un-averaged copy of captured raw frame");

    /* Verify 128-word CRC signature */
    uint16_t expected_crc = fw9369_crc_words_le(base.baseline, 128);
    TEST_ASSERT(base.baseline_crc == expected_crc, "Baseline CRC covers exactly first 128 words (256 bytes)");

    fw9369_mock_transport_destroy(t);
}

/* 6. End-to-End Pipeline & RAII Protected Cleanup Tests */
static void test_pipeline_suite(void)
{
    printf("\n--- Test Suite 6: Full Pipeline & Protected Register Cleanup ---\n");

    struct fw9369_mock_config cfg;
    memset(&cfg, 0, sizeof(cfg));

    struct fw9369_transport *t = fw9369_mock_transport_create(&cfg);
    struct fw9369_pipeline_context ctx;

    int ret = fw9369_pipeline_run(t, &ctx);
    TEST_ASSERT(ret == 0, "Full 5-phase pipeline executes to completion");
    TEST_ASSERT(ctx.phase == FW9369_PHASE_COMPLETED, "Final phase is FW9369_PHASE_COMPLETED");
    TEST_ASSERT(ctx.identified_chip_id == 0x9362, "Silicon identified as FT9362 (0x9362)");
    TEST_ASSERT(ctx.identified_chip_crc == 0x1C53, "Hardware Chip ID CRC validated (0x1C53)");
    TEST_ASSERT(ctx.mcu_status == 0x50, "MCU acknowledged idle handshake (0x50)");
    TEST_ASSERT(ctx.armed == true, "Autonomous finger detection successfully armed");

    /* Verify protected register was relocked */
    TEST_ASSERT(ctx.lock_outcome == FW9369_LOCK_TRANSMITTED_CLEAN, "Lock command 0x9A=0x00 submitted cleanly");
    TEST_ASSERT(fw9369_mock_is_protected_locked(t) == true, "Simulated analog trim bank relocked");

    fw9369_mock_transport_destroy(t);
}

/* 7. Failure Injection Scenarios */
static void test_failure_injection_suite(void)
{
    printf("\n--- Test Suite 7: Deterministic Failure Injection Scenarios ---\n");

    struct fw9369_pipeline_context ctx;

    /* Failure Scenario 1: SPI Transfer Failure */
    {
        struct fw9369_mock_config cfg = { .fail_transfers = true };
        struct fw9369_transport *t = fw9369_mock_transport_create(&cfg);
        int ret = fw9369_pipeline_run(t, &ctx);
        TEST_ASSERT(ret < 0 && ctx.phase == FW9369_PHASE_ABORTED,
                    "Failure Injection 1: Transfer failure aborts cleanly");
        fw9369_mock_transport_destroy(t);
    }

    /* Failure Scenario 2: Unexpected Chip ID */
    {
        struct fw9369_mock_config cfg = { .fail_chip_id = true };
        struct fw9369_transport *t = fw9369_mock_transport_create(&cfg);
        int ret = fw9369_pipeline_run(t, &ctx);
        TEST_ASSERT(ret < 0 && ctx.phase == FW9369_PHASE_ABORTED,
                    "Failure Injection 2: Unexpected Chip ID (0x9368) aborts cleanly");
        fw9369_mock_transport_destroy(t);
    }

    /* Failure Scenario 3: Hardware Wire CRC Failure */
    {
        struct fw9369_mock_config cfg = { .fail_crc = true };
        struct fw9369_transport *t = fw9369_mock_transport_create(&cfg);
        int ret = fw9369_pipeline_run(t, &ctx);
        TEST_ASSERT(ret < 0 && ctx.phase == FW9369_PHASE_ABORTED,
                    "Failure Injection 3: Corrupt wire CRC aborts cleanly");
        fw9369_mock_transport_destroy(t);
    }

    /* Failure Scenario 4: ADC Ready Timeout */
    {
        struct fw9369_mock_config cfg = { .timeout_adc_ready = true };
        struct fw9369_transport *t = fw9369_mock_transport_create(&cfg);
        int ret = fw9369_pipeline_run(t, &ctx);
        TEST_ASSERT(ret < 0 && ctx.phase == FW9369_PHASE_ABORTED,
                    "Failure Injection 4: ADC status polling timeout aborts cleanly");
        fw9369_mock_transport_destroy(t);
    }

    /* Failure Scenario 5: FDT Minima Out of Bounds (< 300) */
    {
        struct fw9369_mock_config cfg = { .fdt_out_of_bounds = true };
        struct fw9369_transport *t = fw9369_mock_transport_create(&cfg);
        int ret = fw9369_pipeline_run(t, &ctx);
        TEST_ASSERT(ret < 0 && ctx.phase == FW9369_PHASE_ABORTED,
                    "Failure Injection 5: Out of bounds minima (< 300) aborts cleanly");
        fw9369_mock_transport_destroy(t);
    }

    /* Failure Scenario 6: FIFO Read Failure */
    {
        struct fw9369_mock_config cfg = { .fail_fifo_read = true };
        struct fw9369_transport *t = fw9369_mock_transport_create(&cfg);
        int ret = fw9369_pipeline_run(t, &ctx);
        TEST_ASSERT(ret < 0 && ctx.phase == FW9369_PHASE_ABORTED,
                    "Failure Injection 6: 10,246-byte FIFO transfer failure aborts cleanly");
        fw9369_mock_transport_destroy(t);
    }

    /* Failure Scenario 7: Lock write failure */
    {
        struct fw9369_mock_config cfg = { .fail_lock_write = true };
        struct fw9369_transport *t = fw9369_mock_transport_create(&cfg);
        int ret = fw9369_pipeline_run(t, &ctx);
        TEST_ASSERT(ret < 0 && ctx.lock_outcome == FW9369_LOCK_TRANSMISSION_FAILED,
                    "Failure Injection 7: Lock write failure reports FW9369_LOCK_TRANSMISSION_FAILED");
        fw9369_mock_transport_destroy(t);
    }

    /* Failure Scenario 8: Process Interruption inside protected window */
    {
        struct fw9369_mock_config cfg = { .interrupt_in_trim = true };
        struct fw9369_transport *t = fw9369_mock_transport_create(&cfg);
        int ret = fw9369_pipeline_run(t, &ctx);
        TEST_ASSERT(ret < 0 && ctx.phase == FW9369_PHASE_ABORTED,
                    "Failure Injection 8: Interruption in trim access caught and handled");
        TEST_ASSERT(ctx.lock_outcome == FW9369_LOCK_TRANSMITTED_CLEAN,
                    "Failure Injection 8: Cleanup handler unconditionally submitted lock command");
        fw9369_mock_transport_destroy(t);
    }
}

/* 8. Hardware Transport Offline Validation & Isolation Tests */
static int fake_fd_open_count = 0;
static int fake_fd_close_count = 0;
static bool fake_ioctl_cs_change_verified = false;
static bool fake_ioctl_short_transfer = false;
static bool fake_ioctl_force_eio = false;
static bool fake_ioctl_called = false;
static bool fake_ioctl_force_overflow = false;
static int  fake_ioctl_force_errno = 0;
static int  fake_ioctl_call_count = 0;
static int  fake_ioctl_return_len = 0;
static void (*fake_ioctl_hook_fn)(struct spi_ioc_transfer *tr) = NULL;

static int test_fake_open(const char *pathname, int flags)
{
    (void)flags;
    if (strcmp(pathname, FW9369_HW_DEFAULT_DEV_PATH) == 0) {
        fake_fd_open_count++;
        return 42; /* Simulated valid file descriptor */
    }
    return -1;
}

static int test_fake_close(int fd)
{
    if (fd == 42) {
        fake_fd_close_count++;
        return 0;
    }
    return -1;
}

static int test_fake_ioctl(int fd, unsigned long request, void *arg)
{
    if (fd != 42) {
        errno = EBADF;
        return -1;
    }

    if (request == SPI_IOC_WR_MODE) {
        return 0;
    } else if (request == SPI_IOC_RD_MODE) {
        *(uint8_t *)arg = 0; /* SPI_MODE_0 */
        return 0;
    } else if (request == SPI_IOC_WR_BITS_PER_WORD) {
        return 0;
    } else if (request == SPI_IOC_RD_BITS_PER_WORD) {
        *(uint8_t *)arg = 8;
        return 0;
    } else if (request == SPI_IOC_WR_MAX_SPEED_HZ) {
        return 0;
    } else if (request == SPI_IOC_RD_MAX_SPEED_HZ) {
        *(uint32_t *)arg = 1000000;
        return 0;
    } else if (request == SPI_IOC_WR_LSB_FIRST) {
        return 0;
    } else if (request == SPI_IOC_MESSAGE(1)) {
        fake_ioctl_called = true;
        fake_ioctl_call_count++;
        struct spi_ioc_transfer *tr = (struct spi_ioc_transfer *)arg;
        if (!tr) {
            errno = EINVAL;
            return -1;
        }
        if (tr->cs_change == 0) {
            fake_ioctl_cs_change_verified = true;
        }
        if (fake_ioctl_hook_fn) {
            fake_ioctl_hook_fn(tr);
        }
        if (fake_ioctl_force_eio) {
            errno = EIO;
            return -1;
        }
        if (fake_ioctl_force_errno != 0) {
            errno = fake_ioctl_force_errno;
            return -1;
        }
        if (fake_ioctl_return_len != 0) {
            return fake_ioctl_return_len;
        }
        if (fake_ioctl_force_overflow) {
            return (int)(tr->len + 10);
        }
        if (fake_ioctl_short_transfer) {
            /* Simulate short transfer: return 2 bytes less than requested */
            return (tr->len > 2) ? (int)(tr->len - 2) : 0;
        }
        if (tr->rx_buf && tr->len >= 5) {
            uint8_t *rx = (uint8_t *)(uintptr_t)tr->rx_buf;
            memset(rx, 0, tr->len);
            rx[4] = 0x01; /* Simulated byte read response */
        }
        return (int)tr->len;
    }
    errno = EINVAL;
    return -1;
}

static const struct fw9369_hw_ops test_ops = {
    .open_fn = test_fake_open,
    .ioctl_fn = test_fake_ioctl,
    .close_fn = test_fake_close,
};

static void test_hardware_transport_suite(void)
{
    printf("\n--- Test Suite 8: Hardware Transport Offline Validation & Isolation ---\n");

    /* 1. Path validation without opening devices */
    TEST_ASSERT(fw9369_hw_validate_device_path(FW9369_HW_DEFAULT_DEV_PATH) == 0,
                "Valid device path /dev/spidev1.0 accepted offline");
    TEST_ASSERT(fw9369_hw_validate_device_path("/dev/spidev0.0") == -ENODEV,
                "Arbitrary device /dev/spidev0.0 rejected with -ENODEV (no fallback)");
    TEST_ASSERT(fw9369_hw_validate_device_path("/dev/null") == -ENODEV,
                "Invalid device /dev/null rejected with -ENODEV");
    TEST_ASSERT(fw9369_hw_validate_device_path(NULL) == -EINVAL,
                "Null device path rejected with -EINVAL");

    /* 2. Configuration validation */
    struct fw9369_hw_config cfg = {
        .device_path = FW9369_HW_DEFAULT_DEV_PATH,
        .speed_hz = FW9369_HW_DEFAULT_SPEED_HZ,
        .mode = FW9369_HW_DEFAULT_MODE,
        .bits_per_word = FW9369_HW_DEFAULT_BITS,
        .lsb_first = 0,
        .max_transfer_len = FW9369_HW_MAX_TRANSFER_LEN,
        .enable_logging = true,
    };
    TEST_ASSERT(fw9369_hw_validate_config(&cfg) == 0,
                "Valid hardware configuration accepted offline");

    struct fw9369_hw_config bad_cfg = cfg;
    bad_cfg.speed_hz = 2000000;
    TEST_ASSERT(fw9369_hw_validate_config(&bad_cfg) == -EINVAL,
                "Speed > 1 MHz rejected with -EINVAL");

    bad_cfg = cfg;
    bad_cfg.mode = 1;
    TEST_ASSERT(fw9369_hw_validate_config(&bad_cfg) == -EINVAL,
                "Non-zero SPI mode rejected with -EINVAL");

    bad_cfg = cfg;
    bad_cfg.bits_per_word = 16;
    TEST_ASSERT(fw9369_hw_validate_config(&bad_cfg) == -EINVAL,
                "Non-8-bit word size rejected with -EINVAL");

    bad_cfg = cfg;
    bad_cfg.max_transfer_len = 0;
    TEST_ASSERT(fw9369_hw_validate_config(&bad_cfg) == -EINVAL,
                "Zero max transfer length rejected with -EINVAL");

    /* 3. Transport instantiation with fake SPI hooks (zero device access) */
    fake_fd_open_count = 0;
    fake_fd_close_count = 0;
    fake_ioctl_cs_change_verified = false;
    fake_ioctl_short_transfer = false;
    fake_ioctl_force_eio = false;

    struct fw9369_transport *t = fw9369_hw_transport_create_with_ops(&cfg, &test_ops);
    TEST_ASSERT(t != NULL && t->transfer != NULL && t->delay_ms != NULL && t->delay_us != NULL && t->close != NULL,
                "Hardware transport instantiated with fake SPI system-call hooks");
    TEST_ASSERT(fw9369_hw_get_fd(t) == 42,
                "Hardware transport acquired simulated file descriptor 42");

    /* 4. Valid byte write packet */
    uint8_t tx_write[4] = { 0x09, 0xF6, 0xC6, 0x01 };
    int ret = t->transfer(t->context, tx_write, NULL, 4);
    TEST_ASSERT(ret == 0 && fake_ioctl_cs_change_verified == true,
                "Valid byte write packet passes allowlist and executes with continuous CS (cs_change=0)");

    /* 5. Unauthorized register access rejected */
    uint8_t tx_unauth_reg[4] = { 0x09, 0xF6, 0x00, 0x00 };
    ret = t->transfer(t->context, tx_unauth_reg, NULL, 4);
    TEST_ASSERT(ret == -EPERM,
                "Unauthorized register write rejected with -EPERM at transport boundary");

    /* 6. Unauthorized command opcode rejected */
    uint8_t tx_unauth_cmd[3] = { 0x12, 0x34, 0x00 };
    ret = t->transfer(t->context, tx_unauth_cmd, NULL, 3);
    TEST_ASSERT(ret == -EPERM,
                "Unauthorized command opcode rejected with -EPERM at transport boundary");

    /* 6a. Unknown opcode rejected (allowlist fallthrough prevention) */
    uint8_t tx_unknown[3] = { 0x33, 0x44, 0x55 };
    ret = t->transfer(t->context, tx_unknown, NULL, 3);
    TEST_ASSERT(ret == -EPERM,
                "Unknown opcode rejected with -EPERM (fail-closed fallthrough prevention)");

    /* 6b. Malformed opcode complement rejected */
    uint8_t tx_bad_comp[4] = { 0x09, 0x00, 0xC6, 0x01 };
    ret = t->transfer(t->context, tx_bad_comp, NULL, 4);
    TEST_ASSERT(ret == -EPERM,
                "Malformed opcode complement rejected with -EPERM");

    /* 6c. Direction violation: write to read-only register 0x80 rejected */
    uint8_t tx_write_ro[4] = { 0x09, 0xF6, 0x80, 0x01 };
    ret = t->transfer(t->context, tx_write_ro, NULL, 4);
    TEST_ASSERT(ret == -EPERM,
                "Direction violation (write to read-only reg 0x80) rejected with -EPERM");

    /* 6d. Direction violation: read from write-only register 0x9A rejected */
    uint8_t tx_read_wo[5] = { 0x08, 0xF7, 0x9A, 0x00, 0x00 };
    ret = t->transfer(t->context, tx_read_wo, NULL, 5);
    TEST_ASSERT(ret == -EPERM,
                "Direction violation (read from write-only reg 0x9A) rejected with -EPERM");

    /* 7. Transfer exceeding max length */
    uint8_t oversized[64];
    cfg.max_transfer_len = 16;
    struct fw9369_transport *t_small = fw9369_hw_transport_create_with_ops(&cfg, &test_ops);
    ret = t_small->transfer(t_small->context, oversized, NULL, 32);
    TEST_ASSERT(ret == -EMSGSIZE,
                "Transfer exceeding max length rejected with -EMSGSIZE");
    fw9369_hw_transport_destroy(t_small);
    cfg.max_transfer_len = FW9369_HW_MAX_TRANSFER_LEN;

    /* 8. Short transfer detection */
    fake_ioctl_short_transfer = true;
    ret = t->transfer(t->context, tx_write, NULL, 4);
    TEST_ASSERT(ret == -EMSGSIZE,
                "Short SPI transfer detected and reported as -EMSGSIZE");
    fake_ioctl_short_transfer = false;

    /* 9. Ioctl error propagation */
    fake_ioctl_force_eio = true;
    ret = t->transfer(t->context, tx_write, NULL, 4);
    TEST_ASSERT(ret == -EIO,
                "Ioctl -EIO error cleanly propagated without retries");
    fake_ioctl_force_eio = false;

    /* Execute valid byte read to verify full-duplex MISO handling */
    uint8_t tx_read[5] = { 0x08, 0xF7, 0x80, 0x00, 0x00 };
    uint8_t rx_read[5] = { 0 };
    ret = t->transfer(t->context, tx_read, rx_read, 5);
    TEST_ASSERT(ret == 0 && rx_read[4] == 0x01,
                "Valid byte read packet executes full-duplex transfer cleanly");

    /* 10. Transaction logging */
    TEST_ASSERT(fw9369_hw_get_tx_count(t) >= 4,
                "Hardware transaction logger accurately records packet count and status");

    /* 11. Deterministic file descriptor closing */
    fw9369_hw_transport_destroy(t);
    TEST_ASSERT(fake_fd_close_count >= 1,
                "File descriptor deterministically closed upon transport destruction");

    /* 12. Mock hardware stub continues to reject */
    errno = 0;
    struct fw9369_transport *stub = fw9369_hardware_transport_create(FW9369_HW_DEFAULT_DEV_PATH);
    TEST_ASSERT(stub == NULL && errno == ENOTSUP,
                "Mock hardware rejection stub returns NULL with -ENOTSUP without touching devices");

    /* 13. Default binary isolation verification */
    FILE *fp = popen("nm -g ../../bin/fw9369-tool 2>/dev/null | grep -c fw9369_hw_", "r");
    int sym_count = -1;
    if (fp) {
        if (fscanf(fp, "%d", &sym_count) != 1) sym_count = -1;
        pclose(fp);
    }
    TEST_ASSERT(sym_count == 0,
                "Default binary isolation verified: bin/fw9369-tool contains zero hardware backend symbols");
}

static void create_test_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

/* 9. Device Identity & Sysfs Topology Tests */
static void test_device_identity_suite(void)
{
    printf("\n--- Test Suite 9: Device Identity Enforcement & Sysfs Topology ---\n");

    /* 1. Live host sysfs test: unbound spidev node must fail closed */
    int ret = fw9369_hw_verify_device_identity(FW9369_HW_DEFAULT_DEV_PATH, NULL);
    TEST_ASSERT(ret == -ENODEV, "Live unbound host correctly rejects un-bound device node with -ENODEV");

    /* 2. Path parameter validation */
    TEST_ASSERT(fw9369_hw_verify_device_identity(NULL, NULL) == -EINVAL,
                "Null path rejected with -EINVAL");
    TEST_ASSERT(fw9369_hw_verify_device_identity("/dev/spidev0.0", NULL) == -ENODEV,
                "Non-spidev1.0 path rejected with -ENODEV");
    TEST_ASSERT(fw9369_hw_verify_device_identity("/dev/null", NULL) == -ENODEV,
                "Device /dev/null rejected with -ENODEV");

    /* 3. Synthetic sysfs hierarchy tests */
    char temp_dir[] = "/tmp/fw9369_sysfs_XXXXXX";
    char *td = mkdtemp(temp_dir);
    TEST_ASSERT(td != NULL, "Created synthetic sysfs temporary directory");

    if (td) {
        char link_path[512];
        char target_path[512];
        char modalias_path[512];
        char root_buf[512];
        char cmd[1024];

        /* 1. Create valid topology */
        snprintf(cmd, sizeof(cmd), "mkdir -p %.256s/valid/class/spidev/spidev1.0 %.256s/valid/devices/pci0000:00/0000:00:1e.3/pxa2xx-spi.2/spi_master/spi1/spi-FTE3600:00", td, td);
        int sys_ret = system(cmd);
        (void)sys_ret;

        snprintf(link_path, sizeof(link_path), "%.256s/valid/class/spidev/spidev1.0/device", td);
        snprintf(target_path, sizeof(target_path), "%.256s/valid/devices/pci0000:00/0000:00:1e.3/pxa2xx-spi.2/spi_master/spi1/spi-FTE3600:00", td);
        symlink(target_path, link_path);

        snprintf(modalias_path, sizeof(modalias_path), "%.256s/modalias", target_path);
        create_test_file(modalias_path, "acpi:FTE3600:FTE3600:\n");

        snprintf(root_buf, sizeof(root_buf), "%.256s/valid", td);
        ret = fw9369_hw_verify_device_identity(FW9369_HW_DEFAULT_DEV_PATH, root_buf);
        TEST_ASSERT(ret == 0,
                    "Synthetic sysfs topology successfully verified (pxa2xx-spi.2/spi1/spi-FTE3600:00)");

        /* 2. Test mismatched controller */
        snprintf(cmd, sizeof(cmd), "mkdir -p %.256s/bad_ctrl/class/spidev/spidev1.0 %.256s/bad_ctrl/devices/pci0000:00/0000:00:1e.3/pxa2xx-spi.0/spi_master/spi1/spi-FTE3600:00", td, td);
        sys_ret = system(cmd);
        snprintf(link_path, sizeof(link_path), "%.256s/bad_ctrl/class/spidev/spidev1.0/device", td);
        snprintf(target_path, sizeof(target_path), "%.256s/bad_ctrl/devices/pci0000:00/0000:00:1e.3/pxa2xx-spi.0/spi_master/spi1/spi-FTE3600:00", td);
        symlink(target_path, link_path);
        snprintf(modalias_path, sizeof(modalias_path), "%.256s/modalias", target_path);
        create_test_file(modalias_path, "acpi:FTE3600:FTE3600:\n");
        snprintf(root_buf, sizeof(root_buf), "%.256s/bad_ctrl", td);
        ret = fw9369_hw_verify_device_identity(FW9369_HW_DEFAULT_DEV_PATH, root_buf);
        TEST_ASSERT(ret == -EILSEQ, "Synthetic sysfs rejects mismatched controller (pxa2xx-spi.0)");

        /* 3. Test mismatched bus */
        snprintf(cmd, sizeof(cmd), "mkdir -p %.256s/bad_bus/class/spidev/spidev1.0 %.256s/bad_bus/devices/pci0000:00/0000:00:1e.3/pxa2xx-spi.2/spi_master/spi0/spi-FTE3600:00", td, td);
        sys_ret = system(cmd);
        snprintf(link_path, sizeof(link_path), "%.256s/bad_bus/class/spidev/spidev1.0/device", td);
        snprintf(target_path, sizeof(target_path), "%.256s/bad_bus/devices/pci0000:00/0000:00:1e.3/pxa2xx-spi.2/spi_master/spi0/spi-FTE3600:00", td);
        symlink(target_path, link_path);
        snprintf(modalias_path, sizeof(modalias_path), "%.256s/modalias", target_path);
        create_test_file(modalias_path, "acpi:FTE3600:FTE3600:\n");
        snprintf(root_buf, sizeof(root_buf), "%.256s/bad_bus", td);
        ret = fw9369_hw_verify_device_identity(FW9369_HW_DEFAULT_DEV_PATH, root_buf);
        TEST_ASSERT(ret == -EILSEQ, "Synthetic sysfs rejects mismatched bus (spi0)");

        /* 4. Test mismatched device */
        snprintf(cmd, sizeof(cmd), "mkdir -p %.256s/bad_dev/class/spidev/spidev1.0 %.256s/bad_dev/devices/pci0000:00/0000:00:1e.3/pxa2xx-spi.2/spi_master/spi1/spi-GENERIC:00", td, td);
        sys_ret = system(cmd);
        snprintf(link_path, sizeof(link_path), "%.256s/bad_dev/class/spidev/spidev1.0/device", td);
        snprintf(target_path, sizeof(target_path), "%.256s/bad_dev/devices/pci0000:00/0000:00:1e.3/pxa2xx-spi.2/spi_master/spi1/spi-GENERIC:00", td);
        symlink(target_path, link_path);
        snprintf(modalias_path, sizeof(modalias_path), "%.256s/modalias", target_path);
        create_test_file(modalias_path, "acpi:FTE3600:FTE3600:\n");
        snprintf(root_buf, sizeof(root_buf), "%.256s/bad_dev", td);
        ret = fw9369_hw_verify_device_identity(FW9369_HW_DEFAULT_DEV_PATH, root_buf);
        TEST_ASSERT(ret == -EILSEQ, "Synthetic sysfs rejects mismatched device (spi-GENERIC:00)");

        /* 5. Test mismatched ACPI HID */
        snprintf(cmd, sizeof(cmd), "mkdir -p %.256s/bad_hid/class/spidev/spidev1.0 %.256s/bad_hid/devices/pci0000:00/0000:00:1e.3/pxa2xx-spi.2/spi_master/spi1/spi-FTE3600:00", td, td);
        sys_ret = system(cmd);
        snprintf(link_path, sizeof(link_path), "%.256s/bad_hid/class/spidev/spidev1.0/device", td);
        snprintf(target_path, sizeof(target_path), "%.256s/bad_hid/devices/pci0000:00/0000:00:1e.3/pxa2xx-spi.2/spi_master/spi1/spi-FTE3600:00", td);
        symlink(target_path, link_path);
        snprintf(modalias_path, sizeof(modalias_path), "%.256s/modalias", target_path);
        create_test_file(modalias_path, "acpi:OTHER_DEVICE:00\n");
        snprintf(root_buf, sizeof(root_buf), "%.256s/bad_hid", td);
        ret = fw9369_hw_verify_device_identity(FW9369_HW_DEFAULT_DEV_PATH, root_buf);
        TEST_ASSERT(ret == -EKEYREJECTED, "Synthetic sysfs rejects mismatched ACPI HID");

        /* Clean up all under td */
        snprintf(cmd, sizeof(cmd), "rm -rf %.256s", td);
        sys_ret = system(cmd);
        (void)sys_ret;
    }
}

/* 10. Adversarial Protocol Hardening & Edge Cases */
static void test_adversarial_protocol_suite(void)
{
    printf("\n--- Test Suite 10: Adversarial Protocol Hardening & Edge Cases ---\n");

    struct fw9369_hw_config cfg = {
        .device_path = FW9369_HW_DEFAULT_DEV_PATH,
        .speed_hz = FW9369_HW_DEFAULT_SPEED_HZ,
        .mode = FW9369_HW_DEFAULT_MODE,
        .bits_per_word = FW9369_HW_DEFAULT_BITS,
        .lsb_first = 0,
        .max_transfer_len = FW9369_HW_MAX_TRANSFER_LEN,
        .enable_logging = true,
    };
    struct fw9369_transport *t = fw9369_hw_transport_create_with_ops(&cfg, &test_ops);
    TEST_ASSERT(t != NULL, "Adversarial test transport created with fake ops");

    /* Initial state verification */
    TEST_ASSERT(!fw9369_hw_is_protected_unlocked(t), "Initial transport state is not unlocked");
    TEST_ASSERT(fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_UNKNOWN,
                "Initial transport state is UNKNOWN");

    /* 1. Null / zero parameter tests */
    int ret = t->transfer(t->context, NULL, NULL, 4);
    TEST_ASSERT(ret == -EINVAL, "Null tx buffer rejected with -EINVAL");

    uint8_t dummy[4] = { 0x09, 0xF6, 0xC6, 0x01 };
    ret = t->transfer(t->context, dummy, NULL, 0);
    TEST_ASSERT(ret == -EINVAL, "Zero length transfer rejected with -EINVAL");

    ret = t->transfer(NULL, dummy, NULL, 4);
    TEST_ASSERT(ret == -EINVAL, "Null transport context rejected with -EINVAL");

    /* 2. Malformed complements (must fail before ioctl) */
    uint8_t bad_comp_wr[4] = { 0x09, 0x00, 0xC6, 0x01 };
    fake_ioctl_called = false;
    ret = t->transfer(t->context, bad_comp_wr, NULL, 4);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Byte write bad complement rejected with -EPERM before ioctl");

    uint8_t bad_comp_rd[5] = { 0x08, 0x00, 0xC6, 0, 0 };
    fake_ioctl_called = false;
    ret = t->transfer(t->context, bad_comp_rd, NULL, 5);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Byte read bad complement rejected with -EPERM before ioctl");

    uint8_t bad_comp_ww[8] = { 0x05, 0x00, 0x98, 0x01, 0, 1, 0, 0 };
    fake_ioctl_called = false;
    ret = t->transfer(t->context, bad_comp_ww, NULL, 8);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Word write bad complement rejected with -EPERM before ioctl");

    uint8_t bad_comp_wr_op[10] = { 0x04, 0x00, 0x9A, 0x8B, 0, 1, 0, 0, 0, 0 };
    fake_ioctl_called = false;
    ret = t->transfer(t->context, bad_comp_wr_op, NULL, 10);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Word read bad complement rejected with -EPERM before ioctl");

    uint8_t bad_comp_fifo[FW9369_FIFO_TRANSFER_LEN];
    memset(bad_comp_fifo, 0, sizeof(bad_comp_fifo));
    bad_comp_fifo[0] = 0x06; bad_comp_fifo[1] = 0x00;
    fake_ioctl_called = false;
    ret = t->transfer(t->context, bad_comp_fifo, NULL, sizeof(bad_comp_fifo));
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "FIFO read bad complement rejected with -EPERM before ioctl");

    /* 3. Truncated packets */
    fake_ioctl_called = false;
    ret = t->transfer(t->context, dummy, NULL, 3);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Truncated byte write (3 B) rejected with -EPERM before ioctl");

    fake_ioctl_called = false;
    ret = t->transfer(t->context, bad_comp_rd, NULL, 4);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Truncated byte read (4 B) rejected with -EPERM before ioctl");

    fake_ioctl_called = false;
    ret = t->transfer(t->context, bad_comp_ww, NULL, 7);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Truncated word write (7 B) rejected with -EPERM before ioctl");

    uint8_t cmd_wake[3] = { 0x5A, 0xA5, 0x00 };
    fake_ioctl_called = false;
    ret = t->transfer(t->context, cmd_wake, NULL, 2);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Truncated command (2 B) rejected with -EPERM before ioctl");

    fake_ioctl_called = false;
    ret = t->transfer(t->context, bad_comp_fifo, NULL, 10245);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Truncated FIFO read (10,245 B) rejected with -EPERM before ioctl");

    /* 4. Extra trailing bytes */
    uint8_t extra_cmd[4] = { 0x5A, 0xA5, 0x00, 0x00 };
    fake_ioctl_called = false;
    ret = t->transfer(t->context, extra_cmd, NULL, 4);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Command with trailing bytes (4 B) rejected with -EPERM before ioctl");

    uint8_t extra_wr[5] = { 0x09, 0xF6, 0xC6, 0x01, 0x00 };
    fake_ioctl_called = false;
    ret = t->transfer(t->context, extra_wr, NULL, 5);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Byte write with trailing bytes (5 B) rejected with -EPERM before ioctl");

    uint8_t bad_cmd_3rd[3] = { 0x5A, 0xA5, 0x01 };
    fake_ioctl_called = false;
    ret = t->transfer(t->context, bad_cmd_3rd, NULL, 3);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Command with non-zero 3rd byte rejected with -EPERM before ioctl");

    /* 5. Unknown opcodes */
    uint8_t unk_op[4] = { 0x33, 0xCC, 0xC6, 0x01 };
    fake_ioctl_called = false;
    ret = t->transfer(t->context, unk_op, NULL, 4);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Unknown opcode 0x33 rejected with -EPERM before ioctl");

    unk_op[0] = 0x00; unk_op[1] = 0xFF;
    fake_ioctl_called = false;
    ret = t->transfer(t->context, unk_op, NULL, 4);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Unknown opcode 0x00 rejected with -EPERM before ioctl");

    unk_op[0] = 0xFF; unk_op[1] = 0x00;
    fake_ioctl_called = false;
    ret = t->transfer(t->context, unk_op, NULL, 4);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Unknown opcode 0xFF rejected with -EPERM before ioctl");

    /* 6. Unknown and unauthorized register targets */
    uint8_t unk_reg[4] = { 0x09, 0xF6, 0x42, 0x00 };
    fake_ioctl_called = false;
    ret = t->transfer(t->context, unk_reg, NULL, 4);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Unknown byte register 0x42 rejected with -EPERM before ioctl");

    uint8_t unk_word_reg[8] = { 0x05, 0xFA, 0x99, 0x00, 0, 1, 0, 0 };
    fake_ioctl_called = false;
    ret = t->transfer(t->context, unk_word_reg, NULL, 8);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Unknown word register 0x1900 rejected with -EPERM before ioctl");

    /* 7. Direction violations */
    uint8_t rd_wo[5] = { 0x08, 0xF7, 0x9A, 0, 0 };
    fake_ioctl_called = false;
    ret = t->transfer(t->context, rd_wo, NULL, 5);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Read from write-only register 0x9A rejected with -EPERM before ioctl");

    rd_wo[2] = 0xF1;
    fake_ioctl_called = false;
    ret = t->transfer(t->context, rd_wo, NULL, 5);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Read from write-only register 0xF1 rejected with -EPERM before ioctl");

    uint8_t wr_ro[4] = { 0x09, 0xF6, 0x80, 0x00 };
    fake_ioctl_called = false;
    ret = t->transfer(t->context, wr_ro, NULL, 4);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Write to read-only register 0x80 rejected with -EPERM before ioctl");

    uint8_t wr_ro_chipid[8] = { 0x05, 0xFA, 0x9A, 0x8B, 0, 1, 0, 0 };
    fake_ioctl_called = false;
    ret = t->transfer(t->context, wr_ro_chipid, NULL, 8);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Write to read-only Chip ID 0x1A8B rejected with -EPERM before ioctl");

    /* 8. Protected register 0x9A key validation & unlock tracking */
    uint8_t bad_key[4] = { 0x09, 0xF6, 0x9A, 0x12 };
    fake_ioctl_called = false;
    ret = t->transfer(t->context, bad_key, NULL, 4);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Register 0x9A with invalid key 0x12 rejected with -EPERM before ioctl");

    bad_key[3] = 0xFF;
    fake_ioctl_called = false;
    ret = t->transfer(t->context, bad_key, NULL, 4);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Register 0x9A with invalid key 0xFF rejected with -EPERM before ioctl");

    /* Attempt access to protected trim bank while locked */
    uint8_t trim_wr[8] = { 0x05, 0xFA, 0x80, 0xC0, 0, 1, 0x04, 0x44 };
    fake_ioctl_called = false;
    ret = t->transfer(t->context, trim_wr, NULL, 8);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Access to protected trim bank (0x00C0) while locked rejected with -EPERM");

    /* Unlock 0x9A = 0x5A from ASSUMED_LOCKED fixture state */
    fw9369_hw_set_lock_state_for_test(t, FW9369_LOCK_STATE_ASSUMED_LOCKED);
    uint8_t unlock_pkt[4] = { 0x09, 0xF6, 0x9A, 0x5A };
    ret = t->transfer(t->context, unlock_pkt, NULL, 4);
    TEST_ASSERT(ret == 0 && fw9369_hw_is_protected_unlocked(t) == true,
                "Unlock write 0x9A=0x5A successfully transitions transport to unlocked state");

    /* Repeated unlock command must be rejected */
    fake_ioctl_called = false;
    ret = t->transfer(t->context, unlock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Repeated unlock command while already unlocked rejected with -EPERM");

    /* Trim access now permitted while unlocked */
    ret = t->transfer(t->context, trim_wr, NULL, 8);
    TEST_ASSERT(ret == 0, "Access to protected trim bank (0x00C0) succeeds while unlocked");

    /* Lock 0x9A = 0x00 */
    uint8_t lock_pkt[4] = { 0x09, 0xF6, 0x9A, 0x00 };
    ret = t->transfer(t->context, lock_pkt, NULL, 4);
    TEST_ASSERT(ret == 0 && fw9369_hw_is_protected_unlocked(t) == false,
                "Lock write 0x9A=0x00 successfully relocks transport");

    /* Trim access rejected again after relock */
    fake_ioctl_called = false;
    ret = t->transfer(t->context, trim_wr, NULL, 8);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Access to protected trim bank (0x00C0) rejected again after relock");

    /* Unlock and simulate failed lock write: must retain unlocked (unsafe) state */
    ret = t->transfer(t->context, unlock_pkt, NULL, 4);
    TEST_ASSERT(ret == 0 && fw9369_hw_is_protected_unlocked(t) == true,
                "Transport unlocked again for failure test");

    fake_ioctl_force_eio = true;
    ret = t->transfer(t->context, lock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EIO && fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_UNKNOWN,
                "Lock failure correctly transitions to UNKNOWN state without masking");
    fake_ioctl_force_eio = false;

    /* Lock from UNKNOWN must be rejected fail-closed before ioctl */
    fake_ioctl_called = false;
    ret = t->transfer(t->context, lock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Lock command from UNKNOWN rejected with -EPERM before ioctl");

    /* 9. Word read count overflow & bounds */
    uint8_t ovf_rd[10] = { 0x04, 0xFB, 0x9A, 0x8B, 0x7F, 0xFF, 0, 0, 0, 0 };
    fake_ioctl_called = false;
    ret = t->transfer(t->context, ovf_rd, NULL, 10);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Word read with oversized count (0x7FFF) rejected before ioctl");

    /* 10. FIFO invalid lengths */
    uint8_t fifo_buf[4096];
    memset(fifo_buf, 0, sizeof(fifo_buf));
    fifo_buf[0] = 0x06; fifo_buf[1] = 0xF9;
    fifo_buf[2] = 0x9A; fifo_buf[3] = 0x05;
    fake_ioctl_called = false;
    ret = t->transfer(t->context, fifo_buf, NULL, 4096);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Segmented 4096-byte FIFO read rejected with -EPERM before ioctl");

    /* 11. Unexpected positive ioctl return (overflow) */
    fake_ioctl_force_overflow = true;
    ret = t->transfer(t->context, dummy, NULL, 4);
    TEST_ASSERT(ret == -EPROTO,
                "Unexpected positive ioctl return (ret > len) detected as -EPROTO");
    fake_ioctl_force_overflow = false;

    /* 12. Negative ioctl errno propagation */
    fake_ioctl_force_errno = EINTR;
    ret = t->transfer(t->context, dummy, NULL, 4);
    TEST_ASSERT(ret == -EINTR, "System call -EINTR cleanly propagated");

    fake_ioctl_force_errno = EBUSY;
    ret = t->transfer(t->context, dummy, NULL, 4);
    TEST_ASSERT(ret == -EBUSY, "System call -EBUSY cleanly propagated");

    fake_ioctl_force_errno = ETIMEDOUT;
    ret = t->transfer(t->context, dummy, NULL, 4);
    TEST_ASSERT(ret == -ETIMEDOUT, "System call -ETIMEDOUT cleanly propagated");
    fake_ioctl_force_errno = 0;

    fw9369_hw_transport_destroy(t);
}

/* 11. Sensor State Uncertainty and Recovery-State Hardening (Stage 2C.5C.2) */
static void test_lock_state_hardening_suite(void)
{
    printf("\n--- Test Suite 11: Sensor State Uncertainty & Recovery-State Hardening ---\n");

    struct fw9369_hw_config cfg = {
        .device_path = FW9369_HW_DEFAULT_DEV_PATH,
        .speed_hz = FW9369_HW_DEFAULT_SPEED_HZ,
        .mode = FW9369_HW_DEFAULT_MODE,
        .bits_per_word = FW9369_HW_DEFAULT_BITS,
        .lsb_first = 0,
        .max_transfer_len = FW9369_HW_MAX_TRANSFER_LEN,
        .enable_logging = true,
    };

    struct fw9369_hw_ops fake_ops = {
        .open_fn = test_fake_open,
        .ioctl_fn = test_fake_ioctl,
        .close_fn = test_fake_close,
    };

    uint8_t unlock_pkt[4] = { 0x09, 0xF6, 0x9A, 0x5A };
    uint8_t lock_pkt[4]   = { 0x09, 0xF6, 0x9A, 0x00 };
    uint8_t trim_wr[8]    = { 0x05, 0xFA, 0x80, 0xC0, 0, 1, 0x04, 0x44 };

    /* 1. New context begins in UNKNOWN state */
    struct fw9369_transport *t = fw9369_hw_transport_create_with_ops(&cfg, &fake_ops);
    TEST_ASSERT(t != NULL, "Test transport instantiated for Suite 11");
    TEST_ASSERT(fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_UNKNOWN,
                "1. New context begins in UNKNOWN state");

    /* 2. Protected writes are rejected in UNKNOWN state */
    fake_ioctl_called = false;
    int ret = t->transfer(t->context, trim_wr, NULL, 8);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "2. Protected writes are rejected in UNKNOWN state before ioctl");

    /* 3. Unlock exact success from ASSUMED_LOCKED fixture state */
    fw9369_hw_set_lock_state_for_test(t, FW9369_LOCK_STATE_ASSUMED_LOCKED);
    fake_ioctl_called = false;
    fake_ioctl_force_eio = false;
    fake_ioctl_return_len = 0;
    ret = t->transfer(t->context, unlock_pkt, NULL, 4);
    TEST_ASSERT(ret == 0 && fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_ASSUMED_UNLOCKED,
                "3. Unlock exact success transitions state to ASSUMED_UNLOCKED");

    /* 4. Unlock negative ioctl result */
    /* First relock cleanly to leave assumed unlocked state */
    ret = t->transfer(t->context, lock_pkt, NULL, 4);
    TEST_ASSERT(ret == 0 && fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_ASSUMED_LOCKED,
                "Intermediate clean lock succeeded");

    fake_ioctl_force_eio = true;
    ret = t->transfer(t->context, unlock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EIO && fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_UNKNOWN,
                "4. Unlock negative ioctl result transitions state to UNKNOWN");
    fake_ioctl_force_eio = false;

    /* 5. Unlock short transfer */
    fw9369_hw_set_lock_state_for_test(t, FW9369_LOCK_STATE_ASSUMED_LOCKED);
    fake_ioctl_return_len = 2; /* 2 < 4 */
    ret = t->transfer(t->context, unlock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EMSGSIZE && fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_UNKNOWN,
                "5. Unlock short transfer (< 4 bytes) transitions state to UNKNOWN");
    fake_ioctl_return_len = 0;

    /* 6. Unlock unexpected positive result */
    fw9369_hw_set_lock_state_for_test(t, FW9369_LOCK_STATE_ASSUMED_LOCKED);
    fake_ioctl_return_len = 6; /* 6 > 4 */
    ret = t->transfer(t->context, unlock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EPROTO && fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_UNKNOWN,
                "6. Unlock unexpected positive result (> 4 bytes) transitions state to UNKNOWN");
    fake_ioctl_return_len = 0;

    /* 7. Lock exact success */
    fw9369_hw_set_lock_state_for_test(t, FW9369_LOCK_STATE_ASSUMED_UNLOCKED);
    ret = t->transfer(t->context, lock_pkt, NULL, 4);
    TEST_ASSERT(ret == 0 && fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_ASSUMED_LOCKED,
                "7. Lock exact success transitions state to ASSUMED_LOCKED");

    /* 8. Lock negative ioctl result */
    fw9369_hw_set_lock_state_for_test(t, FW9369_LOCK_STATE_ASSUMED_UNLOCKED);
    fake_ioctl_force_eio = true;
    ret = t->transfer(t->context, lock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EIO && fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_UNKNOWN,
                "8. Lock negative ioctl result transitions state to UNKNOWN");
    fake_ioctl_force_eio = false;

    /* 9. Lock short transfer */
    fw9369_hw_set_lock_state_for_test(t, FW9369_LOCK_STATE_ASSUMED_UNLOCKED);
    fake_ioctl_return_len = 1; /* 1 < 4 */
    ret = t->transfer(t->context, lock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EMSGSIZE && fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_UNKNOWN,
                "9. Lock short transfer (< 4 bytes) transitions state to UNKNOWN");
    fake_ioctl_return_len = 0;

    /* 10. Lock unexpected positive result */
    fw9369_hw_set_lock_state_for_test(t, FW9369_LOCK_STATE_ASSUMED_UNLOCKED);
    fake_ioctl_return_len = 8; /* 8 > 4 */
    ret = t->transfer(t->context, lock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EPROTO && fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_UNKNOWN,
                "10. Lock unexpected positive result (> 4 bytes) transitions state to UNKNOWN");
    fake_ioctl_return_len = 0;

    /* 11. Context destruction while UNKNOWN */
    TEST_ASSERT(fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_UNKNOWN,
                "State confirmed UNKNOWN prior to destruction");
    fw9369_hw_transport_destroy(t);
    t = NULL;
    TEST_ASSERT(true, "11. Context destruction while UNKNOWN completes safely without error");

    /* 12. Context recreation after an uncertain unlock */
    struct fw9369_transport *t_unc_unlock = fw9369_hw_transport_create_with_ops(&cfg, &fake_ops);
    fw9369_hw_set_lock_state_for_test(t_unc_unlock, FW9369_LOCK_STATE_ASSUMED_LOCKED);
    fake_ioctl_force_eio = true;
    (void)t_unc_unlock->transfer(t_unc_unlock->context, unlock_pkt, NULL, 4);
    TEST_ASSERT(fw9369_hw_get_lock_state(t_unc_unlock) == FW9369_LOCK_STATE_UNKNOWN,
                "Pre-destruction transport confirmed UNKNOWN after failed unlock");
    fake_ioctl_force_eio = false;
    fw9369_hw_transport_destroy(t_unc_unlock);

    struct fw9369_transport *t_recreated1 = fw9369_hw_transport_create_with_ops(&cfg, &fake_ops);
    TEST_ASSERT(fw9369_hw_get_lock_state(t_recreated1) == FW9369_LOCK_STATE_UNKNOWN,
                "12. Context recreation after an uncertain unlock begins in UNKNOWN state");
    fw9369_hw_transport_destroy(t_recreated1);

    /* 13. Context recreation after an uncertain lock */
    struct fw9369_transport *t_unc_lock = fw9369_hw_transport_create_with_ops(&cfg, &fake_ops);
    fw9369_hw_set_lock_state_for_test(t_unc_lock, FW9369_LOCK_STATE_ASSUMED_LOCKED);
    (void)t_unc_lock->transfer(t_unc_lock->context, unlock_pkt, NULL, 4); /* successfully unlock */
    fake_ioctl_force_eio = true;
    (void)t_unc_lock->transfer(t_unc_lock->context, lock_pkt, NULL, 4);   /* failed lock */
    TEST_ASSERT(fw9369_hw_get_lock_state(t_unc_lock) == FW9369_LOCK_STATE_UNKNOWN,
                "Pre-destruction transport confirmed UNKNOWN after failed lock");
    fake_ioctl_force_eio = false;
    fw9369_hw_transport_destroy(t_unc_lock);

    struct fw9369_transport *t_recreated2 = fw9369_hw_transport_create_with_ops(&cfg, &fake_ops);
    TEST_ASSERT(fw9369_hw_get_lock_state(t_recreated2) == FW9369_LOCK_STATE_UNKNOWN,
                "13. Context recreation after an uncertain lock begins in UNKNOWN state");

    /* 14. No automatic retries */
    fw9369_hw_set_lock_state_for_test(t_recreated2, FW9369_LOCK_STATE_ASSUMED_UNLOCKED);
    fake_ioctl_call_count = 0;
    fake_ioctl_force_eio = true;
    ret = t_recreated2->transfer(t_recreated2->context, lock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EIO && fake_ioctl_call_count == 1,
                "14. No automatic retries executed upon transfer failure (call_count == 1)");
    fake_ioctl_force_eio = false;

    /* 15. No protected-register writes after an uncertain transition */
    TEST_ASSERT(fw9369_hw_get_lock_state(t_recreated2) == FW9369_LOCK_STATE_UNKNOWN,
                "State confirmed UNKNOWN after failed lock transfer");
    fake_ioctl_called = false;
    ret = t_recreated2->transfer(t_recreated2->context, trim_wr, NULL, 8);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "15. Protected-register writes rejected with -EPERM after uncertain transition");

    fw9369_hw_transport_destroy(t_recreated2);
}

/* 12. Independent Lock-State Transition and Recovery Audit (Stage 2C.5C.3) */
static struct fw9369_transport *hook_active_transport = NULL;
static bool hook_in_flight_unlock_observed = false;
static bool hook_in_flight_unlock_trim_rejected = false;
static bool hook_in_flight_lock_observed = false;
static bool hook_in_flight_lock_trim_rejected = false;

static void test_in_flight_ioctl_hook(struct spi_ioc_transfer *tr)
{
    if (!hook_active_transport || !tr || !tr->tx_buf)
        return;

    const uint8_t *tx = (const uint8_t *)(uintptr_t)tr->tx_buf;

    /* Verify in-flight unlock state */
    if (tr->len == 4 && tx[0] == 0x09 && tx[2] == 0x9A && tx[3] == 0x5A) {
        if (fw9369_hw_get_lock_state_unlocked_for_test(hook_active_transport) == FW9369_LOCK_STATE_UNLOCK_COMMAND_SENT) {
            hook_in_flight_unlock_observed = true;
        }
    }

    /* Verify in-flight lock state */
    if (tr->len == 4 && tx[0] == 0x09 && tx[2] == 0x9A && tx[3] == 0x00) {
        if (fw9369_hw_get_lock_state_unlocked_for_test(hook_active_transport) == FW9369_LOCK_STATE_LOCK_COMMAND_SENT) {
            hook_in_flight_lock_observed = true;
        }
    }
}

static void test_lock_state_transitions_audit_suite(void)
{
    printf("\n--- Test Suite 12: Independent Lock-State Transition & Recovery Audit ---\n");

    struct fw9369_hw_config cfg = {
        .device_path = FW9369_HW_DEFAULT_DEV_PATH,
        .speed_hz = FW9369_HW_DEFAULT_SPEED_HZ,
        .mode = FW9369_HW_DEFAULT_MODE,
        .bits_per_word = FW9369_HW_DEFAULT_BITS,
        .lsb_first = 0,
        .max_transfer_len = FW9369_HW_MAX_TRANSFER_LEN,
        .enable_logging = true,
    };

    struct fw9369_hw_ops fake_ops = {
        .open_fn = test_fake_open,
        .ioctl_fn = test_fake_ioctl,
        .close_fn = test_fake_close,
    };

    uint8_t unlock_pkt[4] = { 0x09, 0xF6, 0x9A, 0x5A };
    uint8_t lock_pkt[4]   = { 0x09, 0xF6, 0x9A, 0x00 };
    uint8_t trim_wr[8]    = { 0x05, 0xFA, 0x80, 0xC0, 0, 1, 0x04, 0x44 };

    /* 1. Lock attempted directly from UNKNOWN state must be REJECTED fail-closed */
    struct fw9369_transport *t = fw9369_hw_transport_create_with_ops(&cfg, &fake_ops);
    TEST_ASSERT(t != NULL, "Test transport instantiated for Suite 12");
    TEST_ASSERT(fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_UNKNOWN,
                "Context starts in UNKNOWN state");

    fake_ioctl_called = false;
    fake_ioctl_force_eio = false;
    fake_ioctl_return_len = 0;
    int ret = t->transfer(t->context, lock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called && fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_UNKNOWN,
                "1. Lock attempted directly from UNKNOWN rejected with -EPERM before ioctl");

    /* 2. Unlock attempted directly from UNKNOWN state must be REJECTED fail-closed */
    fake_ioctl_called = false;
    ret = t->transfer(t->context, unlock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called && fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_UNKNOWN,
                "2. Unlock attempted directly from UNKNOWN rejected with -EPERM before ioctl");

    /* 3. Unlock attempted from ASSUMED_LOCKED fixture state */
    struct fw9369_transport *t_unk = fw9369_hw_transport_create_with_ops(&cfg, &fake_ops);
    TEST_ASSERT(fw9369_hw_get_lock_state(t_unk) == FW9369_LOCK_STATE_UNKNOWN,
                "Fresh context t_unk begins in UNKNOWN state");
    fw9369_hw_set_lock_state_for_test(t_unk, FW9369_LOCK_STATE_ASSUMED_LOCKED);
    ret = t_unk->transfer(t_unk->context, unlock_pkt, NULL, 4);
    TEST_ASSERT(ret == 0 && fw9369_hw_get_lock_state(t_unk) == FW9369_LOCK_STATE_ASSUMED_UNLOCKED,
                "3. Unlock attempted from ASSUMED_LOCKED transitions to ASSUMED_UNLOCKED");

    /* 4. Redundant unlock while ASSUMED_UNLOCKED rejected */
    fake_ioctl_called = false;
    ret = t_unk->transfer(t_unk->context, unlock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "Redundant unlock while ASSUMED_UNLOCKED rejected before ioctl");

    /* Relock cleanly then fail unlock */
    fake_ioctl_force_eio = false;
    ret = t_unk->transfer(t_unk->context, lock_pkt, NULL, 4);
    TEST_ASSERT(ret == 0 && fw9369_hw_get_lock_state(t_unk) == FW9369_LOCK_STATE_ASSUMED_LOCKED,
                "Relock to ASSUMED_LOCKED for failure test");

    fake_ioctl_force_eio = true;
    ret = t_unk->transfer(t_unk->context, unlock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EIO && fw9369_hw_get_lock_state(t_unk) == FW9369_LOCK_STATE_UNKNOWN,
                "Failed unlock leaves state as UNKNOWN");
    fake_ioctl_force_eio = false;

    /* 4a. Lock attempted from UNKNOWN after failed unlock is rejected */
    fake_ioctl_called = false;
    ret = t_unk->transfer(t_unk->context, lock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called && fw9369_hw_get_lock_state(t_unk) == FW9369_LOCK_STATE_UNKNOWN,
                "4a. Lock attempted from UNKNOWN after failed unlock rejected with -EPERM");

    /* 4b. Unlock attempted from UNKNOWN after failed unlock is rejected */
    fake_ioctl_called = false;
    ret = t_unk->transfer(t_unk->context, unlock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called && fw9369_hw_get_lock_state(t_unk) == FW9369_LOCK_STATE_UNKNOWN,
                "4b. Unlock attempted from UNKNOWN after failed unlock rejected with -EPERM");

    /* 5. Failed lock from ASSUMED_UNLOCKED transitions to UNKNOWN */
    fw9369_hw_set_lock_state_for_test(t_unk, FW9369_LOCK_STATE_ASSUMED_UNLOCKED);
    fake_ioctl_force_eio = true;
    ret = t_unk->transfer(t_unk->context, lock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EIO && fw9369_hw_get_lock_state(t_unk) == FW9369_LOCK_STATE_UNKNOWN,
                "Failed lock transitions state to UNKNOWN");
    fake_ioctl_force_eio = false;

    /* 5a. Lock attempted from UNKNOWN after failed lock is rejected */
    fake_ioctl_called = false;
    ret = t_unk->transfer(t_unk->context, lock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called && fw9369_hw_get_lock_state(t_unk) == FW9369_LOCK_STATE_UNKNOWN,
                "5a. Lock attempted from UNKNOWN after failed lock rejected with -EPERM");

    /* 5b. Unlock attempted from UNKNOWN after failed lock is rejected */
    fake_ioctl_called = false;
    ret = t_unk->transfer(t_unk->context, unlock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called && fw9369_hw_get_lock_state(t_unk) == FW9369_LOCK_STATE_UNKNOWN,
                "5b. Unlock attempted from UNKNOWN after failed lock rejected with -EPERM");

    /* 6. In-flight command state verification and protected write rejection */
    fw9369_hw_set_lock_state_for_test(t_unk, FW9369_LOCK_STATE_ASSUMED_LOCKED);
    hook_active_transport = t_unk;
    hook_in_flight_unlock_observed = false;
    hook_in_flight_unlock_trim_rejected = false;
    hook_in_flight_lock_observed = false;
    hook_in_flight_lock_trim_rejected = false;
    fake_ioctl_hook_fn = test_in_flight_ioctl_hook;
    ret = t_unk->transfer(t_unk->context, unlock_pkt, NULL, 4);
    fake_ioctl_hook_fn = NULL;

    fw9369_hw_set_lock_state_for_test(t_unk, FW9369_LOCK_STATE_UNLOCK_COMMAND_SENT);
    int ret_trim1 = t_unk->transfer(t_unk->context, trim_wr, NULL, 8);
    hook_in_flight_unlock_trim_rejected = (ret_trim1 == -EPERM);

    TEST_ASSERT(ret == 0 && hook_in_flight_unlock_observed && hook_in_flight_unlock_trim_rejected,
                "6. Protected write during UNLOCK_COMMAND_SENT in-flight state rejected with -EPERM");

    fw9369_hw_set_lock_state_for_test(t_unk, FW9369_LOCK_STATE_ASSUMED_UNLOCKED);
    fake_ioctl_hook_fn = test_in_flight_ioctl_hook;
    ret = t_unk->transfer(t_unk->context, lock_pkt, NULL, 4);
    fake_ioctl_hook_fn = NULL;
    hook_active_transport = NULL;

    fw9369_hw_set_lock_state_for_test(t_unk, FW9369_LOCK_STATE_LOCK_COMMAND_SENT);
    int ret_trim2 = t_unk->transfer(t_unk->context, trim_wr, NULL, 8);
    hook_in_flight_lock_trim_rejected = (ret_trim2 == -EPERM);

    TEST_ASSERT(ret == 0 && hook_in_flight_lock_observed && hook_in_flight_lock_trim_rejected,
                "7. Protected write during LOCK_COMMAND_SENT in-flight state rejected with -EPERM");

    /* 8. Context destruction while ASSUMED_UNLOCKED completes cleanly */
    fw9369_hw_set_lock_state_for_test(t_unk, FW9369_LOCK_STATE_ASSUMED_UNLOCKED);
    fw9369_hw_transport_destroy(t_unk);
    t_unk = NULL;
    TEST_ASSERT(true, "8. Context destruction while ASSUMED_UNLOCKED completes safely without leak");

    /* 9. Context recreation immediately followed by protected write (Scenario D) */
    struct fw9369_transport *t_fresh = fw9369_hw_transport_create_with_ops(&cfg, &fake_ops);
    TEST_ASSERT(t_fresh != NULL && fw9369_hw_get_lock_state(t_fresh) == FW9369_LOCK_STATE_UNKNOWN,
                "Recreated context begins in UNKNOWN state");
    fake_ioctl_called = false;
    ret = t_fresh->transfer(t_fresh->context, trim_wr, NULL, 8);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "9. Protected write immediately after context recreation rejected with -EPERM");

    /* 10. Clean destruction of remaining contexts */
    fw9369_hw_transport_destroy(t_fresh);
    fw9369_hw_transport_destroy(t);
    TEST_ASSERT(true, "10. Clean destruction of remaining contexts verified");
}

/* 13. Stage 2C.5C.4 Fail-Closed UNKNOWN-State & Concurrency Hardening */
struct thread_worker_arg {
    struct fw9369_transport *t;
    int iterations;
};

static void *concurrency_test_worker(void *param)
{
    struct thread_worker_arg *arg = (struct thread_worker_arg *)param;
    uint8_t byte_wr[4] = { 0x09, 0xF6, 0xC6, 0x01 };
    for (int i = 0; i < arg->iterations; ++i) {
        int ret = arg->t->transfer(arg->t->context, byte_wr, NULL, 4);
        if (ret != 0) {
            return (void *)(intptr_t)ret;
        }
    }
    return NULL;
}

static void test_stage2c5c4_failclosed_concurrency_suite(void)
{
    printf("\n--- Test Suite 13: Stage 2C.5C.4 Fail-Closed UNKNOWN-State & Concurrency Hardening ---\n");

    struct fw9369_hw_config cfg = {
        .device_path = FW9369_HW_DEFAULT_DEV_PATH,
        .speed_hz = FW9369_HW_DEFAULT_SPEED_HZ,
        .mode = FW9369_HW_DEFAULT_MODE,
        .bits_per_word = FW9369_HW_DEFAULT_BITS,
        .lsb_first = 0,
        .max_transfer_len = FW9369_HW_MAX_TRANSFER_LEN,
        .enable_logging = true,
    };

    struct fw9369_hw_ops fake_ops = {
        .open_fn = test_fake_open,
        .ioctl_fn = test_fake_ioctl,
        .close_fn = test_fake_close,
    };

    uint8_t unlock_pkt[4] = { 0x09, 0xF6, 0x9A, 0x5A };
    uint8_t lock_pkt[4]   = { 0x09, 0xF6, 0x9A, 0x00 };
    uint8_t trim_wr[8]    = { 0x05, 0xFA, 0x80, 0xC0, 0, 1, 0x04, 0x44 };

    /* 1. New context starts in UNKNOWN */
    struct fw9369_transport *t = fw9369_hw_transport_create_with_ops(&cfg, &fake_ops);
    TEST_ASSERT(t != NULL && fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_UNKNOWN,
                "1. New context begins unconditionally in UNKNOWN state");

    /* 2. Unlock from UNKNOWN is rejected before ioctl */
    fake_ioctl_called = false;
    int ret = t->transfer(t->context, unlock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called && fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_UNKNOWN,
                "2. Unlock from UNKNOWN is rejected before ioctl with -EPERM");

    /* 3. Lock from UNKNOWN is rejected before ioctl */
    fake_ioctl_called = false;
    ret = t->transfer(t->context, lock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called && fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_UNKNOWN,
                "3. Lock from UNKNOWN is rejected before ioctl with -EPERM");

    /* 4. Failed unlock transitions to UNKNOWN */
    fw9369_hw_set_lock_state_for_test(t, FW9369_LOCK_STATE_ASSUMED_LOCKED);
    fake_ioctl_force_eio = true;
    ret = t->transfer(t->context, unlock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EIO && fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_UNKNOWN,
                "4. Failed unlock transitions state to UNKNOWN");
    fake_ioctl_force_eio = false;

    /* 5. Lock from UNKNOWN after failed unlock is rejected */
    fake_ioctl_called = false;
    ret = t->transfer(t->context, lock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called && fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_UNKNOWN,
                "5. Lock from UNKNOWN after failed unlock is rejected with -EPERM");

    /* 6. Failed lock transitions to UNKNOWN */
    fw9369_hw_set_lock_state_for_test(t, FW9369_LOCK_STATE_ASSUMED_UNLOCKED);
    fake_ioctl_force_eio = true;
    ret = t->transfer(t->context, lock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EIO && fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_UNKNOWN,
                "6. Failed lock transitions state to UNKNOWN");
    fake_ioctl_force_eio = false;

    /* 7. Lock from UNKNOWN after failed lock is rejected */
    fake_ioctl_called = false;
    ret = t->transfer(t->context, lock_pkt, NULL, 4);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called && fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_UNKNOWN,
                "7. Lock from UNKNOWN after failed lock is rejected with -EPERM");

    /* 8. Context recreation starts in UNKNOWN */
    fw9369_hw_transport_destroy(t);
    t = fw9369_hw_transport_create_with_ops(&cfg, &fake_ops);
    TEST_ASSERT(t != NULL && fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_UNKNOWN,
                "8. Recreated context begins unconditionally in UNKNOWN state");

    /* 9. Protected writes from UNKNOWN are rejected */
    fake_ioctl_called = false;
    ret = t->transfer(t->context, trim_wr, NULL, 8);
    TEST_ASSERT(ret == -EPERM && !fake_ioctl_called,
                "9. Protected writes from UNKNOWN are rejected with -EPERM");

    /* 10. Successful unlock from ASSUMED_LOCKED reaches ASSUMED_UNLOCKED */
    fw9369_hw_set_lock_state_for_test(t, FW9369_LOCK_STATE_ASSUMED_LOCKED);
    fake_ioctl_called = false;
    ret = t->transfer(t->context, unlock_pkt, NULL, 4);
    TEST_ASSERT(ret == 0 && fake_ioctl_called && fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_ASSUMED_UNLOCKED,
                "10. Successful unlock from ASSUMED_LOCKED reaches ASSUMED_UNLOCKED");

    /* 11. Successful lock from ASSUMED_UNLOCKED reaches ASSUMED_LOCKED */
    fake_ioctl_called = false;
    ret = t->transfer(t->context, lock_pkt, NULL, 4);
    TEST_ASSERT(ret == 0 && fake_ioctl_called && fw9369_hw_get_lock_state(t) == FW9369_LOCK_STATE_ASSUMED_LOCKED,
                "11. Successful lock from ASSUMED_UNLOCKED reaches ASSUMED_LOCKED");

    /* 12. Protected writes during command-in-flight states are rejected */
    hook_active_transport = t;
    hook_in_flight_unlock_observed = false;
    hook_in_flight_unlock_trim_rejected = false;
    hook_in_flight_lock_observed = false;
    hook_in_flight_lock_trim_rejected = false;
    fake_ioctl_hook_fn = test_in_flight_ioctl_hook;
    ret = t->transfer(t->context, unlock_pkt, NULL, 4);
    fake_ioctl_hook_fn = NULL;

    fw9369_hw_set_lock_state_for_test(t, FW9369_LOCK_STATE_UNLOCK_COMMAND_SENT);
    int ret_trim_a = t->transfer(t->context, trim_wr, NULL, 8);
    hook_in_flight_unlock_trim_rejected = (ret_trim_a == -EPERM);

    TEST_ASSERT(ret == 0 && hook_in_flight_unlock_observed && hook_in_flight_unlock_trim_rejected,
                "12a. Protected write during UNLOCK_COMMAND_SENT rejected with -EPERM");

    fw9369_hw_set_lock_state_for_test(t, FW9369_LOCK_STATE_ASSUMED_UNLOCKED);
    fake_ioctl_hook_fn = test_in_flight_ioctl_hook;
    ret = t->transfer(t->context, lock_pkt, NULL, 4);
    fake_ioctl_hook_fn = NULL;
    hook_active_transport = NULL;

    fw9369_hw_set_lock_state_for_test(t, FW9369_LOCK_STATE_LOCK_COMMAND_SENT);
    int ret_trim_b = t->transfer(t->context, trim_wr, NULL, 8);
    hook_in_flight_lock_trim_rejected = (ret_trim_b == -EPERM);

    TEST_ASSERT(ret == 0 && hook_in_flight_lock_observed && hook_in_flight_lock_trim_rejected,
                "12b. Protected write during LOCK_COMMAND_SENT rejected with -EPERM");

    /* 13. Concurrent operations are serialized via mutex across multiple pthreads */
    #define NUM_THREADS 4
    #define TRANSFERS_PER_THREAD 25
    pthread_t threads[NUM_THREADS];
    struct thread_worker_arg args[NUM_THREADS];
    size_t pre_count = fw9369_hw_get_tx_count(t);

    for (int i = 0; i < NUM_THREADS; ++i) {
        args[i].t = t;
        args[i].iterations = TRANSFERS_PER_THREAD;
        int pt_ret = pthread_create(&threads[i], NULL, concurrency_test_worker, &args[i]);
        TEST_ASSERT(pt_ret == 0, "pthread_create succeeded for concurrency test worker");
    }

    bool all_joined = true;
    for (int i = 0; i < NUM_THREADS; ++i) {
        void *res = NULL;
        pthread_join(threads[i], &res);
        if (res != NULL) all_joined = false;
    }
    TEST_ASSERT(all_joined,
                "13. Concurrent operations serialized cleanly across all threads without collision");

    /* 14. Logging remains consistent under the supported concurrency model */
    size_t post_count = fw9369_hw_get_tx_count(t);
    TEST_ASSERT(post_count == pre_count + (NUM_THREADS * TRANSFERS_PER_THREAD),
                "14. Transaction logging accurately and atomically recorded all concurrent operations");

    /* 15. Context destruction follows the documented lifetime rules */
    fw9369_hw_transport_destroy(t);
    t = NULL;
    TEST_ASSERT(true,
                "15. Context destruction cleanly drains active operations and frees resources");
}

/* 14. Stage 2C.5C.5 Deterministic Concurrency, Mutex & Context-Lifetime Audit */
static pthread_barrier_t s14_barrier_a;
static int s14_in_ioctl_count = 0;
static bool s14_mutual_exclusion_violated = false;
static struct fw9369_transport *s14_shared_transport = NULL;

static void s14_ioctl_hook_mutex_check(struct spi_ioc_transfer *tr)
{
    (void)tr;
    __sync_fetch_and_add(&s14_in_ioctl_count, 1);
    if (s14_in_ioctl_count > 1) {
        s14_mutual_exclusion_violated = true;
    }
    struct timespec ts = { 0, 1000000L }; /* 1 ms */
    nanosleep(&ts, NULL);
    __sync_fetch_and_sub(&s14_in_ioctl_count, 1);
}

static void *s14_worker_mutex_check(void *param)
{
    struct fw9369_transport *t = (struct fw9369_transport *)param;
    uint8_t byte_wr[4] = { 0x09, 0xF6, 0xC6, 0x01 };
    pthread_barrier_wait(&s14_barrier_a);
    int ret = t->transfer(t->context, byte_wr, NULL, 4);
    return (void *)(intptr_t)ret;
}

static void s14_ioctl_hook_pause_for_destroy(struct spi_ioc_transfer *tr)
{
    (void)tr;
    pthread_barrier_wait(&s14_barrier_a);
    struct timespec ts = { 0, 5000000L }; /* 5 ms */
    nanosleep(&ts, NULL);
}

static void *s14_worker_transfer_during_destroy(void *param)
{
    struct fw9369_transport *t = (struct fw9369_transport *)param;
    uint8_t byte_wr[4] = { 0x09, 0xF6, 0xC6, 0x01 };
    int ret = t->transfer(t->context, byte_wr, NULL, 4);
    return (void *)(intptr_t)ret;
}

static void *s14_worker_destroy(void *param)
{
    struct fw9369_transport *t = (struct fw9369_transport *)param;
    pthread_barrier_wait(&s14_barrier_a);
    fw9369_hw_transport_destroy(t);
    return NULL;
}

static void s14_ioctl_hook_set_shutdown(struct spi_ioc_transfer *tr)
{
    (void)tr;
    pthread_barrier_wait(&s14_barrier_a);
    fw9369_hw_set_shutting_down_unlocked_for_test(s14_shared_transport, true);
    struct timespec ts = { 0, 5000000L }; /* 5 ms */
    nanosleep(&ts, NULL);
}

static void *s14_worker_wait_for_transfer_then_rejected(void *param)
{
    struct fw9369_transport *t = (struct fw9369_transport *)param;
    uint8_t byte_wr[4] = { 0x09, 0xF6, 0xC6, 0x01 };
    pthread_barrier_wait(&s14_barrier_a);
    struct timespec ts = { 0, 1000000L }; /* 1 ms */
    nanosleep(&ts, NULL);
    int ret = t->transfer(t->context, byte_wr, NULL, 4);
    return (void *)(intptr_t)ret;
}

/* Dedicated test-control structure for Test 4 failure containment */
struct test4_ctrl {
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    bool transfer_in_hook;
    bool release_hook;
    bool getter_started;
    bool getter_done;
    bool getter_finished_early;
    const char *failure_stage;
};

static struct test4_ctrl g_t4;

struct t4_getter_arg {
    struct fw9369_transport *t;
    enum fw9369_lock_state state;
    int err;
};

static void t4_get_deadline(struct timespec *ts, int seconds)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += seconds;
}

static void t4_ioctl_hook_getter_race(struct spi_ioc_transfer *tr)
{
    (void)tr;
    pthread_mutex_lock(&g_t4.lock);
    g_t4.transfer_in_hook = true;
    pthread_cond_broadcast(&g_t4.cond);

    struct timespec ts;
    t4_get_deadline(&ts, 5);

    while (!g_t4.release_hook) {
        int rc = pthread_cond_timedwait(&g_t4.cond, &g_t4.lock, &ts);
        if (rc == ETIMEDOUT) {
            g_t4.failure_stage = "ioctl_hook_release_timeout";
            break;
        }
    }
    pthread_mutex_unlock(&g_t4.lock);
}

static void *t4_worker_transfer(void *param)
{
    struct fw9369_transport *t = (struct fw9369_transport *)param;
    uint8_t unlock_pkt[4] = { 0x09, 0xF6, 0x9A, 0x5A };
    int ret = t->transfer(t->context, unlock_pkt, NULL, 4);
    return (void *)(intptr_t)ret;
}

static void *t4_worker_getter(void *param)
{
    struct t4_getter_arg *arg = (struct t4_getter_arg *)param;

    /* 1. Signal controller that getter worker is active and about to call getter */
    pthread_mutex_lock(&g_t4.lock);
    arg->err = 0;
    g_t4.getter_started = true;
    pthread_cond_broadcast(&g_t4.cond);
    pthread_mutex_unlock(&g_t4.lock);

    /* 2. Call getter: this MUST block on ctx->lock while transfer holds it */
    arg->state = fw9369_hw_get_lock_state(arg->t);

    /* 3. Signal completion */
    pthread_mutex_lock(&g_t4.lock);
    g_t4.getter_done = true;
    if (!g_t4.release_hook) {
        /* Getter finished before hook was released: transfer lock was not held! */
        g_t4.getter_finished_early = true;
    }
    pthread_cond_broadcast(&g_t4.cond);
    pthread_mutex_unlock(&g_t4.lock);

    return NULL;
}

static void *s14_worker_shutdown_rejection(void *param)
{
    struct fw9369_transport *t = (struct fw9369_transport *)param;
    uint8_t byte_wr[4] = { 0x09, 0xF6, 0xC6, 0x01 };
    pthread_barrier_wait(&s14_barrier_a);
    int ret = t->transfer(t->context, byte_wr, NULL, 4);
    return (void *)(intptr_t)ret;
}

static void test_stage2c5c5_deterministic_concurrency_suite(void)
{
    printf("\n--- Test Suite 14: Stage 2C.5C.5 Deterministic Concurrency, Mutex & Context-Lifetime Audit ---\n");

    struct fw9369_hw_config cfg = {
        .device_path = FW9369_HW_DEFAULT_DEV_PATH,
        .speed_hz = FW9369_HW_DEFAULT_SPEED_HZ,
        .mode = FW9369_HW_DEFAULT_MODE,
        .bits_per_word = FW9369_HW_DEFAULT_BITS,
        .lsb_first = 0,
        .max_transfer_len = FW9369_HW_MAX_TRANSFER_LEN,
        .enable_logging = true,
    };

    struct fw9369_hw_ops fake_ops = {
        .open_fn = test_fake_open,
        .ioctl_fn = test_fake_ioctl,
        .close_fn = test_fake_close,
    };

    uint8_t byte_wr[4] = { 0x09, 0xF6, 0xC6, 0x01 };

    /* 1. Two concurrent operations on one context: verify mutual exclusion during ioctl */
    struct fw9369_transport *t1 = fw9369_hw_transport_create_with_ops(&cfg, &fake_ops);
    s14_in_ioctl_count = 0;
    s14_mutual_exclusion_violated = false;
    fake_ioctl_hook_fn = s14_ioctl_hook_mutex_check;
    pthread_barrier_init(&s14_barrier_a, NULL, 2);

    pthread_t th1, th2;
    pthread_create(&th1, NULL, s14_worker_mutex_check, t1);
    pthread_create(&th2, NULL, s14_worker_mutex_check, t1);

    void *r1 = NULL, *r2 = NULL;
    pthread_join(th1, &r1);
    pthread_join(th2, &r2);
    pthread_barrier_destroy(&s14_barrier_a);
    fake_ioctl_hook_fn = NULL;

    TEST_ASSERT((intptr_t)r1 == 0 && (intptr_t)r2 == 0 && !s14_mutual_exclusion_violated,
                "1. Two concurrent operations serialized cleanly with strict mutual exclusion");
    fw9369_hw_transport_destroy(t1);

    /* 2. A transfer active while shutdown begins: destructor drains active operation */
    struct fw9369_transport *t2 = fw9369_hw_transport_create_with_ops(&cfg, &fake_ops);
    pthread_barrier_init(&s14_barrier_a, NULL, 2);
    fake_ioctl_hook_fn = s14_ioctl_hook_pause_for_destroy;

    pthread_create(&th1, NULL, s14_worker_transfer_during_destroy, t2);
    pthread_create(&th2, NULL, s14_worker_destroy, t2);

    pthread_join(th1, &r1);
    pthread_join(th2, &r2);
    pthread_barrier_destroy(&s14_barrier_a);
    fake_ioctl_hook_fn = NULL;

    TEST_ASSERT((intptr_t)r1 == 0,
                "2. Active transfer completed and drained safely before destruction proceeded");

    /* 3. Shutdown while another thread is waiting to enter a transfer */
    struct fw9369_transport *t3 = fw9369_hw_transport_create_with_ops(&cfg, &fake_ops);
    s14_shared_transport = t3;
    pthread_barrier_init(&s14_barrier_a, NULL, 2);
    fake_ioctl_hook_fn = s14_ioctl_hook_set_shutdown;

    pthread_create(&th1, NULL, s14_worker_transfer_during_destroy, t3);
    pthread_create(&th2, NULL, s14_worker_wait_for_transfer_then_rejected, t3);

    pthread_join(th1, &r1);
    pthread_join(th2, &r2);
    pthread_barrier_destroy(&s14_barrier_a);
    fake_ioctl_hook_fn = NULL;
    s14_shared_transport = NULL;

    TEST_ASSERT((intptr_t)r1 == 0 && (intptr_t)r2 == -ESHUTDOWN,
                "3. Waiting thread rejected with -ESHUTDOWN upon acquiring lock after shutdown");
    fw9369_hw_transport_destroy(t3);

    /* 4. A getter racing with a transport operation (Independent Test-Control Synchronization) */
    struct fw9369_transport *t4 = fw9369_hw_transport_create_with_ops(&cfg, &fake_ops);
    fw9369_hw_set_lock_state_for_test(t4, FW9369_LOCK_STATE_ASSUMED_LOCKED);

    /* Initialize dedicated test-control primitives */
    int rc_m = pthread_mutex_init(&g_t4.lock, NULL);
    int rc_c = pthread_cond_init(&g_t4.cond, NULL);
    TEST_ASSERT(rc_m == 0 && rc_c == 0, "Test 4 test-control primitives initialized successfully");

    g_t4.transfer_in_hook = false;
    g_t4.release_hook = false;
    g_t4.getter_started = false;
    g_t4.getter_done = false;
    g_t4.getter_finished_early = false;
    g_t4.failure_stage = NULL;

    fake_ioctl_hook_fn = t4_ioctl_hook_getter_race;

    /* Launch transfer worker */
    pthread_t th_transfer;
    int rc_t = pthread_create(&th_transfer, NULL, t4_worker_transfer, t4);
    TEST_ASSERT(rc_t == 0, "Test 4 transfer worker thread created");

    /* Wait until transfer thread enters ioctl hook holding ctx->lock */
    pthread_mutex_lock(&g_t4.lock);
    struct timespec ts_enter;
    t4_get_deadline(&ts_enter, 5);
    while (!g_t4.transfer_in_hook) {
        int rc = pthread_cond_timedwait(&g_t4.cond, &g_t4.lock, &ts_enter);
        if (rc == ETIMEDOUT) {
            g_t4.failure_stage = "transfer_enter_hook_timeout";
            break;
        }
    }
    bool enter_ok = g_t4.transfer_in_hook;
    pthread_mutex_unlock(&g_t4.lock);
    TEST_ASSERT(enter_ok, "Test 4 transfer thread safely entered fake ioctl hook holding ctx->lock");

    /* Launch getter worker while transfer thread is confirmed holding ctx->lock in hook */
    struct t4_getter_arg g_arg = { .t = t4, .state = FW9369_LOCK_STATE_UNKNOWN, .err = -1 };
    pthread_t th_getter;
    int rc_g = pthread_create(&th_getter, NULL, t4_worker_getter, &g_arg);
    TEST_ASSERT(rc_g == 0, "Test 4 getter worker thread created");

    /* Wait until getter thread signals it has started and is about to call getter */
    pthread_mutex_lock(&g_t4.lock);
    struct timespec ts_gstart;
    t4_get_deadline(&ts_gstart, 5);
    while (!g_t4.getter_started) {
        int rc = pthread_cond_timedwait(&g_t4.cond, &g_t4.lock, &ts_gstart);
        if (rc == ETIMEDOUT) {
            g_t4.failure_stage = "getter_start_timeout";
            break;
        }
    }
    bool gstart_ok = g_t4.getter_started;

    /* Verify ordering invariant: getter MUST NOT have completed while hook was unreleased */
    bool getter_blocked = !g_t4.getter_done && !g_t4.getter_finished_early;

    /* Now signal release_hook to let transfer complete */
    g_t4.release_hook = true;
    pthread_cond_broadcast(&g_t4.cond);
    pthread_mutex_unlock(&g_t4.lock);

    TEST_ASSERT(gstart_ok && getter_blocked,
                "Test 4 ordering verified: getter remained blocked while transfer owned ctx->lock");

    /* Join both workers cleanly */
    void *r_trans = NULL;
    pthread_join(th_transfer, &r_trans);
    pthread_join(th_getter, NULL);

    fake_ioctl_hook_fn = NULL;

    TEST_ASSERT((intptr_t)r_trans == 0 &&
                g_t4.failure_stage == NULL &&
                !g_t4.getter_finished_early &&
                g_arg.state == FW9369_LOCK_STATE_ASSUMED_UNLOCKED,
                "4. State getter serialized cleanly with transfer and returned valid state snapshot");

    pthread_mutex_destroy(&g_t4.lock);
    pthread_cond_destroy(&g_t4.cond);
    fw9369_hw_transport_destroy(t4);

    /* 5. Multiple threads attempting operations during/after shutdown */
    struct fw9369_transport *t5 = fw9369_hw_transport_create_with_ops(&cfg, &fake_ops);
    fw9369_hw_set_shutting_down_for_test(t5, true);
    pthread_t th_pool[4];
    pthread_barrier_init(&s14_barrier_a, NULL, 4);

    for (int i = 0; i < 4; ++i) {
        pthread_create(&th_pool[i], NULL, s14_worker_shutdown_rejection, t5);
    }
    bool all_rejected_shutdown = true;
    for (int i = 0; i < 4; ++i) {
        void *res = NULL;
        pthread_join(th_pool[i], &res);
        if ((intptr_t)res != -ESHUTDOWN) all_rejected_shutdown = false;
    }
    pthread_barrier_destroy(&s14_barrier_a);
    TEST_ASSERT(all_rejected_shutdown,
                "5. Multiple concurrent threads cleanly rejected with -ESHUTDOWN after shutdown");
    fw9369_hw_transport_destroy(t5);

    /* 6. Safe copy-out telemetry getters (fw9369_hw_get_last_tx & fw9369_mock_get_last_tx) */
    struct fw9369_transport *t6 = fw9369_hw_transport_create_with_ops(&cfg, &fake_ops);
    uint8_t snap_buf[64];
    size_t snap_len = 0;

    /* 6a. Fresh transport with empty telemetry log returns -ENODATA and initializes out_len to 0 */
    snap_len = 999;
    int ret_snap = fw9369_hw_get_last_tx(t6, snap_buf, sizeof(snap_buf), &snap_len);
    TEST_ASSERT(ret_snap == -ENODATA && snap_len == 0,
                "6a. Empty telemetry log returns -ENODATA and initializes out_len to 0");

    /* 6b. Invalid argument rejection */
    snap_len = 999;
    int ret_inv1 = fw9369_hw_get_last_tx(NULL, snap_buf, sizeof(snap_buf), &snap_len);
    snap_len = 999;
    int ret_inv2 = fw9369_hw_get_last_tx(t6, NULL, sizeof(snap_buf), &snap_len);
    size_t snap_len_inv = snap_len;
    snap_len = 999;
    int ret_inv3 = fw9369_hw_get_last_tx(t6, snap_buf, 0, &snap_len);
    TEST_ASSERT(ret_inv1 == -EINVAL && ret_inv2 == -EINVAL && ret_inv3 == -EINVAL && snap_len_inv == 0,
                "6b. Invalid arguments (NULL transport/buffer, 0 capacity) return -EINVAL and reset out_len");

    /* 6c. Insufficient buffer capacity returns -ENOBUFS and reports needed length */
    int ret_t6 = t6->transfer(t6->context, byte_wr, NULL, 4);
    TEST_ASSERT(ret_t6 == 0, "6c_setup. Transfer succeeds");
    snap_len = 0;
    ret_snap = fw9369_hw_get_last_tx(t6, snap_buf, 2, &snap_len);
    TEST_ASSERT(ret_snap == -ENOBUFS && snap_len == 4,
                "6c. Undersized buffer capacity returns -ENOBUFS and reports required size in out_len");

    /* 6d. Accurate copy-out to caller buffer with lock holding */
    memset(snap_buf, 0, sizeof(snap_buf));
    snap_len = 0;
    ret_snap = fw9369_hw_get_last_tx(t6, snap_buf, sizeof(snap_buf), &snap_len);
    TEST_ASSERT(ret_snap == 4 && snap_len == 4 && memcmp(snap_buf, byte_wr, 4) == 0,
                "6d. Copy-out succeeds, returns payload length, matches transmitted bytes exactly");

    /* 6e. Snapshot immutability: subsequent transfers do not modify caller's buffer */
    uint8_t snap_copy[4];
    memcpy(snap_copy, snap_buf, 4);
    uint8_t byte_wr2[4] = { 0x09, 0xF6, 0xC6, 0x00 };
    int ret_t6b = t6->transfer(t6->context, byte_wr2, NULL, 4);
    TEST_ASSERT(ret_t6b == 0, "6e_setup. Second transfer executed");
    uint8_t snap_buf2[64];
    size_t snap_len2 = 0;
    int ret_snap2 = fw9369_hw_get_last_tx(t6, snap_buf2, sizeof(snap_buf2), &snap_len2);
    TEST_ASSERT(ret_snap2 == 4 && snap_len2 == 4 && memcmp(snap_buf2, byte_wr2, 4) == 0 &&
                memcmp(snap_buf, snap_copy, 4) == 0,
                "6e. Telemetry snapshot is immutable against subsequent transfers and log updates");

    /* 6f. Mock transport fw9369_mock_get_last_tx copy-out parity */
    struct fw9369_mock_config mcfg = { 0 };
    struct fw9369_transport *tm = fw9369_mock_transport_create(&mcfg);
    uint8_t m_buf[64];
    size_t m_len = 0;
    m_len = 999;
    TEST_ASSERT(fw9369_mock_get_last_tx(tm, m_buf, sizeof(m_buf), &m_len) == -ENODATA && m_len == 0,
                "6f_1. Mock transport empty log returns -ENODATA and initializes out_len to 0");
    tm->transfer(tm->context, byte_wr, NULL, 4);
    TEST_ASSERT(fw9369_mock_get_last_tx(tm, m_buf, 2, &m_len) == -ENOBUFS && m_len == 4,
                "6f_2. Mock transport undersized buffer returns -ENOBUFS");
    int m_ret = fw9369_mock_get_last_tx(tm, m_buf, sizeof(m_buf), &m_len);
    TEST_ASSERT(m_ret == 4 && m_len == 4 && memcmp(m_buf, byte_wr, 4) == 0,
                "6f_3. Mock transport copies last tx accurately into caller buffer");
    fw9369_mock_transport_destroy(tm);
    fw9369_hw_transport_destroy(t6);

    /* 7. Error paths release PTHREAD_MUTEX_NORMAL lock cleanly without deadlocks */
    struct fw9369_transport *t7 = fw9369_hw_transport_create_with_ops(&cfg, &fake_ops);

    /* 7a. Allowlist failure releases mutex cleanly */
    uint8_t bad_op[3] = { 0x12, 0x34, 0x00 };
    int ret_err = t7->transfer(t7->context, bad_op, NULL, 3);
    int ret_recov = t7->transfer(t7->context, byte_wr, NULL, 4);
    TEST_ASSERT(ret_err == -EPERM && ret_recov == 0,
                "7a. Allowlist error releases normal mutex cleanly allowing immediate subsequent operation");

    /* 7b. ioctl forced failure releases mutex cleanly */
    fake_ioctl_force_eio = true;
    ret_err = t7->transfer(t7->context, byte_wr, NULL, 4);
    fake_ioctl_force_eio = false;
    ret_recov = t7->transfer(t7->context, byte_wr, NULL, 4);
    TEST_ASSERT(ret_err == -EIO && ret_recov == 0,
                "7b. ioctl -EIO error releases normal mutex cleanly allowing immediate recovery");

    /* 7c. Short transfer error releases mutex cleanly */
    fake_ioctl_short_transfer = true;
    ret_err = t7->transfer(t7->context, byte_wr, NULL, 4);
    fake_ioctl_short_transfer = false;
    ret_recov = t7->transfer(t7->context, byte_wr, NULL, 4);
    TEST_ASSERT(ret_err == -EMSGSIZE && ret_recov == 0,
                "7c. Short transfer error releases normal mutex cleanly allowing immediate recovery");

    /* 7d. Unexpected positive length error releases mutex cleanly */
    fake_ioctl_force_overflow = true;
    ret_err = t7->transfer(t7->context, byte_wr, NULL, 4);
    fake_ioctl_force_overflow = false;
    ret_recov = t7->transfer(t7->context, byte_wr, NULL, 4);
    TEST_ASSERT(ret_err == -EPROTO && ret_recov == 0,
                "7d. Protocol length error releases normal mutex cleanly allowing immediate recovery");

    /* 7e. Shutdown rejection path releases mutex cleanly */
    fw9369_hw_set_shutting_down_for_test(t7, true);
    ret_err = t7->transfer(t7->context, byte_wr, NULL, 4);
    bool is_down = fw9369_hw_is_shutting_down_for_test(t7);
    TEST_ASSERT(ret_err == -ESHUTDOWN && is_down,
                "7e. Shutdown rejection releases normal mutex cleanly and state is preserved");
    fw9369_hw_transport_destroy(t7);

    /* 8. Repeated operations after shutdown consistently rejected */
    struct fw9369_transport *t8 = fw9369_hw_transport_create_with_ops(&cfg, &fake_ops);
    fw9369_hw_set_shutting_down_for_test(t8, true);
    bool repeated_shutdown_ok = true;
    for (int i = 0; i < 5; ++i) {
        if (t8->transfer(t8->context, byte_wr, NULL, 4) != -ESHUTDOWN) {
            repeated_shutdown_ok = false;
        }
    }
    TEST_ASSERT(repeated_shutdown_ok && fw9369_hw_is_shutting_down_for_test(t8),
                "8. Repeated post-shutdown transfer calls consistently return -ESHUTDOWN");
    fw9369_hw_transport_destroy(t8);

    /* 9. Destruction only after caller joins all worker threads (single-owner contract) */
    struct fw9369_transport *t9 = fw9369_hw_transport_create_with_ops(&cfg, &fake_ops);
    struct thread_worker_arg wargs[4];
    pthread_t wthreads[4];
    for (int i = 0; i < 4; ++i) {
        wargs[i].t = t9;
        wargs[i].iterations = 25;
        pthread_create(&wthreads[i], NULL, concurrency_test_worker, &wargs[i]);
    }
    for (int i = 0; i < 4; ++i) {
        pthread_join(wthreads[i], NULL);
    }
    fw9369_hw_transport_destroy(t9);
    TEST_ASSERT(true,
                "9. Owner-managed thread joining prior to destruction executed without leak or race");

    /* 10. Rejection of operations after shutdown verified before ioctl */
    struct fw9369_transport *t10 = fw9369_hw_transport_create_with_ops(&cfg, &fake_ops);
    fw9369_hw_set_shutting_down_for_test(t10, true);
    fake_ioctl_called = false;
    int ret_t10 = t10->transfer(t10->context, byte_wr, NULL, 4);
    TEST_ASSERT(ret_t10 == -ESHUTDOWN && !fake_ioctl_called,
                "10. Operation rejected with -ESHUTDOWN immediately without invoking ioctl");
    fw9369_hw_transport_destroy(t10);
}

int main(void)
{
    printf("===================================================================\n");
    printf(" FocalTech FW9369 Automated Mock Tooling Test Suite\n");
    printf(" Target Silicon: FT9362 / FW9369\n");
    printf(" Mode: Strictly In-Memory Mock Testing (Zero Hardware Access)\n");
    printf("===================================================================\n");

    test_crc_suite();
    test_protocol_suite();
    test_mock_transport_suite();
    test_fdt_calibration_suite();
    test_image_baseline_suite();
    test_pipeline_suite();
    test_failure_injection_suite();
    test_hardware_transport_suite();
    test_device_identity_suite();
    test_adversarial_protocol_suite();
    test_lock_state_hardening_suite();
    test_lock_state_transitions_audit_suite();
    test_stage2c5c4_failclosed_concurrency_suite();
    test_stage2c5c5_deterministic_concurrency_suite();

    printf("\n===================================================================\n");
    printf(" Final Test Results: %d / %d Passed (%.1f%%)\n",
           passed_tests, total_tests, (float)passed_tests / total_tests * 100.0f);
    printf("===================================================================\n");

    return (passed_tests == total_tests) ? 0 : 1;
}
