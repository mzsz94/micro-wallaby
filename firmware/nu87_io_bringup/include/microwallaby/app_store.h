/* SPDX-License-Identifier: Apache-2.0 */

#ifndef MICROWALLABY_APP_STORE_H_
#define MICROWALLABY_APP_STORE_H_

#include <stdbool.h>

#include <microwallaby/io_types.h>

enum mw_stat_id {
	MW_STAT_SENSOR_TICK,
	MW_STAT_SAFETY_TICK,
	MW_STAT_BEHAVIOR_TICK,
	MW_STAT_ADC_ERROR,
	MW_STAT_GPIO_ERROR,
	MW_STAT_DEADLINE_MISS,
	MW_STAT_COUNT,
};

void mw_store_publish_input(const struct mw_input_snapshot *input);
bool mw_store_read_input(struct mw_input_snapshot *input);
void mw_store_publish_control(const struct mw_control_snapshot *control);
bool mw_store_read_control(struct mw_control_snapshot *control);
void mw_store_publish_state(enum mw_robot_state state);
enum mw_robot_state mw_store_read_state(void);

void mw_stats_increment(enum mw_stat_id id);
struct mw_runtime_stats mw_stats_snapshot(void);

#endif /* MICROWALLABY_APP_STORE_H_ */
