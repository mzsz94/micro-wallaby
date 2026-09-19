/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MW_IMU_CALIBRATION_H
#define MW_IMU_CALIBRATION_H

#include <stdbool.h>
#include <stdint.h>

#define IMU_CAL_SAMPLES 1000U

struct imu_measurement {
	double accel_g[3];
	double gyro_dps[3];
};

struct imu_calibration {
	uint32_t count;
	double mean[6];
	double m2[6];
	bool rejected;
};

void imu_cal_begin(struct imu_calibration *cal);
/* Returns 0 while collecting, 1 when accepted, or a negative errno on rejection. */
int imu_cal_add(struct imu_calibration *cal, const struct imu_measurement *sample);
double imu_cal_stddev(const struct imu_calibration *cal, unsigned int channel);
bool imu_bias_valid(const double bias[3]);

#endif
