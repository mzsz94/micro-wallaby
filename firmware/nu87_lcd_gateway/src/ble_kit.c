/* SPDX-License-Identifier: Apache-2.0 */
/* NU40-KIT-DISPLAY adapter. The selected identity is RAM-only; boot never
 * connects to an arbitrary advertiser. RX callbacks only copy state and wake
 * threads. GATT setup and connection management run on a dedicated thread. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>
#include "gateway_link.h"
#include "gateway_kit_protocol.h"
#include "gateway_audio.h"
#include "gateway_smp.h"
#include "gateway_alarm.h"
#include "gateway_robot.h"

LOG_MODULE_REGISTER(gateway_kit, CONFIG_GATEWAY_LOG_LEVEL);
extern int wifi_is_running(unsigned char wlan_idx);
#define KIT_NAME "NU40-KIT-DISPLAY"
#define KIT_CANDIDATES 4
#define KIT_FRESH_MS 15000
#define KIT_SETUP_MS 5000
#define KIT_FRAGMENT_MS 1900
static struct bt_uuid_128 service_uuid = BT_UUID_INIT_128(BLE_DISPLAY_UUID_SERVICE_VAL);
static struct bt_uuid_128 text_uuid = BT_UUID_INIT_128(BLE_DISPLAY_UUID_TEXT_VAL);
static struct bt_uuid_128 ack_uuid = BT_UUID_INIT_128(BLE_DISPLAY_UUID_ACK_VAL);
static struct bt_uuid_128 audio_ctrl_uuid = BT_UUID_INIT_128(GATEWAY_AUDIO_CTRL_UUID_VAL);
static struct bt_uuid_128 audio_data_uuid = BT_UUID_INIT_128(GATEWAY_AUDIO_DATA_UUID_VAL);
static struct gateway_audio_ring audio_ring;
static struct bt_gatt_subscribe_params audio_subscription;
static struct bt_gatt_exchange_params audio_mtu_params;
static uint16_t audio_ctrl_handle, audio_data_handle, audio_end, audio_ccc_handle;
static bool audio_bad, audio_descriptor, audio_subscribe_active, audio_subscription_live, audio_mtu_active;
static int audio_setup_error;

struct candidate {
	bt_addr_le_t address;
	int64_t service_seen, name_seen, connectable_seen;
	int8_t rssi;
	bool used;
};
static struct candidate candidates[KIT_CANDIDATES];
/* Aggregate diagnostics contain no unrelated advertiser identifiers. Counters
 * saturate instead of wrapping, and callbacks never format or print logs. */
struct scan_counters {
	uint32_t total, adv_ind, scan_rsp, other, wifi_down, identity_reject;
	uint32_t service, name, merged, full_table, errors;
};
static struct scan_counters scan_counts;
static bool scan_active;
static uint8_t requested_scan_type = BT_LE_SCAN_TYPE_ACTIVE;
static uint8_t active_scan_type = BT_LE_SCAN_TYPE_ACTIVE;
static uint32_t scan_generation;
static struct k_spinlock lock;
static struct gateway_state state;
static bt_addr_le_t selected;
static bool start_requested, initialization_started, enabled, connecting, connect_done, closed = true;
static int connect_error;
static struct bt_conn *peer;
static struct bt_gatt_discover_params discovery;
static struct bt_gatt_subscribe_params subscription;
static struct bt_gatt_write_params write_params;
static uint8_t write_buffer[BLE_DISPLAY_FRAME_MAX];
static bool discovery_active, subscribe_active, subscription_live, write_active;
static int setup_error, write_error;
static uint16_t service_start, service_end, text_handle, ack_handle, ack_end, ccc_handle;
static bool command_pending, ack_received, final_chunk_submitted;
static uint16_t sequence, expected_sequence;
static int command_error;
static int64_t ack_time;
K_MUTEX_DEFINE(operation_lock);
K_SEM_DEFINE(link_event, 0, 1);
K_SEM_DEFINE(setup_done, 0, 1);
K_SEM_DEFINE(write_done, 0, 1);
K_SEM_DEFINE(command_done, 0, 1);
static void link_thread(void *, void *, void *);
K_THREAD_DEFINE(kit_thread, 4096, link_thread, NULL, NULL, NULL, 7, 0, 0);

static void set_error(int error)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	state.last_error = error;
	k_spin_unlock(&lock, key);
}

static void count_report(uint32_t *counter)
{
	if (*counter != UINT32_MAX) { (*counter)++; }
}

static void set_scan_active(bool active)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	scan_active = active;
	k_spin_unlock(&lock, key);
}

static void scan_error(int error)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	state.last_error = error;
	count_report(&scan_counts.errors);
	k_spin_unlock(&lock, key);
}

int gateway_scan_mode(const char *mode)
{
	if (mode == NULL) { return -EINVAL; }
	uint8_t type;
	if (strcmp(mode, "active") == 0) { type = BT_LE_SCAN_TYPE_ACTIVE; }
	else if (strcmp(mode, "passive") == 0) { type = BT_LE_SCAN_TYPE_PASSIVE; }
	else { return -EINVAL; }
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (peer != NULL || connecting) { k_spin_unlock(&lock, key); return -EBUSY; }
	/* Only the link thread operates the scanner. This remains a pending
	 * request until that thread confirms stopping the previous scan. */
	requested_scan_type = type;
	k_spin_unlock(&lock, key);
	k_sem_give(&link_event);
	return 0;
}

static bool fresh(const struct candidate *item, int64_t now)
{
	return item->used && item->service_seen != 0 && item->name_seen != 0 &&
	       item->connectable_seen != 0 && now - item->service_seen < KIT_FRESH_MS &&
	       now - item->name_seen < KIT_FRESH_MS &&
	       now - item->connectable_seen < KIT_FRESH_MS;
}

