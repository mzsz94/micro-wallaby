/* SPDX-License-Identifier: Apache-2.0 */

#include <assert.h>
#include <stdio.h>

#include <microwallaby/behavior_sm.h>
#include <microwallaby/input_filter.h>
#include <microwallaby/safety_ctrl.h>

static void test_debounce_requires_three_stable_samples(void)
{
	struct mw_debounce filter = {0};
	bool changed;

	assert(!mw_debounce_update(&filter, false, 3, &changed));
	assert(!changed);
	assert(!mw_debounce_update(&filter, true, 3, &changed));
	assert(!mw_debounce_update(&filter, false, 3, &changed));
	assert(!mw_debounce_update(&filter, true, 3, &changed));
	assert(!mw_debounce_update(&filter, true, 3, &changed));
	assert(mw_debounce_update(&filter, true, 3, &changed));
	assert(changed);
	assert(mw_debounce_update(&filter, true, 3, &changed));
	assert(!changed);
}

static void test_axis_normalization(void)
{
	assert(mw_axis_normalize(2048, 0, 2048, 4095, 205) == 0);
	assert(mw_axis_normalize(0, 0, 2048, 4095, 205) == -1000);
	assert(mw_axis_normalize(4095, 0, 2048, 4095, 205) == 1000);
	assert(mw_axis_is_neutral(100, -100, 100));
	assert(!mw_axis_is_neutral(101, 0, 100));
}

static struct mw_input_snapshot valid_combined_input(void)
{
	return (struct mw_input_snapshot){
		.sequence = 1,
		.captured_ms = 1000,
		.profile = MW_FIXTURE_COMBINED,
		.joystick_present = true,
		.input_ok = true,
	};
}

static struct mw_control_snapshot safety_step_at(struct mw_safety_ctrl *safety,
						 struct mw_input_snapshot *input,
						 int64_t now_ms)
{
	input->sequence++;
	input->captured_ms = now_ms;
	return mw_safety_ctrl_step(safety, input, now_ms);
}

static void test_fault_clamps_manual_and_requires_new_press(void)
{
	struct mw_safety_ctrl safety;
	struct mw_input_snapshot input = valid_combined_input();
	struct mw_control_snapshot output;

	mw_safety_ctrl_init(&safety);
	input.joystick_x_permille = 1000;
	input.touch_active = true;
	input.service_pressed = true;
	output = safety_step_at(&safety, &input, 1000);
	assert(output.fault_latched);
	assert(output.fault == MW_FAULT_TOUCH_ACTIVE);
	assert(!output.manual_requested);

	/* A switch held before recovery must never acknowledge the fault. */
	input.touch_active = false;
	input.joystick_x_permille = 0;
	output = safety_step_at(&safety, &input, 4000);
	assert(output.fault_latched);
	assert(output.clear_hold_ms == 0);

	/* Recovery is armed by a release observed after the fault is safe. */
	input.service_pressed = false;
	output = safety_step_at(&safety, &input, 4020);
	assert(output.fault_latched);

	input.service_pressed = true;
	output = safety_step_at(&safety, &input, 4040);
	assert(output.fault_latched);
	assert(output.clear_hold_ms == 0);
	output = safety_step_at(&safety, &input, 6039);
	assert(output.fault_latched);
	assert(output.clear_hold_ms == MW_CLEAR_HOLD_MS - 1);
	output = safety_step_at(&safety, &input, 6040);
	assert(!output.fault_latched);
	assert(output.just_cleared);
	assert(!output.manual_requested);
}

static void test_interrupted_clear_requires_another_release(void)
{
	struct mw_safety_ctrl safety;
	struct mw_input_snapshot input = valid_combined_input();
	struct mw_control_snapshot output;

	mw_safety_ctrl_init(&safety);
	input.touch_active = true;
	output = safety_step_at(&safety, &input, 1000);
	assert(output.fault_latched);

	input.touch_active = false;
	input.service_pressed = false;
	output = safety_step_at(&safety, &input, 1020);
	input.service_pressed = true;
	output = safety_step_at(&safety, &input, 1040);
	output = safety_step_at(&safety, &input, 2040);
	assert(output.clear_hold_ms == 1000);

	/* Leaving neutral disarms the gesture, even if the switch remains held. */
	input.joystick_x_permille = 1000;
	output = safety_step_at(&safety, &input, 2060);
	assert(output.fault_latched);
	assert(output.clear_hold_ms == 0);
	input.joystick_x_permille = 0;
	output = safety_step_at(&safety, &input, 5000);
	assert(output.fault_latched);

	input.service_pressed = false;
	output = safety_step_at(&safety, &input, 5020);
	input.service_pressed = true;
	output = safety_step_at(&safety, &input, 5040);
	output = safety_step_at(&safety, &input, 7040);
	assert(!output.fault_latched);
	assert(output.just_cleared);
}

static void test_stale_input_fails_closed_and_clamps_manual(void)
{
	struct mw_safety_ctrl safety;
	struct mw_input_snapshot input = valid_combined_input();
	struct mw_control_snapshot output;

	mw_safety_ctrl_init(&safety);
	input.joystick_x_permille = 1000;
	output = mw_safety_ctrl_step(&safety, &input, 1100);
	assert(output.fault_latched);
	assert(output.fault == MW_FAULT_INPUT_STALE);
	assert(!output.manual_requested);
}

static void test_manual_request_is_exposed_only_for_safe_input(void)
{
	struct mw_safety_ctrl safety;
	struct mw_input_snapshot input = valid_combined_input();
	struct mw_control_snapshot output;

	mw_safety_ctrl_init(&safety);
	input.joystick_x_permille = 1000;
	output = safety_step_at(&safety, &input, 1000);
	assert(!output.fault_latched);
	assert(output.input_fresh);
	assert(output.manual_requested);

	input.input_ok = false;
	output = safety_step_at(&safety, &input, 1020);
	assert(output.fault_latched);
	assert(!output.manual_requested);
}

static void test_behavior_state_transitions(void)
{
	struct mw_behavior_sm behavior;
	struct mw_control_snapshot control = {0};

	mw_behavior_sm_init(&behavior);
	assert(mw_behavior_sm_step(&behavior, &control) == MW_STATE_SELF_TEST);
	assert(mw_behavior_sm_step(&behavior, &control) == MW_STATE_SAFE_IDLE);
	control.manual_requested = true;
	assert(mw_behavior_sm_step(&behavior, &control) == MW_STATE_MANUAL_EMU);
	control.fault_latched = true;
	assert(mw_behavior_sm_step(&behavior, &control) == MW_STATE_FAULT);
	control.fault_latched = false;
	assert(mw_behavior_sm_step(&behavior, &control) == MW_STATE_SAFE_IDLE);
}

int main(void)
{
	test_debounce_requires_three_stable_samples();
	test_axis_normalization();
	test_fault_clamps_manual_and_requires_new_press();
	test_interrupted_clear_requires_another_release();
	test_stale_input_fails_closed_and_clamps_manual();
	test_manual_request_is_exposed_only_for_safe_input();
	test_behavior_state_transitions();
	puts("microWallaby host logic tests: PASS");
	return 0;
}
