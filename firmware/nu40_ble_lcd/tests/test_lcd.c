#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "lcd_command.h"
#include "lcd_device.h"

struct fake_bus {
	unsigned int calls;
	unsigned int fail_call;
	int error;
	uint8_t bytes[128][3];
	size_t lengths[128];
	uint32_t waits[128];
	unsigned int wait_count;
};

static const struct lcd_config profile = {
	.address = 0x27, .columns = 16, .rows = 2,
	.rs_bit = 0, .rw_bit = 1, .enable_bit = 2, .backlight_bit = 3,
	.data_bits = {4, 5, 6, 7}, .backlight_active_high = true,
};

static int fake_write(void *context, uint8_t address, const uint8_t *bytes, size_t length)
{
	struct fake_bus *fake = context;

	assert(address == 0x27 || address == 0x3f);
	assert(length == 1 || length == 3);
	assert(fake->calls < 128);
	memcpy(fake->bytes[fake->calls], bytes, length);
	fake->lengths[fake->calls++] = length;
	if (fake->calls == fake->fail_call) {
		return fake->error;
	}
	return 0;
}

static void fake_delay(void *context, uint32_t microseconds)
{
	struct fake_bus *fake = context;

	assert(fake->wait_count < 128);
	fake->waits[fake->wait_count++] = microseconds;
}

static int initialize(struct lcd_device *lcd, struct fake_bus *fake,
		      const struct lcd_config *config)
{
	const struct lcd_bus bus = {
		.context = fake, .write = fake_write, .delay_us = fake_delay,
	};
	return lcd_device_initialize(lcd, config, &bus);
}

static void assert_byte(const struct fake_bus *fake, unsigned int at,
			uint8_t byte, bool data)
{
	uint8_t decoded = (fake->bytes[at][0] & 0xf0) |
		(fake->bytes[at + 1][0] >> 4);

	assert(decoded == byte);
	assert((fake->bytes[at][0] & 1U) == (unsigned int)data);
	assert((fake->bytes[at + 1][0] & 1U) == (unsigned int)data);
}

static void test_initialization_timing(void)
{
	struct fake_bus fake = {0};
	struct lcd_device lcd = {0};
	const uint32_t waits[] = {50000, 4500, 150, 150, 150, 50, 50, 2000, 50, 50};
	const uint8_t commands[] = {0x28, 0x08, 0x01, 0x06, 0x0c};

	assert(initialize(&lcd, &fake, &profile) == 0);
	assert(lcd.initialized && lcd.backlight);
	assert(fake.calls == 15);
	assert(fake.wait_count == sizeof(waits) / sizeof(waits[0]));
	assert(memcmp(fake.waits, waits, sizeof(waits)) == 0);
	assert(fake.lengths[0] == 1 && fake.bytes[0][0] == 0x08);
	for (unsigned int i = 1; i < fake.calls; ++i) {
		assert(fake.lengths[i] == 3);
		assert(fake.bytes[i][0] == fake.bytes[i][2]);
		assert(fake.bytes[i][1] == (fake.bytes[i][0] | 0x04));
		assert((fake.bytes[i][0] & 0x06) == 0); /* E low, RW always low. */
	}
	assert(fake.bytes[1][0] == 0x38);
	assert(fake.bytes[2][0] == 0x38);
	assert(fake.bytes[3][0] == 0x38);
	assert(fake.bytes[4][0] == 0x28);
	for (size_t i = 0; i < sizeof(commands); ++i) {
		assert_byte(&fake, 5 + (unsigned int)i * 2, commands[i], false);
	}
}

static void test_initialization_bus_errors(void)
{
	for (unsigned int fail = 1; fail <= 15; ++fail) {
		struct fake_bus fake = {.fail_call = fail, .error = -ETIMEDOUT};
		struct lcd_device lcd = {0};

		assert(initialize(&lcd, &fake, &profile) == -ETIMEDOUT);
		assert(!lcd.initialized && fake.calls == fail);
		assert(lcd_device_clear(&lcd) == -ENODEV);
		assert(lcd_device_backlight(&lcd, false) == -ENODEV);
		assert(lcd_device_write(&lcd, 0, 0, (const uint8_t *)"x", 1) == -ENODEV);
		assert(fake.calls == fail);
	}
}

