/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

#include <microwallaby/behavior_sm.h>
#include <microwallaby/diag_stats.h>
#include <microwallaby/health_log.h>
#include <microwallaby/safety_ctrl.h>
#include <microwallaby/sensor_fast.h>

LOG_MODULE_DECLARE(mw_bringup);

void mw_health_log_report(const struct mw_input_snapshot *input,
				  const struct mw_control_snapshot *control,
				  enum mw_robot_state state,
				  const struct mw_runtime_stats *stats)
{
	struct mw_diag_snapshot diag;

	mw_diag_stats_snapshot(&diag, k_uptime_get());
	LOG_INF("profile=%s seq=%u adc=%u,%u joy=%d,%d raw_d0=%u raw_sw=%u touch=%u sw=%u "
		"state=%s fault=%s fresh=%u clear_ms=%u",
		mw_fixture_name(input->profile), input->sequence,
		(unsigned int)input->analog_primary,
		(unsigned int)input->analog_secondary,
		input->joystick_x_permille, input->joystick_y_permille,
		(unsigned int)input->touch_raw_level,
		(unsigned int)input->service_raw_level,
		(unsigned int)input->touch_active,
		(unsigned int)input->service_pressed,
		mw_state_name(state), mw_fault_name(control->fault),
		(unsigned int)control->input_fresh,
		(unsigned int)control->clear_hold_ms);

	LOG_INF("ticks=%u/%u/%u errors=adc:%u gpio:%u deadline:%u "
		"diag=ok:%u/%u input:%u adc:%u gpio:%u",
		stats->sensor_ticks, stats->safety_ticks, stats->behavior_ticks,
		stats->adc_errors, stats->gpio_errors, stats->deadline_misses,
		diag.sample_successes, diag.sample_attempts, diag.input_errors,
		diag.adc_errors, diag.gpio_errors);

#if defined(CONFIG_MW_FIXTURE_COMBINED)
	LOG_INF("diag_dig touch=tr:%u/a:%u/idle:%u sw=tr:%u/a:%u/idle:%u "
		"boot_idle=%u/%u done=%u false=%u/%u",
		diag.touch.transitions, diag.touch.assertions, diag.touch.inactive_ms,
		diag.service_switch.transitions, diag.service_switch.assertions,
		diag.service_switch.inactive_ms, diag.boot_idle_elapsed_ms,
		MW_DIAG_BOOT_IDLE_MS, (unsigned int)diag.boot_idle_complete,
		diag.boot_idle_touch_assertions, diag.boot_idle_switch_assertions);

	LOG_INF("diag_adc vrx=n:%u range:%u..%u mean:%u sd:%u "
		"vry=n:%u range:%u..%u mean:%u sd:%u",
		diag.axis_primary.samples, (unsigned int)diag.axis_primary.minimum,
		(unsigned int)diag.axis_primary.maximum,
		(unsigned int)diag.axis_primary.mean,
		(unsigned int)diag.axis_primary.noise_stddev,
		diag.axis_secondary.samples, (unsigned int)diag.axis_secondary.minimum,
		(unsigned int)diag.axis_secondary.maximum,
		(unsigned int)diag.axis_secondary.mean,
		(unsigned int)diag.axis_secondary.noise_stddev);

	LOG_INF("diag_center done=%u elapsed=%u/%u "
		"vrx=n:%u range:%u..%u sd:%u vry=n:%u range:%u..%u sd:%u",
		(unsigned int)diag.center_complete, diag.center_elapsed_ms,
		MW_DIAG_CENTER_WINDOW_MS, diag.center_primary.samples,
		(unsigned int)diag.center_primary.minimum,
		(unsigned int)diag.center_primary.maximum,
		(unsigned int)diag.center_primary.noise_stddev,
		diag.center_secondary.samples,
		(unsigned int)diag.center_secondary.minimum,
		(unsigned int)diag.center_secondary.maximum,
		(unsigned int)diag.center_secondary.noise_stddev);
#else
	LOG_INF("diag_dig touch=tr:%u/a:%u/r:%u idle:%u max_idle:%u samples=%u/%u",
		diag.touch.transitions, diag.touch.assertions, diag.touch.releases,
		diag.touch.inactive_ms, diag.touch.max_inactive_ms,
		diag.sample_successes, diag.sample_attempts);

	LOG_INF("diag_a0 phase=%s progress=%u/%u "
		"idle=%u:%u/%u/%u@%ums touched=%u:%u/%u/%u@%ums",
		mw_diag_a0_phase_name(diag.a0_phase), (unsigned int)diag.a0_progress,
		MW_DIAG_A0_WINDOW_SAMPLES, (unsigned int)diag.a0_idle.complete,
		(unsigned int)diag.a0_idle.minimum, (unsigned int)diag.a0_idle.median,
		(unsigned int)diag.a0_idle.maximum, diag.a0_idle.span_ms,
		(unsigned int)diag.a0_touched.complete,
		(unsigned int)diag.a0_touched.minimum,
		(unsigned int)diag.a0_touched.median,
		(unsigned int)diag.a0_touched.maximum, diag.a0_touched.span_ms);
#endif
}
