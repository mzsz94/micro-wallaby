#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "lcd_command.h"
#include "lcd_device.h"
#include "oled_device.h"

#if defined(CONFIG_LCD_LEGACY_DFU)
#include <hal/nrf_power.h>
#endif

#define LCD_ENCODE_UUID(...) BT_UUID_128_ENCODE(__VA_ARGS__)
#define LCD_SERVICE_UUID_VAL LCD_ENCODE_UUID(LCD_SERVICE_UUID_PARTS)
#define LCD_COMMAND_UUID_VAL LCD_ENCODE_UUID(LCD_COMMAND_UUID_PARTS)
#define LCD_STATUS_UUID_VAL LCD_ENCODE_UUID(LCD_STATUS_UUID_PARTS)
#define STATUS_ATTR_INDEX 4
#define LCD_PROFILE_CONFIGURED (IS_ENABLED(CONFIG_LCD_PCF8574_PROFILE) || \
				IS_ENABLED(CONFIG_LCD_SSD1306_PROFILE))
BUILD_ASSERT(CONFIG_LCD_ROWS == 1 || CONFIG_LCD_ROWS == 2 || CONFIG_LCD_ROWS == 4,
	     "Text profiles support 1, 2, or 4 rows");
BUILD_ASSERT(LCD_COMMAND_MAX_SIZE <= 20 && LCD_STATUS_SIZE <= 20,
	     "LCD protocol must fit the default ATT MTU");

struct pending_command {
	struct lcd_command command;
	struct bt_conn *connection;
	atomic_val_t generation;
};

K_MSGQ_DEFINE(command_queue, sizeof(struct pending_command), 4, 4);
static atomic_t connection_generation;
static struct k_spinlock status_lock;
#if defined(CONFIG_LCD_SSD1306_PROFILE)
static struct oled_device oled;
BUILD_ASSERT(CONFIG_LCD_COLUMNS == OLED_COLUMNS && CONFIG_LCD_ROWS == OLED_ROWS,
	     "SSD1306 128x32 profile requires 16x4 text cells");
#else
static struct lcd_device lcd;
#endif
static uint8_t latest_status[LCD_STATUS_SIZE] = {
	LCD_PROTOCOL_VERSION, LCD_OP_STATUS, 0, 0, LCD_RESULT_UNCONFIGURED, 0, 0, 0,
};

static const struct bt_uuid_128 service_uuid = BT_UUID_INIT_128(LCD_SERVICE_UUID_VAL);
static const struct bt_uuid_128 command_uuid = BT_UUID_INIT_128(LCD_COMMAND_UUID_VAL);
static const struct bt_uuid_128 status_uuid = BT_UUID_INIT_128(LCD_STATUS_UUID_VAL);

static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   void *buffer, uint16_t length, uint16_t offset)
{
	uint8_t snapshot[LCD_STATUS_SIZE];
	k_spinlock_key_t key = k_spin_lock(&status_lock);

	memcpy(snapshot, latest_status, sizeof(snapshot));
	k_spin_unlock(&status_lock, key);
	return bt_gatt_attr_read(conn, attr, buffer, length, offset, snapshot, sizeof(snapshot));
}

static ssize_t write_command(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     const void *buffer, uint16_t length, uint16_t offset,
			     uint8_t flags)
{
	struct pending_command item = {0};
	int rc;

	ARG_UNUSED(attr);
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (flags & (BT_GATT_WRITE_FLAG_PREPARE | BT_GATT_WRITE_FLAG_EXECUTE)) {
		return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
	}
	if (length < LCD_COMMAND_HEADER_SIZE || length > LCD_COMMAND_MAX_SIZE) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	rc = lcd_command_parse(buffer, length, CONFIG_LCD_COLUMNS, CONFIG_LCD_ROWS,
			       &item.command);
	if (rc != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}
	item.connection = bt_conn_ref(conn);
	item.generation = atomic_get(&connection_generation);
	rc = k_msgq_put(&command_queue, &item, K_NO_WAIT);
	if (rc != 0) {
		bt_conn_unref(item.connection);
		return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
	}
	return length;
}

BT_GATT_SERVICE_DEFINE(lcd_service,
	BT_GATT_PRIMARY_SERVICE(&service_uuid),
	BT_GATT_CHARACTERISTIC(&command_uuid.uuid, BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE, NULL, write_command, NULL),
	BT_GATT_CHARACTERISTIC(&status_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_status, NULL, NULL),
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

static const struct bt_data advertising[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, LCD_SERVICE_UUID_VAL),
};
static const struct bt_data scan_response[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void advertising_work(struct k_work *work)
{
	int rc;

	ARG_UNUSED(work);
	rc = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, advertising, ARRAY_SIZE(advertising),
			     scan_response, ARRAY_SIZE(scan_response));
	if (rc != 0 && rc != -EALREADY) {
		printk("LCD BLE advertising error=%d\n", rc);
	}
}
K_WORK_DEFINE(advertising_restart, advertising_work);

