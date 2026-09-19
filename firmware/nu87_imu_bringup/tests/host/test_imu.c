/* SPDX-License-Identifier: Apache-2.0 */
#include "imu_core.h"
#include "imu_calibration.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static uint8_t regs[256];
static unsigned int reads, writes, sleeps;
static unsigned int fail_read, fail_write;
static bool stuck_reset, corrupt_readback;

static int fake_read(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
	(void)ctx;
	if (++reads == fail_read) {
		return -EIO;
	}
	if (reg == 0x12 && !stuck_reset) {
		regs[reg] &= ~1U;
	}
	memcpy(data, regs + reg, len);
	if (corrupt_readback && reg == 0x10) {
		data[0] = 0;
	}
	return 0;
}

static int fake_write(void *ctx, uint8_t reg, uint8_t value)
{
	(void)ctx;
	if (++writes == fail_write) {
		return -EIO;
	}
	regs[reg] = value;
	return 0;
}

static void fake_sleep(unsigned int ms)
{
	sleeps += ms;
}

static const struct imu_bus bus = {
	.read = fake_read, .write = fake_write, .sleep_ms = fake_sleep,
};

static void reset(uint8_t who)
{
	memset(regs, 0, sizeof(regs));
	regs[0x0f] = who;
	reads = writes = sleeps = fail_read = fail_write = 0;
	stuck_reset = corrupt_readback = false;
}

static void test_init(void)
{
	uint8_t observed = 0;

	for (int who = 0x69; who <= 0x6a; ++who) {
		reset(who);
		assert(imu_core_init(&bus, who, &observed) == 0);
		assert(observed == who && writes == 4);
		assert(regs[0x10] == 0x40 && regs[0x11] == 0x40 && regs[0x12] == 0x44);
		assert(sleeps >= 150);
	}
	reset(0x6a);
	assert(imu_core_init(&bus, 0x69, &observed) == -ENODEV);
	assert(writes == 0 && observed == 0x6a);
	assert(imu_core_init(&bus, 0x42, &observed) == -EINVAL);
	reset(0x69);
	stuck_reset = true;
	assert(imu_core_init(&bus, 0x69, &observed) == -ETIMEDOUT);
	assert(sleeps == 50 && writes == 1);
	reset(0x69);
	corrupt_readback = true;
	assert(imu_core_init(&bus, 0x69, &observed) == -EIO);
	for (unsigned int i = 1; i <= 5; ++i) {
		reset(0x69);
		fail_read = i;
		assert(imu_core_init(&bus, 0x69, &observed) == -EIO);
	}
	for (unsigned int i = 1; i <= 4; ++i) {
		reset(0x69);
		fail_write = i;
		assert(imu_core_init(&bus, 0x69, &observed) == -EIO);
	}
}

static void test_fetch_and_units(void)
{
	struct imu_raw sample = {.accel = {99, 99, 99}, .gyro = {99, 99, 99}};
	const uint8_t vector[] = {0xff, 0xff, 0x00, 0x80, 0xff, 0x7f,
				 0x01, 0x00, 0x02, 0x00, 0x03, 0x00};

	reset(0x69);
	for (int status = 0; status < 3; ++status) {
		regs[0x1e] = status;
		assert(imu_core_fetch(&bus, &sample) == -EAGAIN);
		assert(sample.accel[0] == 99);
	}
	regs[0x1e] = 3;
	memcpy(regs + 0x22, vector, sizeof(vector));
	assert(imu_core_fetch(&bus, &sample) == 0);
	assert(sample.gyro[0] == -1 && sample.gyro[1] == -32768 && sample.gyro[2] == 32767);
	assert(sample.accel[0] == 1 && sample.accel[1] == 2 && sample.accel[2] == 3);
	for (unsigned int i = 1; i <= 2; ++i) {
		reads = 0;
		fail_read = i;
		assert(imu_core_fetch(&bus, &sample) == -EIO);
		assert(sample.gyro[0] == -1 && sample.accel[2] == 3);
	}
	assert(imu_accel_micro_ms2(0) == 0 && imu_gyro_micro_rads(0) == 0);
	assert(fabs(imu_accel_micro_ms2(16384) / 1e6 - 9.800) < 0.002);
	assert(fabs(imu_gyro_micro_rads(1000) / 1e6 - 8.75 * 3.141592653589793 / 180) < 1e-6);
	assert(imu_accel_micro_ms2(-100) == -imu_accel_micro_ms2(100));
	assert(imu_gyro_micro_rads(-32768) < 0);
}

static void test_calibration(void)
{
	struct imu_calibration cal;
	struct imu_measurement sample = {.accel_g = {0, 0, 1}, .gyro_dps = {0.1, -0.2, 0.3}};

	imu_cal_begin(&cal);
	for (unsigned int i = 0; i < IMU_CAL_SAMPLES; ++i) {
		assert(imu_cal_add(&cal, &sample) == (i == IMU_CAL_SAMPLES - 1 ? 1 : 0));
	}
	assert(fabs(cal.mean[3] - 0.1) < 1e-9 && cal.mean[2] == 1);
	assert(imu_cal_stddev(&cal, 3) == 0);
	assert(imu_cal_add(&cal, &sample) == -EINVAL);
	imu_cal_begin(&cal);
	sample.accel_g[2] = 0;
	assert(imu_cal_add(&cal, &sample) == -ERANGE); /* free fall */
	imu_cal_begin(&cal);
	sample.accel_g[2] = 1;
	sample.gyro_dps[0] = 11;
	assert(imu_cal_add(&cal, &sample) == -ERANGE);
	imu_cal_begin(&cal);
	sample.gyro_dps[0] = NAN;
	assert(imu_cal_add(&cal, &sample) == -ERANGE);
	imu_cal_begin(&cal);
	sample.gyro_dps[0] = 0;
	for (unsigned int i = 0; i < IMU_CAL_SAMPLES; ++i) {
		sample.gyro_dps[0] = i % 2 ? 1 : -1;
		assert(imu_cal_add(&cal, &sample) == (i == IMU_CAL_SAMPLES - 1 ? -ERANGE : 0));
	}
	assert(imu_cal_stddev(&cal, 3) > 1.0);
	imu_cal_begin(&cal);
	sample.gyro_dps[0] = 6;
	for (unsigned int i = 0; i < IMU_CAL_SAMPLES; ++i) {
		assert(imu_cal_add(&cal, &sample) == (i == IMU_CAL_SAMPLES - 1 ? -ERANGE : 0));
	}
	const double bad[] = {0, INFINITY, 0};
	assert(!imu_bias_valid(bad));
}

int main(void)
{
	test_init();
	test_fetch_and_units();
	test_calibration();
	puts("PASS: chip IDs, reset timeout, I/O faults, readback, fresh data, signed axes, SI units, calibration");
	return 0;
}
