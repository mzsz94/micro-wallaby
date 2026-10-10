#ifndef NU40_LCD_DEVICE_H
#define NU40_LCD_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Explicit HD44780 + PCF8574/PCF8574A write-only hardware profile. */
struct lcd_config {
	uint8_t address;
	uint8_t columns;
	uint8_t rows;
	uint8_t rs_bit;
	uint8_t rw_bit;
	uint8_t enable_bit;
	uint8_t backlight_bit;
	uint8_t data_bits[4]; /* HD44780 DB4, DB5, DB6, DB7. */
	bool backlight_active_high;
};

struct lcd_bus {
	void *context;
	int (*write)(void *context, uint8_t address, const uint8_t *bytes, size_t length);
	void (*delay_us)(void *context, uint32_t microseconds);
};

struct lcd_device {
	struct lcd_config config;
	struct lcd_bus bus;
	bool initialized;
	bool backlight;
};

int lcd_device_initialize(struct lcd_device *lcd, const struct lcd_config *config,
			  const struct lcd_bus *bus);
int lcd_device_clear(struct lcd_device *lcd);
int lcd_device_write(struct lcd_device *lcd, uint8_t row, uint8_t column,
		     const uint8_t *text, size_t length);
int lcd_device_backlight(struct lcd_device *lcd, bool enabled);

#endif
