#ifndef NU40_OLED_DEVICE_H
#define NU40_OLED_DEVICE_H

#include "lcd_device.h"

#define OLED_COLUMNS 16U
#define OLED_ROWS 4U

struct oled_device {
	struct lcd_bus bus;
	uint8_t address;
	bool initialized;
};

/* SSD1306 only: 128x32, internal charge pump, 8x8 text cells, no reset GPIO. */
int oled_device_initialize(struct oled_device *oled, uint8_t address, const struct lcd_bus *bus);
int oled_device_clear(struct oled_device *oled);
int oled_device_write(struct oled_device *oled, uint8_t row, uint8_t column,
		      const uint8_t *text, size_t length);
int oled_device_display(struct oled_device *oled, bool enabled);

#endif
