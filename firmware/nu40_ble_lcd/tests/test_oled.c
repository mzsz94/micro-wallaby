#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "oled_device.h"

struct fake_bus {
	unsigned int calls;
	unsigned int fail_call;
	uint8_t bytes[160][33];
	size_t lengths[160];
	uint32_t wait;
};

static int fake_write(void *context, uint8_t address, const uint8_t *bytes, size_t length)
{
	struct fake_bus *fake = context;

	assert(address == 0x3c || address == 0x3d);
	assert(length <= 33 && fake->calls < 160);
	memcpy(fake->bytes[fake->calls], bytes, length);
	fake->lengths[fake->calls++] = length;
	return fake->calls == fake->fail_call ? -ETIMEDOUT : 0;
}

static void fake_delay(void *context, uint32_t microseconds)
{
	struct fake_bus *fake = context;
	fake->wait += microseconds;
}

static int initialize(struct oled_device *oled, struct fake_bus *fake, uint8_t address)
{
	const struct lcd_bus bus = {
		.context = fake, .write = fake_write, .delay_us = fake_delay,
	};
	return oled_device_initialize(oled, address, &bus);
}

static void test_initialization(void)
{
	struct fake_bus fake = {0};
	struct oled_device oled = {0};

	assert(initialize(&oled, &fake, 0x3c) == 0);
	assert(oled.initialized && fake.calls == 22 && fake.wait == 100000);
	assert(fake.bytes[0][0] == 0 && fake.bytes[0][1] == 0xae);
	assert(fake.bytes[0][4] == 0xa8 && fake.bytes[0][5] == 31);
	for (unsigned int row = 0; row < 4; ++row) {
		unsigned int start = 1 + row * 5;
		uint8_t window[] = {0, 0x21, 0, 127, 0x22, row, row};

		assert(fake.lengths[start] == sizeof(window));
		assert(memcmp(fake.bytes[start], window, sizeof(window)) == 0);
		for (unsigned int chunk = 1; chunk <= 4; ++chunk) {
			assert(fake.lengths[start + chunk] == 33);
			assert(fake.bytes[start + chunk][0] == 0x40);
			for (unsigned int column = 1; column <= 32; ++column) {
				assert(fake.bytes[start + chunk][column] == 0);
			}
		}
	}
	assert(fake.bytes[21][0] == 0 && fake.bytes[21][1] == 0xaf);
}

static void test_all_init_failure_points(void)
{
	for (unsigned int fail = 1; fail <= 22; ++fail) {
		struct fake_bus fake = {.fail_call = fail};
		struct oled_device oled = {0};

		assert(initialize(&oled, &fake, 0x3c) == -ETIMEDOUT);
		assert(!oled.initialized && fake.calls == fail);
		assert(oled_device_clear(&oled) == -ENODEV);
		assert(oled_device_write(&oled, 0, 0, (const uint8_t *)"x", 1) == -ENODEV);
		assert(oled_device_display(&oled, true) == -ENODEV);
		assert(fake.calls == fail);
	}
}

static void test_text_bounds(void)
{
	struct fake_bus fake = {0};
	struct oled_device oled = {0};
	uint8_t window[] = {0, 0x21, 120, 127, 0x22, 3, 3};

	assert(initialize(&oled, &fake, 0x3d) == 0);
	assert(oled_device_write(&oled, 3, 15, (const uint8_t *)"A", 1) == 0);
	assert(memcmp(fake.bytes[22], window, sizeof(window)) == 0);
	assert(fake.lengths[23] == 9 && fake.bytes[23][0] == 0x40);
	assert(fake.bytes[23][1] == 0 && fake.bytes[23][7] == 0 && fake.bytes[23][8] == 0);
	assert(oled_device_write(&oled, 4, 0, (const uint8_t *)"x", 1) == -EINVAL);
	assert(oled_device_write(&oled, 0, 16, (const uint8_t *)"x", 1) == -EINVAL);
	assert(oled_device_write(&oled, 0, 15, (const uint8_t *)"xx", 2) == -EINVAL);
	assert(oled_device_write(&oled, 0, 0, (const uint8_t *)"\n", 1) == -EINVAL);
	assert(oled_device_write(&oled, 0, 0, NULL, 1) == -EINVAL);
	assert(oled_device_write(&oled, 0, 0, (const uint8_t *)"x", 0) == -EINVAL);
	assert(fake.calls == 24);
	assert(oled_device_display(&oled, false) == 0);
	assert(fake.bytes[24][1] == 0xae);
	assert(oled_device_display(&oled, true) == 0);
	assert(fake.bytes[25][1] == 0xaf);
}

static void test_printable_ascii(void)
{
	struct fake_bus fake = {0};
	struct oled_device oled = {0};
	uint8_t text[16];

	assert(initialize(&oled, &fake, 0x3c) == 0);
	for (unsigned int base = 32; base < 127; base += 16) {
		size_t length = 127 - base < 16 ? 127 - base : 16;
		for (size_t i = 0; i < length; ++i) {
			text[i] = base + i;
		}
		assert(oled_device_write(&oled, 0, 0, text, length) == 0);
	}
	/* A space clears a complete cell, including all padding. */
	for (size_t i = 1; i < 9; ++i) {
		assert(fake.bytes[23][i] == 0);
	}
}

static void test_partial_failure_and_bad_address(void)
{
	struct fake_bus fake = {.fail_call = 25};
	struct oled_device oled = {0};

	assert(initialize(&oled, &fake, 0x27) == -EINVAL && fake.calls == 0);
	assert(initialize(&oled, &fake, 0x3c) == 0);
	assert(oled_device_write(&oled, 0, 0, (const uint8_t *)"hello", 5) == -ETIMEDOUT);
	assert(fake.calls == 25 && !oled.initialized);
	assert(oled_device_clear(&oled) == -ENODEV);
	fake.fail_call = 0;
	assert(initialize(&oled, &fake, 0x3c) == 0);
	assert(oled_device_clear(&oled) == 0);
	fake.fail_call = fake.calls + 1;
	assert(oled_device_display(&oled, false) == -ETIMEDOUT);
	assert(!oled.initialized);
}

int main(void)
{
	test_initialization();
	test_all_init_failure_points();
	test_text_bounds();
	test_printable_ascii();
	test_partial_failure_and_bad_address();
	puts("PASS: 5 SSD1306 tests (all 22 init failures, 95 ASCII glyphs, bounds, partial I2C errors)");
	return 0;
}
