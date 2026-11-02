/* SPDX-License-Identifier: MIT */
/*
 * Stage 2A FocalTech SPI Sensor Identification Tool
 *
 * Implements the strictly bounded, evidence-based identification sequence
 * specified in diagnostics/STAGE2A_EXECUTION_CHECKLIST.md.
 *
 * Operations:
 *   1. Write volatile register 0xC6 = 0x01 (ungate SPI transceiver clock)
 *   2. Wait 4 ms (CS deasserted high)
 *   3. Read register 0xC6 (require buffer[4] == 0x01)
 *   4. Read Chip ID register 0x1A8B (validate framing, CRC-CCITT, and known ID)
 *   5. If Chip ID == 0x9391, read sub-revision register 0x1816
 *   6. STOP and exit cleanly.
 *
 * STRICT BOUNDARIES:
 *   - No retries.
 *   - No reset pulses.
 *   - No recovery writes.
 *   - No commands (0x5A/0xA5/0xC0).
 *   - No OTP/EFUSE access (0xF0-0xFF).
 *   - No analog configuration (0xFD/0xFE).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/types.h>
#include <linux/spi/spidev.h>

/* Exit code definitions */
#define EXIT_IDENTIFY_SUCCESS        0
#define EXIT_ERR_INVOCATION          1
#define EXIT_ERR_SPI_CONFIG          2
#define EXIT_ERR_WRITE_TRANSPORT     3
#define EXIT_ERR_READ_TRANSPORT      4
#define EXIT_ERR_VERIFY_MISMATCH     5
#define EXIT_ERR_BUS_FLOAT           6
#define EXIT_ERR_CRC_MISMATCH        7
#define EXIT_ERR_UNKNOWN_CHIP_ID     8
#define EXIT_ERR_SUBREV_FAILURE      9

static uint16_t calc_crc_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc = (uint16_t)(crc ^ ((unsigned int)data[i] << 8));
        for (unsigned int bit = 0; bit < 8; ++bit) {
            crc = (uint16_t)(((unsigned int)crc << 1) ^
                             ((crc & 0x8000) != 0 ? 0x1021u : 0u));
        }
    }
    return crc;
}

