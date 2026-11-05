// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/fte3600-tool.c - Production UAPI & Calibration Diagnostic Utility
 *
 * Implements complete userspace interactions with /dev/fte3600:
 * - Telemetry & state inspection (get-state)
 * - Hardware reset (reset)
 * - Silicon identification & startup prefix (init, init-full)
 * - Capacitive Touch (FDT) AFE calibration & arming (calibrate)
 * - Full-frame image capture & PGM export (capture)
 * - Session abort (abort)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../kernel/fte3600.h"

static void print_state(const struct fte3600_state_snapshot *snap)
{
	const char *state_names[] = {
		"UNINITIALIZED", "RESETTING", "RESET_SEQUENCE_ISSUED",
		"INITIALIZING", "READY", "CAPTURING", "ERROR", "UNKNOWN",
		"IDENTIFIED"
	};
	const char *name = (snap->state <= 8) ? state_names[snap->state] : "INVALID";

	printf(">> FTE3600 State Snapshot:\n");
	printf("   Version:    %u\n", snap->version);
	printf("   State:      %u (%s)\n", snap->state, name);
	printf("   Flags:      0x%08X", snap->flags);
	if (snap->flags & FTE3600_FLAG_OFFLINE) printf(" [OFFLINE]");
	if (snap->flags & FTE3600_FLAG_RESET_SEQUENCE_ISSUED) printf(" [RESET_ISSUED]");
	if (snap->flags & FTE3600_FLAG_STARTUP_PREFIX_COMPLETED) printf(" [PREFIX_COMPLETED]");
	if (snap->flags & FTE3600_FLAG_CALIBRATION_PENDING) printf(" [CALIBRATION_PENDING]");
	if (snap->flags & FTE3600_FLAG_SESSION_ACTIVE) printf(" [SESSION_ACTIVE]");
	printf("\n");
	printf("   Last Error: %u (%s)\n", snap->last_error,
	       snap->last_error ? strerror(snap->last_error) : "None");
}

