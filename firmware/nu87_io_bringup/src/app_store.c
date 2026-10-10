/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <microwallaby/app_store.h>

K_MUTEX_DEFINE(input_lock);
K_MUTEX_DEFINE(control_lock);

static struct mw_input_snapshot latest_input;
static struct mw_control_snapshot latest_control;
static bool input_ready;
static bool control_ready;
static atomic_t current_state;
static atomic_t counters[MW_STAT_COUNT];

void mw_store_publish_input(const struct mw_input_snapshot *input)
{
	k_mutex_lock(&input_lock, K_FOREVER);
	latest_input = *input;
	input_ready = true;
	k_mutex_unlock(&input_lock);
}

bool mw_store_read_input(struct mw_input_snapshot *input)
{
	bool ready;

	k_mutex_lock(&input_lock, K_FOREVER);
	*input = latest_input;
	ready = input_ready;
	k_mutex_unlock(&input_lock);
	return ready;
}

void mw_store_publish_control(const struct mw_control_snapshot *control)
{
	k_mutex_lock(&control_lock, K_FOREVER);
	latest_control = *control;
	control_ready = true;
	k_mutex_unlock(&control_lock);
}

bool mw_store_read_control(struct mw_control_snapshot *control)
{
	bool ready;

	k_mutex_lock(&control_lock, K_FOREVER);
	*control = latest_control;
	ready = control_ready;
	k_mutex_unlock(&control_lock);
	return ready;
}

void mw_store_publish_state(enum mw_robot_state state)
{
	atomic_set(&current_state, state);
}

enum mw_robot_state mw_store_read_state(void)
{
	return (enum mw_robot_state)atomic_get(&current_state);
}

void mw_stats_increment(enum mw_stat_id id)
{
	if (id < MW_STAT_COUNT) {
		atomic_inc(&counters[id]);
	}
}

struct mw_runtime_stats mw_stats_snapshot(void)
{
	return (struct mw_runtime_stats){
		.sensor_ticks = (uint32_t)atomic_get(&counters[MW_STAT_SENSOR_TICK]),
		.safety_ticks = (uint32_t)atomic_get(&counters[MW_STAT_SAFETY_TICK]),
		.behavior_ticks = (uint32_t)atomic_get(&counters[MW_STAT_BEHAVIOR_TICK]),
		.adc_errors = (uint32_t)atomic_get(&counters[MW_STAT_ADC_ERROR]),
		.gpio_errors = (uint32_t)atomic_get(&counters[MW_STAT_GPIO_ERROR]),
		.deadline_misses = (uint32_t)atomic_get(&counters[MW_STAT_DEADLINE_MISS]),
	};
}
