/* SPDX-License-Identifier: Apache-2.0 */

#ifndef MICROWALLABY_IO_TYPES_H_
#define MICROWALLABY_IO_TYPES_H_

#include <stdbool.h>
#include <stdint.h>

enum mw_fixture_profile {
	MW_FIXTURE_COMBINED,
	MW_FIXTURE_TOUCH_ANALOG,
};

enum mw_robot_state {
	MW_STATE_BOOT,
	MW_STATE_SELF_TEST,
	MW_STATE_SAFE_IDLE,
	MW_STATE_MANUAL_EMU,
	MW_STATE_FAULT,
};

enum mw_fault_code {
	MW_FAULT_NONE,
	MW_FAULT_INPUT_UNAVAILABLE,
	MW_FAULT_INPUT_STALE,
	MW_FAULT_TOUCH_ACTIVE,
};

struct mw_input_snapshot {
	uint32_t sequence;
	int64_t captured_ms;
	enum mw_fixture_profile profile;
	uint16_t analog_primary;
	uint16_t analog_secondary;
	int16_t joystick_x_permille;
	int16_t joystick_y_permille;
	bool touch_raw_level;
	bool service_raw_level;
	bool touch_active;
	bool service_pressed;
	bool joystick_present;
	bool touch_analog_present;
	bool input_ok;
};

struct mw_control_snapshot {
	uint32_t source_sequence;
	int64_t updated_ms;
	enum mw_fault_code fault;
	uint16_t clear_hold_ms;
	bool input_fresh;
	bool fault_latched;
	bool manual_requested;
	bool just_cleared;
};

struct mw_runtime_stats {
	uint32_t sensor_ticks;
	uint32_t safety_ticks;
	uint32_t behavior_ticks;
	uint32_t adc_errors;
	uint32_t gpio_errors;
	uint32_t deadline_misses;
};

#endif /* MICROWALLABY_IO_TYPES_H_ */