struct advertisement { bool service, name, wrong_name; };
static bool parse_ad(struct bt_data *data, void *user)
{
	struct advertisement *found = user;
	if (data->type == BT_DATA_UUID128_ALL || data->type == BT_DATA_UUID128_SOME) {
		for (size_t offset = 0; offset + 16 <= data->data_len; offset += 16) {
			if (memcmp(data->data + offset, service_uuid.val, 16) == 0) {
				found->service = true;
			}
		}
	} else if (data->type == BT_DATA_NAME_COMPLETE) {
		found->name = data->data_len == sizeof(KIT_NAME) - 1 &&
			memcmp(data->data, KIT_NAME, sizeof(KIT_NAME) - 1) == 0;
		found->wrong_name = !found->name;
	}
	return true;
}

static void scanned(const bt_addr_le_t *address, int8_t rssi, uint8_t type,
		    struct net_buf_simple *data)
{
	struct advertisement found = {0};
	k_spinlock_key_t key = k_spin_lock(&lock);
	count_report(&scan_counts.total);
	if (type == BT_GAP_ADV_TYPE_ADV_IND) { count_report(&scan_counts.adv_ind); }
	else if (type == BT_GAP_ADV_TYPE_SCAN_RSP) { count_report(&scan_counts.scan_rsp); }
	else { count_report(&scan_counts.other); }
	k_spin_unlock(&lock, key);
	/* This is an atomic readiness read; late queued scan events during a
	 * Wi-Fi outage must not refresh peer candidates while discovery pauses. */
	if (!gateway_wifi_ipv4_ready()) {
		key = k_spin_lock(&lock);
		count_report(&scan_counts.wifi_down);
		k_spin_unlock(&lock, key);
		return;
	}
	if (type != BT_GAP_ADV_TYPE_ADV_IND && type != BT_GAP_ADV_TYPE_SCAN_RSP) { return; }
	/* Parse before the identity check only to distinguish exact target
	 * sightings rejected by that check. Candidate acceptance is unchanged. */
	bt_data_parse(data, parse_ad, &found);
	key = k_spin_lock(&lock);
	if (found.service) { count_report(&scan_counts.service); }
	if (found.name) { count_report(&scan_counts.name); }
	/* Identity addresses only: the chosen identity must remain stable on
	 * reconnect. This peripheral uses an unbonded static random identity. */
	if ((address->type != BT_ADDR_LE_PUBLIC && address->type != BT_ADDR_LE_RANDOM) ||
	    !bt_addr_le_is_identity(address)) {
		count_report(&scan_counts.identity_reject);
		k_spin_unlock(&lock, key);
		return;
	}
	k_spin_unlock(&lock, key);
	if (!found.service && !found.name && !found.wrong_name) { return; }
	int64_t now = k_uptime_get();
	key = k_spin_lock(&lock);
	struct candidate *item = NULL, *available = NULL;
	for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
		if (candidates[i].used && bt_addr_le_cmp(address, &candidates[i].address) == 0) {
			item = &candidates[i];
			break;
		}
		if (!candidates[i].used ||
		    now - MAX(candidates[i].service_seen, candidates[i].name_seen) >= KIT_FRESH_MS) {
			available = &candidates[i];
		}
	}
	if (item == NULL && (found.service || found.name)) {
		item = available;
		if (item != NULL) { memset(item, 0, sizeof(*item)); item->address = *address; item->used = true; }
		else { count_report(&scan_counts.full_table); }
	}
	if (item != NULL) {
		bool was_fresh = fresh(item, now);
		if (found.service) { item->service_seen = now; }
		if (found.name) { item->name_seen = now; }
		if (found.wrong_name) { item->name_seen = 0; }
		if (type == BT_GAP_ADV_TYPE_ADV_IND) { item->connectable_seen = now; }
		item->rssi = rssi;
		if (!was_fresh && fresh(item, now)) { count_report(&scan_counts.merged); }
	}
	k_spin_unlock(&lock, key);
	k_sem_give(&link_event);
}

int gateway_scan_diagnostics(char *json, size_t capacity)
{
	if (json == NULL || capacity == 0) { return -EINVAL; }
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct scan_counters snapshot = scan_counts;
	bool is_enabled = enabled, has_started = initialization_started, is_scanning = scan_active;
	uint8_t requested_type = requested_scan_type, active_type = active_scan_type;
	uint32_t generation = scan_generation;
	k_spin_unlock(&lock, key);
	int count = snprintf(json, capacity,
		"{\"enabled\":%s,\"initializationStarted\":%s,\"scanning\":%s,"
		"\"requestedMode\":\"%s\",\"activeMode\":\"%s\",\"generation\":%lu,\"scanErrors\":%lu,"
		"\"total\":%lu,\"advInd\":%lu,\"scanRsp\":%lu,\"other\":%lu,"
		"\"wifiDown\":%lu,\"identityReject\":%lu,\"service\":%lu,\"name\":%lu,"
		"\"merged\":%lu,\"fullTable\":%lu}",
		is_enabled ? "true" : "false", has_started ? "true" : "false", is_scanning ? "true" : "false",
		requested_type == BT_LE_SCAN_TYPE_ACTIVE ? "active" : "passive",
		!is_scanning ? "none" : active_type == BT_LE_SCAN_TYPE_ACTIVE ? "active" : "passive",
		(unsigned long)generation, (unsigned long)snapshot.errors,
		(unsigned long)snapshot.total, (unsigned long)snapshot.adv_ind,
		(unsigned long)snapshot.scan_rsp, (unsigned long)snapshot.other,
		(unsigned long)snapshot.wifi_down, (unsigned long)snapshot.identity_reject,
		(unsigned long)snapshot.service, (unsigned long)snapshot.name,
		(unsigned long)snapshot.merged, (unsigned long)snapshot.full_table);
	if (count < 0 || (size_t)count >= capacity) { json[0] = '\0'; return -ENOSPC; }
	return count;
}

