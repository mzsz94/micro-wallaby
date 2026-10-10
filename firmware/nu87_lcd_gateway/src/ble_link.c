/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include "gateway_link.h"

LOG_MODULE_REGISTER(gateway_ble, CONFIG_GATEWAY_LOG_LEVEL);
/* Ameba wifi_init() starts an asynchronous vendor initialization task. Its
 * HCI pre-enable hook requires station/AP power-up, before any association. */
extern int wifi_is_running(unsigned char wlan_idx);
#define UUID_ENCODE_PARTS(...) BT_UUID_128_ENCODE(__VA_ARGS__)
#define DFU_UUID_PARTS(x) x, 0x1212, 0xefde, 0x1523, 0x785feabcd123
static struct bt_uuid_128 lcd_service = BT_UUID_INIT_128(UUID_ENCODE_PARTS(LCD_SERVICE_UUID_PARTS));
static struct bt_uuid_128 lcd_command = BT_UUID_INIT_128(UUID_ENCODE_PARTS(LCD_COMMAND_UUID_PARTS));
static struct bt_uuid_128 lcd_status = BT_UUID_INIT_128(UUID_ENCODE_PARTS(LCD_STATUS_UUID_PARTS));
static struct bt_uuid_128 dfu_service = BT_UUID_INIT_128(UUID_ENCODE_PARTS(DFU_UUID_PARTS(0x00001530)));
static struct bt_uuid_128 dfu_control = BT_UUID_INIT_128(UUID_ENCODE_PARTS(DFU_UUID_PARTS(0x00001531)));
static struct bt_uuid_128 dfu_packet = BT_UUID_INIT_128(UUID_ENCODE_PARTS(DFU_UUID_PARTS(0x00001532)));
static struct bt_uuid_128 dfu_revision = BT_UUID_INIT_128(UUID_ENCODE_PARTS(DFU_UUID_PARTS(0x00001534)));

K_MUTEX_DEFINE(link_lock);
K_MUTEX_DEFINE(operation_lock);
K_SEM_DEFINE(command_done, 0, 1);
K_SEM_DEFINE(write_done, 0, 1);
K_SEM_DEFINE(packet_done, 0, 1);
K_SEM_DEFINE(link_ready, 0, 1);
static struct gateway_state state;
static struct bt_conn *peer;
static bt_addr_le_t lcd_address, boot_address;
static bool want_dfu, link_is_dfu, dfu_enter_confirmed;
static bool pending_command, write_inflight, packet_pending;
static uint8_t command_result[LCD_STATUS_SIZE], expected_opcode;
static uint16_t expected_sequence, last_sequence;
static int command_error, write_error, packet_error;
static uint32_t generation;
static uint16_t service_end, command_handle, notify_handle, packet_handle, revision_handle;
static gateway_dfu_notify_fn dfu_callback;
static void *dfu_callback_user;
static struct bt_gatt_discover_params discovery, ccc_discovery;
static struct bt_gatt_subscribe_params subscription;
static struct bt_gatt_read_params revision_read;
static struct bt_gatt_write_params write_params;
static uint8_t write_buffer[LCD_COMMAND_MAX_SIZE];
static void scan_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(scan_work, scan_work_handler);

static void set_error(int error)
{
	k_mutex_lock(&link_lock, K_FOREVER);
	state.last_error = error;
	k_mutex_unlock(&link_lock);
}

