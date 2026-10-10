#ifndef NU40_LCD_COMMAND_H
#define NU40_LCD_COMMAND_H

#include "lcd_protocol.h"

struct lcd_command {
	uint8_t opcode;
	uint16_t sequence;
	uint8_t row;
	uint8_t column;
	uint8_t length;
	uint8_t text[LCD_TEXT_CHUNK_MAX];
	uint8_t backlight;
};

/* Returns zero on success, negative errno on invalid wire data/geometry. */
int lcd_command_parse(const void *bytes, size_t length, uint8_t columns,
		      uint8_t rows, struct lcd_command *command);

#endif
