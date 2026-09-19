/* SPDX-License-Identifier: Apache-2.0 */
#include "imu_core.h"

#include <errno.h>

#define WHO_AM_I 0x0f
#define CTRL1_XL 0x10
#define CTRL2_G  0x11
#define CTRL3_C  0x12
#define STATUS  0x1e
#define OUTX_L_G 0x22

int imu_core_init(const struct imu_bus *bus, uint8_t expected_id, uint8_t *observed_id)
{
	uint8_t value;
	int rc;

	if (expected_id != 0x69 && expected_id != 0x6a) {
		return -EINVAL;
	}
	rc = bus->read(bus->ctx, WHO_AM_I, observed_id, 1);
	if (rc != 0) {
		return rc;
	}
	if (*observed_id != expected_id) {
		/* Never reset/write an unidentified device. */
		return -ENODEV;
	}
	rc = bus->write(bus->ctx, CTRL3_C, 0x01); /* SW_RESET, not BOOT */
	if (rc != 0) {
		return rc;
	}
	for (unsigned int i = 0; i < 50; ++i) {
		bus->sleep_ms(1);
		rc = bus->read(bus->ctx, CTRL3_C, &value, 1);
		if (rc != 0) {
			return rc;
		}
		if ((value & 0x01) == 0) {
			break;
		}
		if (i == 49) {
			return -ETIMEDOUT;
		}
	}
	/* Shared documented register subset: BDU, auto-increment, 104 Hz, 2 g, 250 dps. */
	static const uint8_t config[][2] = {
		{CTRL3_C, 0x44}, {CTRL1_XL, 0x40}, {CTRL2_G, 0x40},
	};
	for (size_t i = 0; i < sizeof(config) / sizeof(config[0]); ++i) {
		rc = bus->write(bus->ctx, config[i][0], config[i][1]);
		if (rc == 0) {
			rc = bus->read(bus->ctx, config[i][0], &value, 1);
		}
		if (rc != 0) {
			return rc;
		}
		if (value != config[i][1]) {
			return -EIO;
		}
	}
	bus->sleep_ms(150); /* Discard power-up transients before calibration. */
	return 0;
}

static int16_t signed_le16(const uint8_t *data)
{
	uint16_t bits = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
	return (int16_t)(bits < 0x8000 ? (int32_t)bits : (int32_t)bits - 65536);
}

int imu_core_fetch(const struct imu_bus *bus, struct imu_raw *sample)
{
	uint8_t status;
	uint8_t buffer[12];
	struct imu_raw next;
	int rc = bus->read(bus->ctx, STATUS, &status, 1);

	if (rc != 0) {
		return rc;
	}
	if ((status & 0x03) != 0x03) {
		return -EAGAIN; /* Do not count a repeated/stale sample. */
	}
	rc = bus->read(bus->ctx, OUTX_L_G, buffer, sizeof(buffer));
	if (rc != 0) {
		return rc;
	}
	for (int axis = 0; axis < 3; ++axis) {
		next.gyro[axis] = signed_le16(buffer + axis * 2);
		next.accel[axis] = signed_le16(buffer + 6 + axis * 2);
	}
	*sample = next;
	return 0;
}

int64_t imu_accel_micro_ms2(int16_t raw)
{
	/* 0.061 mg/LSB at +/-2 g, standard gravity 9.80665 m/s^2. */
	return (int64_t)raw * 61 * 980665 / 100000;
}

int64_t imu_gyro_micro_rads(int16_t raw)
{
	/* 8.75 mdps/LSB at +/-250 dps; pi rounded to 9 decimal places. */
	return (int64_t)raw * 875 * 3141592654LL / 18000000000LL;
}
