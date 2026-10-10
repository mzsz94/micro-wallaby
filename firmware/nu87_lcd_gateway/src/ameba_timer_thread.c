/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The pinned Ameba Zephyr wrapper invokes vendor software timer callbacks from
 * k_timer expiry (an ISR). Wi-Fi authentication callbacks take mutexes, which
 * must run in thread context. Wrap only the vendor timer API with a dedicated
 * preemptible daemon, matching its asynchronous command-completion semantics.
 */
#include "os_wrapper.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include "gateway_timer_core.h"

LOG_MODULE_REGISTER(gateway_timer, LOG_LEVEL_INF);

static struct gateway_timer_core timer_core;
static struct k_spinlock timer_lock;
K_SEM_DEFINE(timer_wake, 0, 1);

static void timer_daemon(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		int64_t wait_ms;
		k_spinlock_key_t key = k_spin_lock(&timer_lock);
		struct gateway_timer *timer =
			gateway_timer_dispatch(&timer_core, k_uptime_get(), &wait_ms);
		k_spin_unlock(&timer_lock, key);
		if (timer == NULL) {
			/* A command arriving between unlock and take leaves a semaphore
			 * token, so an earlier deadline cannot be missed.
			 */
			(void)k_sem_take(&timer_wake, wait_ms < 0 ? K_FOREVER : K_MSEC(wait_ms));
			continue;
		}
		/* running retains the object through stop/delete/self-delete. No
		 * lock is held across vendor code, and no shared workqueue is used.
		 */
		timer->callback(timer);
		key = k_spin_lock(&timer_lock);
		bool reclaim = gateway_timer_complete(&timer_core, timer, k_uptime_get());
		k_spin_unlock(&timer_lock, key);
		if (reclaim) {
			k_free(timer);
		}
	}
}

K_THREAD_DEFINE(gateway_timer_daemon, CONFIG_GATEWAY_TIMER_STACK_SIZE, timer_daemon,
		NULL, NULL, NULL, K_PRIO_PREEMPT(4), 0, 0);

int __wrap_rtos_timer_create(rtos_timer_t *handle, const char *name, uint32_t id,
			     uint32_t interval_ms, uint8_t reload, void (*callback)(void *))
{
	ARG_UNUSED(name);
	if (handle == NULL || callback == NULL) {
		return RTK_FAIL;
	}
	struct gateway_timer *timer = k_malloc(sizeof(*timer));
	if (timer == NULL) {
		LOG_ERR("Cannot allocate a vendor software timer");
		return RTK_FAIL;
	}
	memset(timer, 0, sizeof(*timer));
	timer->id = id;
	timer->interval_ms = interval_ms;
	timer->periodic = reload != 0;
	timer->callback = callback;
	k_spinlock_key_t key = k_spin_lock(&timer_lock);
	gateway_timer_add(&timer_core, timer);
	k_spin_unlock(&timer_lock, key);
	*handle = timer;
	return RTK_SUCCESS;
}

int __wrap_rtos_timer_create_static(rtos_timer_t *handle, const char *name, uint32_t id,
				    uint32_t interval_ms, uint8_t reload, void (*callback)(void *))
{
	return __wrap_rtos_timer_create(handle, name, id, interval_ms, reload, callback);
}

int __wrap_rtos_timer_delete(rtos_timer_t handle, uint32_t wait_ms)
{
	/* wait_ms is queue-enqueue wait, not permission to wait for a callback
	 * holding/awaiting the caller's Wi-Fi mutex. Commands apply immediately.
	 */
	ARG_UNUSED(wait_ms);
	k_spinlock_key_t key = k_spin_lock(&timer_lock);
	enum gateway_timer_delete_result result = gateway_timer_retire(&timer_core, handle);
	k_spin_unlock(&timer_lock, key);
	if (result == GATEWAY_TIMER_DELETE_RECLAIM) {
		k_free(handle);
	}
	k_sem_give(&timer_wake);
	return result == GATEWAY_TIMER_DELETE_INVALID ? RTK_FAIL : RTK_SUCCESS;
}

int __wrap_rtos_timer_delete_static(rtos_timer_t handle, uint32_t wait_ms)
{
	return __wrap_rtos_timer_delete(handle, wait_ms);
}

int __wrap_rtos_timer_start(rtos_timer_t handle, uint32_t wait_ms)
{
	ARG_UNUSED(wait_ms);
	k_spinlock_key_t key = k_spin_lock(&timer_lock);
	struct gateway_timer *timer = gateway_timer_find(&timer_core, handle);
	/* Zero uses the pinned wrapper's next-tick one-shot behavior. */
	bool ok = timer != NULL &&
		gateway_timer_arm(&timer_core, handle, timer->interval_ms, k_uptime_get());
	k_spin_unlock(&timer_lock, key);
	k_sem_give(&timer_wake);
	return ok ? RTK_SUCCESS : RTK_FAIL;
}

int __wrap_rtos_timer_stop(rtos_timer_t handle, uint32_t wait_ms)
{
	ARG_UNUSED(wait_ms);
	k_spinlock_key_t key = k_spin_lock(&timer_lock);
	bool ok = gateway_timer_stop(&timer_core, handle);
	k_spin_unlock(&timer_lock, key);
	k_sem_give(&timer_wake);
	return ok ? RTK_SUCCESS : RTK_FAIL;
}

int __wrap_rtos_timer_change_period(rtos_timer_t handle, uint32_t interval_ms, uint32_t wait_ms)
{
	ARG_UNUSED(wait_ms);
	k_spinlock_key_t key = k_spin_lock(&timer_lock);
	bool ok = gateway_timer_arm(&timer_core, handle, interval_ms, k_uptime_get());
	k_spin_unlock(&timer_lock, key);
	k_sem_give(&timer_wake);
	return ok ? RTK_SUCCESS : RTK_FAIL;
}

uint32_t __wrap_rtos_timer_is_timer_active(rtos_timer_t handle)
{
	k_spinlock_key_t key = k_spin_lock(&timer_lock);
	struct gateway_timer *timer = gateway_timer_find(&timer_core, handle);
	bool active = timer != NULL && timer->active && !timer->retired;
	k_spin_unlock(&timer_lock, key);
	return active ? TRUE : FALSE;
}

uint32_t __wrap_rtos_timer_get_id(rtos_timer_t handle)
{
	k_spinlock_key_t key = k_spin_lock(&timer_lock);
	struct gateway_timer *timer = gateway_timer_find(&timer_core, handle);
	uint32_t id = timer != NULL ? timer->id : 0;
	k_spin_unlock(&timer_lock, key);
	return id;
}
