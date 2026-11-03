/* SPDX-License-Identifier: GPL-2.0-only */
/* tools/fte3600-tool.c - Minimal Hardware Validation Utility */

#include <errno.h>
#include <fcntl.h>
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
	printf("\n");
	printf("   Last Error: %u (%s)\n", snap->last_error,
	       snap->last_error ? strerror(snap->last_error) : "None");
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <get-state|reset|init|capture>\n", argv[0]);
		return 1;
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
			/* libc ioctl() returns -1 on error and sets errno */
			perror("ioctl(FTE3600_IOC_RESET)");
			close(fd);
			return 1;
		}
		printf(">> Reset ioctl returned 0 (Success).\n");
	} else if (strcmp(argv[1], "init") == 0) {
		struct fte3600_init_params params = { .version = FTE3600_UAPI_VERSION };
		printf(">> Issuing FTE3600_IOC_INIT...\n");
		if (ioctl(fd, FTE3600_IOC_INIT, &params) < 0) {
			/* libc ioctl() returns -1 on error and sets errno */
			perror("ioctl(FTE3600_IOC_INIT)");
			close(fd);
			return 1;
		}
		printf(">> Init ioctl returned 0 (Identification Success).\n");
	} else if (strcmp(argv[1], "capture") == 0) {
		struct fte3600_frame_buffer fb = {
			.version = FTE3600_UAPI_VERSION,
			.length = FTE3600_IMAGE_BYTES
		};
		printf(">> Issuing FTE3600_IOC_CAPTURE (Expect failure with errno == EOPNOTSUPP)...\n");
		if (ioctl(fd, FTE3600_IOC_CAPTURE, &fb) < 0) {
			if (errno == EOPNOTSUPP) {
				printf(">> Capture rejected as expected: %s (errno=%d, EOPNOTSUPP)\n",
				       strerror(errno), errno);
			} else {
				fprintf(stderr, ">> Capture failed with unexpected errno: %s (errno=%d)\n",
				        strerror(errno), errno);
				close(fd);
				return 1;
			}
		} else {
			fprintf(stderr, "ERROR: Capture unexpectedly succeeded!\n");
			close(fd);
			return 1;
		}
	} else {
		fprintf(stderr, "Unknown command: %s\n", argv[1]);
		close(fd);
		return 1;
	}

	close(fd);
	return 0;
}
