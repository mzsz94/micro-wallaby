CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2
CPPFLAGS += -I../../include
BUILD_DIR ?= build

.PHONY: test clean
test: $(BUILD_DIR)/test_imu
	$(abspath $(BUILD_DIR)/test_imu)

$(BUILD_DIR)/test_imu: test_imu.c ../../src/imu_core.c ../../src/imu_calibration.c
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@

clean:
	$(RM) $(BUILD_DIR)/test_imu
