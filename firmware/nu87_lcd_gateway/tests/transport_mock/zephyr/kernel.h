/* SPDX-License-Identifier: Apache-2.0 */
#ifndef TRANSPORT_MOCK_KERNEL_H
#define TRANSPORT_MOCK_KERNEL_H
#include <assert.h>
#include <pthread.h>
/* Threaded host model tests serialization and snapshot consistency, not IRQ
 * latency or the physical UART. Assert against recursive diagnostic locking. */
struct k_spinlock { int unused; };
typedef int k_spinlock_key_t;
static pthread_mutex_t transport_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int transport_lock_held;
static inline k_spinlock_key_t k_spin_lock(struct k_spinlock *lock)
{
	(void)lock;
	assert(transport_lock_held == 0);
	assert(pthread_mutex_lock(&transport_mutex) == 0);
	transport_lock_held = 1;
	return 0;
}
static inline void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key)
{
	(void)lock; (void)key;
	assert(transport_lock_held == 1);
	transport_lock_held = 0;
	assert(pthread_mutex_unlock(&transport_mutex) == 0);
}
#endif
