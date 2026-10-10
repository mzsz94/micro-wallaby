/* SPDX-License-Identifier: Apache-2.0 */
#include "gateway_imu.h"
#include "imu_device.h"
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <stdint.h>

LOG_MODULE_REGISTER(gateway_imu, LOG_LEVEL_INF);
static const struct device *bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static uint16_t address;
K_MUTEX_DEFINE(imu_state_lock);
static struct {
	bool ready, id_valid;
	uint8_t id, addr;
	uint32_t sequence, errors;
	int last_error;
	int64_t received;
	int32_t accel[3], gyro[3];
} state;

static int read_reg(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
	ARG_UNUSED(ctx);
	return i2c_write_read(bus, address, &reg, 1, data, len);
}
static int write_reg(void *ctx, uint8_t reg, uint8_t value)
{
	ARG_UNUSED(ctx);
	uint8_t data[] = {reg, value};
	return i2c_write(bus, data, sizeof(data), address);
}
static void sleep_ms(void *ctx, unsigned ms)
{
	ARG_UNUSED(ctx);
	k_msleep(ms);
}
static void acquire(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	struct imu_device imu = {.io = {.read = read_reg, .write = write_reg,
		.sleep_ms = sleep_ms}};
	unsigned failures = 0;
	LOG_INF("NU87 live IMU v1: SDA=PA26 SCL=PA25 I2C=100000 raw axes; no calibration writes");
	while (true) {
		if (!imu.ready) {
			int rc = -ENODEV;
			for (address = 0x6a; device_is_ready(bus) && address <= 0x6b; address++) {
				rc = imu_device_init(&imu);
				LOG_INF("probe addr=0x%02x WHO_AM_I=%s0x%02x profile=%s result=%d",
					address, imu.id_valid ? "" : "unavailable:",
					imu.who_am_i, imu_device_name(&imu), rc);
				k_mutex_lock(&imu_state_lock, K_FOREVER);
				state.id_valid = imu.id_valid;
				state.id = imu.who_am_i; state.addr = address;
				state.last_error = rc; state.ready = rc == 0;
				k_mutex_unlock(&imu_state_lock);
				if (!rc) { failures = 0; break; }
			}
			if (rc) { k_msleep(2000); continue; }
		}
		struct imu_sample sample;
		int rc = imu_device_read(&imu, &sample);
		k_mutex_lock(&imu_state_lock, K_FOREVER);
		if (!rc) {
			state.ready = true; state.last_error = 0;
			state.sequence++; state.received = k_uptime_get();
			for (unsigned i = 0; i < 3; i++) {
				state.accel[i] = (int32_t)(sample.accel_g[i] * 1000.0);
				state.gyro[i] = (int32_t)(sample.gyro_dps[i] * 1000.0);
			}
			failures = 0;
		} else if (rc != -EAGAIN) {
			state.errors++; state.last_error = rc; state.ready = false;
			if (++failures >= 5) { imu.ready = false; }
		}
		k_mutex_unlock(&imu_state_lock);
		k_msleep(10);
	}
}
K_THREAD_STACK_DEFINE(imu_stack, 3072);
static struct k_thread imu_thread;
void gateway_imu_start(void)
{
	k_tid_t id = k_thread_create(&imu_thread, imu_stack, K_THREAD_STACK_SIZEOF(imu_stack),
		acquire, NULL, NULL, NULL, 10, 0, K_NO_WAIT);
	k_thread_name_set(id, "live_imu");
}
int gateway_imu_status(char *buffer, size_t capacity)
{
	k_mutex_lock(&imu_state_lock, K_FOREVER);
	int64_t age = state.sequence ? k_uptime_get() - state.received : -1;
	int n = snprintk(buffer, capacity,
		"{\"configured\":true,\"source\":\"nu87-i2c\",\"ready\":%s,\"fresh\":%s,"
		"\"sample\":%u,\"ageMs\":%lld,\"uptimeMs\":%lld,\"whoAmI\":%d,\"address\":%u,"
		"\"accelMg\":[%d,%d,%d],\"gyroMdps\":[%d,%d,%d],\"errors\":%u,\"lastError\":%d}",
		state.ready ? "true" : "false",
		state.ready && age >= 0 && age <= 500 ? "true" : "false",
		state.sequence, age, k_uptime_get(), state.id_valid ? state.id : -1, state.addr,
		state.accel[0], state.accel[1], state.accel[2],
		state.gyro[0], state.gyro[1], state.gyro[2], state.errors, state.last_error);
	k_mutex_unlock(&imu_state_lock);
	return n;
}
