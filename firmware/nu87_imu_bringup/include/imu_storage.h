/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MW_IMU_STORAGE_H
#define MW_IMU_STORAGE_H
#include <stdint.h>
int imu_storage_init(void);
int imu_storage_load(uint8_t who, double bias[3]);
int imu_storage_save(uint8_t who, const double bias[3]);
int imu_storage_clear(void);
#endif
