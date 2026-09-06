/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <microwallaby/app_store.h>
#include <microwallaby/behavior_sm.h>
#include <microwallaby/diag_stats.h>
#include <microwallaby/health_log.h>
#include <microwallaby/indicator.h>
#include <microwallaby/safety_ctrl.h>
#include <microwallaby/sensor_fast.h>

LOG_MODULE_REGISTER(mw_bringup, LOG_LEVEL_INF);

#define SENSOR_PERIOD_MS 10
#define SAFETY_PERIOD_MS 20
#define BEHAVIOR_PERIOD_MS 50
#define INDICATOR_PERIOD_MS 20
#define HEALTH_PERIOD_MS 1000

#define SENSOR_PRIORITY 3
#define SAFETY_PRIORITY 2
#define INDICATOR_PRIORITY 4
#define BEHAVIOR_PRIORITY 5
#define HEALTH_PRIORITY 7

#define WORKER_STACK_SIZE 2048

K_THREAD_STACK_DEFINE(sensor_stack, WORKER_STACK_SIZE);
K_THREAD_STACK_DEFINE(safety_stack, WORKER_STACK_SIZE);
K_THREAD_STACK_DEFINE(behavior_stack, WORKER_STACK_SIZE);
K_THREAD_STACK_DEFINE(indicator_stack, WORKER_STACK_SIZE);
K_THREAD_STACK_DEFINE(health_stack, WORKER_STACK_SIZE);

static struct k_thread sensor_thread_data;
static struct k_thread safety_thread_data;
static struct k_thread behavior_thread_data;
static struct k_thread indicator_thread_data;
static struct k_thread health_thread_data;
static struct mw_safety_ctrl safety_ctx;
static bool indicator_available;

static void sleep_periodic(int64_t *deadline_ms, int32_t period_ms)
{
	int64_t now_ms;

	*deadline_ms += period_ms;
	now_ms = k_uptime_get();
	if (now_ms > *deadline_ms) {
		mw_stats_increment(MW_STAT_DEADLINE_MISS);
		*deadline_ms = now_ms;
		return;
	}

	k_sleep(K_MSEC(*deadline_ms - now_ms));
}

static void sensor_thread(void *unused1, void *unused2, void *unused3)
{
	int64_t deadline_ms = k_uptime_get();

	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	while (true) {
		struct mw_input_snapshot input;

		if (mw_sensor_fast_sample(&input) < 0) {
			input.input_ok = false;
		}
		mw_store_publish_input(&input);
		mw_stats_increment(MW_STAT_SENSOR_TICK);
		sleep_periodic(&deadline_ms, SENSOR_PERIOD_MS);
	}
}

static void safety_thread(void *unused1, void *unused2, void *unused3)
{
	int64_t deadline_ms = k_uptime_get();

	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	while (true) {
		struct mw_input_snapshot input = {0};
		struct mw_control_snapshot control;

		if (!mw_store_read_input(&input)) {
			input.input_ok = false;
		}
		control = mw_safety_ctrl_step(&safety_ctx, &input, k_uptime_get());
		mw_store_publish_control(&control);
		mw_stats_increment(MW_STAT_SAFETY_TICK);
		sleep_periodic(&deadline_ms, SAFETY_PERIOD_MS);
	}
}

static void behavior_thread(void *unused1, void *unused2, void *unused3)
{
	struct mw_behavior_sm behavior;
	int64_t deadline_ms = k_uptime_get();

	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);
	mw_behavior_sm_init(&behavior);

	while (true) {
		struct mw_control_snapshot control = {
			.fault = MW_FAULT_INPUT_UNAVAILABLE,
			.fault_latched = true,
		};
		enum mw_robot_state state;

		(void)mw_store_read_control(&control);
		state = mw_behavior_sm_step(&behavior, &control);
		mw_store_publish_state(state);
		mw_stats_increment(MW_STAT_BEHAVIOR_TICK);
		sleep_periodic(&deadline_ms, BEHAVIOR_PERIOD_MS);
	}
}

static void indicator_thread(void *unused1, void *unused2, void *unused3)
{
	int64_t deadline_ms = k_uptime_get();

	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	while (true) {
		struct mw_control_snapshot control = {
			.fault = MW_FAULT_INPUT_UNAVAILABLE,
			.fault_latched = true,
		};
		enum mw_robot_state state = mw_store_read_state();

		if (!mw_store_read_control(&control) || control.fault_latched) {
			state = MW_STATE_FAULT;
		}
		if (indicator_available && mw_indicator_apply(state) < 0) {
			mw_stats_increment(MW_STAT_GPIO_ERROR);
			indicator_available = false;
		}
		sleep_periodic(&deadline_ms, INDICATOR_PERIOD_MS);
	}
}

