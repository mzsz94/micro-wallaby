#include "imu_device.h"

#include <errno.h>

/* Register layout and sensitivities checked against the ST platform drivers:
 * https://github.com/STMicroelectronics/lsm6ds3-pid
 * https://github.com/STMicroelectronics/lsm6ds3tr-c-pid
 * Only their common basic accel/gyro register subset is used here.
 */
enum {
	REG_WHO_AM_I = 0x0f,
	REG_CTRL1_XL = 0x10,
	REG_CTRL2_G = 0x11,
	REG_CTRL3_C = 0x12,
	REG_STATUS = 0x1e,
	REG_OUTX_L_G = 0x22,
	WHO_LSM6DS3 = 0x69,
	WHO_LSM6DS3TR_C = 0x6a,
	CTRL3_SW_RESET = 0x01,
	CTRL3_BDU_IF_INC = 0x44,
	CTRL1_104HZ_2G = 0x40,
	CTRL2_104HZ_FS0 = 0x40,
	STATUS_ACCEL_GYRO_READY = 0x03,
	RESET_POLL_COUNT = 50,
	RESET_POLL_MS = 2,
};

static int checked_write(struct imu_device *dev, uint8_t reg, uint8_t value,
			 const char *write_stage, const char *read_stage,
			 const char *mismatch_stage)
{
	uint8_t actual;
	int err;

	dev->stage = write_stage;
	err = dev->io.write(dev->io.ctx, reg, value);
	if (err != 0) {
		return err;
	}
	dev->stage = read_stage;
	err = dev->io.read(dev->io.ctx, reg, &actual, 1);
	if (err != 0) {
		return err;
	}
	if (actual != value) {
		dev->stage = mismatch_stage;
		return -EIO;
	}
	return 0;
}

const char *imu_device_name(const struct imu_device *dev)
{
	if (dev == NULL || !dev->id_valid) {
		return "unidentified";
	}
	switch (dev->who_am_i) {
	case WHO_LSM6DS3:
		return "LSM6DS3";
	case WHO_LSM6DS3TR_C:
		return "LSM6DS3TR-C";
	default:
		return "unsupported";
	}
}

int imu_device_init(struct imu_device *dev)
{
	uint8_t who;
	uint8_t ctrl3 = CTRL3_SW_RESET;
	int err;

	if (dev == NULL) {
		return -EINVAL;
	}
	dev->who_am_i = 0;
	dev->id_valid = false;
	dev->ready = false;
	dev->stage = "validate-transport";
	if (dev->io.read == NULL || dev->io.write == NULL ||
	    dev->io.sleep_ms == NULL) {
		return -EINVAL;
	}

	dev->stage = "power-on-settle";
	dev->io.sleep_ms(dev->io.ctx, 100);
	dev->stage = "read-WHO_AM_I";
	err = dev->io.read(dev->io.ctx, REG_WHO_AM_I, &who, 1);
	if (err != 0) {
		return err;
	}
	dev->who_am_i = who;
	dev->id_valid = true;
	dev->stage = "unsupported-WHO_AM_I";
	if (who != WHO_LSM6DS3 && who != WHO_LSM6DS3TR_C) {
		return -ENODEV;
	}

	/* An acknowledged but unknown device must never receive reset/config writes. */
	dev->stage = "write-reset";
	err = dev->io.write(dev->io.ctx, REG_CTRL3_C, CTRL3_SW_RESET);
	if (err != 0) {
		return err;
	}
	dev->stage = "read-reset-status";
	for (unsigned poll = 0; poll < RESET_POLL_COUNT; ++poll) {
		dev->io.sleep_ms(dev->io.ctx, RESET_POLL_MS);
		err = dev->io.read(dev->io.ctx, REG_CTRL3_C, &ctrl3, 1);
		if (err != 0) {
			return err;
		}
		if ((ctrl3 & CTRL3_SW_RESET) == 0) {
			break;
		}
	}
	if ((ctrl3 & CTRL3_SW_RESET) != 0) {
		dev->stage = "reset-timeout";
		return -ETIMEDOUT;
	}

	err = checked_write(dev, REG_CTRL3_C, CTRL3_BDU_IF_INC,
			    "write-CTRL3_C", "read-CTRL3_C", "verify-CTRL3_C");
	if (err != 0) {
		return err;
	}
	err = checked_write(dev, REG_CTRL1_XL, CTRL1_104HZ_2G,
			    "write-CTRL1_XL", "read-CTRL1_XL", "verify-CTRL1_XL");
	if (err != 0) {
		return err;
	}
	err = checked_write(dev, REG_CTRL2_G, CTRL2_104HZ_FS0,
			    "write-CTRL2_G", "read-CTRL2_G", "verify-CTRL2_G");
	if (err != 0) {
		return err;
	}

	dev->stage = "sensor-settle";
	dev->io.sleep_ms(dev->io.ctx, 100);
	dev->ready = true;
	dev->stage = "ready";
	return 0;
}

static int32_t signed_le16(const uint8_t *bytes)
{
	uint32_t word = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8);

	/* Avoid implementation-defined uint16_t -> int16_t conversion. */
	return word >= 0x8000U ? (int32_t)word - 65536 : (int32_t)word;
}

int imu_device_read(struct imu_device *dev, struct imu_sample *sample)
{
	uint8_t status;
	uint8_t raw[12];
	struct imu_sample next;
	int err;

	if (dev == NULL || sample == NULL) {
		return -EINVAL;
	}
	if (!dev->ready) {
		return -ENODEV;
	}
	dev->stage = "read-STATUS_REG";
	err = dev->io.read(dev->io.ctx, REG_STATUS, &status, 1);
	if (err != 0) {
		return err;
	}
	if ((status & STATUS_ACCEL_GYRO_READY) != STATUS_ACCEL_GYRO_READY) {
		dev->stage = "wait-fresh-sample";
		return -EAGAIN;
	}
	dev->stage = "read-gyro-accel-burst";
	err = dev->io.read(dev->io.ctx, REG_OUTX_L_G, raw, sizeof(raw));
	if (err != 0) {
		return err;
	}
	for (unsigned axis = 0; axis < 3; ++axis) {
		next.gyro_dps[axis] = signed_le16(&raw[axis * 2]) * 0.00875;
		next.accel_g[axis] = signed_le16(&raw[6 + axis * 2]) * 0.000061;
	}
	*sample = next;
	dev->stage = "sample-ready";
	return 0;
}
