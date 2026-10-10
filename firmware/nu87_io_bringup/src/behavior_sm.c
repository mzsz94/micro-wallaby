/* SPDX-License-Identifier: Apache-2.0 */

#include <microwallaby/behavior_sm.h>

void mw_behavior_sm_init(struct mw_behavior_sm *ctx)
{
	ctx->state = MW_STATE_BOOT;
}
enum mw_robot_state mw_behavior_sm_step(struct mw_behavior_sm *ctx,
						const struct mw_control_snapshot *control)
{
	if (control->fault_latched) {
		ctx->state = MW_STATE_FAULT;
		return ctx->state;
	}

	switch (ctx->state) {
	case MW_STATE_BOOT:
		ctx->state = MW_STATE_SELF_TEST;
		break;
	case MW_STATE_SELF_TEST:
	case MW_STATE_FAULT:
		ctx->state = MW_STATE_SAFE_IDLE;
		break;
	case MW_STATE_SAFE_IDLE:
	case MW_STATE_MANUAL_EMU:
		ctx->state = control->manual_requested ? MW_STATE_MANUAL_EMU :
								 MW_STATE_SAFE_IDLE;
		break;
	default:
		ctx->state = MW_STATE_FAULT;
		break;
	}

	return ctx->state;
}

const char *mw_state_name(enum mw_robot_state state)
{
	switch (state) {
	case MW_STATE_BOOT:
		return "BOOT";
	case MW_STATE_SELF_TEST:
		return "SELF_TEST";
	case MW_STATE_SAFE_IDLE:
		return "SAFE_IDLE";
	case MW_STATE_MANUAL_EMU:
		return "MANUAL_EMU";
	case MW_STATE_FAULT:
		return "FAULT";
	default:
		return "UNKNOWN";
	}
}