int gateway_peer_candidates(char *json, size_t capacity)
{
	struct candidate snapshot[KIT_CANDIDATES];
	if (json == NULL || capacity < 3) { return -EINVAL; }
	k_spinlock_key_t key = k_spin_lock(&lock);
	memcpy(snapshot, candidates, sizeof(snapshot));
	k_spin_unlock(&lock, key);
	int64_t now = k_uptime_get();
	size_t used = 1;
	bool first = true;
	json[0] = '[';
	for (size_t i = 0; i < ARRAY_SIZE(snapshot); i++) {
		if (!fresh(&snapshot[i], now)) { continue; }
		char address[BT_ADDR_STR_LEN];
		bt_addr_to_str(&snapshot[i].address.a, address, sizeof(address));
		int count = snprintf(json + used, capacity - used,
			"%s{\"address\":\"%s\",\"type\":\"%s\",\"name\":\"" KIT_NAME "\",\"rssi\":%d}",
			first ? "" : ",", address,
			snapshot[i].address.type == BT_ADDR_LE_PUBLIC ? "public" : "random", snapshot[i].rssi);
		if (count < 0 || (size_t)count >= capacity - used) { json[0] = '\0'; return -ENOSPC; }
		used += count;
		first = false;
	}
	if (capacity - used < 2) { json[0] = '\0'; return -ENOSPC; }
	json[used++] = ']'; json[used] = '\0';
	return (int)used;
}

int gateway_select_peer(const char *address, const char *type)
{
	bt_addr_le_t requested;
	if (address == NULL || type == NULL ||
	    (strcmp(type, "random") != 0 && strcmp(type, "public") != 0) ||
	    bt_addr_le_from_str(address, type, &requested) != 0 || !bt_addr_le_is_identity(&requested)) {
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	int rc = -ENOENT;
	if (!enabled) { rc = -EAGAIN; }
	else if (peer != NULL || connecting) {
		rc = state.ble_configured && bt_addr_le_cmp(&selected, &requested) == 0 ? 0 : -EBUSY;
	} else {
		for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
			if (fresh(&candidates[i], k_uptime_get()) &&
			    bt_addr_le_cmp(&candidates[i].address, &requested) == 0) {
				selected = requested;
				state.ble_configured = true;
				state.last_error = 0;
				rc = 0;
				break;
			}
		}
	}
	k_spin_unlock(&lock, key);
	if (rc == 0) { k_sem_give(&link_event); }
	return rc;
}

static bool is_attempt(struct bt_conn *conn)
{
	return peer == conn || (connecting && peer == NULL &&
		bt_addr_le_cmp(bt_conn_get_dst(conn), &selected) == 0);
}

static void connected(struct bt_conn *conn, uint8_t error)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (is_attempt(conn)) {
		connect_done = true;
		connect_error = error == 0 ? 0 : -ECONNREFUSED;
		closed = error != 0;
	}
	k_spin_unlock(&lock, key);
	k_sem_give(&link_event);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(reason);
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (is_attempt(conn)) {
		closed = true;
		state.ble_connected = state.lcd_ready = false;
		state.last_error = -ENOTCONN;
		gateway_smp_disconnected(conn);
		gateway_alarm_disconnected(conn);
		(void)gateway_robot_command("stop");
		audio_ring.available = audio_ring.running = false;
		audio_ring.queued = 0; audio_ring.error = -ENOTCONN;
		if (command_pending && !ack_received) {
			command_error = -ENOTCONN;
			command_pending = false;
		}
		k_sem_give(&command_done);
		k_sem_give(&setup_done);
	}
	k_spin_unlock(&lock, key);
	k_sem_give(&link_event);
}
BT_CONN_CB_DEFINE(kit_connections) = { .connected = connected, .disconnected = disconnected };

static uint8_t discovered(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  struct bt_gatt_discover_params *params)
{
	uint8_t result = BT_GATT_ITER_CONTINUE;
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (conn != peer || closed) { attr = NULL; setup_error = -ENOTCONN; }
	if (attr == NULL) { discovery_active = false; result = BT_GATT_ITER_STOP; }
	else if (params->type == BT_GATT_DISCOVER_PRIMARY) {
		const struct bt_gatt_service_val *service = attr->user_data;
		service_start = attr->handle + 1;
		service_end = service->end_handle;
		discovery_active = false;
		result = BT_GATT_ITER_STOP;
	} else if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
		const struct bt_gatt_chrc *ch = attr->user_data;
		if (ack_handle != 0 && attr->handle > ack_handle && ack_end == service_end) {
			ack_end = attr->handle - 1;
		}
		if (audio_data_handle != 0 && attr->handle > audio_data_handle && audio_end == service_end) {
			audio_end = attr->handle - 1;
		}
		if (bt_uuid_cmp(ch->uuid, &audio_ctrl_uuid.uuid) == 0) {
			if (audio_ctrl_handle != 0 || !(ch->properties & BT_GATT_CHRC_WRITE)) { audio_bad = true; }
			audio_ctrl_handle = ch->value_handle;
		} else if (bt_uuid_cmp(ch->uuid, &audio_data_uuid.uuid) == 0) {
			if (audio_data_handle != 0 || !(ch->properties & BT_GATT_CHRC_NOTIFY)) { audio_bad = true; }
			audio_data_handle = ch->value_handle;
		} else if (bt_uuid_cmp(ch->uuid, &text_uuid.uuid) == 0) {
			if (text_handle != 0 || !(ch->properties & BT_GATT_CHRC_WRITE)) { setup_error = -EPROTO; }
			text_handle = ch->value_handle;
		} else if (bt_uuid_cmp(ch->uuid, &ack_uuid.uuid) == 0) {
			if (ack_handle != 0 || !(ch->properties & BT_GATT_CHRC_NOTIFY)) { setup_error = -EPROTO; }
			ack_handle = ch->value_handle;
		}
	} else if (params->type == BT_GATT_DISCOVER_DESCRIPTOR) {
		if (audio_descriptor) { audio_ccc_handle = attr->handle; }
		else { ccc_handle = attr->handle; }
		discovery_active = false;
		result = BT_GATT_ITER_STOP;
	}
	if (!discovery_active) { k_sem_give(&setup_done); }
	k_spin_unlock(&lock, key);
	return result;
}

