/* SPDX-License-Identifier: Apache-2.0 */
#include "gateway_timer_core.h"
#include <stddef.h>

void gateway_timer_add(struct gateway_timer_core *core, struct gateway_timer *timer)
{
	timer->next = core->head;
	core->head = timer;
}

struct gateway_timer *gateway_timer_find(struct gateway_timer_core *core, const void *handle)
{
	for (struct gateway_timer *timer = core->head; timer != NULL; timer = timer->next) {
		if (timer == handle) {
			return timer;
		}
	}
	return NULL;
}

bool gateway_timer_arm(struct gateway_timer_core *core, const void *handle,
		       uint32_t interval_ms, int64_t now_ms)
{
	struct gateway_timer *timer = gateway_timer_find(core, handle);
	if (timer == NULL || timer->retired) {
		return false;
	}
	timer->interval_ms = interval_ms;
	timer->generation++;
	/* Match the pinned k_timer wrapper: duration zero expires on the next
	 * tick and period zero means one-shot, even if reload was requested.
	 */
	timer->deadline_ms = now_ms + (interval_ms != 0 ? (int64_t)interval_ms : 1);
	timer->active = true;
	return true;
}

bool gateway_timer_stop(struct gateway_timer_core *core, const void *handle)
{
	struct gateway_timer *timer = gateway_timer_find(core, handle);
	if (timer == NULL || timer->retired) {
		return false;
	}
	timer->active = false;
	timer->generation++;
	return true;
}

static void unlink_timer(struct gateway_timer_core *core, struct gateway_timer *timer)
{
	struct gateway_timer **link = &core->head;
	while (*link != NULL && *link != timer) {
		link = &(*link)->next;
	}
	if (*link == timer) {
		*link = timer->next;
		timer->next = NULL;
	}
}

enum gateway_timer_delete_result gateway_timer_retire(struct gateway_timer_core *core,
						      const void *handle)
{
	struct gateway_timer *timer = gateway_timer_find(core, handle);
	if (timer == NULL || timer->retired) {
		return GATEWAY_TIMER_DELETE_INVALID;
	}
	timer->retired = true;
	timer->generation++;
	timer->active = false;
	if (timer->running) {
		return GATEWAY_TIMER_DELETE_DEFERRED;
	}
	unlink_timer(core, timer);
	return GATEWAY_TIMER_DELETE_RECLAIM;
}

struct gateway_timer *gateway_timer_dispatch(struct gateway_timer_core *core,
					     int64_t now_ms, int64_t *wait_ms)
{
	struct gateway_timer *next = NULL;
	for (struct gateway_timer *timer = core->head; timer != NULL; timer = timer->next) {
		if (!timer->active || timer->retired || timer->running) {
			continue;
		}
		if (next == NULL || timer->deadline_ms < next->deadline_ms) {
			next = timer;
		}
	}
	if (next == NULL) {
		*wait_ms = -1;
		return NULL;
	}
	if (next->deadline_ms > now_ms) {
		*wait_ms = next->deadline_ms - now_ms;
		return NULL;
	}
	next->running = true;
	next->dispatched_generation = next->generation;
	next->active = next->periodic && next->interval_ms != 0;
	/* Arm before releasing the lock. Callback or concurrent stop/change/start
	 * commands can replace this deadline without completion overwriting them.
	 */
	next->deadline_ms = now_ms + (int64_t)next->interval_ms;
	*wait_ms = 0;
	return next;
}

bool gateway_timer_complete(struct gateway_timer_core *core, struct gateway_timer *timer,
			    int64_t now_ms)
{
	timer->running = false;
	if (timer->retired) {
		unlink_timer(core, timer);
		return true;
	}
	/* A slow periodic callback never causes an unlimited catch-up loop. A
	 * future deadline explicitly set by the callback remains unchanged.
	 */
	if (timer->periodic && timer->active && timer->generation == timer->dispatched_generation &&
	    timer->deadline_ms <= now_ms) {
		timer->deadline_ms = now_ms + (int64_t)timer->interval_ms;
	}
	return false;
}