static void health_thread(void *unused1, void *unused2, void *unused3)
{
	int64_t deadline_ms = k_uptime_get();

	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	while (true) {
		struct mw_input_snapshot input = {0};
		struct mw_control_snapshot control = {0};
		struct mw_runtime_stats stats = mw_stats_snapshot();

		(void)mw_store_read_input(&input);
		(void)mw_store_read_control(&control);
		mw_health_log_report(&input, &control, mw_store_read_state(), &stats);
		sleep_periodic(&deadline_ms, HEALTH_PERIOD_MS);
	}
}

int main(void)
{
	struct mw_input_snapshot initial_input;
	struct mw_control_snapshot initial_control;
	int ret;

	LOG_INF("microWallaby NU-87 I/O bring-up starting");
	ret = mw_sensor_fast_init();
	if (ret < 0) {
		LOG_ERR("Input initialization failed: %d", ret);
	}

	/* Evaluate and publish a fail-closed input before any cosmetic self-test. */
	if (mw_sensor_fast_sample(&initial_input) < 0) {
		initial_input.input_ok = false;
	}
	mw_store_publish_input(&initial_input);
	mw_safety_ctrl_init(&safety_ctx);
	initial_control = mw_safety_ctrl_step(&safety_ctx, &initial_input, k_uptime_get());
	mw_store_publish_control(&initial_control);

	ret = mw_indicator_init();
	if (ret < 0) {
		LOG_ERR("RGB initialization failed; monitoring will continue: %d", ret);
		mw_stats_increment(MW_STAT_GPIO_ERROR);
	} else {
		indicator_available = true;
		ret = initial_control.fault_latched ? mw_indicator_apply(MW_STATE_FAULT) :
						      mw_indicator_self_test();
		if (ret < 0) {
			LOG_ERR("RGB startup indication failed; monitoring will continue: %d", ret);
			mw_stats_increment(MW_STAT_GPIO_ERROR);
			indicator_available = false;
		}
	}

	/* Refresh the snapshot after the blocking RGB test before periodic work starts. */
	if (mw_sensor_fast_sample(&initial_input) < 0) {
		initial_input.input_ok = false;
	}
	mw_store_publish_input(&initial_input);
	initial_control = mw_safety_ctrl_step(&safety_ctx, &initial_input, k_uptime_get());
	mw_store_publish_control(&initial_control);
	if (indicator_available && initial_control.fault_latched &&
	    mw_indicator_apply(MW_STATE_FAULT) < 0) {
		LOG_ERR("RGB fault indication failed; monitoring will continue");
		mw_stats_increment(MW_STAT_GPIO_ERROR);
		indicator_available = false;
	}

	/* Arm formal measurement windows immediately before periodic sampling. */
	if (initial_input.input_ok) {
		mw_diag_stats_init(initial_input.profile, k_uptime_get());
	}

	k_thread_create(&sensor_thread_data, sensor_stack,
			K_THREAD_STACK_SIZEOF(sensor_stack), sensor_thread,
			NULL, NULL, NULL, SENSOR_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&sensor_thread_data, "sensor_fast");

	k_thread_create(&safety_thread_data, safety_stack,
			K_THREAD_STACK_SIZEOF(safety_stack), safety_thread,
			NULL, NULL, NULL, SAFETY_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&safety_thread_data, "safety_ctrl");

	k_thread_create(&behavior_thread_data, behavior_stack,
			K_THREAD_STACK_SIZEOF(behavior_stack), behavior_thread,
			NULL, NULL, NULL, BEHAVIOR_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&behavior_thread_data, "behavior_sm");

	k_thread_create(&indicator_thread_data, indicator_stack,
			K_THREAD_STACK_SIZEOF(indicator_stack), indicator_thread,
			NULL, NULL, NULL, INDICATOR_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&indicator_thread_data, "indicator");

	k_thread_create(&health_thread_data, health_stack,
			K_THREAD_STACK_SIZEOF(health_stack), health_thread,
			NULL, NULL, NULL, HEALTH_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&health_thread_data, "health_log");

	return 0;
}
