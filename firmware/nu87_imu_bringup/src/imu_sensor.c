/* SPDX-License-Identifier: Apache-2.0 */
#include "imu_core.h"

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(mw_imu_sensor, LOG_LEVEL_INF);

struct imu_config {
	struct i2c_dt_spec i2c;
	uint8_t expected_id;
};

struct imu_data {
	struct imu_bus bus;
	struct imu_raw raw;
	struct k_mutex lock;
	bool valid;
};

static int reg_read(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
	const struct imu_config *cfg = ctx;
	return i2c_burst_read_dt(&cfg->i2c, reg, data, len);
}

static int reg_write(void *ctx, uint8_t reg, uint8_t value)
{
	const struct imu_config *cfg = ctx;
	return i2c_reg_write_byte_dt(&cfg->i2c, reg, value);
}

static void delay_ms(unsigned int ms)
{
	k_msleep(ms);
}

static int imu_init(const struct device *dev)
{
	const struct imu_config *cfg = dev->config;
	struct imu_data *data = dev->data;
	uint8_t observed = 0;
	int rc;

	k_mutex_init(&data->lock);
	if (!i2c_is_ready_dt(&cfg->i2c)) {
		return -ENODEV;
	}
	data->bus = (struct imu_bus){
		.ctx = (void *)cfg, .read = reg_read, .write = reg_write, .sleep_ms = delay_ms,
	};
	rc = imu_core_init(&data->bus, cfg->expected_id, &observed);
	LOG_INF("I2C=0x%02x WHO_AM_I=0x%02x expected=0x%02x init=%d",
		cfg->i2c.addr, observed, cfg->expected_id, rc);
	return rc;
}

static int imu_fetch(const struct device *dev, enum sensor_channel chan)
{
	struct imu_data *data = dev->data;
	int rc;

	if (chan != SENSOR_CHAN_ALL && chan != SENSOR_CHAN_ACCEL_XYZ &&
	    chan != SENSOR_CHAN_GYRO_XYZ) {
		return -ENOTSUP;
	}
	k_mutex_lock(&data->lock, K_FOREVER);
	rc = imu_core_fetch(&data->bus, &data->raw);
	data->valid = rc == 0;
	k_mutex_unlock(&data->lock);
	return rc;
}

static int imu_get(const struct device *dev, enum sensor_channel chan, struct sensor_value *val)
{
	struct imu_data *data = dev->data;
	int rc = 0;

	k_mutex_lock(&data->lock, K_FOREVER);
	if (!data->valid) {
		rc = -ENODATA;
	} else if (chan == SENSOR_CHAN_ACCEL_XYZ || chan == SENSOR_CHAN_GYRO_XYZ) {
		for (int i = 0; i < 3; ++i) {
			int64_t micro = chan == SENSOR_CHAN_ACCEL_XYZ ?
				imu_accel_micro_ms2(data->raw.accel[i]) :
				imu_gyro_micro_rads(data->raw.gyro[i]);
			val[i].val1 = micro / 1000000;
			val[i].val2 = micro % 1000000;
		}
	} else {
		rc = -ENOTSUP;
	}
	k_mutex_unlock(&data->lock);
	return rc;
}

static DEVICE_API(sensor, imu_api) = {
	.sample_fetch = imu_fetch,
	.channel_get = imu_get,
};

#define IMU_DEFINE(node, id)                                                                        \
	static struct imu_data imu_data_##node;                                                    \
	static const struct imu_config imu_config_##node = {                                       \
		.i2c = I2C_DT_SPEC_GET(node), .expected_id = id,                                    \
	};                                                                                         \
	SENSOR_DEVICE_DT_DEFINE(node, imu_init, NULL, &imu_data_##node, &imu_config_##node,            \
				POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY, &imu_api);

#define IMU_DS3(node) IMU_DEFINE(node, 0x69)
#define IMU_DS3TR_C(node) IMU_DEFINE(node, 0x6a)
DT_FOREACH_STATUS_OKAY(st_lsm6ds3, IMU_DS3)
DT_FOREACH_STATUS_OKAY(st_lsm6ds3tr_c, IMU_DS3TR_C)
