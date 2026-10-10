/* SPDX-License-Identifier: Apache-2.0 */
#include "gateway_timer_core.h"
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>

static void one_shot_and_stop(void)
{
	struct gateway_timer_core core = {0};
	struct gateway_timer timer = {.interval_ms = 10};
	int64_t wait;
	gateway_timer_add(&core, &timer);
	assert(gateway_timer_arm(&core, &timer, 10, 100));
	assert(gateway_timer_dispatch(&core, 109, &wait) == NULL && wait == 1);
	assert(gateway_timer_stop(&core, &timer));
	assert(gateway_timer_dispatch(&core, 110, &wait) == NULL && wait == -1);
	assert(gateway_timer_arm(&core, &timer, 10, 200));
	assert(gateway_timer_dispatch(&core, 210, &wait) == &timer);
	assert(timer.running && !timer.active);
	assert(!gateway_timer_complete(&core, &timer, 211));
	assert(gateway_timer_dispatch(&core, 1000, &wait) == NULL);
}

static void deletion_and_callback_lifetime(void)
{
	struct gateway_timer_core core = {0};
	struct gateway_timer timer = {.id = 123, .periodic = true};
	int64_t wait;
	gateway_timer_add(&core, &timer);
	assert(gateway_timer_arm(&core, &timer, 10, 0));
	assert(gateway_timer_dispatch(&core, 10, &wait) == &timer);
	/* Self-delete and deletion by another caller both defer reclamation. */
	assert(gateway_timer_retire(&core, &timer) == GATEWAY_TIMER_DELETE_DEFERRED);
	assert(gateway_timer_find(&core, &timer)->id == 123);
	assert(!gateway_timer_arm(&core, &timer, 10, 11));
	assert(!gateway_timer_stop(&core, &timer));
	assert(gateway_timer_dispatch(&core, 500, &wait) == NULL);
	assert(gateway_timer_complete(&core, &timer, 501));
	assert(gateway_timer_find(&core, &timer) == NULL);
	assert(gateway_timer_retire(&core, &timer) == GATEWAY_TIMER_DELETE_INVALID);
	struct gateway_timer queued = {0};
	gateway_timer_add(&core, &queued);
	assert(gateway_timer_arm(&core, &queued, 1, 0));
	assert(gateway_timer_retire(&core, &queued) == GATEWAY_TIMER_DELETE_RECLAIM);
	assert(gateway_timer_dispatch(&core, 1, &wait) == NULL);
}

static void callback_commands_win(void)
{
	struct gateway_timer_core core = {0};
	struct gateway_timer timer = {.periodic = true};
	int64_t wait;
	gateway_timer_add(&core, &timer);
	assert(gateway_timer_arm(&core, &timer, 10, 0));
	assert(gateway_timer_dispatch(&core, 10, &wait) == &timer);
	assert(gateway_timer_stop(&core, &timer));
	assert(!gateway_timer_complete(&core, &timer, 50));
	assert(gateway_timer_dispatch(&core, 100, &wait) == NULL);
	assert(gateway_timer_arm(&core, &timer, 10, 100));
	assert(gateway_timer_dispatch(&core, 110, &wait) == &timer);
	assert(gateway_timer_arm(&core, &timer, 80, 120));
	assert(!gateway_timer_complete(&core, &timer, 150));
	assert(gateway_timer_dispatch(&core, 151, &wait) == NULL && wait == 49);
	assert(gateway_timer_dispatch(&core, 200, &wait) == &timer);
	/* Slow callback coalesces missed periodic ticks. */
	assert(!gateway_timer_complete(&core, &timer, 1000));
	assert(gateway_timer_dispatch(&core, 1000, &wait) == NULL && wait == 80);
	/* One-shot callback can re-arm itself. */
	timer.periodic = false;
	assert(gateway_timer_dispatch(&core, 1080, &wait) == &timer);
	assert(gateway_timer_arm(&core, &timer, 5, 1081));
	assert(!gateway_timer_complete(&core, &timer, 1082));
	assert(gateway_timer_dispatch(&core, 1086, &wait) == &timer);
	assert(!gateway_timer_complete(&core, &timer, 1087));
	/* An explicit periodic re-arm that expires during a slow callback must
	 * dispatch at its requested deadline, not be shifted by completion.
	 */
	timer.periodic = true;
	assert(gateway_timer_arm(&core, &timer, 10, 1100));
	assert(gateway_timer_dispatch(&core, 1110, &wait) == &timer);
	assert(gateway_timer_arm(&core, &timer, 5, 1111));
	assert(!gateway_timer_complete(&core, &timer, 1120));
	assert(gateway_timer_dispatch(&core, 1120, &wait) == &timer);
}

static void deadline_order_and_large_period(void)
{
	struct gateway_timer_core core = {0};
	struct gateway_timer late = {0}, early = {0};
	int64_t wait;
	gateway_timer_add(&core, &late);
	gateway_timer_add(&core, &early);
	assert(gateway_timer_arm(&core, &late, UINT32_MAX, INT64_C(10000000000)));
	assert(gateway_timer_arm(&core, &early, 10, INT64_C(10000000000)));
	assert(gateway_timer_dispatch(&core, INT64_C(10000000010), &wait) == &early);
	assert(!gateway_timer_complete(&core, &early, INT64_C(10000000011)));
	assert(gateway_timer_dispatch(&core, INT64_C(10000000011), &wait) == NULL);
	assert(wait == INT64_C(4294967284));
	assert(gateway_timer_retire(&core, &late) == GATEWAY_TIMER_DELETE_RECLAIM);
	assert(gateway_timer_dispatch(&core, INT64_MAX, &wait) == NULL);
}

static void zero_period_expires_once(void)
{
	struct gateway_timer_core core = {0};
	struct gateway_timer timer = {.periodic = true};
	int64_t wait;
	gateway_timer_add(&core, &timer);
	assert(gateway_timer_arm(&core, &timer, 0, 100));
	assert(gateway_timer_dispatch(&core, 100, &wait) == NULL && wait == 1);
	assert(gateway_timer_dispatch(&core, 101, &wait) == &timer);
	assert(!timer.active);
	assert(!gateway_timer_complete(&core, &timer, 102));
	assert(gateway_timer_dispatch(&core, 1000, &wait) == NULL && wait == -1);
	/* A callback may explicitly schedule another immediate one-shot. */
	assert(gateway_timer_arm(&core, &timer, 0, 1000));
	assert(gateway_timer_dispatch(&core, 1001, &wait) == &timer);
	assert(gateway_timer_arm(&core, &timer, 0, 1002));
	assert(!gateway_timer_complete(&core, &timer, 1004));
	assert(gateway_timer_dispatch(&core, 1004, &wait) == &timer);
}

int main(void)
{
	one_shot_and_stop();
	deletion_and_callback_lifetime();
	callback_commands_win();
	deadline_order_and_large_period();
	zero_period_expires_once();
	puts("gateway timer core lifecycle tests passed");
	return 0;
}
