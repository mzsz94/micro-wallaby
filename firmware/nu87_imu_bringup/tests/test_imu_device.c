#include "imu_device.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

struct fake_bus {
	uint8_t regs[256];
	unsigned writes;
	unsigned burst_reads;
	unsigned reset_reads;
	unsigned reset_busy_polls;
	unsigned elapsed_ms;
	unsigned sleep_calls;
	int fail_read_reg;
	int fail_write_reg;
	int corrupt_readback_reg;
	bool resetting;
	bool reset_stuck;
};

static int fake_read(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
	struct fake_bus *bus = ctx;

	assert((size_t)reg + len <= sizeof(bus->regs));
	if (reg == 0x22) {
		assert(len == 12);
		++bus->burst_reads;
	}
	if (bus->fail_read_reg == reg) {
		/* Partial/error results must not become identity or sample data. */
		memset(data, 0xa5, len);
		return -EIO;
	}
	if (reg == 0x12 && bus->resetting) {
		++bus->reset_reads;
		if (!bus->reset_stuck && bus->reset_reads > bus->reset_busy_polls) {
			bus->regs[0x12] = 0x04;
			bus->resetting = false;
		}
	}
	memcpy(data, &bus->regs[reg], len);
	if (bus->corrupt_readback_reg == reg && !bus->resetting) {
		data[0] ^= 0x04;
	}
	return 0;
}

static int fake_write(void *ctx, uint8_t reg, uint8_t value)
{
	struct fake_bus *bus = ctx;

	++bus->writes;
	if (bus->fail_write_reg == reg) {
		return -EIO;
	}
	bus->regs[reg] = value;
	if (reg == 0x12 && (value & 1U) != 0) {
		bus->resetting = true;
	}
	return 0;
}

static void fake_sleep(void *ctx, unsigned ms)
{
	struct fake_bus *bus = ctx;

	bus->elapsed_ms += ms;
	++bus->sleep_calls;
}

static struct imu_device fixture(struct fake_bus *bus, uint8_t who)
{
	memset(bus, 0, sizeof(*bus));
	bus->regs[0x0f] = who;
	bus->fail_read_reg = -1;
	bus->fail_write_reg = -1;
	bus->corrupt_readback_reg = -1;
	return (struct imu_device){
		.io = {
			.read = fake_read,
			.write = fake_write,
			.sleep_ms = fake_sleep,
			.ctx = bus,
		},
	};
}

static void test_identity_failure_is_not_a_sensor_response(void)
{
	struct fake_bus bus;
	struct imu_device dev = fixture(&bus, 0x69);

	assert(imu_device_init(&dev) == 0);
	assert(dev.ready && dev.id_valid);
	bus.fail_read_reg = 0x0f;
	bus.writes = 0;
	assert(imu_device_init(&dev) == -EIO);
	assert(!dev.id_valid && !dev.ready);
	assert(bus.writes == 0);
	assert(strcmp(dev.stage, "read-WHO_AM_I") == 0);
	assert(strcmp(imu_device_name(&dev), "unidentified") == 0);
	assert(dev.io.ctx == &bus && dev.io.read == fake_read);
}

static void test_unknown_id_is_never_written(void)
{
	const uint8_t unknown[] = {0x00, 0xff, 0x68, 0x6c};

	for (size_t i = 0; i < sizeof(unknown); ++i) {
		struct fake_bus bus;
		struct imu_device dev = fixture(&bus, unknown[i]);

		assert(imu_device_init(&dev) == -ENODEV);
		assert(dev.id_valid && dev.who_am_i == unknown[i]);
		assert(!dev.ready && bus.writes == 0);
		assert(strcmp(dev.stage, "unsupported-WHO_AM_I") == 0);
		assert(strcmp(imu_device_name(&dev), "unsupported") == 0);
	}
}

static void test_both_supported_register_profiles(void)
{
	const uint8_t identities[] = {0x69, 0x6a};
	const char *names[] = {"LSM6DS3", "LSM6DS3TR-C"};

	for (size_t i = 0; i < sizeof(identities); ++i) {
		struct fake_bus bus;
		struct imu_device dev = fixture(&bus, identities[i]);

		bus.reset_busy_polls = 3;
		assert(imu_device_init(&dev) == 0);
		assert(dev.ready && dev.id_valid && dev.who_am_i == identities[i]);
		assert(strcmp(imu_device_name(&dev), names[i]) == 0);
		assert(bus.regs[0x10] == 0x40 && bus.regs[0x11] == 0x40);
		assert(bus.regs[0x12] == 0x44);
		assert(bus.reset_reads == 4 && bus.writes == 4);
		assert(bus.elapsed_ms == 208);
	}
}

static void test_reset_is_bounded(void)
{
	struct fake_bus bus;
	struct imu_device dev = fixture(&bus, 0x69);

	bus.reset_stuck = true;
	assert(imu_device_init(&dev) == -ETIMEDOUT);
	assert(dev.id_valid && !dev.ready);
	assert(bus.reset_reads == 50 && bus.writes == 1);
	assert(bus.elapsed_ms == 200);
	assert(strcmp(dev.stage, "reset-timeout") == 0);
}

