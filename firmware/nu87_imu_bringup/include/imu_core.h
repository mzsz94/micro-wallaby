/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MW_IMU_CORE_H
#define MW_IMU_CORE_H

#include <stddef.h>
#include <stdint.h>

struct imu_bus {
	void *ctx;
	int (*read)(void *ctx, uint8_t reg, uint8_t *data, size_t len);
	int (*write)(void *ctx, uint8_t reg, uint8_t value);
	void (*sleep_ms)(unsigned int ms);
};

struct imu_raw {
	int16_t accel[3];
	int16_t gyro[3];
};

int imu_core_init(const struct imu_bus *bus, uint8_t expected_id, uint8_t *observed_id);
int imu_core_fetch(const struct imu_bus *bus, struct imu_raw *sample);
int64_t imu_accel_micro_ms2(int16_t raw);
int64_t imu_gyro_micro_rads(int16_t raw);

#endif
