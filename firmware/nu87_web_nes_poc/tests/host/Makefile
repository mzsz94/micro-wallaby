CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic

APP_ROOT := ../..
IO_ROOT := ../../../nu87_io_bringup
BUILD_DIR := build
TARGET := $(BUILD_DIR)/web_nes_host_tests
SOURCES := \
	$(APP_ROOT)/src/input_protocol.c \
	$(APP_ROOT)/src/websocket_control.c \
	$(IO_ROOT)/src/input_filter.c \
	test_web_nes.c

.PHONY: test clean

test: $(TARGET)
	./$(TARGET)

$(TARGET): $(SOURCES)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(APP_ROOT)/include -I$(IO_ROOT)/include $(SOURCES) -o $(TARGET)

clean:
	rm -f $(TARGET)
