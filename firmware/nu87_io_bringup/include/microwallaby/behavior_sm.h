/* SPDX-License-Identifier: Apache-2.0 */

#ifndef MICROWALLABY_BEHAVIOR_SM_H_
#define MICROWALLABY_BEHAVIOR_SM_H_

#include <microwallaby/io_types.h>

struct mw_behavior_sm {
	enum mw_robot_state state;
};

void mw_behavior_sm_init(struct mw_behavior_sm *ctx);
enum mw_robot_state mw_behavior_sm_step(struct mw_behavior_sm *ctx,
						const struct mw_control_snapshot *control);
const char *mw_state_name(enum mw_robot_state state);

#endif /* MICROWALLABY_BEHAVIOR_SM_H_ */
