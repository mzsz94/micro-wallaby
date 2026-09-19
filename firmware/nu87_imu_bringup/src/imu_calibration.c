/* SPDX-License-Identifier: Apache-2.0 */
#include "imu_calibration.h"

#include <errno.h>
#include <math.h>
#include <string.h>

void imu_cal_begin(struct imu_calibration *cal)
{
	memset(cal, 0, sizeof(*cal));
}

bool imu_bias_valid(const double bias[3])
{
	for (int i = 0; i < 3; ++i) {
		if (!isfinite(bias[i]) || fabs(bias[i]) > 5.0) {
			return false;
		}
	}
	return true;
}

double imu_cal_stddev(const struct imu_calibration *cal, unsigned int channel)
{
	if (cal->count < 2 || channel >= 6) {
		return 0;
	}
	return sqrt(fmax(0, cal->m2[channel] / (cal->count - 1)));
}

int imu_cal_add(struct imu_calibration *cal, const struct imu_measurement *sample)
{
	double norm_squared = 0;

	if (cal->rejected || cal->count >= IMU_CAL_SAMPLES) {
		return -EINVAL;
	}
	for (int i = 0; i < 3; ++i) {
		if (!isfinite(sample->accel_g[i]) || !isfinite(sample->gyro_dps[i]) ||
		    fabs(sample->gyro_dps[i]) > 10.0) {
			cal->rejected = true;
			return -ERANGE;
		}
		norm_squared += sample->accel_g[i] * sample->accel_g[i];
	}
	if (norm_squared < 0.9 * 0.9 || norm_squared > 1.1 * 1.1) {
		cal->rejected = true;
		return -ERANGE;
	}
	++cal->count;
	for (int i = 0; i < 6; ++i) {
		double value = i < 3 ? sample->accel_g[i] : sample->gyro_dps[i - 3];
		double delta = value - cal->mean[i];
		cal->mean[i] += delta / cal->count;
		cal->m2[i] += delta * (value - cal->mean[i]);
	}
	if (cal->count < IMU_CAL_SAMPLES) {
		return 0;
	}
	for (int i = 0; i < 3; ++i) {
		if (imu_cal_stddev(cal, i) > 0.015 || imu_cal_stddev(cal, i + 3) > 0.5) {
			cal->rejected = true;
			return -ERANGE;
		}
	}
	if (!imu_bias_valid(cal->mean + 3)) {
		cal->rejected = true;
		return -ERANGE;
	}
	return 1;
}
