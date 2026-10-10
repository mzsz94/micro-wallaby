/* SPDX-License-Identifier: Apache-2.0 */
/* Ported from firmware/nu40_imu_bringup with NU87 pins and UART console. */
#include "imu_device.h"
#include "imu_calibration.h"
#include "imu_storage.h"

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(nu87_imu, LOG_LEVEL_INF);

static const struct device *const bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static const struct device *const gpioa = DEVICE_DT_GET(DT_NODELABEL(gpioa));
static K_MUTEX_DEFINE(state_lock);
static struct imu_device imu;
static struct imu_calibration cal;
static uint8_t address;
static double bias[3];
static bool bias_valid, calibrating, storage_attempted;
static bool connected_before;
static uint8_t previous_address, previous_who;
static uint32_t fresh, stale, errors, late, reconnects, consecutive_errors;
static int last_error, storage_error = -ENODEV;
static int64_t last_fresh, cal_started;
static atomic_t streaming = ATOMIC_INIT(1);
static atomic_t pending;
enum { REQ_NONE, REQ_RETRY, REQ_CALIBRATE, REQ_SAVE, REQ_CLEAR };

static int read_bus_levels(gpio_port_value_t *levels)
{
	/* Read the input register only; never change pins owned by the I2C driver. */
	return device_is_ready(gpioa) ? gpio_port_get_raw(gpioa, levels) : -ENODEV;
}

static int bus_read(void *ctx, uint8_t reg, uint8_t *data, size_t size)
{
	return i2c_write_read(bus, *(uint8_t *)ctx, &reg, 1, data, size);
}

static int bus_write(void *ctx, uint8_t reg, uint8_t value)
{
	return i2c_reg_write_byte(bus, *(uint8_t *)ctx, reg, value);
}

static void bus_sleep(void *ctx, unsigned ms)
{
	ARG_UNUSED(ctx);
	k_msleep(ms);
}

static int command_request(const struct shell *sh, int request)
{
	if (!atomic_cas(&pending, REQ_NONE, request)) {
		shell_error(sh, "A command is already pending");
		return -EBUSY;
	}
	shell_print(sh, "Request queued");
	return 0;
}

#define REQUEST_COMMAND(name, value) \
	static int name(const struct shell *sh, size_t argc, char **argv) \
	{ ARG_UNUSED(argc); ARG_UNUSED(argv); return command_request(sh, value); }
REQUEST_COMMAND(cmd_retry, REQ_RETRY)
REQUEST_COMMAND(cmd_calibrate, REQ_CALIBRATE)
REQUEST_COMMAND(cmd_save, REQ_SAVE)
REQUEST_COMMAND(cmd_clear, REQ_CLEAR)

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	k_mutex_lock(&state_lock, K_FOREVER);
	shell_print(sh, "NU87 IMU v1.2: SDA=PA26 SCL=PA25 I2C=100000 Hz");
	gpio_port_value_t levels;
	if (read_bus_levels(&levels) == 0) {
		shell_print(sh, "bus_levels: SDA=%d SCL=%d (idle should be 1/1)",
			!!(levels & BIT(26)), !!(levels & BIT(25)));
	}
	shell_print(sh, "ready=%d address=0x%02x profile=%s last_error=%d stage=%s",
		imu.ready, address, imu_device_name(&imu), last_error,
		imu.stage ? imu.stage : "waiting");
	if (imu.id_valid) {
		shell_print(sh, "WHO_AM_I=0x%02x", imu.who_am_i);
	} else {
		shell_print(sh, "WHO_AM_I=unavailable (no successful ID read)");
	}
	shell_print(sh, "fresh=%u stale=%u errors=%u late=%u reconnects=%u age_ms=%lld",
		fresh, stale, errors, late, reconnects,
		fresh ? k_uptime_get() - last_fresh : -1LL);
	shell_print(sh, "calibrating=%d count=%u bias_valid=%d bias_dps=%.6f,%.6f,%.6f storage=%d",
		calibrating, cal.count, bias_valid, bias[0], bias[1], bias[2], storage_error);
	k_mutex_unlock(&state_lock);
	return 0;
}