static uint8_t notification(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
			    const void *data, uint16_t length)
{
	ARG_UNUSED(params);
	uint16_t incoming_sequence;
	uint8_t status;
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (conn != peer) { k_spin_unlock(&lock, key); return BT_GATT_ITER_STOP; }
	if (data == NULL) {
		subscription_live = false;
		subscribe_active = false;
		state.lcd_ready = false;
		k_spin_unlock(&lock, key);
		k_sem_give(&link_event);
		return BT_GATT_ITER_STOP;
	}
	if (command_pending && !ack_received && final_chunk_submitted &&
	    ble_display_parse_ack(data, length, &incoming_sequence, &status) &&
	    incoming_sequence == expected_sequence) {
		ack_received = true;
		ack_time = k_uptime_get();
		command_error = status == BLE_DISPLAY_OK ? 0 : status == BLE_DISPLAY_BAD_TEXT ? -EINVAL :
			status == BLE_DISPLAY_BUSY ? -EBUSY : -EIO;
		k_sem_give(&command_done);
	}
	k_spin_unlock(&lock, key);
	return BT_GATT_ITER_CONTINUE;
}

static void subscribed(struct bt_conn *conn, uint8_t error,
			struct bt_gatt_subscribe_params *params)
{
	ARG_UNUSED(params);
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (conn == peer) {
		subscribe_active = false;
		setup_error = closed ? -ENOTCONN : error == 0 ? 0 : -EPROTO;
		k_sem_give(&setup_done);
	}
	k_spin_unlock(&lock, key);
}

static int discover_range(struct bt_conn *conn, uint8_t type, const struct bt_uuid *uuid,
			  uint16_t start, uint16_t end)
{
	k_sem_reset(&setup_done);
	k_spinlock_key_t key = k_spin_lock(&lock);
	discovery = (struct bt_gatt_discover_params) {
		.uuid = uuid, .func = discovered, .start_handle = start, .end_handle = end, .type = type,
	};
	discovery_active = true;
	setup_error = 0;
	k_spin_unlock(&lock, key);
	int rc = bt_gatt_discover(conn, &discovery);
	if (rc != 0) {
		key = k_spin_lock(&lock); discovery_active = false; k_spin_unlock(&lock, key);
		return rc;
	}
	if (k_sem_take(&setup_done, K_MSEC(KIT_SETUP_MS)) != 0) { return -ETIMEDOUT; }
	key = k_spin_lock(&lock);
	rc = closed ? -ENOTCONN : setup_error;
	k_spin_unlock(&lock, key);
	return rc;
}

static uint8_t audio_notification(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
				 const void *data, uint16_t length)
{
	ARG_UNUSED(params);
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (conn != peer) { k_spin_unlock(&lock, key); return BT_GATT_ITER_STOP; }
	if (data == NULL) {
		audio_subscribe_active = audio_subscription_live = false;
		audio_ring.available = audio_ring.running = false;
		audio_ring.queued = 0; audio_ring.error = -ENOTCONN;
		k_spin_unlock(&lock, key);
		k_sem_give(&setup_done);
		return BT_GATT_ITER_STOP;
	}
	if (!closed && audio_subscription_live) { (void)gateway_audio_push(&audio_ring, data, length); }
	k_spin_unlock(&lock, key);
	return BT_GATT_ITER_CONTINUE;
}

static void audio_subscribed(struct bt_conn *conn, uint8_t error, struct bt_gatt_subscribe_params *params)
{
	ARG_UNUSED(params);
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (conn == peer) {
		audio_subscribe_active = false;
		audio_setup_error = closed ? -ENOTCONN : error == 0 ? 0 : -EPROTO;
		if (error != 0) { audio_subscription_live = false; }
		k_sem_give(&setup_done);
	}
	k_spin_unlock(&lock, key);
}

static void audio_mtu_exchanged(struct bt_conn *conn, uint8_t error, struct bt_gatt_exchange_params *params)
{
	ARG_UNUSED(params);
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (conn == peer) {
		audio_mtu_active = false;
		audio_setup_error = closed ? -ENOTCONN : error == 0 ? 0 : -EPROTO;
		k_sem_give(&setup_done);
	}
	k_spin_unlock(&lock, key);
}

/* Optional setup never changes LCD readiness. Missing audio characteristics,
 * insufficient MTU and subscribe failure leave the existing display usable. */
