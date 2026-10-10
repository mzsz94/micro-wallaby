/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MZSZ_LCD_PROTOCOL_H
#define MZSZ_LCD_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/* Shared wire contract. All multi-byte fields are little-endian.
 * These UUID components can be passed to Zephyr BT_UUID_128_ENCODE.
 */
#define LCD_SERVICE_UUID_PARTS 0x7b1e1001, 0x21a8, 0x4f8c, 0xbb2d, 0x4c7d2156f001
#define LCD_COMMAND_UUID_PARTS 0x7b1e1002, 0x21a8, 0x4f8c, 0xbb2d, 0x4c7d2156f001
#define LCD_STATUS_UUID_PARTS  0x7b1e1003, 0x21a8, 0x4f8c, 0xbb2d, 0x4c7d2156f001
#define LCD_SERVICE_UUID_STRING "7b1e1001-21a8-4f8c-bb2d-4c7d2156f001"
#define LCD_COMMAND_UUID_STRING "7b1e1002-21a8-4f8c-bb2d-4c7d2156f001"
#define LCD_STATUS_UUID_STRING  "7b1e1003-21a8-4f8c-bb2d-4c7d2156f001"

#define LCD_PROTOCOL_VERSION 1U
#define LCD_COMMAND_MAX_SIZE 20U
#define LCD_COMMAND_HEADER_SIZE 4U
#define LCD_WRITE_PREFIX_SIZE 6U
#define LCD_TEXT_CHUNK_MAX 14U
#define LCD_STATUS_SIZE 8U
#define LCD_MAX_COLUMNS 20U
#define LCD_MAX_ROWS 4U
#define LCD_DEVICE_NAME "MZSZ-LCD"

enum lcd_opcode {
	LCD_OP_STATUS = 0,
	LCD_OP_CLEAR = 1,
	LCD_OP_WRITE = 2,
	LCD_OP_BACKLIGHT = 3,
	LCD_OP_ENTER_DFU = 0x10,
};

enum lcd_result {
	LCD_RESULT_OK = 0,
	LCD_RESULT_INVALID = 1,
	LCD_RESULT_BUSY = 2,
	LCD_RESULT_IO = 3,
	LCD_RESULT_UNCONFIGURED = 4,
	LCD_RESULT_DFU_DISABLED = 5,
};

#define LCD_FLAG_READY 0x01U
#define LCD_FLAG_DFU_CAPABLE 0x02U

/* Command: [version, opcode, sequence_lo, sequence_hi, payload...]
 * CLEAR: no payload.
 * WRITE: row, column, then 1..14 printable ASCII cells (0x20..0x7e).
 * BACKLIGHT: one byte, 0 or 1.
 * ENTER_DFU: four literal bytes 'D','F','U','!' (accident guard, not authentication).
 * STATUS: no payload; queries readiness/geometry without changing the display.
 * Reject prepared/offset writes. GATT write acceptance only queues work.
 * Status after completion: [version, opcode, seq_lo, seq_hi, result,
 *                           flags, columns, rows].
 * A gateway must await matching completion before issuing the next command.
 * It must not automatically retry ENTER_DFU or replay commands after reconnect.
 */
static inline uint16_t lcd_get_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline void lcd_put_le16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

#endif
