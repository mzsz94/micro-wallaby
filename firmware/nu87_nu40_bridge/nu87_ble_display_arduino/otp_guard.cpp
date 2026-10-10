/* SPDX-License-Identifier: Apache-2.0 */
#include <stdint.h>

/* The native SDK exposes both ROM write entry points. This demo never provisions
 * factory calibration or identity, including when the logical map is empty.
 * AmebaD 3.1.9 declares both results as u32: 1 is success, 0 is failure.
 */
extern "C" uint32_t __wrap_EFUSEWrite8(uint32_t setting, uint32_t address,
				     uint8_t value, uint8_t voltage)
{
	(void)setting;
	(void)address;
	(void)value;
	(void)voltage;
	return 0;
}

extern "C" uint32_t __wrap_EFUSE_LogicalMap_Write(uint32_t address, uint32_t count, uint8_t *data)
{
	(void)address;
	(void)count;
	(void)data;
	return 0;
}