static int setup_audio(struct bt_conn *conn)
{
	if (audio_bad || audio_ctrl_handle == 0 || audio_data_handle == 0 || audio_data_handle >= audio_end) {
		return -ENOTSUP;
	}
	audio_descriptor = true;
	int rc = discover_range(conn, BT_GATT_DISCOVER_DESCRIPTOR, BT_UUID_GATT_CCC, audio_data_handle + 1, audio_end);
	if (rc != 0) { return rc; }
	if (audio_ccc_handle == 0) { return -ENOENT; }
	if (bt_gatt_get_mtu(conn) < GATEWAY_AUDIO_MIN_MTU) {
		k_sem_reset(&setup_done);
		audio_mtu_params.func = audio_mtu_exchanged;
		k_spinlock_key_t key = k_spin_lock(&lock);
		audio_setup_error = 0; audio_mtu_active = true;
		k_spin_unlock(&lock, key);
		rc = bt_gatt_exchange_mtu(conn, &audio_mtu_params);
		if (rc != 0) {
			key = k_spin_lock(&lock); audio_mtu_active = false; k_spin_unlock(&lock, key);
		} else if (k_sem_take(&setup_done, K_MSEC(KIT_SETUP_MS)) != 0) { return -ETIMEDOUT; }
		key = k_spin_lock(&lock);
		int error = audio_setup_error;
		k_spin_unlock(&lock, key);
		if (error != 0) { return error; }
	}
	if (bt_gatt_get_mtu(conn) < GATEWAY_AUDIO_MIN_MTU) { return -EMSGSIZE; }
	k_sem_reset(&setup_done);
	k_spinlock_key_t key = k_spin_lock(&lock);
	memset(&audio_subscription, 0, sizeof(audio_subscription));
	audio_subscription.notify = audio_notification;
	audio_subscription.subscribe = audio_subscribed;
	audio_subscription.value_handle = audio_data_handle;
	audio_subscription.ccc_handle = audio_ccc_handle;
	audio_subscription.value = BT_GATT_CCC_NOTIFY;
	atomic_set_bit(audio_subscription.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);
	audio_subscribe_active = audio_subscription_live = true;
	audio_setup_error = 0;
	k_spin_unlock(&lock, key);
	rc = bt_gatt_subscribe(conn, &audio_subscription);
	if (rc != 0) {
		key = k_spin_lock(&lock);
		audio_subscribe_active = audio_subscription_live = false;
		k_spin_unlock(&lock, key);
		return rc;
	}
	if (k_sem_take(&setup_done, K_MSEC(KIT_SETUP_MS)) != 0) { return -ETIMEDOUT; }
	key = k_spin_lock(&lock);
	rc = (closed || !audio_subscription_live) ? -ENOTCONN : audio_setup_error;
	audio_ring.available = rc == 0;
	k_spin_unlock(&lock, key);
	return rc;
}

static int setup_peer(struct bt_conn *conn)
{
	service_start = service_end = text_handle = ack_handle = ccc_handle = 0;
	audio_ctrl_handle = audio_data_handle = audio_ccc_handle = 0;
	audio_bad = audio_descriptor = false;
	k_spinlock_key_t audio_key = k_spin_lock(&lock);
	audio_ring.available = audio_ring.running = false;
	audio_ring.queued = 0; audio_ring.error = -ENOTSUP;
	k_spin_unlock(&lock, audio_key);
	int rc = discover_range(conn, BT_GATT_DISCOVER_PRIMARY, &service_uuid.uuid,
		BT_ATT_FIRST_ATTRIBUTE_HANDLE, BT_ATT_LAST_ATTRIBUTE_HANDLE);
	if (rc != 0) { return rc; }
	if (service_start == 0 || service_start > service_end) { return -ENOENT; }
	ack_end = audio_end = service_end;
	rc = discover_range(conn, BT_GATT_DISCOVER_CHARACTERISTIC, NULL, service_start, service_end);
	if (rc != 0) { return rc; }
	if (text_handle == 0 || ack_handle == 0 || ack_handle >= ack_end) { return -ENOENT; }
	rc = discover_range(conn, BT_GATT_DISCOVER_DESCRIPTOR, BT_UUID_GATT_CCC, ack_handle + 1, ack_end);
	if (rc != 0) { return rc; }
	if (ccc_handle == 0) { return -ENOENT; }
	k_sem_reset(&setup_done);
	k_spinlock_key_t key = k_spin_lock(&lock);
	memset(&subscription, 0, sizeof(subscription));
	subscription.notify = notification;
	subscription.subscribe = subscribed;
	subscription.value_handle = ack_handle;
	subscription.ccc_handle = ccc_handle;
	subscription.value = BT_GATT_CCC_NOTIFY;
	atomic_set_bit(subscription.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);
	subscribe_active = subscription_live = true;
	setup_error = 0;
	k_spin_unlock(&lock, key);
	rc = bt_gatt_subscribe(conn, &subscription);
	if (rc != 0) {
		key = k_spin_lock(&lock);
		subscribe_active = subscription_live = false;
		k_spin_unlock(&lock, key);
		return rc;
	}
	if (k_sem_take(&setup_done, K_MSEC(KIT_SETUP_MS)) != 0) { return -ETIMEDOUT; }
	key = k_spin_lock(&lock);
	rc = (closed || !subscription_live) ? -ENOTCONN : setup_error;
	if (rc == 0) { state.lcd_ready = true; state.last_error = 0; }
	k_spin_unlock(&lock, key);
	return rc;
}

/* Disconnect drains ATT requests and calls their normal error callbacks.
 * Do not use bt_gatt_cancel() here: this pinned SDK's request lookup dereferences
 * an idle channel's NULL request, including a race after a response completes.
 * Wait for callbacks before reusing static storage on a new connection. */