static void connected(struct bt_conn *conn, uint8_t error)
{
	ARG_UNUSED(conn);
	if (error == 0) {
		atomic_inc(&connection_generation);
		printk("LCD BLE connected\n");
	} else {
		k_work_submit(&advertising_restart);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	atomic_inc(&connection_generation);
	printk("LCD BLE disconnected reason=%u; queued commands expire\n", reason);
}

static void recycled(void)
{
	/* Wait for the old connection references to be released before advertising. */
	k_work_submit(&advertising_restart);
}

BT_CONN_CB_DEFINE(connection_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
};

static bool display_ready(void)
{
#if defined(CONFIG_LCD_SSD1306_PROFILE)
	return oled.initialized;
#else
	return lcd.initialized;
#endif
}

static void update_status(uint8_t opcode, uint16_t sequence, uint8_t result)
{
	uint8_t status[LCD_STATUS_SIZE] = {
		LCD_PROTOCOL_VERSION, opcode, 0, 0, result, 0, 0, 0,
	};
	k_spinlock_key_t key;

	lcd_put_le16(status + 2, sequence);
	if (display_ready()) {
		status[5] |= LCD_FLAG_READY;
	}
	if (IS_ENABLED(CONFIG_LCD_LEGACY_DFU)) {
		status[5] |= LCD_FLAG_DFU_CAPABLE;
	}
	if (LCD_PROFILE_CONFIGURED) {
		status[6] = CONFIG_LCD_COLUMNS;
		status[7] = CONFIG_LCD_ROWS;
	}
	key = k_spin_lock(&status_lock);
	memcpy(latest_status, status, sizeof(status));
	k_spin_unlock(&status_lock, key);
}

#if defined(CONFIG_LCD_PCF8574_PROFILE) || defined(CONFIG_LCD_SSD1306_PROFILE)
static const struct device *const i2c_bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));
#if defined(CONFIG_LCD_PCF8574_PROFILE)
static const struct lcd_config profile = {
	.address = CONFIG_LCD_I2C_ADDRESS,
	.columns = CONFIG_LCD_COLUMNS,
	.rows = CONFIG_LCD_ROWS,
	.rs_bit = CONFIG_LCD_RS_BIT,
	.rw_bit = CONFIG_LCD_RW_BIT,
	.enable_bit = CONFIG_LCD_ENABLE_BIT,
	.backlight_bit = CONFIG_LCD_BACKLIGHT_BIT,
	.data_bits = {CONFIG_LCD_DB4_BIT, CONFIG_LCD_DB5_BIT, CONFIG_LCD_DB6_BIT, CONFIG_LCD_DB7_BIT},
	.backlight_active_high = IS_ENABLED(CONFIG_LCD_BACKLIGHT_ACTIVE_HIGH),
};
#endif

static int lcd_i2c_write(void *context, uint8_t address, const uint8_t *bytes, size_t length)
{
	return i2c_write(context, bytes, length, address);
}

static void lcd_delay(void *context, uint32_t microseconds)
{
	ARG_UNUSED(context);
	k_usleep(microseconds);
}
#endif

static int initialize_lcd(void)
{
#if defined(CONFIG_LCD_PCF8574_PROFILE) || defined(CONFIG_LCD_SSD1306_PROFILE)
	const struct lcd_bus bus = {
		.context = (void *)i2c_bus,
		.write = lcd_i2c_write,
		.delay_us = lcd_delay,
	};
	if (!device_is_ready(i2c_bus)) {
		return -ENODEV;
	}
#if defined(CONFIG_LCD_SSD1306_PROFILE)
	return oled_device_initialize(&oled, CONFIG_LCD_I2C_ADDRESS, &bus);
#else
	return lcd_device_initialize(&lcd, &profile, &bus);
#endif
#else
	return -ENOTSUP;
#endif
}

#if defined(CONFIG_LCD_LEGACY_DFU)
K_SEM_DEFINE(dfu_ack_sent, 0, 1);
static atomic_t dfu_notification_generation;

static void dfu_notify_complete(struct bt_conn *conn, void *user_data)
{
	ARG_UNUSED(conn);
	if ((uintptr_t)user_data == (uintptr_t)atomic_get(&dfu_notification_generation)) {
		k_sem_give(&dfu_ack_sent);
	}
}

