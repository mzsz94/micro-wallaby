#ifndef NU87_IMU_DEVICE_H
#define NU87_IMU_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The transport owns the I2C address. Return 0 on success or negative errno. */
struct imu_io {
	int (*read)(void *ctx, uint8_t reg, uint8_t *data, size_t len);
	int (*write)(void *ctx, uint8_t reg, uint8_t value);
	void (*sleep_ms)(void *ctx, unsigned ms);
	void *ctx;
};

struct imu_device {
	struct imu_io io;
	uint8_t who_am_i;
	/* false means that who_am_i must NOT be reported as a sensor response. */
	bool id_valid;
	bool ready;
	const char *stage;
};

struct imu_sample {
	double accel_g[3];
	double gyro_dps[3];
};

/* Preserves io and resets all diagnostic state, including stale identity. */
int imu_device_init(struct imu_device *dev);

/* Requires new gyro AND accelerometer data; leaves sample unchanged on error.
 * -EAGAIN is normal while waiting for a fresh sample. Axes are sensor-native.
 */
int imu_device_read(struct imu_device *dev, struct imu_sample *sample);

/* Diagnostic name for the supported register profile selected by WHO_AM_I. */
const char *imu_device_name(const struct imu_device *dev);

#endif