static bool cleanup_peer(struct bt_conn *conn)
{
	(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	int64_t deadline = k_uptime_get() + KIT_SETUP_MS;
	while (true) {
		k_spinlock_key_t key = k_spin_lock(&lock);
		bool done = closed && !discovery_active && !subscribe_active &&
			!subscription_live && !write_active && !audio_subscribe_active &&
			!audio_subscription_live && !audio_mtu_active && gateway_smp_drained() && gateway_alarm_drained();
		k_spin_unlock(&lock, key);
		if (done) { return true; }
		if (k_uptime_get() >= deadline) { return false; }
		k_sleep(K_MSEC(20));
	}
}

/* Used only between connections. Keep association/DHCP free of BLE scanning;
 * an established BLE link survives Wi-Fi loss. Waiting for IPv4 has no timeout
 * and does not consume another controller-initialization attempt. */
static bool schedule_discovery(bool *scanning)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool can_scan = enabled && peer == NULL && !connecting;
	uint8_t requested_type = requested_scan_type, active_type = active_scan_type;
	k_spin_unlock(&lock, key);
	if (!can_scan) { return false; }
	bool wifi_ready = gateway_wifi_ipv4_ready();
	if (!wifi_ready || (*scanning && requested_type != active_type)) {
		key = k_spin_lock(&lock);
		/* Require new sightings after an outage or mode transition. */
		memset(candidates, 0, sizeof(candidates));
		k_spin_unlock(&lock, key);
		if (*scanning) {
			int rc = bt_le_scan_stop();
			if (rc != 0 && rc != -EALREADY) {
				/* Keep the old mode/state until a later stop succeeds. Do
				 * not connect or begin a new scan on a failed stop. */
				scan_error(rc);
				return false;
			}
			*scanning = false;
			set_scan_active(false);
		}
	}
	if (!gateway_wifi_ipv4_ready()) { return false; }
	if (!*scanning) {
		/* A mode request may have changed while the stop command waited. */
		key = k_spin_lock(&lock);
		requested_type = requested_scan_type;
		k_spin_unlock(&lock, key);
		/* Repeat advertisements refresh the bounded selection window.
		 * The standard ACTIVE macro filters duplicates indefinitely. */
		int rc = bt_le_scan_start(BT_LE_SCAN_PARAM(requested_type, 0,
			BT_GAP_SCAN_FAST_INTERVAL, BT_GAP_SCAN_FAST_WINDOW), scanned);
		*scanning = rc == 0 || rc == -EALREADY;
		key = k_spin_lock(&lock);
		scan_active = *scanning;
		if (*scanning) {
			active_scan_type = requested_type;
			count_report(&scan_generation);
		}
		k_spin_unlock(&lock, key);
		if (!*scanning) { scan_error(rc); }
		else {
			LOG_INF("Wi-Fi ready; %s scanning NU40-KIT-DISPLAY",
				requested_type == BT_LE_SCAN_TYPE_ACTIVE ? "active" : "passive");
			gateway_radio_coex_diagnostics();
		}
	}
	/* A new request during scan-start must be handled before connecting. */
	key = k_spin_lock(&lock);
	bool current_mode = requested_scan_type == active_scan_type;
	k_spin_unlock(&lock, key);
	return *scanning && current_mode && gateway_wifi_ipv4_ready();
}

static void initialize_controller_when_ready(void)
{
	/* Delay the single HCI initialization until association and DHCP finish.
	 * The vendor HCI startup changes shared RF/coexistence state even without
	 * scanning. Waiting consumes no attempt and has no DHCP deadline. */
	if (!gateway_wifi_ipv4_ready() || !wifi_is_running(0)) { return; }
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool initialize = start_requested && !initialization_started;
	if (initialize) { initialization_started = true; }
	k_spin_unlock(&lock, key);
	if (!initialize) { return; }
	/* Vendor HCI initialization can block. Keep it off the main HTTP thread,
	 * and never start an overlapping or automatic second enable attempt. */
	LOG_INF("Starting NU87 Bluetooth controller after Wi-Fi IPv4 readiness");
	int auth_rc = gateway_smp_auth_init();
	if (auth_rc != 0) { set_error(auth_rc); return; }
	gateway_radio_diagnostics("before_bt_enable");
	int rc = bt_enable(NULL);
	gateway_radio_diagnostics("after_bt_enable");
	LOG_INF("Bluetooth controller initialization result %d", rc);
	if (rc != 0) { set_error(rc); return; }
	key = k_spin_lock(&lock); enabled = true; k_spin_unlock(&lock, key);
	LOG_INF("Bluetooth ready; Wi-Fi and explicit peer selection required");
}

static void link_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	bool scanning = false;
	for (;;) {
		(void)k_sem_take(&link_event, K_MSEC(500));
		initialize_controller_when_ready();
		if (!schedule_discovery(&scanning)) { continue; }
		k_spinlock_key_t key = k_spin_lock(&lock);
		bool target_seen = false;
		bt_addr_le_t target = selected;
		if (state.ble_configured && requested_scan_type == active_scan_type) {
			for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
				if (fresh(&candidates[i], k_uptime_get()) &&
				    bt_addr_le_cmp(&candidates[i].address, &target) == 0) { target_seen = true; }
			}
		}
		/* Reserve the chosen identity against concurrent selection changes. */
		if (target_seen) { connecting = true; connect_done = false; closed = false; }
		k_spin_unlock(&lock, key);
		if (!target_seen) { continue; }
		int stop_rc = scanning ? bt_le_scan_stop() : 0;
		if (stop_rc == 0 || stop_rc == -EALREADY) { scanning = false; set_scan_active(false); }
		if (scanning || !gateway_wifi_ipv4_ready()) {
			key = k_spin_lock(&lock);
			connecting = false;
			closed = true;
			if (scanning) { state.last_error = stop_rc; count_report(&scan_counts.errors); }
			k_spin_unlock(&lock, key);
			continue;
		}
		struct bt_conn *conn = NULL;
		int rc = bt_conn_le_create(&target, BT_CONN_LE_CREATE_CONN, BT_LE_CONN_PARAM_DEFAULT, &conn);
		key = k_spin_lock(&lock);
		peer = conn;
		if (rc != 0) { peer = NULL; connecting = false; closed = true; state.last_error = rc; }
		k_spin_unlock(&lock, key);
		if (rc != 0) { if (conn != NULL) { bt_conn_unref(conn); } k_sleep(K_SECONDS(1)); continue; }
		int64_t deadline = k_uptime_get() + 12000;
		for (;;) {
			key = k_spin_lock(&lock);
			bool done = connect_done || closed;
			rc = closed ? -ENOTCONN : connect_error;
			k_spin_unlock(&lock, key);
			if (done) { break; }
			if (k_uptime_get() >= deadline) { rc = -ETIMEDOUT; break; }
			(void)k_sem_take(&link_event, K_MSEC(100));
		}
		key = k_spin_lock(&lock);
		connecting = false;
		if (rc == 0 && !closed) {
			state.ble_connected = true;
			bt_addr_le_to_str(&target, state.peer_addr, sizeof(state.peer_addr));
		}
		k_spin_unlock(&lock, key);
		if (rc == 0) { rc = setup_peer(conn); }
		if (rc == 0) {
			LOG_INF("NU40-KIT-DISPLAY ready (selected peer, ACK subscribed)");
			int audio_rc = setup_audio(conn);
			key = k_spin_lock(&lock); audio_ring.error = audio_rc; k_spin_unlock(&lock, key);
			LOG_INF("Optional NU40 microphone setup result %d", audio_rc);
			int alarm_rc = gateway_alarm_prepare(conn);
			LOG_INF("Optional NU40 alarm setup result %d", alarm_rc);
			int smp_rc = gateway_smp_prepare(conn);
			LOG_INF("Optional NU40 SMP setup result %d", smp_rc);
		}
		while (rc == 0) {
			(void)k_sem_take(&link_event, K_MSEC(500));
			key = k_spin_lock(&lock);
			if (closed || !state.lcd_ready) { rc = -ENOTCONN; }
			k_spin_unlock(&lock, key);
		}
		set_error(rc);
		key = k_spin_lock(&lock); state.lcd_ready = false; k_spin_unlock(&lock, key);
		/* A command owns its connection reference and static write storage. */
		k_mutex_lock(&operation_lock, K_FOREVER);
		bool cleaned = cleanup_peer(conn);
		key = k_spin_lock(&lock);
		state.ble_connected = false;
		peer = NULL;
		if (!cleaned) { enabled = false; state.last_error = -ETIMEDOUT; }
		/* Require a new advertisement before reconnecting the same identity. */
		memset(candidates, 0, sizeof(candidates));
		k_spin_unlock(&lock, key);
		bt_conn_unref(conn);
		k_mutex_unlock(&operation_lock);
		if (!cleaned) { LOG_ERR("BLE cleanup incomplete; reboot required before another link"); }
		k_sleep(K_SECONDS(1));
	}
}

