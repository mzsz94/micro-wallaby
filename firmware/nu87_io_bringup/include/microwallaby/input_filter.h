/* SPDX-License-Identifier: Apache-2.0 */

#ifndef MICROWALLABY_INPUT_FILTER_H_
#define MICROWALLABY_INPUT_FILTER_H_

#include <stdbool.h>
#include <stdint.h>

struct mw_debounce {
	bool initialized;
	bool stable;
	bool candidate;
	uint8_t consecutive;
};

bool mw_debounce_update(struct mw_debounce *filter, bool raw,
			uint8_t required_samples, bool *changed);

int16_t mw_axis_normalize(uint16_t raw, uint16_t minimum, uint16_t center,
			  uint16_t maximum, uint16_t deadzone);

bool mw_axis_is_neutral(int16_t x_permille, int16_t y_permille,
			uint16_t neutral_limit_permille);

#endif /* MICROWALLABY_INPUT_FILTER_H_ */