static int do_calibrate(int fd)
{
	struct fte3600_session_begin sb;
	struct fte3600_session_fdt sfdt;
	struct fte3600_session_thresholds sthresh;
	struct fte3600_session_commit scommit;
	uint16_t samples[10][4];
	uint16_t min_adc[4] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
	int consecutive_stable = 0;
	int stable_count = 0;
	int k, c;

	memset(&sb, 0, sizeof(sb));
	sb.version = FTE3600_UAPI_VERSION;
	if (ioctl(fd, FTE3600_IOC_SESSION_BEGIN, &sb) < 0) {
		perror("ioctl(FTE3600_IOC_SESSION_BEGIN)");
		return 1;
	}
	printf(">> Session lease acquired: token=0x%016llX, watchdog=%u ms\n",
	       (unsigned long long)sb.session_token, sb.watchdog_ms);

	/* 10-sample stability loop */
	for (k = 0; k < 10; k++) {
		memset(&sfdt, 0, sizeof(sfdt));
		sfdt.version = FTE3600_UAPI_VERSION;
		sfdt.session_token = sb.session_token;
		sfdt.channel_mode = 1;
		sfdt.dac_val = 27; /* Nominal baseline DAC */

		if (ioctl(fd, FTE3600_IOC_SESSION_FDT_SAMPLE, &sfdt) < 0) {
			perror("ioctl(FTE3600_IOC_SESSION_FDT_SAMPLE)");
			return 1;
		}

		printf("   Sample %d: [%u, %u, %u, %u] (wire_crc=0x%04x)\n",
		       k, sfdt.adc_samples[0], sfdt.adc_samples[1], sfdt.adc_samples[2], sfdt.adc_samples[3],
		       sfdt.wire_crc);

		for (c = 0; c < 4; c++) {
			samples[k][c] = sfdt.adc_samples[c];
			if (sfdt.adc_samples[c] < min_adc[c])
				min_adc[c] = sfdt.adc_samples[c];
		}

		if (k == 0) {
			consecutive_stable = 1;
		} else {
			bool pass = true;
			for (c = 0; c < 4; c++) {
				int delta = abs((int)samples[k][c] - (int)samples[k - 1][c]);
				if (delta > 5) {
					pass = false;
					break;
				}
			}
			if (pass)
				consecutive_stable++;
			else
				consecutive_stable = 1;
		}

		stable_count = k + 1;
		if (consecutive_stable >= 3)
			break;
	}

	if (consecutive_stable < 3) {
		fprintf(stderr, ">> Error: Calibration failed to stabilize after %d samples\n", stable_count);
		return 1;
	}
	printf(">> FDT analog front-end stabilized after %d samples (3 consecutive passes delta <= 5)\n",
	       stable_count);

	/* Format 22-byte threshold bank */
	memset(&sthresh, 0, sizeof(sthresh));
	sthresh.version = FTE3600_UAPI_VERSION;
	sthresh.session_token = sb.session_token;
	sthresh.bank_len = FTE3600_THRESHOLD_BANK_LEN;

	sthresh.threshold_bank[0] = 0x05;
	sthresh.threshold_bank[1] = 0xFA;
	sthresh.threshold_bank[2] = 0x80;
	sthresh.threshold_bank[3] = 0xB0; /* Non-Fab AA default */
	sthresh.threshold_bank[4] = 0x00;
	sthresh.threshold_bank[5] = 0x08;

	for (c = 0; c < 4; c++) {
		if (min_adc[c] < 300 || min_adc[c] > 700) {
			fprintf(stderr, ">> Error: Channel %d min ADC %u out of bounds [300, 700]\n",
				c, min_adc[c]);
			return 1;
		}
		uint16_t t = (min_adc[c] > 30) ? (min_adc[c] - 30) : 0;
		sthresh.threshold_bank[6 + 2 * c] = (uint8_t)(t >> 8);
		sthresh.threshold_bank[7 + 2 * c] = (uint8_t)(t & 0xFF);
		printf("   Channel %d: min=%u -> threshold=%u\n", c, min_adc[c], t);
	}

	if (ioctl(fd, FTE3600_IOC_SESSION_SET_THRESHOLDS, &sthresh) < 0) {
		perror("ioctl(FTE3600_IOC_SESSION_SET_THRESHOLDS)");
		return 1;
	}
	printf(">> 22-byte threshold bank successfully written to hardware.\n");

	/* Commit calibration and arm touch detector */
	memset(&scommit, 0, sizeof(scommit));
	scommit.version = FTE3600_UAPI_VERSION;
	scommit.session_token = sb.session_token;

	if (ioctl(fd, FTE3600_IOC_SESSION_COMMIT, &scommit) < 0) {
		perror("ioctl(FTE3600_IOC_SESSION_COMMIT)");
		return 1;
	}
	printf(">> Calibration committed: sensor armed, state READY (state=%u)\n",
	       scommit.final_state);
	return 0;
}

