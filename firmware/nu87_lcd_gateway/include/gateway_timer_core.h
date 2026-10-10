/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GATEWAY_TIMER_CORE_H
#define GATEWAY_TIMER_CORE_H

#include <stdbool.h>
#include <stdint.h>

/* All operations require the caller's lock. Only the daemon dispatches and
 * completes callbacks. Opaque handles remain valid until delete is accepted;
 * reusing a deleted handle after allocation reuses its address is caller misuse.
 */
struct gateway_timer {
	struct gateway_timer *next;
	void (*callback)(void *);
	uint32_t id;
	uint32_t interval_ms;
	uint32_t generation;
	uint32_t dispatched_generation;
	int64_t deadline_ms;
	bool periodic;
	bool active;
	bool running;
	bool retired;
};

struct gateway_timer_core {
	struct gateway_timer *head;
};

enum gateway_timer_delete_result {
	GATEWAY_TIMER_DELETE_INVALID = -1,
	GATEWAY_TIMER_DELETE_DEFERRED = 0,
	GATEWAY_TIMER_DELETE_RECLAIM = 1,
};

void gateway_timer_add(struct gateway_timer_core *core, struct gateway_timer *timer);
struct gateway_timer *gateway_timer_find(struct gateway_timer_core *core, const void *handle);
bool gateway_timer_arm(struct gateway_timer_core *core, const void *handle,
		       uint32_t interval_ms, int64_t now_ms);
bool gateway_timer_stop(struct gateway_timer_core *core, const void *handle);
enum gateway_timer_delete_result gateway_timer_retire(struct gateway_timer_core *core,
						      const void *handle);
struct gateway_timer *gateway_timer_dispatch(struct gateway_timer_core *core,
					     int64_t now_ms, int64_t *wait_ms);
bool gateway_timer_complete(struct gateway_timer_core *core, struct gateway_timer *timer,
			    int64_t now_ms);

#endif