static void fail_link(struct bt_conn *conn, int error)
{
	k_mutex_lock(&link_lock, K_FOREVER);
	state.last_error = error;
	if (peer == conn) {
		state.lcd_ready = false;
		state.dfu_ready = false;
	}
	k_mutex_unlock(&link_lock);
	(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

static uint8_t notification(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
			    const void *data, uint16_t length)
{
	gateway_dfu_notify_fn callback = NULL;
	void *user = NULL;
	const uint8_t *bytes = data;
	ARG_UNUSED(params);
	k_mutex_lock(&link_lock, K_FOREVER);
	if (conn != peer) {
		k_mutex_unlock(&link_lock);
		return BT_GATT_ITER_STOP;
	}
	if (data == NULL) {
		state.lcd_ready = false;
		state.dfu_ready = false;
		k_mutex_unlock(&link_lock);
		return BT_GATT_ITER_STOP;
	}
	if (link_is_dfu && state.dfu_ready && dfu_enter_confirmed) {
		callback = dfu_callback;
		user = dfu_callback_user;
	} else if (!link_is_dfu && pending_command && length == LCD_STATUS_SIZE &&
		   bytes[0] == LCD_PROTOCOL_VERSION && bytes[1] == expected_opcode &&
		   lcd_get_le16(bytes + 2) == expected_sequence) {
		memcpy(command_result, data, sizeof(command_result));
		command_error = 0;
		pending_command = false;
		k_sem_give(&command_done);
	}
	k_mutex_unlock(&link_lock);
	if (callback != NULL) {
		callback(data, length, user);
	}
	return BT_GATT_ITER_CONTINUE;
}

static void subscribed(struct bt_conn *conn, uint8_t err,
			struct bt_gatt_subscribe_params *params)
{
	ARG_UNUSED(params);
	k_mutex_lock(&link_lock, K_FOREVER);
	if (conn == peer && err == 0) {
		state.lcd_ready = !link_is_dfu;
		state.dfu_ready = link_is_dfu && dfu_enter_confirmed;
		state.last_error = 0;
		k_sem_give(&link_ready);
	}
	k_mutex_unlock(&link_lock);
	if (err != 0) {
		fail_link(conn, -EPROTO);
	}
}

static int subscribe_peer(struct bt_conn *conn)
{
	memset(&subscription, 0, sizeof(subscription));
	memset(&ccc_discovery, 0, sizeof(ccc_discovery));
	subscription.notify = notification;
	subscription.subscribe = subscribed;
	subscription.value_handle = notify_handle;
	subscription.ccc_handle = BT_GATT_AUTO_DISCOVER_CCC_HANDLE;
	subscription.end_handle = service_end;
	subscription.disc_params = &ccc_discovery;
	subscription.value = BT_GATT_CCC_NOTIFY;
	atomic_set_bit(subscription.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);
	return bt_gatt_subscribe(conn, &subscription);
}

static uint8_t revision_value(struct bt_conn *conn, uint8_t err,
			      struct bt_gatt_read_params *params,
			      const void *data, uint16_t length)
{
	ARG_UNUSED(params);
	/* Fail closed: the uploader implements precisely the 0x0008 protocol. */
	if (err != 0 || data == NULL || length != 2 || sys_get_le16(data) != 0x0008) {
		fail_link(conn, -EPROTONOSUPPORT);
		return BT_GATT_ITER_STOP;
	}
	int rc = subscribe_peer(conn);
	if (rc != 0) {
		fail_link(conn, rc);
	}
	return BT_GATT_ITER_STOP;
}

static uint8_t discover(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			struct bt_gatt_discover_params *params)
{
	int rc;
	if (params->type == BT_GATT_DISCOVER_PRIMARY) {
		if (attr == NULL) {
			fail_link(conn, -ENOENT);
			return BT_GATT_ITER_STOP;
		}
		const struct bt_gatt_service_val *svc = attr->user_data;
		service_end = svc->end_handle;
		params->uuid = NULL;
		params->start_handle = attr->handle + 1;
		params->end_handle = service_end;
		params->type = BT_GATT_DISCOVER_CHARACTERISTIC;
		rc = bt_gatt_discover(conn, params);
		if (rc != 0) { fail_link(conn, rc); }
		return BT_GATT_ITER_STOP;
	}
	if (attr != NULL) {
		const struct bt_gatt_chrc *ch = attr->user_data;
		if (bt_uuid_cmp(ch->uuid, link_is_dfu ? &dfu_control.uuid : &lcd_command.uuid) == 0 &&
		    (ch->properties & BT_GATT_CHRC_WRITE) != 0) {
			command_handle = ch->value_handle;
			if (link_is_dfu && (ch->properties & BT_GATT_CHRC_NOTIFY) != 0) {
				notify_handle = ch->value_handle;
			}
		} else if (!link_is_dfu && bt_uuid_cmp(ch->uuid, &lcd_status.uuid) == 0 &&
			   (ch->properties & BT_GATT_CHRC_NOTIFY) != 0) {
			notify_handle = ch->value_handle;
		} else if (link_is_dfu && bt_uuid_cmp(ch->uuid, &dfu_packet.uuid) == 0 &&
			   (ch->properties & BT_GATT_CHRC_WRITE_WITHOUT_RESP) != 0) {
			packet_handle = ch->value_handle;
		} else if (link_is_dfu && bt_uuid_cmp(ch->uuid, &dfu_revision.uuid) == 0 &&
			   (ch->properties & BT_GATT_CHRC_READ) != 0) {
			revision_handle = ch->value_handle;
		}
		return BT_GATT_ITER_CONTINUE;
	}
	if (command_handle == 0 || notify_handle == 0 ||
	    (link_is_dfu && (packet_handle == 0 || revision_handle == 0))) {
		fail_link(conn, -ENOENT);
		return BT_GATT_ITER_STOP;
	}
	if (link_is_dfu) {
		memset(&revision_read, 0, sizeof(revision_read));
		revision_read.func = revision_value;
		revision_read.handle_count = 1;
		revision_read.single.handle = revision_handle;
		rc = bt_gatt_read(conn, &revision_read);
	} else {
		rc = subscribe_peer(conn);
	}
	if (rc != 0) { fail_link(conn, rc); }
	return BT_GATT_ITER_STOP;
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err != 0) {
		k_mutex_lock(&link_lock, K_FOREVER);
		if (peer == conn) { bt_conn_unref(peer); peer = NULL; }
		state.last_error = -ECONNREFUSED;
		k_mutex_unlock(&link_lock);
		k_work_reschedule(&scan_work, K_SECONDS(2));
		return;
	}
	k_mutex_lock(&link_lock, K_FOREVER);
	if (peer != conn || bt_addr_le_cmp(bt_conn_get_dst(conn),
					 link_is_dfu ? &boot_address : &lcd_address) != 0) {
		k_mutex_unlock(&link_lock);
		fail_link(conn, -EACCES);
		return;
	}
	state.ble_connected = true;
	bt_addr_le_to_str(bt_conn_get_dst(conn), state.peer_addr, sizeof(state.peer_addr));
	command_handle = notify_handle = packet_handle = revision_handle = 0;
	memset(&discovery, 0, sizeof(discovery));
	discovery.uuid = link_is_dfu ? &dfu_service.uuid : &lcd_service.uuid;
	discovery.func = discover;
	discovery.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	discovery.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	discovery.type = BT_GATT_DISCOVER_PRIMARY;
	k_mutex_unlock(&link_lock);
	int rc = bt_gatt_discover(conn, &discovery);
	if (rc != 0) { fail_link(conn, rc); }
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(reason);
	k_mutex_lock(&link_lock, K_FOREVER);
	if (conn == peer) {
		state.ble_connected = state.lcd_ready = state.dfu_ready = false;
		state.last_error = -ENOTCONN;
		/* Never silently resume an interrupted legacy DFU session. */
		if (link_is_dfu) {
			dfu_enter_confirmed = false;
			want_dfu = false;
		}
		/* A valid completion received before reboot remains authoritative. */
		if (pending_command) {
			pending_command = false;
			command_error = -ENOTCONN;
			k_sem_give(&command_done);
		}
		if (packet_pending) {
			packet_pending = false;
			packet_error = -ENOTCONN;
			k_sem_give(&packet_done);
		}
		bt_conn_unref(peer);
		peer = NULL;
		generation++;
	}
	k_mutex_unlock(&link_lock);
	/* Delay prevents reusing discovery/subscription storage in callbacks from
	 * the just-closed ATT connection. Connection pool recycling is also needed. */
	k_work_reschedule(&scan_work, K_SECONDS(1));
}

BT_CONN_CB_DEFINE(gateway_connections) = {
	.connected = connected,
	.disconnected = disconnected,
};

static void scanned(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
		    struct net_buf_simple *ad)
{
	ARG_UNUSED(rssi); ARG_UNUSED(ad);
	if (type != BT_GAP_ADV_TYPE_ADV_IND && type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND) { return; }
	k_mutex_lock(&link_lock, K_FOREVER);
	if (peer != NULL || bt_addr_le_cmp(addr, want_dfu ? &boot_address : &lcd_address) != 0) {
		k_mutex_unlock(&link_lock);
		return;
	}
	link_is_dfu = want_dfu;
	int rc = bt_le_scan_stop();
	if (rc == 0) {
		rc = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, BT_LE_CONN_PARAM_DEFAULT, &peer);
	}
	if (rc != 0) { state.last_error = rc; }
	k_mutex_unlock(&link_lock);
	if (rc != 0) { k_work_reschedule(&scan_work, K_SECONDS(2)); }
}

