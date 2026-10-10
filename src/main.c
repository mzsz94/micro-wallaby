#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

#define TOUCH_NODE DT_PATH(zephyr_user)
#define SAMPLE_INTERVAL_MS 100
#define PRINT_EVERY_SAMPLES 5

#define BT_UUID_HOME_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x7b1e0001, 0x21a8, 0x4f8c, 0xbb2d, 0x4c7d2156f001)
#define BT_UUID_HOME_SENSOR_VAL \
	BT_UUID_128_ENCODE(0x7b1e0002, 0x21a8, 0x4f8c, 0xbb2d, 0x4c7d2156f001)
#define BT_UUID_HOME_COMMAND_VAL \
	BT_UUID_128_ENCODE(0x7b1e0003, 0x21a8, 0x4f8c, 0xbb2d, 0x4c7d2156f001)

static const struct gpio_dt_spec touch_d0 =
	GPIO_DT_SPEC_GET(TOUCH_NODE, touch_d0_gpios);
static const struct adc_dt_spec touch_a0 =
	ADC_DT_SPEC_GET_BY_IDX(TOUCH_NODE, 0);
static const struct adc_dt_spec joystick_x =
	ADC_DT_SPEC_GET_BY_IDX(TOUCH_NODE, 1);
static const struct adc_dt_spec joystick_y =
	ADC_DT_SPEC_GET_BY_IDX(TOUCH_NODE, 2);
static const struct gpio_dt_spec joystick_sw =
	GPIO_DT_SPEC_GET(TOUCH_NODE, joystick_sw_gpios);
static const struct device *const imu_bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static const struct gpio_dt_spec house_leds[] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios),
};

struct sensor_packet {
	uint8_t version;
	uint8_t flags;
	uint16_t sequence;
	uint16_t touch_mv;
	uint16_t accel_mg[3];
	uint16_t gyro_centi_dps[3];
	uint8_t joystick_x;
	uint8_t joystick_y;
} __packed;

BUILD_ASSERT(sizeof(struct sensor_packet) == 20,
	     "BLE sensor packet must fit the default ATT payload");

static struct sensor_packet latest_packet = { .version = 2 };
static bool notifications_enabled;

static const struct bt_uuid_128 home_service_uuid =
	BT_UUID_INIT_128(BT_UUID_HOME_SERVICE_VAL);
static const struct bt_uuid_128 home_sensor_uuid =
	BT_UUID_INIT_128(BT_UUID_HOME_SENSOR_VAL);
static const struct bt_uuid_128 home_command_uuid =
	BT_UUID_INIT_128(BT_UUID_HOME_COMMAND_VAL);

static ssize_t read_sensor_packet(struct bt_conn *conn,
				  const struct bt_gatt_attr *attr, void *buf,
				  uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &latest_packet,
				 sizeof(latest_packet));
}

static ssize_t write_house_command(struct bt_conn *conn,
				   const struct bt_gatt_attr *attr,
				   const void *buf, uint16_t len,
				   uint16_t offset, uint8_t flags)
{
	const uint8_t *command = buf;

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len != 3) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	if (command[0] != 0x01 || command[1] >= ARRAY_SIZE(house_leds) ||
	    command[2] > 1) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	gpio_pin_set_dt(&house_leds[command[1]], command[2]);
	printk("HOUSE room=%u state=%u\n", command[1], command[2]);
	return len;
}

static void sensor_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	notifications_enabled = value == BT_GATT_CCC_NOTIFY;
	printk("BLE sensor notifications: %s\n",
	       notifications_enabled ? "ON" : "OFF");
}

BT_GATT_SERVICE_DEFINE(home_service,
	BT_GATT_PRIMARY_SERVICE(&home_service_uuid),
	BT_GATT_CHARACTERISTIC(&home_sensor_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_sensor_packet, NULL,
			       &latest_packet),
	BT_GATT_CCC(sensor_ccc_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&home_command_uuid.uuid,
			       BT_GATT_CHRC_WRITE |
			       BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, write_house_command, NULL),
);

