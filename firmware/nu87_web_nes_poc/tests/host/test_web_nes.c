/* SPDX-License-Identifier: Apache-2.0 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <microwallaby/input_filter.h>
#include <microwallaby_web/input_protocol.h>
#include <microwallaby_web/websocket_control.h>

static void test_axis_normalization_used_by_web_input(void)
{
	assert(mw_axis_normalize(0, 0, 2048, 4095, 205) == -1000);
	assert(mw_axis_normalize(2048, 0, 2048, 4095, 205) == 0);
	assert(mw_axis_normalize(4095, 0, 2048, 4095, 205) == 1000);
	assert(mw_axis_normalize(4095, 2048, 2048, 4095, 205) == 0);
}

static void test_switch_debounce(void)
{
	struct mw_debounce filter = {0};
	bool changed;

	assert(!mw_debounce_update(&filter, false, 3, &changed));
	assert(!mw_debounce_update(&filter, true, 3, &changed));
	assert(!mw_debounce_update(&filter, true, 3, &changed));
	assert(mw_debounce_update(&filter, true, 3, &changed));
	assert(changed);
}

static void test_protocol_payload_is_bounded_and_complete(void)
{
	const struct mw_joystick_sample sample = {
		.sequence = UINT32_MAX,
		.captured_ms = INT64_MAX - 100,
		.x_permille = -1000,
		.y_permille = 1000,
		.sw_pressed = true,
		.input_ok = true,
	};
	char payload[192];
	char too_small[16];
	int length = mw_input_event_format(payload, sizeof(payload), &sample,
					   123U, INT64_MAX);

	assert(length > 0);
	assert(length < 160);
	assert((size_t)length == strlen(payload));
	assert(strstr(payload, "\"v\":1") != NULL);
	assert(strstr(payload, "\"seq\":123") != NULL);
	assert(strstr(payload, "\"sample_seq\":4294967295") != NULL);
	assert(strstr(payload, "\"x\":-1000") != NULL);
	assert(strstr(payload, "\"sw\":true") != NULL);
	assert(mw_input_event_format(too_small, sizeof(too_small), &sample,
				     123U, INT64_MAX) == -ENOSPC);
	assert(mw_input_event_format(NULL, 0, &sample, 0U, 0) == -EINVAL);
}

static void test_stale_input_is_forced_safe(void)
{
	struct mw_joystick_sample sample = {
		.sequence = 7U,
		.captured_ms = 1000,
		.x_permille = 900,
		.y_permille = -900,
		.sw_pressed = true,
		.input_ok = true,
	};

	assert(!mw_input_sample_sanitize(&sample, 1100, 100U));
	assert(sample.sw_pressed);
	assert(mw_input_sample_sanitize(&sample, 1101, 100U));
	assert(!sample.input_ok);
	assert(!sample.sw_pressed);
	assert(sample.x_permille == 0);
	assert(sample.y_permille == 0);

	sample.captured_ms = 2000;
	sample.input_ok = true;
	sample.sw_pressed = true;
	assert(mw_input_sample_sanitize(&sample, 1999, 100U));
	assert(!sample.sw_pressed);
}

static void test_websocket_control_frames_must_be_final(void)
{
	assert(mw_websocket_control_frame_validate(true, true, false, false, 0U) == 0);
	assert(mw_websocket_control_frame_validate(true, false, true, false, 0U) == 0);
	assert(mw_websocket_control_frame_validate(true, false, false, true, 0U) == 0);

	assert(mw_websocket_control_frame_validate(false, true, false, false, 0U) ==
	       -EPROTO);
	assert(mw_websocket_control_frame_validate(false, false, true, false, 0U) ==
	       -EPROTO);
	assert(mw_websocket_control_frame_validate(false, false, false, true, 0U) ==
	       -EPROTO);
	assert(mw_websocket_control_frame_validate(true, false, false, false, 0U) ==
	       -EPROTO);
	assert(mw_websocket_control_frame_validate(true, true, true, false, 0U) ==
	       -EPROTO);
	assert(mw_websocket_control_frame_validate(true, false, true, false, 1U) ==
	       -EMSGSIZE);
}

int main(void)
{
	test_axis_normalization_used_by_web_input();
	test_switch_debounce();
	test_protocol_payload_is_bounded_and_complete();
	test_stale_input_is_forced_safe();
	test_websocket_control_frames_must_be_final();
	puts("microWallaby web NES host tests: PASS");
	return 0;
}