static void scan_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	k_mutex_lock(&link_lock, K_FOREVER);
	bool can_scan = state.ble_configured && peer == NULL;
	k_mutex_unlock(&link_lock);
	if (can_scan) {
		int rc = bt_le_scan_start(BT_LE_SCAN_PASSIVE, scanned);
		if (rc != 0 && rc != -EALREADY) {
			set_error(rc);
			k_work_reschedule(&scan_work, K_SECONDS(2));
		}
	}
}

static void written(struct bt_conn *conn, uint8_t err, struct bt_gatt_write_params *params)
{
	ARG_UNUSED(params);
	k_mutex_lock(&link_lock, K_FOREVER);
	write_error = err == 0 ? 0 : (conn != peer ? -ENOTCONN : -EIO);
	write_inflight = false;
	k_sem_give(&write_done);
	k_mutex_unlock(&link_lock);
}

/* operation_lock held. Storage remains valid through cancellation/completion. */
static int write_request(struct bt_conn *conn, uint16_t handle,
			 const uint8_t *data, size_t len, int timeout_ms)
{
	k_mutex_lock(&link_lock, K_FOREVER);
	if (write_inflight) { k_mutex_unlock(&link_lock); return -EBUSY; }
	k_sem_reset(&write_done);
	memcpy(write_buffer, data, len);
	write_params = (struct bt_gatt_write_params) {
		.func = written, .handle = handle, .data = write_buffer, .length = len,
	};
	write_error = -EINPROGRESS;
	write_inflight = true;
	k_mutex_unlock(&link_lock);
	int rc = bt_gatt_write(conn, &write_params);
	if (rc != 0) {
		k_mutex_lock(&link_lock, K_FOREVER);
		write_inflight = false;
		k_mutex_unlock(&link_lock);
		return rc;
	}
	rc = k_sem_take(&write_done, K_MSEC(timeout_ms));
	if (rc != 0) {
		bt_gatt_cancel(conn, &write_params);
		fail_link(conn, -ETIMEDOUT);
		return -ETIMEDOUT;
	}
	k_mutex_lock(&link_lock, K_FOREVER);
	rc = write_error;
	k_mutex_unlock(&link_lock);
	return rc;
}

