/* SPDX-License-Identifier: Apache-2.0 */

#include <microwallaby/input_filter.h>

static int16_t clamp_permille(int32_t value)
{
	if (value > 1000) {
		return 1000;
	}
	if (value < -1000) {
		return -1000;
	}
	return (int16_t)value;
}
bool mw_debounce_update(struct mw_debounce *filter, bool raw,
			uint8_t required_samples, bool *changed)
{
	*changed = false;
	if (!filter->initialized) {
		filter->initialized = true;
		filter->stable = raw;
		filter->candidate = raw;
		filter->consecutive = 0;
		return filter->stable;
	}

	if (raw == filter->stable) {
		filter->candidate = raw;
		filter->consecutive = 0;
		return filter->stable;
	}

	if (raw != filter->candidate) {
		filter->candidate = raw;
		filter->consecutive = 1;
	} else if (filter->consecutive < UINT8_MAX) {
		filter->consecutive++;
	}

	if (filter->consecutive >= required_samples) {
		filter->stable = filter->candidate;
		filter->consecutive = 0;
		*changed = true;
	}

	return filter->stable;
}

int16_t mw_axis_normalize(uint16_t raw, uint16_t minimum, uint16_t center,
			  uint16_t maximum, uint16_t deadzone)
{
	uint32_t upper_start = (uint32_t)center + deadzone;
	int32_t lower_start = (int32_t)center - deadzone;

	if (minimum >= center || center >= maximum) {
		return 0;
	}

	if (raw > upper_start) {
		uint32_t span = maximum > upper_start ? maximum - upper_start : 1U;
		return clamp_permille(((int32_t)raw - (int32_t)upper_start) * 1000 /
				       (int32_t)span);
	}

	if ((int32_t)raw < lower_start) {
		uint32_t span = lower_start > minimum ? (uint32_t)lower_start - minimum : 1U;
		return clamp_permille(-(((int32_t)lower_start - raw) * 1000 /
					(int32_t)span));
	}

	return 0;
}

bool mw_axis_is_neutral(int16_t x_permille, int16_t y_permille,
			uint16_t neutral_limit_permille)
{
	int32_t x = x_permille;
	int32_t y = y_permille;

	if (x < 0) {
		x = -x;
	}
	if (y < 0) {
		y = -y;
	}

	return x <= neutral_limit_permille && y <= neutral_limit_permille;
}
