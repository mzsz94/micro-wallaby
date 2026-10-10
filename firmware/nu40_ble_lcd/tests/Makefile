CC ?= cc
CPPFLAGS += -I../include -I../../wireless_lcd_common/include
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Werror -pedantic
BUILD_DIR ?= /tmp/nu40-ble-lcd-host-tests
TEST_BIN = $(BUILD_DIR)/test_lcd
OLED_TEST_BIN = $(BUILD_DIR)/test_oled

.PHONY: test
test: $(TEST_BIN) $(OLED_TEST_BIN)
	$(TEST_BIN)
	$(OLED_TEST_BIN)

$(TEST_BIN): test_lcd.c ../src/lcd_device.c ../src/lcd_command.c ../include/lcd_device.h ../include/lcd_command.h ../../wireless_lcd_common/include/lcd_protocol.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) test_lcd.c ../src/lcd_device.c ../src/lcd_command.c $(LDFLAGS) -o $@

$(OLED_TEST_BIN): test_oled.c ../src/oled_device.c ../src/oled_font.h ../include/oled_device.h ../include/lcd_device.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) test_oled.c ../src/oled_device.c $(LDFLAGS) -o $@