static int lcd_command_locked(const uint8_t *data, size_t len,
			      uint8_t status[LCD_STATUS_SIZE], int timeout_ms)
{
	if (data == NULL || status == NULL || len < LCD_COMMAND_HEADER_SIZE ||
	    len > LCD_COMMAND_MAX_SIZE || data[0] != LCD_PROTOCOL_VERSION || timeout_ms <= 0) {
		return -EINVAL;
	}
	k_mutex_lock(&link_lock, K_FOREVER);
	if (!state.lcd_ready || peer == NULL || link_is_dfu) {
		k_mutex_unlock(&link_lock); return -ENOTCONN;
	}
	struct bt_conn *conn = bt_conn_ref(peer);
	uint16_t handle = command_handle;
	k_sem_reset(&command_done);
	expected_opcode = data[1];
	expected_sequence = lcd_get_le16(data + 2);
	last_sequence = expected_sequence;
	pending_command = true;
	command_error = -EINPROGRESS;
	k_mutex_unlock(&link_lock);
	int64_t deadline = k_uptime_get() + timeout_ms;
	int rc = write_request(conn, handle, data, len, timeout_ms);
	if (rc == 0) {
		int64_t remaining = deadline - k_uptime_get();
		rc = k_sem_take(&command_done, K_MSEC(MAX(remaining, 0)));
		if (rc != 0) { rc = -ETIMEDOUT; }
		else {
			k_mutex_lock(&link_lock, K_FOREVER);
			rc = command_error;
			if (rc == 0) { memcpy(status, command_result, LCD_STATUS_SIZE); }
			k_mutex_unlock(&link_lock);
		}
	}
	k_mutex_lock(&link_lock, K_FOREVER);
	pending_command = false;
	state.last_error = rc;
	k_mutex_unlock(&link_lock);
	bt_conn_unref(conn);
	return rc;
}

