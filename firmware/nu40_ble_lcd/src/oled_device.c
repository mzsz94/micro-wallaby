#include "oled_device.h"

#include <errno.h>
#include <string.h>

#include "oled_font.h"

static int write_bus(struct oled_device *oled, const uint8_t *bytes, size_t length)
{
	int rc = oled->bus.write(oled->bus.context, oled->address, bytes, length);

	if (rc != 0) {
		oled->initialized = false;
		return rc < 0 ? rc : -EIO;
	}
	return 0;
}

static int address_window(struct oled_device *oled, uint8_t row, uint8_t column,
			  uint8_t pixel_count)
{
	const uint8_t commands[] = {
		0x00, 0x21, column, column + pixel_count - 1, 0x22, row, row,
	};
	return write_bus(oled, commands, sizeof(commands));
}

static int clear_pages(struct oled_device *oled)
{
	const uint8_t zeroes[33] = {0x40};
	int rc;

	for (uint8_t row = 0; row < OLED_ROWS; ++row) {
		rc = address_window(oled, row, 0, 128);
		if (rc != 0) {
			return rc;
		}
		for (uint8_t chunk = 0; chunk < 4; ++chunk) {
			rc = write_bus(oled, zeroes, sizeof(zeroes));
			if (rc != 0) {
				return rc;
			}
		}
	}
	return 0;
}

int oled_device_initialize(struct oled_device *oled, uint8_t address, const struct lcd_bus *bus)
{
	/* SSD1306 128x32 sequential COM pins and internal charge pump profile. */
	const uint8_t commands[] = {
		0x00, 0xae, 0xd5, 0x80, 0xa8, 0x1f, 0xd3, 0x00, 0x40,
		0x8d, 0x14, 0x20, 0x00, 0xa1, 0xc8, 0xda, 0x02,
		0x81, 0x8f, 0xd9, 0xf1, 0xdb, 0x40, 0xa4, 0xa6, 0x2e,
	};
	const uint8_t on[] = {0x00, 0xaf};
	int rc;

	if (!oled) {
		return -EINVAL;
	}
	oled->initialized = false;
	if (!bus || !bus->write || !bus->delay_us || (address != 0x3c && address != 0x3d)) {
		return -EINVAL;
	}
	oled->address = address;
	oled->bus = *bus;
	oled->bus.delay_us(oled->bus.context, 100000);
	rc = write_bus(oled, commands, sizeof(commands));
	if (rc == 0) {
		rc = clear_pages(oled);
	}
	if (rc == 0) {
		rc = write_bus(oled, on, sizeof(on));
	}
	if (rc == 0) {
		oled->initialized = true;
	}
	return rc;
}

int oled_device_clear(struct oled_device *oled)
{
	if (!oled || !oled->initialized) {
		return -ENODEV;
	}
	return clear_pages(oled);
}

int oled_device_write(struct oled_device *oled, uint8_t row, uint8_t column,
		      const uint8_t *text, size_t length)
{
	int rc;

	if (!oled || !oled->initialized) {
		return -ENODEV;
	}
	if (!text || length == 0 || row >= OLED_ROWS || column >= OLED_COLUMNS ||
	    length > OLED_COLUMNS - column) {
		return -EINVAL;
	}
	for (size_t i = 0; i < length; ++i) {
		if (text[i] < 0x20 || text[i] > 0x7e) {
			return -EINVAL;
		}
	}
	rc = address_window(oled, row, column * 8U, length * 8U);
	for (size_t i = 0; rc == 0 && i < length; ++i) {
		uint8_t glyph[9] = {0x40};

		/* Five columns centered in each independent 8x8 cell; blank padding. */
		for (size_t x = 0; x < 5; ++x) {
			glyph[x + 2] = oled_font_5x5[text[i] - 0x20][x] << 1;
		}
		rc = write_bus(oled, glyph, sizeof(glyph));
	}
	return rc;
}

int oled_device_display(struct oled_device *oled, bool enabled)
{
	const uint8_t command[] = {0x00, enabled ? 0xaf : 0xae};

	if (!oled || !oled->initialized) {
		return -ENODEV;
	}
	return write_bus(oled, command, sizeof(command));
}