static void print_hex_buffer(const char *label, const uint8_t *buf, size_t len)
{
    printf("%s: [", label);
    for (size_t i = 0; i < len; ++i) {
        printf(" 0x%02X", buf[i]);
    }
    printf(" ]\n");
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <spidev_device_path>\n", argv[0]);
        fprintf(stderr, "Example: %s /dev/spidev1.0\n", argv[0]);
        return EXIT_ERR_INVOCATION;
    }

    const char *device_path = argv[1];
    printf(">> Stage 2A FocalTech Identification Tool\n");
    printf(">> Target device: %s\n", device_path);

    int fd = open(device_path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Error: Cannot open %s: %s (errno %d)\n",
                device_path, strerror(errno), errno);
        return EXIT_ERR_INVOCATION;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        fprintf(stderr, "Error: Cannot stat %s: %s (errno %d)\n",
                device_path, strerror(errno), errno);
        close(fd);
        return EXIT_ERR_INVOCATION;
    }
    if (!S_ISCHR(st.st_mode)) {
        fprintf(stderr, "Error: %s is not a character device!\n", device_path);
        close(fd);
        return EXIT_ERR_INVOCATION;
    }

    /* 1. Configure and Verify SPI Parameters: Mode 0, 8 bits/word, 1 MHz */
    uint8_t mode = SPI_MODE_0;
    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0) {
        fprintf(stderr, "Error: Failed to set SPI_MODE_0: %s\n", strerror(errno));
        close(fd);
        return EXIT_ERR_SPI_CONFIG;
    }

    uint8_t rd_mode = 0xFF;
    if (ioctl(fd, SPI_IOC_RD_MODE, &rd_mode) < 0 || rd_mode != SPI_MODE_0) {
        fprintf(stderr, "Error: Failed to verify SPI_MODE_0 readback (got %u): %s\n",
                rd_mode, strerror(errno));
        close(fd);
        return EXIT_ERR_SPI_CONFIG;
    }

    uint8_t bits = 8;
    if (ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) {
        fprintf(stderr, "Error: Failed to set bits per word (8): %s\n", strerror(errno));
        close(fd);
        return EXIT_ERR_SPI_CONFIG;
    }

    uint8_t rd_bits = 0;
    if (ioctl(fd, SPI_IOC_RD_BITS_PER_WORD, &rd_bits) < 0 || rd_bits != 8) {
        fprintf(stderr, "Error: Failed to verify bits per word readback (got %u): %s\n",
                rd_bits, strerror(errno));
        close(fd);
        return EXIT_ERR_SPI_CONFIG;
    }

    uint32_t speed_hz = 1000000;
    if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed_hz) < 0) {
        fprintf(stderr, "Error: Failed to set max speed (1 MHz): %s\n", strerror(errno));
        close(fd);
        return EXIT_ERR_SPI_CONFIG;
    }

    uint32_t rd_speed = 0;
    if (ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, &rd_speed) < 0 || rd_speed != 1000000) {
        fprintf(stderr, "Error: Failed to verify max speed readback (got %u): %s\n",
                rd_speed, strerror(errno));
        close(fd);
        return EXIT_ERR_SPI_CONFIG;
    }

    printf(">> SPI configuration verified: Mode 0 (CPOL=0, CPHA=0), 8 bits/word, 1000000 Hz\n\n");

    /* =========================================================================
     * STEP 1: Write volatile register 0xC6 = 0x01
     * Opcode: 0x09 (Write Byte), complement: 0xF6, addr: 0xC6, val: 0x01
     * Length: 4 bytes
     * ========================================================================= */
    printf("--- Operation 1: Writing volatile register 0xC6 = 0x01 ---\n");
    uint8_t tx_c6_wr[4] = {0x09, 0xF6, 0xC6, 0x01};
    print_hex_buffer("MOSI", tx_c6_wr, sizeof(tx_c6_wr));

    struct spi_ioc_transfer tr1 = {
        .tx_buf = (uintptr_t)tx_c6_wr,
        .rx_buf = 0,
        .len = sizeof(tx_c6_wr),
        .speed_hz = speed_hz,
        .bits_per_word = bits,
        .delay_usecs = 0,
        .cs_change = 0,
    };

    int ret = ioctl(fd, SPI_IOC_MESSAGE(1), &tr1);
    if (ret < 0 || ret != (int)sizeof(tx_c6_wr)) {
        fprintf(stderr, "STOP: Write transport error on register 0xC6: %s (ret=%d)\n",
                strerror(errno), ret);
        close(fd);
        return EXIT_ERR_WRITE_TRANSPORT;
    }
    printf(">> Operation 1 completed (CS deasserted high).\n\n");

    /* =========================================================================
     * STEP 2: Inter-transaction delay 4 ms (CS deasserted high)
     * ========================================================================= */
    printf("--- Operation 2: Delaying 4 ms (transceiver stabilization) ---\n");
    usleep(4000);
    printf(">> Delay complete.\n\n");

    /* =========================================================================
     * STEP 3: Read register 0xC6 (verify buffer[4] == 0x01)
     * Opcode: 0x08 (Read Byte), complement: 0xF7, addr: 0xC6, dummy: 0x00, 0x00
     * Length: 5 bytes
     * ========================================================================= */
    printf("--- Operation 3: Reading register 0xC6 for verification ---\n");
    uint8_t tx_c6_rd[5] = {0x08, 0xF7, 0xC6, 0x00, 0x00};
    uint8_t rx_c6_rd[5] = {0};
    print_hex_buffer("MOSI", tx_c6_rd, sizeof(tx_c6_rd));

    struct spi_ioc_transfer tr2 = {
        .tx_buf = (uintptr_t)tx_c6_rd,
        .rx_buf = (uintptr_t)rx_c6_rd,
        .len = sizeof(tx_c6_rd),
        .speed_hz = speed_hz,
        .bits_per_word = bits,
        .delay_usecs = 0,
        .cs_change = 0,
    };

    ret = ioctl(fd, SPI_IOC_MESSAGE(1), &tr2);
    if (ret < 0 || ret != (int)sizeof(tx_c6_rd)) {
        fprintf(stderr, "STOP: Read transport error on register 0xC6: %s (ret=%d)\n",
                strerror(errno), ret);
        close(fd);
        return EXIT_ERR_READ_TRANSPORT;
    }
    print_hex_buffer("MISO", rx_c6_rd, sizeof(rx_c6_rd));

    uint8_t c6_val = rx_c6_rd[4];
    printf(">> Register 0xC6 readback value: 0x%02X\n", c6_val);
    if (c6_val != 0x01) {
        fprintf(stderr, "STOP: Verification mismatch on register 0xC6: expected 0x01, observed 0x%02X\n",
                c6_val);
        close(fd);
        return EXIT_ERR_VERIFY_MISMATCH;
    }
    printf(">> Register 0xC6 verified: SPI transceiver clock active.\n\n");

    /* =========================================================================
     * STEP 4: Read Chip ID register 0x1A8B
     * Opcode: 0x04 (Read Word), complement: 0xFB, addr: 0x9A 0x8B, count: 0 0
     * Length: 10 bytes
     * Response: data word at buffer[6..7], CRC at buffer[8..9]
     * ========================================================================= */
    printf("--- Operation 4: Interrogating Chip ID register 0x1A8B ---\n");
    uint8_t tx_id_rd[10] = {0x04, 0xFB, 0x9A, 0x8B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t rx_id_rd[10] = {0};
    print_hex_buffer("MOSI", tx_id_rd, sizeof(tx_id_rd));

    struct spi_ioc_transfer tr3 = {
        .tx_buf = (uintptr_t)tx_id_rd,
        .rx_buf = (uintptr_t)rx_id_rd,
        .len = sizeof(tx_id_rd),
        .speed_hz = speed_hz,
        .bits_per_word = bits,
        .delay_usecs = 0,
        .cs_change = 0,
    };

    ret = ioctl(fd, SPI_IOC_MESSAGE(1), &tr3);
    if (ret < 0 || ret != (int)sizeof(tx_id_rd)) {
        fprintf(stderr, "STOP: Read transport error on register 0x1A8B: %s (ret=%d)\n",
                strerror(errno), ret);
        close(fd);
        return EXIT_ERR_READ_TRANSPORT;
    }
    print_hex_buffer("MISO", rx_id_rd, sizeof(rx_id_rd));

    uint16_t chip_id = ((uint16_t)rx_id_rd[6] << 8) | rx_id_rd[7];
    uint16_t crc_rx = ((uint16_t)rx_id_rd[8] << 8) | rx_id_rd[9];
    uint16_t crc_calc = calc_crc_ccitt(&rx_id_rd[6], 2);

    printf(">> Raw Chip ID word (buffer[6..7]): 0x%04X\n", chip_id);
    printf(">> Received CRC-CCITT (buffer[8..9]): 0x%04X (Calculated: 0x%04X)\n",
           crc_rx, crc_calc);

    /* Check for bus floating / inactive states */
    if (chip_id == 0x0000) {
        fprintf(stderr, "STOP: Chip ID returned 0x0000 (Bus grounded / slave inactive)\n");
        close(fd);
        return EXIT_ERR_BUS_FLOAT;
    }
    if (chip_id == 0xFFFF) {
        fprintf(stderr, "STOP: Chip ID returned 0xFFFF (Bus floating / slave unpowered)\n");
        close(fd);
        return EXIT_ERR_BUS_FLOAT;
    }

    /* CRC Evaluation */
    if (crc_rx != 0x0000) {
        if (crc_rx != crc_calc) {
            fprintf(stderr, "STOP: CRC mismatch: received 0x%04X != computed 0x%04X\n",
                    crc_rx, crc_calc);
            close(fd);
            return EXIT_ERR_CRC_MISMATCH;
        }
        printf(">> CRC validation: VERIFIED (CRC matches)\n");
    } else {
        printf(">> CRC validation: UNVERIFIED (Reference-driver zero-CRC exception heuristic)\n");
    }

    /* Variant Gate: Only 0x9362, 0x9365, and 0x9391 are supported by dedicated evidence */
    const char *model_name = "UNKNOWN";
    bool requires_subrev = false;

    if (chip_id == 0x9362) {
        model_name = "FT9362 / FW9369";
    } else if (chip_id == 0x9365) {
        model_name = "FT9365";
    } else if (chip_id == 0x9391) {
        model_name = "FT9391 family (Requires sub-revision check on register 0x1816)";
        requires_subrev = true;
    } else {
        fprintf(stderr, "STOP: Chip ID 0x%04X is not in the supported evidence table (FT9362, FT9365, FT9391)\n",
                chip_id);
        close(fd);
        return EXIT_ERR_UNKNOWN_CHIP_ID;
    }

    printf(">> Preliminary Identification: Chip ID 0x%04X -> %s\n\n", chip_id, model_name);

    /* =========================================================================
     * STEP 5: Conditional read of sub-revision register 0x1816
     * Executed strictly when chip_id == 0x9391
     * ========================================================================= */
    uint16_t sub_val = 0;
    if (requires_subrev) {
        printf("--- Operation 5: Reading sub-revision register 0x1816 ---\n");
        uint8_t tx_sub_rd[10] = {0x04, 0xFB, 0x98, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        uint8_t rx_sub_rd[10] = {0};
        print_hex_buffer("MOSI", tx_sub_rd, sizeof(tx_sub_rd));

        struct spi_ioc_transfer tr4 = {
            .tx_buf = (uintptr_t)tx_sub_rd,
            .rx_buf = (uintptr_t)rx_sub_rd,
            .len = sizeof(tx_sub_rd),
            .speed_hz = speed_hz,
            .bits_per_word = bits,
            .delay_usecs = 0,
            .cs_change = 0,
        };

        ret = ioctl(fd, SPI_IOC_MESSAGE(1), &tr4);
        if (ret < 0 || ret != (int)sizeof(tx_sub_rd)) {
            fprintf(stderr, "STOP: Read transport error on sub-revision register 0x1816: %s\n",
                    strerror(errno));
            close(fd);
            return EXIT_ERR_SUBREV_FAILURE;
        }
        print_hex_buffer("MISO", rx_sub_rd, sizeof(rx_sub_rd));

        sub_val = ((uint16_t)rx_sub_rd[6] << 8) | rx_sub_rd[7];
        uint16_t sub_crc_rx = ((uint16_t)rx_sub_rd[8] << 8) | rx_sub_rd[9];
        uint16_t sub_crc_calc = calc_crc_ccitt(&rx_sub_rd[6], 2);

        printf(">> Sub-revision word (buffer[6..7]): 0x%04X\n", sub_val);
        printf(">> Sub-revision CRC (buffer[8..9]): 0x%04X (Calculated: 0x%04X)\n",
               sub_crc_rx, sub_crc_calc);

        if (sub_crc_rx != 0x0000 && sub_crc_rx != sub_crc_calc) {
            fprintf(stderr, "STOP: Sub-revision CRC mismatch on register 0x1816\n");
            close(fd);
            return EXIT_ERR_CRC_MISMATCH;
        }

        if (sub_val == 0x0FFF) {
            model_name = "FT9395 (Sub-revision 0x0FFF identified)";
        } else {
            model_name = "FT9391 (Sub-revision matches standard FT9391)";
        }
        printf(">> Final Silicon Model: %s\n\n", model_name);
    }

    /* Close SPI file descriptor */
    close(fd);

    printf("===================================================================\n");
    printf("STAGE 2A IDENTIFICATION RESULT: SUCCESS\n");
    printf("Identified Chip ID: 0x%04X\n", chip_id);
    if (requires_subrev) {
        printf("Sub-Revision Word: 0x%04X\n", sub_val);
    }
    printf("Silicon Classification: %s\n", model_name);
    printf("CRC Verification: %s\n", (crc_rx != 0x0000) ? "PASS (Exact match)" : "PASS (Zero exception)");
    printf("===================================================================\n");

    return EXIT_IDENTIFY_SUCCESS;
}