int gateway_lcd_command(const uint8_t *data, size_t len,
			uint8_t status[LCD_STATUS_SIZE], int timeout_ms)
{
	if (k_mutex_lock(&operation_lock, K_NO_WAIT) != 0) { return -EBUSY; }
	k_mutex_lock(&link_lock, K_FOREVER);
	bool busy = want_dfu;
	k_mutex_unlock(&link_lock);
	int rc = busy ? -EBUSY : lcd_command_locked(data, len, status, timeout_ms);
	k_mutex_unlock(&operation_lock);
	return rc;
}

int gateway_dfu_enter(void)
{
	if (k_mutex_lock(&operation_lock, K_NO_WAIT) != 0) { return -EBUSY; }
	uint8_t cmd[8] = {LCD_PROTOCOL_VERSION, LCD_OP_ENTER_DFU, 0, 0, 'D', 'F', 'U', '!'};
	uint8_t status[LCD_STATUS_SIZE];
	k_mutex_lock(&link_lock, K_FOREVER);
	if (want_dfu) { k_mutex_unlock(&link_lock); k_mutex_unlock(&operation_lock); return -EBUSY; }
	lcd_put_le16(cmd + 2, last_sequence + 1U);
	want_dfu = true;
	dfu_enter_confirmed = false;
	k_sem_reset(&link_ready);
	k_mutex_unlock(&link_lock);
	int rc = lcd_command_locked(cmd, sizeof(cmd), status, 5000);
	if (rc == 0 && status[4] != LCD_RESULT_OK) { rc = -EACCES; }
	if (rc == 0) {
		k_mutex_lock(&link_lock, K_FOREVER);
		dfu_enter_confirmed = true;
		k_mutex_unlock(&link_lock);
		rc = k_sem_take(&link_ready, K_SECONDS(30));
		if (rc != 0) { rc = -ETIMEDOUT; }
		k_mutex_lock(&link_lock, K_FOREVER);
		if (rc == 0 && !state.dfu_ready) { rc = -ENOTCONN; }
		k_mutex_unlock(&link_lock);
	}
	k_mutex_unlock(&operation_lock);
	if (rc != 0) { gateway_dfu_leave(); }
	return rc;
}

int gateway_dfu_control(const uint8_t *data, size_t len)
{
	if (data == NULL || len == 0 || len > 20) { return -EINVAL; }
	if (k_mutex_lock(&operation_lock, K_NO_WAIT) != 0) { return -EBUSY; }
	k_mutex_lock(&link_lock, K_FOREVER);
	struct bt_conn *conn = (state.dfu_ready && peer != NULL) ? bt_conn_ref(peer) : NULL;
	uint16_t handle = command_handle;
	k_mutex_unlock(&link_lock);
	int rc = conn == NULL ? -ENOTCONN : write_request(conn, handle, data, len, 5000);
	if (conn != NULL) { bt_conn_unref(conn); }
	k_mutex_unlock(&operation_lock);
	return rc;
}

static void packet_sent(struct bt_conn *conn, void *user)
{
	k_mutex_lock(&link_lock, K_FOREVER);
	if (conn == peer && generation == (uint32_t)(uintptr_t)user) {
		packet_error = 0;
		packet_pending = false;
		k_sem_give(&packet_done);
	}
	k_mutex_unlock(&link_lock);
}