static int enter_legacy_dfu(struct bt_conn *conn)
{
	struct bt_gatt_notify_params params = {
		.attr = &lcd_service.attrs[STATUS_ATTR_INDEX],
		.data = latest_status,
		.len = sizeof(latest_status),
		.func = dfu_notify_complete,
	};
	int rc;

	if (!bt_gatt_is_subscribed(conn, params.attr, BT_GATT_CCC_NOTIFY)) {
		return -EACCES;
	}
	/* A late callback from a timed-out attempt must not acknowledge a retry. */
	params.user_data = (void *)(uintptr_t)(atomic_inc(&dfu_notification_generation) + 1);
	k_sem_reset(&dfu_ack_sent);
	rc = bt_gatt_notify_cb(conn, &params);
	if (rc != 0 || k_sem_take(&dfu_ack_sent, K_SECONDS(2)) != 0) {
		return rc != 0 ? rc : -ETIMEDOUT;
	}
	/* Cold reset lets the preserved bootloader initialize its own S140 stack. */
	NRF_POWER->GPREGRET = 0xa8;
	NVIC_SystemReset();
	return -EIO;
}
#endif

static uint8_t apply_command(const struct lcd_command *command)
{
	int rc;

	if (command->opcode == LCD_OP_STATUS) {
		return LCD_RESULT_OK;
	}
	if (command->opcode == LCD_OP_ENTER_DFU) {
		return IS_ENABLED(CONFIG_LCD_LEGACY_DFU) ? LCD_RESULT_OK : LCD_RESULT_DFU_DISABLED;
	}
	if (!LCD_PROFILE_CONFIGURED) {
		return LCD_RESULT_UNCONFIGURED;
	}
	/* No implicit clear/reinitialize on an I2C error: preserve truthful status. */
	if (!display_ready()) {
		return LCD_RESULT_IO;
	}
	switch (command->opcode) {
	case LCD_OP_CLEAR:
#if defined(CONFIG_LCD_SSD1306_PROFILE)
		rc = oled_device_clear(&oled);
#else
		rc = lcd_device_clear(&lcd);
#endif
		break;
	case LCD_OP_WRITE:
#if defined(CONFIG_LCD_SSD1306_PROFILE)
		rc = oled_device_write(&oled, command->row, command->column,
				       command->text, command->length);
#else
		rc = lcd_device_write(&lcd, command->row, command->column,
				      command->text, command->length);
#endif
		break;
	case LCD_OP_BACKLIGHT:
#if defined(CONFIG_LCD_SSD1306_PROFILE)
		rc = oled_device_display(&oled, command->backlight != 0);
#else
		rc = lcd_device_backlight(&lcd, command->backlight != 0);
#endif
		break;
	default:
		return LCD_RESULT_INVALID;
	}
	return rc == 0 ? LCD_RESULT_OK : LCD_RESULT_IO;
}

int main(void)
{
	struct pending_command item;
	int rc = initialize_lcd();

	printk("NU40 BLE LCD v1; legacy 0x26000..0xDC000; LCD profile=%s init=%d DFU=%s\n",
	       LCD_PROFILE_CONFIGURED ? "configured" : "unconfigured", rc,
	       IS_ENABLED(CONFIG_LCD_LEGACY_DFU) ? "enabled" : "disabled");
	update_status(LCD_OP_STATUS, 0, rc == 0 ? LCD_RESULT_OK :
		(LCD_PROFILE_CONFIGURED ? LCD_RESULT_IO : LCD_RESULT_UNCONFIGURED));
	rc = bt_enable(NULL);
	if (rc != 0) {
		printk("LCD BLE initialization error=%d\n", rc);
		return rc;
	}
	k_work_submit(&advertising_restart);
	while (true) {
		struct bt_conn_info info;
		uint8_t result;

		k_msgq_get(&command_queue, &item, K_FOREVER);
		if (item.generation != atomic_get(&connection_generation) ||
		    bt_conn_get_info(item.connection, &info) != 0 ||
		    info.state != BT_CONN_STATE_CONNECTED) {
			bt_conn_unref(item.connection);
			continue;
		}
		result = apply_command(&item.command);
		update_status(item.command.opcode, item.command.sequence, result);
#if defined(CONFIG_LCD_LEGACY_DFU)
		if (item.command.opcode == LCD_OP_ENTER_DFU && result == LCD_RESULT_OK) {
			rc = enter_legacy_dfu(item.connection);
			printk("LCD DFU entry aborted error=%d\n", rc);
			update_status(item.command.opcode, item.command.sequence, LCD_RESULT_IO);
		}
#endif
		rc = bt_gatt_notify(item.connection, &lcd_service.attrs[STATUS_ATTR_INDEX],
				    latest_status, sizeof(latest_status));
		printk("LCD applied seq=%u op=%u result=%u notify=%d\n",
		       item.command.sequence, item.command.opcode, latest_status[4], rc);
		bt_conn_unref(item.connection);
	}
	return 0;
}
