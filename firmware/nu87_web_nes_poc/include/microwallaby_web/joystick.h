/* SPDX-License-Identifier: Apache-2.0 */

#ifndef MICROWALLABY_WEB_JOYSTICK_H_
#define MICROWALLABY_WEB_JOYSTICK_H_

#include <stdbool.h>
#include <stdint.h>

struct mw_joystick_sample {
	uint32_t sequence;
	int64_t captured_ms;
	uint16_t raw_x;
	uint16_t raw_y;
	int16_t x_permille;
	int16_t y_permille;
	bool sw_pressed;
	bool input_ok;
};

struct mw_joystick_stats {
	uint32_t samples;
	uint32_t input_errors;
	uint32_t deadline_misses;
};

int mw_joystick_start(void);
bool mw_joystick_latest(struct mw_joystick_sample *sample);
void mw_joystick_get_stats(struct mw_joystick_stats *stats);

#endif /* MICROWALLABY_WEB_JOYSTICK_H_ */
