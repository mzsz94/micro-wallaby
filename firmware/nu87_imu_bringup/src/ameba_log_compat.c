/* SPDX-License-Identifier: Apache-2.0 */
/* Pinned HAL flash/I2C diagnostics need the same missing logging ABI as Wi-Fi.
 * Keep this small non-Wi-Fi adapter local until the HAL logger is shared upstream.
 */
#include <soc.h>
#include <ameba_soc.h>
#include <stdarg.h>
#include <zephyr/sys/cbprintf.h>

static int character_out(int character, void *context)
{
	(void)context;
	LOGUART_PutChar((uint8_t)character);
	return character;
}

int DiagVprintf(const char *format, va_list args)
{
	return cbvprintf(character_out, NULL, format, args);
}