static const struct bt_data advertising_data[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_HOME_SERVICE_VAL),
};

static const struct bt_data scan_response_data[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static int start_advertising(void)
{
	return bt_le_adv_start(BT_LE_ADV_CONN_FAST_1,
			       advertising_data, ARRAY_SIZE(advertising_data),
			       scan_response_data, ARRAY_SIZE(scan_response_data));
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err == 0) {
		printk("BLE connected\n");
	} else {
		printk("BLE connection failed: %u\n", err);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	int ret;

	notifications_enabled = false;
	printk("BLE disconnected: %u\n", reason);
	ret = start_advertising();
	if (ret == 0 || ret == -EALREADY) {
		printk("BLE advertising restarted: %s\n", CONFIG_BT_DEVICE_NAME);
	} else {
		printk("BLE advertising restart failed: %d\n", ret);
	}
}

BT_CONN_CB_DEFINE(connection_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

static int initialize_bluetooth(void)
{
	int ret = bt_enable(NULL);

	if (ret != 0) {
		return ret;
	}

	return start_advertising();
}

enum imu_kind {
	IMU_NONE,
	IMU_MPU6XXX,
	IMU_LSM6DS3TR,
};

struct imu_device {
	enum imu_kind kind;
	uint8_t address;
	uint8_t who_am_i;
};

static int16_t be16(const uint8_t *data)
{
	return (int16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static int16_t le16(const uint8_t *data)
{
	return (int16_t)(((uint16_t)data[1] << 8) | data[0]);
}

static int i2c_read_register(uint8_t address, uint8_t reg, uint8_t *value)
{
	return i2c_write_read(imu_bus, address, &reg, sizeof(reg), value, 1);
}

static void scan_i2c_bus(void)
{
	uint8_t value;
	bool found = false;

	printk("I2C scan (SDA=P0.26, SCL=P0.27):");
	for (uint8_t address = 0x08; address <= 0x77; address++) {
		if (i2c_read(imu_bus, &value, 1, address) == 0) {
			printk(" 0x%02x", address);
			found = true;
		}
	}
	printk(found ? "\n" : " no devices\n");
}

static struct imu_device detect_imu(void)
{
	static const uint8_t mpu_ids[] = {
		0x12, /* ICM-20602 */
		0x68, /* MPU-6050 */
		0x70, /* MPU-6500 */
		0x71, /* MPU-9250 */
		0x73, /* MPU-9255 */
		0x98, /* ICM-20689 */
	};
	struct imu_device imu = { 0 };

	for (uint8_t address = 0x68; address <= 0x69; address++) {
		uint8_t who_am_i;

		if (i2c_read_register(address, 0x75, &who_am_i) != 0) {
			continue;
		}

		printk("IMU probe: address=0x%02x WHO_AM_I(0x75)=0x%02x\n",
		       address, who_am_i);

		for (size_t i = 0; i < ARRAY_SIZE(mpu_ids); i++) {
			if (who_am_i == mpu_ids[i]) {
				imu.kind = IMU_MPU6XXX;
				imu.address = address;
				imu.who_am_i = who_am_i;
				return imu;
			}
		}
	}

	for (uint8_t address = 0x6a; address <= 0x6b; address++) {
		uint8_t who_am_i;

		if (i2c_read_register(address, 0x0f, &who_am_i) != 0) {
			continue;
		}

		printk("IMU probe: address=0x%02x WHO_AM_I(0x0f)=0x%02x\n",
		       address, who_am_i);
		if (who_am_i == 0x6a) {
			imu.kind = IMU_LSM6DS3TR;
			imu.address = address;
			imu.who_am_i = who_am_i;
			return imu;
		}
	}

	/* Print common alternate identification registers for diagnosis. */
	for (uint8_t address = 0x08; address <= 0x77; address++) {
		uint8_t chip_id = 0xff;
		uint8_t who_am_i = 0xff;

		if (i2c_read_register(address, 0x00, &chip_id) == 0 ||
		    i2c_read_register(address, 0x0f, &who_am_i) == 0) {
			printk("Unknown I2C device: 0x%02x ID00=0x%02x ID0F=0x%02x\n",
			       address, chip_id, who_am_i);
		}
	}

	return imu;
}

static int initialize_mpu6xxx(const struct imu_device *imu)
{
	int ret;

	/* Wake up, use the gyro PLL, and select +/-2 g and +/-250 dps. */
	ret = i2c_reg_write_byte(imu_bus, imu->address, 0x6b, 0x01);
	if (ret != 0) {
		return ret;
	}
	k_msleep(100);

	ret = i2c_reg_write_byte(imu_bus, imu->address, 0x1a, 0x03);
	ret |= i2c_reg_write_byte(imu_bus, imu->address, 0x19, 0x09);
	ret |= i2c_reg_write_byte(imu_bus, imu->address, 0x1b, 0x00);
	ret |= i2c_reg_write_byte(imu_bus, imu->address, 0x1c, 0x00);
	return ret;
}

static int read_mpu6xxx(const struct imu_device *imu, int32_t accel_mg[3],
			int32_t gyro_mdps[3])
{
	uint8_t start_reg = 0x3b;
	uint8_t data[14];
	int ret;

	ret = i2c_write_read(imu_bus, imu->address, &start_reg, sizeof(start_reg),
			     data, sizeof(data));
	if (ret != 0) {
		return ret;
	}

	/* +/-2 g: 16384 LSB/g. +/-250 dps: 131 LSB/(degree/s). */
	accel_mg[0] = ((int32_t)be16(&data[0]) * 1000) / 16384;
	accel_mg[1] = ((int32_t)be16(&data[2]) * 1000) / 16384;
	accel_mg[2] = ((int32_t)be16(&data[4]) * 1000) / 16384;
	gyro_mdps[0] = ((int32_t)be16(&data[8]) * 1000) / 131;
	gyro_mdps[1] = ((int32_t)be16(&data[10]) * 1000) / 131;
	gyro_mdps[2] = ((int32_t)be16(&data[12]) * 1000) / 131;
	return 0;
}

static int initialize_lsm6ds3tr(const struct imu_device *imu)
{
	int ret;

	/* Block data updates and auto-increment multi-byte register reads. */
	ret = i2c_reg_write_byte(imu_bus, imu->address, 0x12, 0x44);
	/* Accelerometer: 104 Hz, +/-2 g. Gyroscope: 104 Hz, +/-245 dps. */
	ret |= i2c_reg_write_byte(imu_bus, imu->address, 0x10, 0x40);
	ret |= i2c_reg_write_byte(imu_bus, imu->address, 0x11, 0x40);
	k_msleep(100);
	return ret;
}

static int read_lsm6ds3tr(const struct imu_device *imu, int32_t accel_mg[3],
			  int32_t gyro_mdps[3])
{
	uint8_t start_reg = 0x22;
	uint8_t data[12];
	int ret;

	ret = i2c_write_read(imu_bus, imu->address, &start_reg, sizeof(start_reg),
			     data, sizeof(data));
	if (ret != 0) {
		return ret;
	}

	/* OUTX_L_G..OUTZ_H_G, then OUTX_L_XL..OUTZ_H_XL. */
	for (size_t axis = 0; axis < 3; axis++) {
		/* +/-2 g sensitivity: 0.061 mg/LSB. */
		accel_mg[axis] = ((int32_t)le16(&data[6 + axis * 2]) * 61) / 1000;
		/* +/-245 dps sensitivity: 8.75 mdps/LSB. */
		gyro_mdps[axis] = ((int32_t)le16(&data[axis * 2]) * 875) / 100;
	}

	return 0;
}

static uint8_t adc_raw_to_u8(int16_t raw)
{
	return (uint8_t)CLAMP(((int32_t)raw * UINT8_MAX) / 4095,
			      0, UINT8_MAX);
}

int main(void)
{
	struct adc_sequence touch_sequence = { 0 };
	struct adc_sequence joystick_x_sequence = { 0 };
	struct adc_sequence joystick_y_sequence = { 0 };
	struct imu_device imu;
	int16_t touch_raw = 0;
	int16_t joystick_x_raw = 0;
	int16_t joystick_y_raw = 0;
	uint16_t packet_sequence = 0;
	uint8_t print_counter = 0;
	int ret;

	k_msleep(1000);
	printk("\nNU40-DK BLE sensor game remote\n");
	printk("Touch: D0=P0.02, A0=P0.03/AIN1, interval=%d ms\n",
	       SAMPLE_INTERVAL_MS);
	printk("Joystick: X=P0.04/AIN2, Y=P0.28/AIN4, SW=P1.03\n");

	if (!gpio_is_ready_dt(&touch_d0) || !adc_is_ready_dt(&touch_a0) ||
	    !adc_is_ready_dt(&joystick_x) || !adc_is_ready_dt(&joystick_y) ||
	    !gpio_is_ready_dt(&joystick_sw)) {
		printk("ERROR: touch or joystick GPIO/ADC is not ready\n");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&touch_d0, GPIO_INPUT);
	ret |= gpio_pin_configure_dt(&joystick_sw, GPIO_INPUT);
	ret |= adc_channel_setup_dt(&touch_a0);
	ret |= adc_channel_setup_dt(&joystick_x);
	ret |= adc_channel_setup_dt(&joystick_y);
	ret |= adc_sequence_init_dt(&touch_a0, &touch_sequence);
	ret |= adc_sequence_init_dt(&joystick_x, &joystick_x_sequence);
	ret |= adc_sequence_init_dt(&joystick_y, &joystick_y_sequence);
	if (ret != 0) {
		printk("ERROR: touch or joystick input setup failed (%d)\n", ret);
		return ret;
	}

	if (!device_is_ready(imu_bus)) {
		printk("ERROR: I2C0 is not ready\n");
		return -ENODEV;
	}

	for (size_t i = 0; i < ARRAY_SIZE(house_leds); i++) {
		if (!gpio_is_ready_dt(&house_leds[i]) ||
		    gpio_pin_configure_dt(&house_leds[i], GPIO_OUTPUT_INACTIVE) != 0) {
			printk("ERROR: house LED %u is not ready\n", i);
			return -ENODEV;
		}
	}

	scan_i2c_bus();
	imu = detect_imu();
	if (imu.kind == IMU_MPU6XXX) {
		ret = initialize_mpu6xxx(&imu);
		printk("IMU: MPU6xxx-compatible at 0x%02x, init=%s\n",
		       imu.address, ret == 0 ? "OK" : "FAILED");
	} else if (imu.kind == IMU_LSM6DS3TR) {
		ret = initialize_lsm6ds3tr(&imu);
		printk("IMU: LSM6DS3TR at 0x%02x, init=%s\n",
		       imu.address, ret == 0 ? "OK" : "FAILED");
	} else {
		printk("IMU: no supported device; touch monitoring continues\n");
	}

	ret = initialize_bluetooth();
	if (ret == 0) {
		printk("BLE advertising: %s\n", CONFIG_BT_DEVICE_NAME);
	} else {
		printk("ERROR: BLE initialization failed (%d)\n", ret);
	}

	printk("Columns: time_ms touch_D0 touch_ADC touch_mV "
	       "joy_X joy_Y joy_SW accel_mg[x,y,z] gyro_mdps[x,y,z]\n");

	while (true) {
		int32_t touch_mv;
		int touch_digital;
		int joystick_pressed;
		int touch_ret;
		int joystick_x_ret;
		int joystick_y_ret;
		int32_t accel_mg[3] = { 0 };
		int32_t gyro_mdps[3] = { 0 };
		int imu_ret = -ENODEV;

		touch_digital = gpio_pin_get_dt(&touch_d0);
		joystick_pressed = gpio_pin_get_dt(&joystick_sw);

		touch_sequence.buffer = &touch_raw;
		touch_sequence.buffer_size = sizeof(touch_raw);
		touch_ret = adc_read_dt(&touch_a0, &touch_sequence);
		touch_mv = touch_raw;
		if (touch_ret == 0) {
			touch_ret = adc_raw_to_millivolts_dt(&touch_a0, &touch_mv);
		}

		joystick_x_sequence.buffer = &joystick_x_raw;
		joystick_x_sequence.buffer_size = sizeof(joystick_x_raw);
		joystick_x_ret = adc_read_dt(&joystick_x, &joystick_x_sequence);
		joystick_y_sequence.buffer = &joystick_y_raw;
		joystick_y_sequence.buffer_size = sizeof(joystick_y_raw);
		joystick_y_ret = adc_read_dt(&joystick_y, &joystick_y_sequence);

		if (imu.kind == IMU_MPU6XXX) {
			imu_ret = read_mpu6xxx(&imu, accel_mg, gyro_mdps);
		} else if (imu.kind == IMU_LSM6DS3TR) {
			imu_ret = read_lsm6ds3tr(&imu, accel_mg, gyro_mdps);
		}

		if (imu_ret == 0) {
			latest_packet.version = 2;
			latest_packet.flags = (touch_digital > 0 ? BIT(0) : 0) |
					      (joystick_pressed > 0 ? BIT(1) : 0);
			latest_packet.sequence = sys_cpu_to_le16(++packet_sequence);
			latest_packet.touch_mv =
				sys_cpu_to_le16((uint16_t)CLAMP(touch_mv, 0, UINT16_MAX));
			for (size_t axis = 0; axis < 3; axis++) {
				latest_packet.accel_mg[axis] =
					sys_cpu_to_le16((uint16_t)CLAMP(accel_mg[axis],
								    INT16_MIN, INT16_MAX));
				latest_packet.gyro_centi_dps[axis] =
					sys_cpu_to_le16((uint16_t)CLAMP(gyro_mdps[axis] / 10,
								    INT16_MIN, INT16_MAX));
			}
			latest_packet.joystick_x = joystick_x_ret == 0 ?
				adc_raw_to_u8(joystick_x_raw) : 127;
			latest_packet.joystick_y = joystick_y_ret == 0 ?
				adc_raw_to_u8(joystick_y_raw) : 127;

			if (notifications_enabled) {
				bt_gatt_notify(NULL, &home_service.attrs[2],
					       &latest_packet, sizeof(latest_packet));
			}
		}

		if (++print_counter >= PRINT_EVERY_SAMPLES) {
			print_counter = 0;
			printk("DATA %lld D0=%d ADC=%d mV=",
			       k_uptime_get(), touch_digital, touch_raw);
			if (touch_ret == 0) {
				printk("%d", touch_mv);
			} else {
				printk("ERR(%d)", touch_ret);
			}
			printk(" JX=");
			if (joystick_x_ret == 0) {
				printk("%d", joystick_x_raw);
			} else {
				printk("ERR(%d)", joystick_x_ret);
			}
			printk(" JY=");
			if (joystick_y_ret == 0) {
				printk("%d", joystick_y_raw);
			} else {
				printk("ERR(%d)", joystick_y_ret);
			}
			printk(" JSW=%d", joystick_pressed);
			if (imu_ret == 0) {
				printk(" AX=%d AY=%d AZ=%d GX=%d GY=%d GZ=%d",
				       accel_mg[0], accel_mg[1], accel_mg[2],
				       gyro_mdps[0], gyro_mdps[1], gyro_mdps[2]);
			} else {
				printk(" IMU=unavailable");
			}
			printk("\n");
		}

		k_msleep(SAMPLE_INTERVAL_MS);
	}

	return 0;
}
