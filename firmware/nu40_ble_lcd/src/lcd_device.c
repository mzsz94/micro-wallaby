#include "lcd_device.h"

#include <errno.h>

static int validate_config(const struct lcd_config *config, const struct lcd_bus *bus)
{
	uint16_t used = 0;
	uint8_t bits[8];

	if (!config || !bus || !bus->write || !bus->delay_us ||
	    !((config->address >= 0x20 && config->address <= 0x27) ||
	      (config->address >= 0x38 && config->address <= 0x3f)) ||
	    config->columns == 0 || config->columns > 40 ||
	    !(config->rows == 1 || config->rows == 2 || config->rows == 4) ||
	    (unsigned int)config->rows * config->columns > 80) {
		return -EINVAL;
	}
	bits[0] = config->rs_bit;
	bits[1] = config->rw_bit;
	bits[2] = config->enable_bit;
	bits[3] = config->backlight_bit;
	for (size_t i = 0; i < 4; ++i) {
		bits[4 + i] = config->data_bits[i];
	}
	for (size_t i = 0; i < 8; ++i) {
		if (bits[i] > 7 || (used & (1U << bits[i]))) {
			return -EINVAL;
		}
		used |= 1U << bits[i];
	}
	return 0;
}

static uint8_t backlight_mask(const struct lcd_device *lcd)
{
	return lcd->backlight == lcd->config.backlight_active_high ?
		(1U << lcd->config.backlight_bit) : 0;
}

static int write_bus(struct lcd_device *lcd, const uint8_t *bytes, size_t length)
{
	int rc = lcd->bus.write(lcd->bus.context, lcd->config.address, bytes, length);

	if (rc != 0) {
		lcd->initialized = false;
		return rc < 0 ? rc : -EIO;
	}
	return 0;
}

static int write_nibble(struct lcd_device *lcd, uint8_t nibble, bool data)
{
	uint8_t value = backlight_mask(lcd);
	uint8_t bytes[3];

	if (data) {
		value |= 1U << lcd->config.rs_bit;
	}
	for (size_t i = 0; i < 4; ++i) {
		if (nibble & (1U << i)) {
			value |= 1U << lcd->config.data_bits[i];
		}
	}
	/* RW stays LOW. Standard-speed I2C provides >450 ns E-high time. */
	bytes[0] = value;
	bytes[1] = value | (1U << lcd->config.enable_bit);
	bytes[2] = value;
	return write_bus(lcd, bytes, sizeof(bytes));
}

static int write_byte(struct lcd_device *lcd, uint8_t byte, bool data)
{
	int rc = write_nibble(lcd, byte >> 4, data);

	if (rc == 0) {
		rc = write_nibble(lcd, byte & 0x0f, data);
	}
	if (rc == 0) {
		lcd->bus.delay_us(lcd->bus.context,
			(!data && (byte == 0x01 || byte == 0x02)) ? 2000 : 50);
	}
	return rc;
}

int lcd_device_initialize(struct lcd_device *lcd, const struct lcd_config *config,
			  const struct lcd_bus *bus)
{
	int rc;
	uint8_t latch;
	static const uint8_t nibbles[] = {3, 3, 3, 2};
	static const uint32_t waits[] = {4500, 150, 150, 150};
	uint8_t commands[5];

	if (!lcd) {
		return -EINVAL;
	}
	lcd->initialized = false;
	rc = validate_config(config, bus);
	if (rc != 0) {
		return rc;
	}
	lcd->config = *config;
	lcd->bus = *bus;
	lcd->backlight = true;
	latch = backlight_mask(lcd);
	rc = write_bus(lcd, &latch, 1);
	if (rc != 0) {
		return rc;
	}
	lcd->bus.delay_us(lcd->bus.context, 50000);
	for (size_t i = 0; i < sizeof(nibbles); ++i) {
		rc = write_nibble(lcd, nibbles[i], false);
		if (rc != 0) {
			return rc;
		}
		lcd->bus.delay_us(lcd->bus.context, waits[i]);
	}
	commands[0] = config->rows > 1 ? 0x28 : 0x20;
	commands[1] = 0x08;
	commands[2] = 0x01;
	commands[3] = 0x06;
	commands[4] = 0x0c;
	for (size_t i = 0; i < sizeof(commands); ++i) {
		rc = write_byte(lcd, commands[i], false);
		if (rc != 0) {
			return rc;
		}
	}
	lcd->initialized = true;
	return 0;
}

int lcd_device_clear(struct lcd_device *lcd)
{
	if (!lcd || !lcd->initialized) {
		return -ENODEV;
	}
	return write_byte(lcd, 0x01, false);
}

int lcd_device_write(struct lcd_device *lcd, uint8_t row, uint8_t column,
		     const uint8_t *text, size_t length)
{
	uint8_t address;
	int rc;

	if (!lcd || !lcd->initialized) {
		return -ENODEV;
	}
	if (!text || length == 0 || row >= lcd->config.rows ||
	    column >= lcd->config.columns || length > lcd->config.columns - column) {
		return -EINVAL;
	}
	for (size_t i = 0; i < length; ++i) {
		if (text[i] < 0x20 || text[i] > 0x7e) {
			return -EINVAL;
		}
	}
	address = column + (row % 2 ? 0x40 : 0x00) +
		(row >= 2 ? lcd->config.columns : 0);
	rc = write_byte(lcd, 0x80 | address, false);
	for (size_t i = 0; rc == 0 && i < length; ++i) {
		rc = write_byte(lcd, text[i], true);
	}
	return rc;
}

int lcd_device_backlight(struct lcd_device *lcd, bool enabled)
{
	uint8_t latch;

	if (!lcd || !lcd->initialized) {
		return -ENODEV;
	}
	lcd->backlight = enabled;
	latch = backlight_mask(lcd);
	return write_bus(lcd, &latch, 1);
}
