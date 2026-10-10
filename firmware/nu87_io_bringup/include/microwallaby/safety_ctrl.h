/* SPDX-License-Identifier: Apache-2.0 */

#ifndef MICROWALLABY_SAFETY_CTRL_H_
#define MICROWALLABY_SAFETY_CTRL_H_

#include <stdint.h>

#include <microwallaby/io_types.h>

#define MW_INPUT_MAX_AGE_MS 50
#define MW_CLEAR_HOLD_MS 2000
#define MW_NEUTRAL_LIMIT_PERMILLE 100

struct mw_safety_ctrl {
	enum mw_fault_code fault;
	int64_t clear_press_started_ms;
	uint16_t clear_hold_ms;
	bool fault_latched;
	bool clear_release_seen;
};

void mw_safety_ctrl_init(struct mw_safety_ctrl *ctx);

struct mw_control_snapshot mw_safety_ctrl_step(struct mw_safety_ctrl *ctx,
						const struct mw_input_snapshot *input,
						int64_t now_ms);

const char *mw_fault_name(enum mw_fault_code fault);

#endif /* MICROWALLABY_SAFETY_CTRL_H_ */
