CC ?= cc
PYTHON ?= python3
CPPFLAGS += -I../include
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Werror -pedantic
LDLIBS += -lm
BUILD_DIR ?= /tmp/nu87-imu-host-tests
TEST_BIN = $(BUILD_DIR)/test_imu_device

.PHONY: test
test: $(TEST_BIN)
	$(TEST_BIN)
	$(PYTHON) test_i2c_polling.py

$(TEST_BIN): test_imu_device.c ../src/imu_device.c ../include/imu_device.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) test_imu_device.c ../src/imu_device.c $(LDFLAGS) $(LDLIBS) -o $@