static void written(struct bt_conn *conn, uint8_t error, struct bt_gatt_write_params *params)
{
	ARG_UNUSED(params);
	k_spinlock_key_t key = k_spin_lock(&lock);
	write_error = (conn != peer || closed) ? -ENOTCONN : error == 0 ? 0 : -EIO;
	write_active = false;
	k_spin_unlock(&lock, key);
	k_sem_give(&write_done);
}

static int send_to_handle(struct bt_conn *conn, uint16_t handle, const uint8_t *frame, size_t length, int64_t deadline)
{
	int64_t remaining = deadline - k_uptime_get();
	if (remaining <= 0) { return -ETIMEDOUT; }
	k_sem_reset(&write_done);
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (closed || peer != conn) { k_spin_unlock(&lock, key); return -ENOTCONN; }
	if (write_active) { k_spin_unlock(&lock, key); return -EBUSY; }
	memcpy(write_buffer, frame, length);
	write_params = (struct bt_gatt_write_params) {
		.func = written, .handle = handle, .data = write_buffer, .length = length,
	};
	write_active = true;
	write_error = -EINPROGRESS;
	k_spin_unlock(&lock, key);
	int rc = bt_gatt_write(conn, &write_params);
	if (rc != 0) {
		key = k_spin_lock(&lock); write_active = false; k_spin_unlock(&lock, key);
		return rc;
	}
	remaining = deadline - k_uptime_get();
	if (k_sem_take(&write_done, K_MSEC(MAX(remaining, 0))) != 0) {
		key = k_spin_lock(&lock); state.lcd_ready = false; k_spin_unlock(&lock, key);
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		k_sem_give(&link_event);
		return -ETIMEDOUT;
	}
	key = k_spin_lock(&lock); rc = write_error; k_spin_unlock(&lock, key);
	return rc;
}

int gateway_display_text(const uint8_t *text, size_t length, int timeout_ms)
{
	char lines[BLE_DISPLAY_ROWS][BLE_DISPLAY_COLUMNS + 1];
	if (timeout_ms <= 0 || !ble_display_split_lines((const char *)text, length, lines)) { return -EINVAL; }
	if (k_mutex_lock(&operation_lock, K_NO_WAIT) != 0) { return -EBUSY; }
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (peer == NULL || closed || !state.lcd_ready) {
		k_spin_unlock(&lock, key); k_mutex_unlock(&operation_lock); return -ENOTCONN;
	}
	if (write_active) { k_spin_unlock(&lock, key); k_mutex_unlock(&operation_lock); return -EBUSY; }
	struct bt_conn *conn = bt_conn_ref(peer);
	uint16_t current_sequence = ++sequence;
	expected_sequence = current_sequence;
	command_pending = true;
	ack_received = final_chunk_submitted = false;
	command_error = -EINPROGRESS;
	k_sem_reset(&command_done);
	k_spin_unlock(&lock, key);
	int64_t start = k_uptime_get();
	int64_t fragment_deadline = start + MIN(timeout_ms, KIT_FRAGMENT_MS);
	int64_t deadline = start + timeout_ms;
	int rc = 0;
	for (uint8_t chunk = 0; (size_t)chunk * BLE_DISPLAY_CHUNK < length; chunk++) {
		uint8_t frame[BLE_DISPLAY_FRAME_MAX];
		size_t frame_length = ble_display_build_text((const char *)text, length, current_sequence, chunk, frame);
		key = k_spin_lock(&lock);
		final_chunk_submitted = ((size_t)chunk + 1) * BLE_DISPLAY_CHUNK >= length;
		k_spin_unlock(&lock, key);
		rc = send_to_handle(conn, text_handle, frame, frame_length, fragment_deadline);
		if (rc != 0) { break; }
	}
	if (rc == 0) {
		int64_t remaining = deadline - k_uptime_get();
		if (k_sem_take(&command_done, K_MSEC(MAX(remaining, 0))) != 0) { rc = -ETIMEDOUT; }
	}
	key = k_spin_lock(&lock);
	/* A matching render ACK is authoritative even if it preceded the final
	 * ATT response or the peer disconnected immediately after rendering. */
	if (ack_received && ack_time <= deadline) { rc = command_error; }
	else if (rc == 0) { rc = command_error == -EINPROGRESS ? -ETIMEDOUT : command_error; }
	command_pending = false;
	state.last_error = rc;
	bool break_link = rc != 0 && !ack_received;
	if (break_link) { state.lcd_ready = false; }
	k_spin_unlock(&lock, key);
	if (break_link) {
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		k_sem_give(&link_event);
	}
	bt_conn_unref(conn);
	k_mutex_unlock(&operation_lock);
	return rc;
}