int gateway_dfu_packet(const uint8_t *data, size_t len)
{
	if (data == NULL || len == 0 || len > 20) { return -EINVAL; }
	if (k_mutex_lock(&operation_lock, K_NO_WAIT) != 0) { return -EBUSY; }
	k_mutex_lock(&link_lock, K_FOREVER);
	struct bt_conn *conn = (state.dfu_ready && peer != NULL) ? bt_conn_ref(peer) : NULL;
	uint16_t handle = packet_handle;
	uint32_t current_generation = generation;
	packet_error = -EINPROGRESS;
	packet_pending = true;
	k_sem_reset(&packet_done);
	k_mutex_unlock(&link_lock);
	int rc = conn == NULL ? -ENOTCONN : bt_gatt_write_without_response_cb(conn, handle, data, len,
				 false, packet_sent, (void *)(uintptr_t)current_generation);
	if (rc == 0) {
		rc = k_sem_take(&packet_done, K_SECONDS(5));
		if (rc != 0) { rc = -ETIMEDOUT; fail_link(conn, rc); }
		else {
			k_mutex_lock(&link_lock, K_FOREVER);
			rc = packet_error;
			k_mutex_unlock(&link_lock);
		}
	}
	k_mutex_lock(&link_lock, K_FOREVER);
	packet_pending = false;
	k_mutex_unlock(&link_lock);
	if (conn != NULL) { bt_conn_unref(conn); }
	k_mutex_unlock(&operation_lock);
	return rc;
}

void gateway_dfu_set_notify_callback(gateway_dfu_notify_fn callback, void *user)
{
	k_mutex_lock(&link_lock, K_FOREVER);
	dfu_callback = callback;
	dfu_callback_user = user;
	k_mutex_unlock(&link_lock);
}

void gateway_dfu_leave(void)
{
	k_mutex_lock(&operation_lock, K_FOREVER);
	k_mutex_lock(&link_lock, K_FOREVER);
	want_dfu = false;
	dfu_enter_confirmed = false;
	state.dfu_ready = false;
	struct bt_conn *conn = peer != NULL && link_is_dfu ? bt_conn_ref(peer) : NULL;
	k_mutex_unlock(&link_lock);
	if (conn != NULL) {
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(conn);
	}
	(void)bt_le_scan_stop();
	k_work_reschedule(&scan_work, K_SECONDS(1));
	k_mutex_unlock(&operation_lock);
}

void gateway_get_state(struct gateway_state *snapshot)
{
	if (snapshot == NULL) { return; }
	k_mutex_lock(&link_lock, K_FOREVER);
	*snapshot = state;
	k_mutex_unlock(&link_lock);
	snapshot->wifi_connected = gateway_wifi_address(snapshot->wifi_ipv4,
					 sizeof(snapshot->wifi_ipv4)) >= 0;
}

int gateway_radio_init(void)
{
	int rc = gateway_wifi_start();
	if (rc != 0 && strlen(CONFIG_GATEWAY_WIFI_SSID) != 0) { return rc; }
	if (strlen(CONFIG_GATEWAY_PEER_ADDRESS) == 0) {
		LOG_WRN("BLE commands disabled: explicit NU40 peer address is unconfigured");
		return 0;
	}
	rc = bt_addr_le_from_str(CONFIG_GATEWAY_PEER_ADDRESS, CONFIG_GATEWAY_PEER_ADDRESS_TYPE,
				&lcd_address);
	if (rc != 0) { return rc; }
	boot_address = lcd_address;
	if (strlen(CONFIG_GATEWAY_DFU_ADDRESS) != 0) {
		rc = bt_addr_le_from_str(CONFIG_GATEWAY_DFU_ADDRESS, CONFIG_GATEWAY_DFU_ADDRESS_TYPE,
					&boot_address);
		if (rc != 0) { return rc; }
	}
	if (!bt_addr_le_is_identity(&lcd_address) || !bt_addr_le_is_identity(&boot_address)) {
		return -EINVAL;
	}
	int64_t radio_deadline = k_uptime_get() + 30000;
	while (!wifi_is_running(0)) {
		if (k_uptime_get() >= radio_deadline) { set_error(-ETIMEDOUT); return -ETIMEDOUT; }
		k_sleep(K_MSEC(100));
	}
	rc = bt_enable(NULL);
	if (rc != 0) { set_error(rc); return rc; }
	k_mutex_lock(&link_lock, K_FOREVER);
	state.ble_configured = true;
	k_mutex_unlock(&link_lock);
	k_work_reschedule(&scan_work, K_NO_WAIT);
	return 0;
}
