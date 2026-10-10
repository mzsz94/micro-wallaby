/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NU87_NATIVE_SCAN_FILTER_H_
#define NU87_NATIVE_SCAN_FILTER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NATIVE_SERVICE_AD_SIZE 18

/* Validate AD framing, then expose only one target UUID to the SDK parser.
 * The SDK's service list has seven entries and its parser does not enforce that limit.
 */
static inline bool native_scan_sanitize(const uint8_t *data, size_t length,
					const uint8_t uuid[16], uint8_t output[18])
{
	bool matched = false;
	size_t position = 0;

	if (data == NULL || uuid == NULL || output == NULL || length > 31) {
		return false;
	}
	while (position < length) {
		uint8_t field_length = data[position++];

		if (field_length == 0) {
			break;
		}
		if (field_length > length - position) {
			return false;
		}
		if (data[position] == 0x06 || data[position] == 0x07) {
			size_t payload_length = field_length - 1U;

			if (payload_length == 0 || payload_length % 16U != 0) {
				return false;
			}
			for (size_t offset = 0; offset < payload_length; offset += 16U) {
				if (memcmp(data + position + 1U + offset, uuid, 16) == 0) {
					matched = true;
				}
			}
		}
		position += field_length;
	}
	if (matched) {
		output[0] = 17;
		output[1] = 0x07;
		memcpy(output + 2, uuid, 16);
	}
	return matched;
}

#endif
