CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic

APP_ROOT := ../..
BUILD_DIR := build
TARGET := $(BUILD_DIR)/logic_tests
SOURCES := \
	$(APP_ROOT)/src/behavior_sm.c \
	$(APP_ROOT)/src/input_filter.c \
	$(APP_ROOT)/src/safety_ctrl.c \
	test_logic.c

.PHONY: test clean

test: $(TARGET)
	./$(TARGET)

$(TARGET): $(SOURCES)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(APP_ROOT)/include $(SOURCES) -o $(TARGET)

clean:
	rm -f $(TARGET)
