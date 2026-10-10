/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GATEWAY_IMU_H
#define GATEWAY_IMU_H
#include <stddef.h>
#include <zephyr/sys/printk.h>
#ifdef CONFIG_GATEWAY_IMU
void gateway_imu_start(void);
int gateway_imu_status(char *buffer, size_t capacity);
#else
static inline void gateway_imu_start(void) {}
static inline int gateway_imu_status(char *buffer, size_t capacity)
{
	return snprintk(buffer, capacity, "{\"configured\":false,\"ready\":false,"
		       "\"fresh\":false,\"source\":\"nu87-i2c\"}");
}
#endif
#endif