static int do_capture(int fd, const char *outfile)
{
	uint8_t *image = malloc(FTE3600_IMAGE_BYTES);
	if (!image) {
		fprintf(stderr, "malloc failed\n");
		return 1;
	}

	struct fte3600_frame_buffer fb;
	memset(&fb, 0, sizeof(fb));
	fb.version = FTE3600_UAPI_VERSION;
	fb.length = FTE3600_IMAGE_BYTES;
	fb.buffer_ptr = (uint64_t)(uintptr_t)image;

	printf(">> Place your finger on the sensor now... capturing in 3 seconds...\n");
	sleep(1);
	printf("   2...\n");
	sleep(1);
	printf("   1... capturing!\n");
	sleep(1);

	printf(">> Issuing FTE3600_IOC_CAPTURE (length=%u bytes)...\n", fb.length);
	if (ioctl(fd, FTE3600_IOC_CAPTURE, &fb) < 0) {
		perror("ioctl(FTE3600_IOC_CAPTURE)");
		free(image);
		return 1;
	}
	printf(">> Frame captured successfully (10,240 bytes, 5,120 pixels, 64x80 matrix).\n");

	uint16_t min_p = 0xFFFF, max_p = 0;
	size_t non_zero = 0;
	for (size_t i = 0; i < FTE3600_IMAGE_BYTES / 2; i++) {
		uint16_t p = ((uint16_t)image[2 * i] << 8) | image[2 * i + 1];
		if (p < min_p) min_p = p;
		if (p > max_p) max_p = p;
		if (p != 0) non_zero++;
	}
	printf("   Pixel Stats: min=%u, max=%u, non-zero pixels=%zu / 5120\n",
	       (min_p == 0xFFFF) ? 0 : min_p, max_p, non_zero);
	printf("   Raw Bytes [0..15]: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
	       image[0], image[1], image[2], image[3], image[4], image[5], image[6], image[7],
	       image[8], image[9], image[10], image[11], image[12], image[13], image[14], image[15]);
	printf("   Sample Pixels [0..7]: %u %u %u %u %u %u %u %u\n",
	       ((uint16_t)image[0] << 8) | image[1],
	       ((uint16_t)image[2] << 8) | image[3],
	       ((uint16_t)image[4] << 8) | image[5],
	       ((uint16_t)image[6] << 8) | image[7],
	       ((uint16_t)image[8] << 8) | image[9],
	       ((uint16_t)image[10] << 8) | image[11],
	       ((uint16_t)image[12] << 8) | image[13],
	       ((uint16_t)image[14] << 8) | image[15]);

	if (outfile) {
		FILE *f = fopen(outfile, "wb");
		if (!f) {
			perror("fopen(outfile)");
			free(image);
			return 1;
		}
		if (strstr(outfile, ".pgm")) {
			/* Write standard 16-bit P5 binary PGM header */
			fprintf(f, "P5\n64 80\n65535\n");
		}
		fwrite(image, 1, FTE3600_IMAGE_BYTES, f);
		fclose(f);
		printf(">> Saved frame data to %s\n", outfile);
	}

	free(image);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
		fprintf(stderr, "Usage: %s <get-state|reset|init|init-full|calibrate|capture [outfile]|abort [token]>\n", argv[0]);
		return (argc < 2) ? 1 : 0;
	}

	int fd = open("/dev/fte3600", O_RDWR);
	if (fd < 0) {
		perror("open(/dev/fte3600)");
		return 1;
	}

	if (strcmp(argv[1], "get-state") == 0) {
		struct fte3600_state_snapshot snap;
		if (ioctl(fd, FTE3600_IOC_GET_STATE, &snap) < 0) {
			perror("ioctl(FTE3600_IOC_GET_STATE)");
			close(fd);
			return 1;
		}
		print_state(&snap);
	} else if (strcmp(argv[1], "reset") == 0) {
		printf(">> Issuing FTE3600_IOC_RESET...\n");
		if (ioctl(fd, FTE3600_IOC_RESET) < 0) {
			perror("ioctl(FTE3600_IOC_RESET)");
			close(fd);
			return 1;
		}
		printf(">> Reset ioctl returned 0 (Success).\n");
	} else if (strcmp(argv[1], "init") == 0 || strcmp(argv[1], "init-full") == 0) {
		bool full_startup = (strcmp(argv[1], "init-full") == 0);
		struct fte3600_init_params params = {
			.version = FTE3600_UAPI_VERSION,
			.flags = full_startup ? FTE3600_INIT_FLAG_FULL_STARTUP : FTE3600_INIT_FLAG_IDENTIFY_ONLY
		};
		printf(">> Issuing FTE3600_IOC_INIT (flags=0x%08X)...\n", params.flags);
		if (ioctl(fd, FTE3600_IOC_INIT, &params) < 0) {
			perror("ioctl(FTE3600_IOC_INIT)");
			close(fd);
			return 1;
		}
		printf(">> Init ioctl returned 0 (%s Success).\n",
		       full_startup ? "Volatile Startup Prefix" : "Identification");
	} else if (strcmp(argv[1], "calibrate") == 0) {
		if (do_calibrate(fd) != 0) {
			close(fd);
			return 1;
		}
	} else if (strcmp(argv[1], "capture") == 0) {
		const char *outfile = (argc >= 3) ? argv[2] : NULL;
		if (do_capture(fd, outfile) != 0) {
			close(fd);
			return 1;
		}
	} else if (strcmp(argv[1], "abort") == 0) {
		struct fte3600_session_abort sabort;
		memset(&sabort, 0, sizeof(sabort));
		sabort.version = FTE3600_UAPI_VERSION;
		if (argc >= 3)
			sabort.session_token = strtoull(argv[2], NULL, 0);
		printf(">> Issuing FTE3600_IOC_SESSION_ABORT (token=0x%016llX)...\n",
		       (unsigned long long)sabort.session_token);
		if (ioctl(fd, FTE3600_IOC_SESSION_ABORT, &sabort) < 0) {
			perror("ioctl(FTE3600_IOC_SESSION_ABORT)");
			close(fd);
			return 1;
		}
		printf(">> Session aborted successfully.\n");
	} else {
		fprintf(stderr, "Unknown command: %s\n", argv[1]);
		close(fd);
		return 1;
	}

	close(fd);
	return 0;
}
