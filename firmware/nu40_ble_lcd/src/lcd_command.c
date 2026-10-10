#include "lcd_command.h"

#include <errno.h>
#include <string.h>

int lcd_command_parse(const void *bytes, size_t length, uint8_t columns,
		      uint8_t rows, struct lcd_command *command)
{
	const uint8_t *wire = bytes;
	struct lcd_command parsed = {0};

	if (!wire || !command || length < LCD_COMMAND_HEADER_SIZE ||
	    length > LCD_COMMAND_MAX_SIZE || wire[0] != LCD_PROTOCOL_VERSION ||
	    columns == 0 || columns > LCD_MAX_COLUMNS ||
	    !(rows == 1 || rows == 2 || rows == 4)) {
		return -EINVAL;
	}
	parsed.opcode = wire[1];
	parsed.sequence = lcd_get_le16(wire + 2);
	switch (parsed.opcode) {
	case LCD_OP_STATUS:
	case LCD_OP_CLEAR:
		if (length != LCD_COMMAND_HEADER_SIZE) {
			return -EINVAL;
		}
		break;
	case LCD_OP_WRITE:
		if (length <= LCD_WRITE_PREFIX_SIZE) {
			return -EINVAL;
		}
		parsed.row = wire[4];
		parsed.column = wire[5];
		parsed.length = length - LCD_WRITE_PREFIX_SIZE;
		if (parsed.row >= rows || parsed.column >= columns ||
		    parsed.length > columns - parsed.column) {
			return -ERANGE;
		}
		for (size_t i = 0; i < parsed.length; ++i) {
			if (wire[LCD_WRITE_PREFIX_SIZE + i] < 0x20 ||
			    wire[LCD_WRITE_PREFIX_SIZE + i] > 0x7e) {
				return -EINVAL;
			}
		}
		memcpy(parsed.text, wire + LCD_WRITE_PREFIX_SIZE, parsed.length);
		break;
	case LCD_OP_BACKLIGHT:
		if (length != LCD_COMMAND_HEADER_SIZE + 1 || wire[4] > 1) {
			return -EINVAL;
		}
		parsed.backlight = wire[4];
		break;
	case LCD_OP_ENTER_DFU:
		if (length != LCD_COMMAND_HEADER_SIZE + 4 ||
		    memcmp(wire + LCD_COMMAND_HEADER_SIZE, "DFU!", 4) != 0) {
			return -EINVAL;
		}
		break;
	default:
		return -ENOTSUP;
	}
	*command = parsed;
	return 0;
}