static void test_write_clear_backlight(void)
{
	struct fake_bus fake = {0};
	struct lcd_device lcd = {0};

	assert(initialize(&lcd, &fake, &profile) == 0);
	assert(lcd_device_write(&lcd, 1, 3, (const uint8_t *)"Hi", 2) == 0);
	assert_byte(&fake, 15, 0xc3, false);
	assert_byte(&fake, 17, 'H', true);
	assert_byte(&fake, 19, 'i', true);
	assert(lcd_device_clear(&lcd) == 0);
	assert_byte(&fake, 21, 0x01, false);
	assert(fake.waits[fake.wait_count - 1] == 2000);
	assert(lcd_device_backlight(&lcd, false) == 0);
	assert(fake.bytes[23][0] == 0 && !lcd.backlight);
	assert(lcd_device_backlight(&lcd, true) == 0);
	assert(fake.bytes[24][0] == 8 && lcd.backlight);
}

static void test_bounds_and_invalid_data(void)
{
	struct fake_bus fake = {0};
	struct lcd_device lcd = {0};
	const uint8_t invalid[] = {0xc3, 0xa9};

	assert(initialize(&lcd, &fake, &profile) == 0);
	assert(lcd_device_write(&lcd, 2, 0, (const uint8_t *)"x", 1) == -EINVAL);
	assert(lcd_device_write(&lcd, 0, 16, (const uint8_t *)"x", 1) == -EINVAL);
	assert(lcd_device_write(&lcd, 0, 15, (const uint8_t *)"xx", 2) == -EINVAL);
	assert(lcd_device_write(&lcd, 0, 0, invalid, sizeof(invalid)) == -EINVAL);
	assert(lcd_device_write(&lcd, 0, 0, NULL, 1) == -EINVAL);
	assert(lcd_device_write(&lcd, 0, 0, (const uint8_t *)"", 0) == -EINVAL);
	assert(fake.calls == 15);
	assert(lcd_device_write(&lcd, 1, 15, (const uint8_t *)"~", 1) == 0);
}

static void test_partial_io_and_recovery(void)
{
	struct fake_bus fake = {.fail_call = 18, .error = -EIO};
	struct lcd_device lcd = {0};

	assert(initialize(&lcd, &fake, &profile) == 0);
	assert(lcd_device_write(&lcd, 0, 0, (const uint8_t *)"partial", 7) == -EIO);
	assert(fake.calls == 18 && !lcd.initialized);
	assert(lcd_device_clear(&lcd) == -ENODEV);
	fake.fail_call = 0;
	assert(initialize(&lcd, &fake, &profile) == 0);
	assert(lcd_device_clear(&lcd) == 0);
	fake.fail_call = fake.calls + 1;
	fake.error = 1;
	assert(lcd_device_backlight(&lcd, false) == -EIO);
	assert(!lcd.initialized);
}

static void test_profile_validation(void)
{
	struct fake_bus fake = {0};
	struct lcd_device lcd = {0};
	struct lcd_config config = profile;

	config.address = 0x30;
	assert(initialize(&lcd, &fake, &config) == -EINVAL);
	config = profile;
	config.data_bits[1] = config.data_bits[0];
	assert(initialize(&lcd, &fake, &config) == -EINVAL);
	config = profile;
	config.rs_bit = 8;
	assert(initialize(&lcd, &fake, &config) == -EINVAL);
	config = profile;
	config.rows = 3;
	assert(initialize(&lcd, &fake, &config) == -EINVAL);
	config = profile;
	config.columns = 0;
	assert(initialize(&lcd, &fake, &config) == -EINVAL);
	assert(fake.calls == 0);
}

static void test_four_row_and_inverted_backlight(void)
{
	struct fake_bus fake = {0};
	struct lcd_device lcd = {0};
	struct lcd_config config = profile;

	config.columns = 20;
	config.rows = 4;
	config.address = 0x3f;
	config.backlight_active_high = false;
	assert(initialize(&lcd, &fake, &config) == 0);
	assert(fake.bytes[0][0] == 0);
	assert(lcd_device_write(&lcd, 2, 0, (const uint8_t *)"A", 1) == 0);
	assert_byte(&fake, 15, 0x94, false);
	assert(lcd_device_write(&lcd, 3, 0, (const uint8_t *)"B", 1) == 0);
	assert_byte(&fake, 19, 0xd4, false);
	assert(lcd_device_backlight(&lcd, false) == 0);
	assert(fake.bytes[23][0] == 8);
}

