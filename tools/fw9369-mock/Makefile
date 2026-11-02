# SPDX-License-Identifier: LGPL-2.1-or-later
CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -pedantic -std=c11 -O2 -pthread
LDFLAGS ?= -pthread
INCLUDES = -Iinclude

SRCS_COMMON = src/fw9369_crc.c \
              src/fw9369_protocol.c \
              src/fw9369_transport_mock.c \
              src/fw9369_calibration.c \
              src/fw9369_state.c

OBJS_COMMON = $(SRCS_COMMON:.c=.o)

SRCS_HW = src/fw9369_crc.c \
          src/fw9369_protocol.c \
          src/fw9369_transport_hw.c \
          src/fw9369_calibration.c \
          src/fw9369_state.c

OBJS_HW = $(SRCS_HW:.c=.o)

BIN_DIR = ../../bin
TOOL_BIN = $(BIN_DIR)/fw9369-tool
TEST_BIN = $(BIN_DIR)/fw9369-tests
HW_TOOL_BIN = $(BIN_DIR)/fw9369-hw-tool

.PHONY: all clean test hw-tool

all: $(TOOL_BIN) $(TEST_BIN)

hw-tool: $(HW_TOOL_BIN)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(TOOL_BIN): $(BIN_DIR) $(OBJS_COMMON) src/fw9369_main.o
	$(CC) $(CFLAGS) $(INCLUDES) $(OBJS_COMMON) src/fw9369_main.o $(LDFLAGS) -o $@

$(TEST_BIN): $(BIN_DIR) $(OBJS_COMMON) src/fw9369_transport_hw.o tests/test_runner.o
	$(CC) $(CFLAGS) $(INCLUDES) $(OBJS_COMMON) src/fw9369_transport_hw.o tests/test_runner.o $(LDFLAGS) -o $@

$(HW_TOOL_BIN): $(BIN_DIR) $(OBJS_HW) src/fw9369_main_hw.o
	$(CC) $(CFLAGS) $(INCLUDES) $(OBJS_HW) src/fw9369_main_hw.o $(LDFLAGS) -o $@

tests/%.o: tests/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

test: $(TEST_BIN)
	$(TEST_BIN)

clean:
	rm -f src/*.o tests/*.o $(TOOL_BIN) $(TEST_BIN) $(HW_TOOL_BIN)
