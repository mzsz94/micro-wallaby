/* SPDX-License-Identifier: Apache-2.0 */
#include "imu_calibration.h"
#include "imu_storage.h"

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(mw_imu, LOG_LEVEL_INF);

#define IMU_NODE DT_ALIAS(imu0)
#define WHO_ID (DT_NODE_HAS_COMPAT(IMU_NODE, st_lsm6ds3) ? 0x69 : 0x6a)
#define RAD_TO_DEG 57.29577951308232

static const struct device *const imu = DEVICE_DT_GET(IMU_NODE);
static K_MUTEX_DEFINE(state_lock);
static struct imu_calibration cal;
static double bias[3];
static bool bias_valid;
static bool calibrating;
static uint32_t fresh, stale, errors, late, storage_pauses;
static atomic_t ready;
static atomic_t request;
static atomic_t streaming = ATOMIC_INIT(1);
enum { REQ_NONE, REQ_CALIBRATE, REQ_SAVE, REQ_CLEAR };

static int command_request(const struct shell *sh, int value)
{
	if (!atomic_get(&ready)) {
		shell_error(sh, "IMU is not ready; check boot diagnostics and wiring");
		return -ENODEV;
	}
	if (!atomic_cas(&request, REQ_NONE, value)) {
		shell_error(sh, "A command is already pending");
		return -EBUSY;
	}
	shell_print(sh, "Request queued; see the result in the log");
	return 0;
}

static int cmd_calibrate(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	return command_request(sh, REQ_CALIBRATE);
}

static int cmd_save(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	return command_request(sh, REQ_SAVE);
}

static int cmd_clear(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	return command_request(sh, REQ_CLEAR);
}

