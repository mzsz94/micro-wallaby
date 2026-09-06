/* SPDX-License-Identifier: Apache-2.0 */

#ifndef MICROWALLABY_HEALTH_LOG_H_
#define MICROWALLABY_HEALTH_LOG_H_

#include <microwallaby/io_types.h>

void mw_health_log_report(const struct mw_input_snapshot *input,
				  const struct mw_control_snapshot *control,
				  enum mw_robot_state state,
				  const struct mw_runtime_stats *stats);

#endif /* MICROWALLABY_HEALTH_LOG_H_ */