static void test_parser_valid_commands(void)
{
	struct lcd_command command;
	uint8_t clear[] = {1, LCD_OP_CLEAR, 0x34, 0x12};
	uint8_t status[] = {1, LCD_OP_STATUS, 0x35, 0x12};
	uint8_t write[] = {1, LCD_OP_WRITE, 0xff, 0xff, 1, 14, 'O', 'K'};
	uint8_t light[] = {1, LCD_OP_BACKLIGHT, 0, 0, 1};
	uint8_t dfu[] = {1, LCD_OP_ENTER_DFU, 1, 0, 'D', 'F', 'U', '!'};

	assert(lcd_command_parse(clear, sizeof(clear), 16, 2, &command) == 0);
	assert(command.sequence == 0x1234 && command.opcode == LCD_OP_CLEAR);
	assert(lcd_command_parse(status, sizeof(status), 16, 2, &command) == 0);
	assert(command.sequence == 0x1235 && command.opcode == LCD_OP_STATUS);
	assert(lcd_command_parse(write, sizeof(write), 16, 2, &command) == 0);
	assert(command.sequence == 0xffff && command.row == 1 && command.column == 14);
	assert(command.length == 2 && memcmp(command.text, "OK", 2) == 0);
	assert(lcd_command_parse(light, sizeof(light), 16, 2, &command) == 0);
	assert(command.backlight == 1);
	assert(lcd_command_parse(dfu, sizeof(dfu), 16, 2, &command) == 0);
	assert(command.opcode == LCD_OP_ENTER_DFU);
}

static void test_parser_rejections(void)
{
	struct lcd_command command;
	struct lcd_command original;
	uint8_t packet[21] = {1, LCD_OP_CLEAR, 1, 0};

	memset(&command, 0x55, sizeof(command));
	original = command;
	for (size_t size = 0; size < 4; ++size) {
		assert(lcd_command_parse(packet, size, 16, 2, &command) < 0);
	}
	assert(lcd_command_parse(packet, sizeof(packet), 16, 2, &command) < 0);
	assert(lcd_command_parse(packet, 5, 16, 2, &command) < 0);
	packet[0] = 2;
	assert(lcd_command_parse(packet, 4, 16, 2, &command) < 0);
	packet[0] = 1;
	packet[1] = 0x7f;
	assert(lcd_command_parse(packet, 4, 16, 2, &command) < 0);
	packet[1] = LCD_OP_BACKLIGHT;
	packet[4] = 2;
	assert(lcd_command_parse(packet, 5, 16, 2, &command) < 0);
	packet[1] = LCD_OP_ENTER_DFU;
	memcpy(packet + 4, "DFU?", 4);
	assert(lcd_command_parse(packet, 8, 16, 2, &command) < 0);
	assert(memcmp(&command, &original, sizeof(command)) == 0);
	assert(lcd_command_parse(NULL, 8, 16, 2, &command) < 0);
	assert(lcd_command_parse(packet, 8, 0, 2, &command) < 0);
	assert(lcd_command_parse(packet, 8, 16, 3, &command) < 0);
}

static void test_parser_text_edges(void)
{
	struct lcd_command command;
	uint8_t packet[20] = {1, LCD_OP_WRITE, 0, 0, 1, 2};

	memset(packet + 6, 'X', 14);
	assert(lcd_command_parse(packet, sizeof(packet), 16, 2, &command) == 0);
	assert(command.length == 14);
	assert(lcd_command_parse(packet, 6, 16, 2, &command) < 0);
	packet[5] = 3;
	assert(lcd_command_parse(packet, sizeof(packet), 16, 2, &command) < 0);
	packet[5] = 2;
	packet[4] = 2;
	assert(lcd_command_parse(packet, 7, 16, 2, &command) < 0);
	packet[4] = 1;
	packet[6] = '\n';
	assert(lcd_command_parse(packet, 7, 16, 2, &command) < 0);
	packet[6] = 0x7f;
	assert(lcd_command_parse(packet, 7, 16, 2, &command) < 0);
	packet[6] = 0xe1;
	assert(lcd_command_parse(packet, 7, 16, 2, &command) < 0);
}

int main(void)
{
	test_initialization_timing();
	test_initialization_bus_errors();
	test_write_clear_backlight();
	test_bounds_and_invalid_data();
	test_partial_io_and_recovery();
	test_profile_validation();
	test_four_row_and_inverted_backlight();
	test_parser_valid_commands();
	test_parser_rejections();
	test_parser_text_edges();
	puts("PASS: 10 LCD driver/protocol tests (including all 15 init I2C failure points)");
	return 0;
}