static int cmd_stream(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	if (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0) {
		shell_error(sh, "Usage: imu stream on|off");
		return -EINVAL;
	}
	atomic_set(&streaming, strcmp(argv[1], "on") == 0);
	return 0;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	k_mutex_lock(&state_lock, K_FOREVER);
	shell_print(sh, "ready=%d fresh=%u stale=%u errors=%u late=%u storage_pauses=%u",
		(int)atomic_get(&ready), fresh, stale, errors, late, storage_pauses);
	shell_print(sh, "calibrating=%d count=%u bias_valid=%d bias_dps=%.6f,%.6f,%.6f",
		calibrating, cal.count, bias_valid, bias[0], bias[1], bias[2]);
	k_mutex_unlock(&state_lock);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(imu_commands,
	SHELL_CMD_ARG(status, NULL, "Show sample counters and gyro bias", cmd_status, 1, 0),
	SHELL_CMD_ARG(calibrate, NULL, "Keep still for 1000 fresh samples (~10 s)", cmd_calibrate, 1, 0),
	SHELL_CMD_ARG(save, NULL, "Persist accepted gyro bias in NVS", cmd_save, 1, 0),
	SHELL_CMD_ARG(clear, NULL, "Delete saved bias and clear RAM bias", cmd_clear, 1, 0),
	SHELL_CMD_ARG(stream, NULL, "on|off: 10 Hz sample log", cmd_stream, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(imu, &imu_commands, "microWallaby IMU bring-up", NULL);

static bool process_request(int64_t now, int64_t *cal_start)
{
	int action = atomic_set(&request, REQ_NONE);
	int rc;

	if (action == REQ_CALIBRATE) {
		imu_cal_begin(&cal);
		calibrating = true;
		*cal_start = now;
		LOG_INF("Calibration started: keep the sensor still; previous bias retained until success");
	} else if (action == REQ_SAVE) {
		if (!bias_valid || calibrating) {
			LOG_WRN("Save rejected: complete calibration first");
			return false;
		}
		rc = imu_storage_save(WHO_ID, bias);
		LOG_INF("Calibration save result=%d (0=success)", rc);
		++storage_pauses;
		return true;
	} else if (action == REQ_CLEAR) {
		rc = imu_storage_clear();
		if (rc == 0 || rc == -ENOENT) {
			memset(bias, 0, sizeof(bias));
			bias_valid = false;
			calibrating = false;
		}
		LOG_INF("Calibration clear result=%d", rc);
		++storage_pauses;
		return true;
	}
	return false;
}

int main(void)
{
	struct sensor_value accel[3], gyro[3];
	struct imu_measurement sample;
	int64_t next, next_log, next_health, cal_start = 0, last_fresh;
	uint32_t previous_fresh = 0;
	int rc;

	LOG_INF("microWallaby NU-87 IMU bring-up: sensor ODR=104 Hz, poll target=100 Hz");
	LOG_INF("No Wi-Fi, joystick, touch, motors or ROMs enabled; console=1500000 baud");
	if (!device_is_ready(imu)) {
		LOG_ERR("IMU initialization failed; check 3.3 V logic, SDA/SCL, address and chip variant");
		return 0;
	}
	rc = imu_storage_init();
	if (rc == 0) {
		rc = imu_storage_load(WHO_ID, bias);
		bias_valid = rc == 0;
	}
	LOG_INF("Calibration load result=%d bias_valid=%d (missing record is normal on first boot)",
		rc, bias_valid);
	LOG_INF("Commands: imu status | imu calibrate | imu save | imu clear | imu stream off");
	k_mutex_lock(&state_lock, K_FOREVER);
	atomic_set(&ready, 1);
	k_mutex_unlock(&state_lock);
	next = k_uptime_get();
	last_fresh = next;
	next_log = next;
	next_health = next + 10000;
	while (true) {
		int64_t now = k_uptime_get();

		k_mutex_lock(&state_lock, K_FOREVER);
		if (process_request(now, &cal_start)) {
			next = k_uptime_get(); /* Deliberate flash pause is not a sampling failure. */
		}
		k_mutex_unlock(&state_lock);
		rc = sensor_sample_fetch(imu);
		if (rc == 0) {
			rc = sensor_channel_get(imu, SENSOR_CHAN_ACCEL_XYZ, accel);
		}
		if (rc == 0) {
			rc = sensor_channel_get(imu, SENSOR_CHAN_GYRO_XYZ, gyro);
		}
		now = k_uptime_get();
		k_mutex_lock(&state_lock, K_FOREVER);
		if (calibrating && (now - cal_start > 15000 || now - last_fresh > 100)) {
			calibrating = false;
			LOG_WRN("Calibration cancelled: missing samples or timeout");
		}
		if (rc == 0) {
			++fresh;
			last_fresh = now;
			for (int i = 0; i < 3; ++i) {
				sample.accel_g[i] = sensor_value_to_double(&accel[i]) / 9.80665;
				sample.gyro_dps[i] = sensor_value_to_double(&gyro[i]) * RAD_TO_DEG;
			}
			if (calibrating) {
				int result = imu_cal_add(&cal, &sample);
				if (result != 0) {
					calibrating = false;
					if (result > 0) {
						memcpy(bias, cal.mean + 3, sizeof(bias));
						bias_valid = true;
						LOG_INF("Calibration accepted: gyro_bias_dps=%.6f,%.6f,%.6f; use imu save",
							bias[0], bias[1], bias[2]);
						LOG_INF("Gyro stddev_dps=%.6f,%.6f,%.6f",
							imu_cal_stddev(&cal, 3), imu_cal_stddev(&cal, 4), imu_cal_stddev(&cal, 5));
					} else {
						LOG_WRN("Calibration rejected: motion/noise/range; count=%u", cal.count);
					}
				}
			}
			if (atomic_get(&streaming) && now >= next_log) {
				LOG_INF("sample=%u accel_g=%.4f,%.4f,%.4f gyro_dps=%.4f,%.4f,%.4f corrected_dps=%.4f,%.4f,%.4f",
					fresh, sample.accel_g[0], sample.accel_g[1], sample.accel_g[2],
					sample.gyro_dps[0], sample.gyro_dps[1], sample.gyro_dps[2],
					sample.gyro_dps[0] - bias[0], sample.gyro_dps[1] - bias[1], sample.gyro_dps[2] - bias[2]);
				next_log = now + 100;
			}
		} else if (rc == -EAGAIN) {
			++stale;
		} else {
			++errors;
			calibrating = false;
			if (errors == 1 || errors % 100 == 0) {
				LOG_ERR("Sample failed: %d (errors=%u); calibration cancelled", rc, errors);
			}
		}
		if (now >= next_health) {
			LOG_INF("health: fresh=%u delta_10s=%u stale=%u errors=%u late=%u age_ms=%lld",
				fresh, fresh - previous_fresh, stale, errors, late, now - last_fresh);
			previous_fresh = fresh;
			next_health = now + 10000;
		}
		next += 10;
		if (now > next) {
			++late;
			next = now + 10; /* Never busy catch-up after a failed/slow transfer. */
		}
		k_mutex_unlock(&state_lock);
		k_sleep(K_TIMEOUT_ABS_MS(next));
	}
	return 0;
}