static void test_configuration_must_read_back(void)
{
	const uint8_t regs[] = {0x12, 0x10, 0x11};
	const char *stages[] = {"verify-CTRL3_C", "verify-CTRL1_XL", "verify-CTRL2_G"};

	for (size_t i = 0; i < sizeof(regs); ++i) {
		struct fake_bus bus;
		struct imu_device dev = fixture(&bus, 0x69);

		bus.corrupt_readback_reg = regs[i];
		assert(imu_device_init(&dev) == -EIO);
		assert(dev.id_valid && !dev.ready);
		assert(strcmp(dev.stage, stages[i]) == 0);
	}
}

static void put_axis(struct fake_bus *bus, unsigned offset, int32_t value)
{
	uint16_t word = (uint16_t)value;

	bus->regs[0x22 + offset] = (uint8_t)word;
	bus->regs[0x23 + offset] = (uint8_t)(word >> 8);
}

static void near(double actual, double expected)
{
	assert(fabs(actual - expected) < 1e-12);
}

static void test_signed_axes_and_units(void)
{
	struct fake_bus bus;
	struct imu_device dev = fixture(&bus, 0x6a);
	struct imu_sample sample = {0};

	assert(imu_device_init(&dev) == 0);
	bus.regs[0x1e] = 0x07;
	put_axis(&bus, 0, -32768);
	put_axis(&bus, 2, 32767);
	put_axis(&bus, 4, -1);
	put_axis(&bus, 6, 16384);
	put_axis(&bus, 8, -16384);
	put_axis(&bus, 10, 0);
	assert(imu_device_read(&dev, &sample) == 0);
	assert(bus.burst_reads == 1);
	near(sample.gyro_dps[0], -286.72);
	near(sample.gyro_dps[1], 286.71125);
	near(sample.gyro_dps[2], -0.00875);
	near(sample.accel_g[0], 0.999424);
	near(sample.accel_g[1], -0.999424);
	near(sample.accel_g[2], 0.0);
}

static void test_incomplete_and_failed_reads_preserve_sample(void)
{
	struct fake_bus bus;
	struct imu_device dev = fixture(&bus, 0x69);
	struct imu_sample sample = {{1, 2, 3}, {4, 5, 6}};
	const struct imu_sample expected = sample;
	const uint8_t stale[] = {0, 1, 2, 4, 5, 6};

	assert(imu_device_read(&dev, &sample) == -ENODEV);
	assert(imu_device_init(&dev) == 0);
	for (size_t i = 0; i < sizeof(stale); ++i) {
		bus.regs[0x1e] = stale[i];
		assert(imu_device_read(&dev, &sample) == -EAGAIN);
		assert(memcmp(&sample, &expected, sizeof(sample)) == 0);
	}
	assert(bus.burst_reads == 0);
	bus.fail_read_reg = 0x1e;
	assert(imu_device_read(&dev, &sample) == -EIO);
	assert(strcmp(dev.stage, "read-STATUS_REG") == 0);
	assert(memcmp(&sample, &expected, sizeof(sample)) == 0);
	bus.regs[0x1e] = 3;
	bus.fail_read_reg = 0x22;
	assert(imu_device_read(&dev, &sample) == -EIO);
	assert(strcmp(dev.stage, "read-gyro-accel-burst") == 0);
	assert(memcmp(&sample, &expected, sizeof(sample)) == 0);
}

static void test_init_transport_failures(void)
{
	struct fake_bus bus;
	struct imu_device dev = fixture(&bus, 0x69);

	bus.fail_write_reg = 0x12;
	assert(imu_device_init(&dev) == -EIO);
	assert(strcmp(dev.stage, "write-reset") == 0 && !dev.ready);
	dev = fixture(&bus, 0x69);
	bus.fail_read_reg = 0x12;
	assert(imu_device_init(&dev) == -EIO);
	assert(strcmp(dev.stage, "read-reset-status") == 0 && !dev.ready);
	dev = fixture(&bus, 0x69);
	bus.fail_write_reg = 0x10;
	assert(imu_device_init(&dev) == -EIO);
	assert(strcmp(dev.stage, "write-CTRL1_XL") == 0 && !dev.ready);
	dev = fixture(&bus, 0x69);
	bus.fail_read_reg = 0x11;
	assert(imu_device_init(&dev) == -EIO);
	assert(strcmp(dev.stage, "read-CTRL2_G") == 0 && !dev.ready);
}

static void test_arguments(void)
{
	struct fake_bus bus;
	struct imu_device dev = fixture(&bus, 0x69);
	struct imu_sample sample;

	assert(imu_device_init(NULL) == -EINVAL);
	assert(imu_device_read(NULL, &sample) == -EINVAL);
	assert(imu_device_read(&dev, NULL) == -EINVAL);
	assert(strcmp(imu_device_name(NULL), "unidentified") == 0);
	dev.io.sleep_ms = NULL;
	assert(imu_device_init(&dev) == -EINVAL);
	assert(!dev.ready && !dev.id_valid && bus.writes == 0);
}

int main(void)
{
	test_identity_failure_is_not_a_sensor_response();
	test_unknown_id_is_never_written();
	test_both_supported_register_profiles();
	test_reset_is_bounded();
	test_configuration_must_read_back();
	test_signed_axes_and_units();
	test_incomplete_and_failed_reads_preserve_sample();
	test_init_transport_failures();
	test_arguments();
	puts("PASS: 9 IMU driver tests");
	return 0;
}
