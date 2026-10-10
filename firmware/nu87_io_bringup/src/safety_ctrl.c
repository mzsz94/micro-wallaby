/* SPDX-License-Identifier: Apache-2.0 */

#include <microwallaby/input_filter.h>
#include <microwallaby/safety_ctrl.h>

void mw_safety_ctrl_init(struct mw_safety_ctrl *ctx)
{
	*ctx = (struct mw_safety_ctrl){0};
	ctx->clear_press_started_ms = -1;
}

static void reset_clear_handshake(struct mw_safety_ctrl *ctx)
{
	ctx->clear_press_started_ms = -1;
	ctx->clear_hold_ms = 0;
	ctx->clear_release_seen = false;
}

static void latch_fault(struct mw_safety_ctrl *ctx, enum mw_fault_code fault)
{
	ctx->fault_latched = true;
	ctx->fault = fault;
	reset_clear_handshake(ctx);
}

struct mw_control_snapshot mw_safety_ctrl_step(struct mw_safety_ctrl *ctx,
						const struct mw_input_snapshot *input,
						int64_t now_ms)
{
	struct mw_control_snapshot output = {
		.source_sequence = input->sequence,
		.updated_ms = now_ms,
	};
	bool timestamp_valid = now_ms >= input->captured_ms;
	bool fresh = timestamp_valid && (now_ms - input->captured_ms) <= MW_INPUT_MAX_AGE_MS;
	bool neutral = mw_axis_is_neutral(input->joystick_x_permille,
					       input->joystick_y_permille,
					       MW_NEUTRAL_LIMIT_PERMILLE);
	bool desired_manual = input->joystick_present && !neutral;

	output.input_fresh = input->input_ok && fresh;
	if (input->profile == MW_FIXTURE_TOUCH_ANALOG) {
		desired_manual = input->touch_active;
	}

	if (!input->input_ok) {
		latch_fault(ctx, MW_FAULT_INPUT_UNAVAILABLE);
	} else if (!fresh) {
		latch_fault(ctx, MW_FAULT_INPUT_STALE);
	} else if (input->profile == MW_FIXTURE_COMBINED && input->touch_active) {
		latch_fault(ctx, MW_FAULT_TOUCH_ACTIVE);
	}

	if (ctx->fault_latched) {
		bool clear_conditions_safe = input->profile == MW_FIXTURE_COMBINED &&
					     output.input_fresh && !input->touch_active && neutral;

		if (!clear_conditions_safe) {
			reset_clear_handshake(ctx);
		} else if (!input->service_pressed) {
			/* A release after the fault arms the explicit recovery gesture. */
			ctx->clear_release_seen = true;
			ctx->clear_press_started_ms = -1;
			ctx->clear_hold_ms = 0;
		} else if (ctx->clear_release_seen) {
			if (ctx->clear_press_started_ms < 0) {
				ctx->clear_press_started_ms = now_ms;
			}

			if (now_ms < ctx->clear_press_started_ms) {
				reset_clear_handshake(ctx);
			} else {
				int64_t elapsed_ms = now_ms - ctx->clear_press_started_ms;

				ctx->clear_hold_ms = elapsed_ms < MW_CLEAR_HOLD_MS ?
						     (uint16_t)elapsed_ms : MW_CLEAR_HOLD_MS;
				if (elapsed_ms >= MW_CLEAR_HOLD_MS) {
					ctx->fault_latched = false;
					ctx->fault = MW_FAULT_NONE;
					reset_clear_handshake(ctx);
					output.just_cleared = true;
				}
			}
		}
	}

	output.fault_latched = ctx->fault_latched;
	output.fault = ctx->fault;
	output.clear_hold_ms = ctx->clear_hold_ms;
	output.manual_requested = output.input_fresh && !ctx->fault_latched && desired_manual;
	return output;
}

const char *mw_fault_name(enum mw_fault_code fault)
{
	switch (fault) {
	case MW_FAULT_NONE:
		return "NONE";
	case MW_FAULT_INPUT_UNAVAILABLE:
		return "INPUT_UNAVAILABLE";
	case MW_FAULT_INPUT_STALE:
		return "INPUT_STALE";
	case MW_FAULT_TOUCH_ACTIVE:
		return "TOUCH_ACTIVE";
	default:
		return "UNKNOWN";
	}
}