void gateway_get_state(struct gateway_state *snapshot)
{
	if (snapshot == NULL) { return; }
	k_spinlock_key_t key = k_spin_lock(&lock); *snapshot = state; k_spin_unlock(&lock, key);
	snapshot->wifi_connected = gateway_wifi_address(snapshot->wifi_ipv4, sizeof(snapshot->wifi_ipv4)) >= 0;
}

int gateway_radio_init(void)
{
	int rc = gateway_wifi_start();
	if (rc != 0 && strlen(CONFIG_GATEWAY_WIFI_SSID) != 0) { return rc; }
	k_spinlock_key_t key = k_spin_lock(&lock); start_requested = true; k_spin_unlock(&lock, key);
	k_sem_give(&link_event);
	return 0;
}

int gateway_lcd_command(const uint8_t *data, size_t len, uint8_t status[LCD_STATUS_SIZE], int timeout_ms)
{ ARG_UNUSED(data); ARG_UNUSED(len); ARG_UNUSED(status); ARG_UNUSED(timeout_ms); return -ENOTSUP; }
int gateway_dfu_enter(void) { return -ENOTSUP; }
int gateway_dfu_control(const uint8_t *data, size_t len)
{ ARG_UNUSED(data); ARG_UNUSED(len); return -ENOTSUP; }
int gateway_dfu_packet(const uint8_t *data, size_t len)
{ ARG_UNUSED(data); ARG_UNUSED(len); return -ENOTSUP; }
void gateway_dfu_set_notify_callback(gateway_dfu_notify_fn callback, void *user)
{ ARG_UNUSED(callback); ARG_UNUSED(user); }
void gateway_dfu_leave(void) {}

int gateway_audio_status(char *json, size_t capacity)
{
	if (json == NULL || capacity == 0) { return -EINVAL; }
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool available = audio_ring.available && !closed, running = audio_ring.running;
	uint32_t session = audio_ring.session, received = audio_ring.received, dropped = audio_ring.dropped;
	size_t queued = audio_ring.queued;
	int error = audio_ring.error;
	k_spin_unlock(&lock, key);
	int n = snprintf(json, capacity,
		"{\"available\":%s,\"running\":%s,\"session\":%lu,\"received\":%lu,"
		"\"dropped\":%lu,\"queued\":%u,\"error\":%d,\"sampleRate\":16000,\"codec\":\"ima-adpcm\"}",
		available ? "true" : "false", running ? "true" : "false", (unsigned long)session,
		(unsigned long)received, (unsigned long)dropped, (unsigned)queued, error);
	return n < 0 || (size_t)n >= capacity ? -ENOSPC : n;
}

static int audio_control(bool start, uint32_t session)
{
	if (k_mutex_lock(&operation_lock, K_NO_WAIT) != 0) { return -EBUSY; }
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (start && gateway_smp_busy()) {
		k_spin_unlock(&lock, key); k_mutex_unlock(&operation_lock); return -EBUSY;
	}
	if (peer == NULL || closed || !audio_ring.available) {
		k_spin_unlock(&lock, key); k_mutex_unlock(&operation_lock); return -ENOTSUP;
	}
	if (write_active || (start && audio_ring.running)) {
		k_spin_unlock(&lock, key); k_mutex_unlock(&operation_lock); return -EBUSY;
	}
	if (start && (session == 0 || session == audio_ring.session)) {
		k_spin_unlock(&lock, key); k_mutex_unlock(&operation_lock); return -EINVAL;
	}
	struct bt_conn *conn = bt_conn_ref(peer);
	if (start) { gateway_audio_reset(&audio_ring, session); }
	else { session = audio_ring.session; audio_ring.running = false; }
	k_spin_unlock(&lock, key);
	uint8_t control[6] = { 1, start ? 1 : 0, session, session >> 8, session >> 16, session >> 24 };
	int rc = send_to_handle(conn, audio_ctrl_handle, control, sizeof(control), k_uptime_get() + 4000);
	key = k_spin_lock(&lock);
	audio_ring.error = rc;
	if (rc != 0) { audio_ring.running = false; audio_ring.queued = 0; }
	k_spin_unlock(&lock, key);
	bt_conn_unref(conn);
	k_mutex_unlock(&operation_lock);
	return rc;
}

int gateway_audio_start(uint32_t session) { return audio_control(true, session); }
int gateway_audio_stop(void) { return audio_control(false, 0); }
bool gateway_audio_available(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool available = audio_ring.available && !closed;
	k_spin_unlock(&lock, key);
	return available;
}
int gateway_audio_pause(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool running = audio_ring.running;
	k_spin_unlock(&lock, key);
	return running ? gateway_audio_stop() : 0;
}
size_t gateway_audio_chunk(uint8_t *data, size_t capacity)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	size_t size = gateway_audio_pop(&audio_ring, data, capacity);
	k_spin_unlock(&lock, key);
	return size;
}