static int cmd_stream(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	if (!strcmp(argv[1], "on")) {
		atomic_set(&streaming, 1);
	} else if (!strcmp(argv[1], "off")) {
		atomic_set(&streaming, 0);
	} else {
		shell_error(sh, "Usage: imu stream on|off");
		return -EINVAL;
	}
	shell_print(sh, "Sample stream %s", argv[1]);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(imu_commands,
	SHELL_CMD_ARG(status, NULL, "Show sensor identity and health", cmd_status, 1, 0),
	SHELL_CMD_ARG(retry, NULL, "Probe 0x6a/0x6b and reinitialize", cmd_retry, 1, 0),
	SHELL_CMD_ARG(stream, NULL, "on|off: sample logging", cmd_stream, 2, 0),
	SHELL_CMD_ARG(calibrate, NULL, "Keep still for 1000 fresh samples", cmd_calibrate, 1, 0),
	SHELL_CMD_ARG(save, NULL, "Save accepted gyro bias", cmd_save, 1, 0),
	SHELL_CMD_ARG(clear, NULL, "Clear saved and RAM bias", cmd_clear, 1, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(imu, &imu_commands, "NU87 IMU diagnostics", NULL);

/* Called by the acquisition thread with state_lock held; only it uses I2C. */
static bool connect_sensor(void)
{
	imu.ready = false;
	calibrating = false;
	if (!device_is_ready(bus)) {
		imu.id_valid = false;
		imu.stage = "i2c-controller-not-ready";
		last_error = -ENODEV;
		LOG_ERR("I2C controller not ready; retrying in 2 seconds");
		return false;
	}
	gpio_port_value_t levels;
	if (read_bus_levels(&levels) == 0) {
		LOG_INF("I2C idle before probe: SDA=%d SCL=%d (expected 1/1)",
			!!(levels & BIT(26)), !!(levels & BIT(25)));
	}
	for (uint8_t candidate = 0x6a; candidate <= 0x6b; ++candidate) {
		address = candidate;
		imu.io = (struct imu_io){
			.read = bus_read, .write = bus_write,
			.sleep_ms = bus_sleep, .ctx = &address,
		};
		last_error = imu_device_init(&imu);
		if (imu.id_valid) {
			LOG_INF("probe addr=0x%02x WHO_AM_I=0x%02x profile=%s init=%d stage=%s",
				address, imu.who_am_i, imu_device_name(&imu), last_error, imu.stage);
		} else {
			LOG_WRN("probe addr=0x%02x WHO_AM_I=unavailable read_error=%d stage=%s",
				address, last_error, imu.stage);
		}
		if (last_error == 0) {
			++reconnects;
			consecutive_errors = 0;
			last_fresh = k_uptime_get();
			if (!storage_attempted) {
				storage_error = imu_storage_init();
				storage_attempted = true;
			}
			if (!connected_before || previous_address != address || previous_who != imu.who_am_i) {
				memset(bias, 0, sizeof(bias));
				int loaded = storage_error ? storage_error : imu_storage_load(imu.who_am_i, bias);
				bias_valid = loaded == 0;
				LOG_INF("Calibration load result=%d bias_valid=%d (missing record is normal)", loaded, bias_valid);
			} else {
				LOG_INF("Reconnected same profile/address; current RAM bias retained");
			}
			connected_before = true;
			previous_address = address;
			previous_who = imu.who_am_i;
			LOG_INF("IMU ready: profile=%s addr=0x%02x; sensor=104 Hz poll=100 Hz", imu_device_name(&imu), address);
			return true;
		}
	}
	LOG_WRN("No ready IMU; retrying in 2 seconds. Shell remains available.");
	return false;
}

static void process_request(void)
{
	int request = atomic_set(&pending, REQ_NONE);
	if (request == REQ_NONE) { return; }
	if (request == REQ_RETRY) {
		imu.ready = false;
		calibrating = false;
		return;
	}
	if (!imu.ready) {
		LOG_WRN("Command rejected: IMU not ready; use imu status");
		return;
	}
	if (request == REQ_CALIBRATE) {
		imu_cal_begin(&cal);
		calibrating = true;
		cal_started = k_uptime_get();
		LOG_INF("Calibration started: keep sensor still for ~10 seconds");
	} else if (request == REQ_SAVE) {
		if (!bias_valid || calibrating) {
			LOG_WRN("Save rejected: first complete calibration");
		} else {
			int rc = imu_storage_save(imu.who_am_i, bias);
			LOG_INF("Calibration save result=%d", rc);
		}
	} else if (request == REQ_CLEAR) {
		int rc = imu_storage_clear();
		if (rc == 0 || rc == -ENOENT) {
			memset(bias, 0, sizeof(bias));
			bias_valid = false;
			calibrating = false;
		}
		LOG_INF("Calibration clear result=%d", rc);
	}
}

int main(void)
{
	int64_t next_log = 0, next_health = 0, next_retry = 0;
	uint32_t previous_fresh = 0;

	k_msleep(1000);
	LOG_INF("NU87 IMU v1.2 (%s %s)", __DATE__, __TIME__);
	LOG_INF("NU87 app=0x%lx; SDA=PA26 SCL=PA25 I2C=100000 Hz; UART=1500000 baud",
		(unsigned long)DT_REG_ADDR(DT_CHOSEN(zephyr_code_partition)));
	LOG_INF("Commands: imu status | retry | stream on/off | calibrate | save | clear");
	int64_t next = k_uptime_get();
	next_health = next + 10000;
	while (true) {
		k_mutex_lock(&state_lock, K_FOREVER);
		bool forced_retry = atomic_get(&pending) == REQ_RETRY;
		process_request();
		int64_t now = k_uptime_get();
		if (!imu.ready && (forced_retry || now >= next_retry)) {
			connect_sensor();
			now = k_uptime_get();
			next_retry = now + 2000;
			next = now;
		}
		if (imu.ready) {
			struct imu_sample sample;
			int rc = imu_device_read(&imu, &sample);
			now = k_uptime_get();
			if (calibrating && (now - cal_started > 15000 || now - last_fresh > 100)) {
				calibrating = false;
				LOG_WRN("Calibration cancelled: timeout or missing samples");
			}
			if (rc == 0) {
				++fresh;
				last_fresh = now;
				last_error = 0;
				consecutive_errors = 0;
				if (calibrating) {
					struct imu_measurement measurement;
					memcpy(measurement.accel_g, sample.accel_g, sizeof(sample.accel_g));
					memcpy(measurement.gyro_dps, sample.gyro_dps, sizeof(sample.gyro_dps));
					int result = imu_cal_add(&cal, &measurement);
					if (result != 0) {
						calibrating = false;
						if (result > 0) {
							memcpy(bias, cal.mean + 3, sizeof(bias));
							bias_valid = true;
							LOG_INF("Calibration accepted: count=%u bias_dps=%.6f,%.6f,%.6f; use imu save", cal.count, bias[0], bias[1], bias[2]);
						} else {
							LOG_WRN("Calibration rejected: motion/noise/range count=%u", cal.count);
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
				++consecutive_errors;
				last_error = rc;
				calibrating = false;
				if (consecutive_errors == 1) {
					LOG_ERR("Sample failed rc=%d stage=%s", rc, imu.stage);
				}
			}
			if (consecutive_errors >= 5 || now - last_fresh > 1000) {
				imu.ready = false;
				calibrating = false;
				next_retry = now + 500;
				LOG_WRN("Sensor stalled; scheduling reinitialization");
			}
		}
		if (now >= next_health) {
			LOG_INF("health: ready=%d fresh=%u delta_10s=%u stale=%u errors=%u late=%u",
				imu.ready, fresh, fresh - previous_fresh, stale, errors, late);
			previous_fresh = fresh;
			next_health = now + 10000;
		}
		next += 10;
		if (now > next) { ++late; next = now + 10; }
		k_mutex_unlock(&state_lock);
		k_sleep(K_TIMEOUT_ABS_MS(next));
	}
	return 0;
}
