/* SPDX-License-Identifier: Apache-2.0 */
/* The output firmware ACK, not ATT write completion, confirms a warning. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include "gateway_alarm.h"
#include "gateway_audio.h"
#include "gateway_kit_protocol.h"
#include "gateway_robot.h"

static struct bt_uuid_128 alarm_service = BT_UUID_INIT_128(BLE_DISPLAY_UUID_SERVICE_VAL);
static struct bt_uuid_128 alarm_control_uuid = BT_UUID_INIT_128(BT_UUID_128_ENCODE(
 0x90e2c005, 0x3c0d, 0x4ef3, 0x9d78, 0x574cd1a64087));
static struct bt_uuid_128 alarm_ack_uuid = BT_UUID_INIT_128(BT_UUID_128_ENCODE(
 0x90e2c006, 0x3c0d, 0x4ef3, 0x9d78, 0x574cd1a64087));
static struct k_spinlock alarm_lock;
static struct bt_conn *alarm_peer;
static struct bt_gatt_discover_params alarm_discovery;
static struct bt_gatt_subscribe_params alarm_subscription;
static struct bt_gatt_write_params alarm_write;
static uint8_t alarm_command[6];
static uint16_t alarm_start, alarm_end, alarm_control_handle, alarm_ack_handle, alarm_ack_end, alarm_ccc;
static bool alarm_closed = true, alarm_discovering, alarm_subscribing, alarm_live, alarm_ready;
static bool alarm_pending, alarm_write_active, alarm_received, alarm_target, alarm_active, alarm_known;
static int alarm_setup_error, alarm_result, alarm_write_error;
static uint32_t alarm_sequence;
static uint32_t alarm_events, alarm_generation;
static uint8_t alarm_source, alarm_output_status;
static int64_t alarm_ack_time;
K_SEM_DEFINE(alarm_setup_done, 0, 1);
K_SEM_DEFINE(alarm_write_done, 0, 1);
K_SEM_DEFINE(alarm_reply_done, 0, 1);

static uint8_t alarm_discovered(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                struct bt_gatt_discover_params *params)
{
 k_spinlock_key_t key = k_spin_lock(&alarm_lock);
 uint8_t action = BT_GATT_ITER_CONTINUE;
 if (conn != alarm_peer || alarm_closed) { attr = NULL; alarm_setup_error = -ENOTCONN; }
 if (attr == NULL) { alarm_discovering = false; action = BT_GATT_ITER_STOP; }
 else if (params->type == BT_GATT_DISCOVER_PRIMARY) {
  const struct bt_gatt_service_val *service = attr->user_data;
  alarm_start = attr->handle + 1; alarm_end = alarm_ack_end = service->end_handle;
  alarm_discovering = false; action = BT_GATT_ITER_STOP;
 } else if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
  const struct bt_gatt_chrc *ch = attr->user_data;
  if (alarm_ack_handle && attr->handle > alarm_ack_handle && alarm_ack_end == alarm_end) { alarm_ack_end = attr->handle - 1; }
  if (bt_uuid_cmp(ch->uuid, &alarm_control_uuid.uuid) == 0) {
   if (alarm_control_handle || !(ch->properties & BT_GATT_CHRC_WRITE)) { alarm_setup_error = -EPROTO; }
   alarm_control_handle = ch->value_handle;
  } else if (bt_uuid_cmp(ch->uuid, &alarm_ack_uuid.uuid) == 0) {
   if (alarm_ack_handle || !(ch->properties & BT_GATT_CHRC_NOTIFY)) { alarm_setup_error = -EPROTO; }
   alarm_ack_handle = ch->value_handle;
  }
 } else { alarm_ccc = attr->handle; alarm_discovering = false; action = BT_GATT_ITER_STOP; }
 if (!alarm_discovering) { k_sem_give(&alarm_setup_done); }
 k_spin_unlock(&alarm_lock, key); return action;
}
static int alarm_discover(struct bt_conn *conn, uint8_t type, const struct bt_uuid *uuid, uint16_t first, uint16_t last)
{
 k_sem_reset(&alarm_setup_done);
 k_spinlock_key_t key = k_spin_lock(&alarm_lock);
 alarm_discovery = (struct bt_gatt_discover_params) {
  .uuid = uuid, .func = alarm_discovered, .start_handle = first, .end_handle = last, .type = type,
 };
 alarm_discovering = true; alarm_setup_error = 0;
 k_spin_unlock(&alarm_lock, key);
 int rc = bt_gatt_discover(conn, &alarm_discovery);
 if (rc) { key = k_spin_lock(&alarm_lock); alarm_discovering = false; k_spin_unlock(&alarm_lock, key); return rc; }
 if (k_sem_take(&alarm_setup_done, K_MSEC(4000))) { return -ETIMEDOUT; }
 key = k_spin_lock(&alarm_lock); rc = alarm_closed ? -ENOTCONN : alarm_setup_error; k_spin_unlock(&alarm_lock, key);
 return rc;
}
static uint8_t alarm_notification(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                                  const void *data, uint16_t size)
{
 ARG_UNUSED(params);
 k_spinlock_key_t key = k_spin_lock(&alarm_lock);
 if (conn != alarm_peer) { k_spin_unlock(&alarm_lock, key); return BT_GATT_ITER_STOP; }
 if (data == NULL) {
  alarm_subscribing = alarm_live = alarm_ready = alarm_known = false;
  if (alarm_pending && !alarm_received) { alarm_result = -ENOTCONN; k_sem_give(&alarm_reply_done); }
  k_sem_give(&alarm_setup_done); k_spin_unlock(&alarm_lock, key); return BT_GATT_ITER_STOP;
 }
 const uint8_t *p = data;
 if (!alarm_closed && size == 8 && p[0] == 1 && p[1] <= 1 && p[6] <= 2 &&
     p[7] == 1 && gateway_audio_u32(p + 2) == 0) {
  if (alarm_events != UINT32_MAX) { alarm_events++; }
  alarm_source = 1; alarm_output_status = p[6];
  alarm_active = p[1] != 0; alarm_known = p[6] == 0;
  gateway_robot_alert(true); /* BTN4 stops Wallaby even if speaker output fails. */
  k_spin_unlock(&alarm_lock, key); return BT_GATT_ITER_CONTINUE;
 }
 if (!alarm_closed && alarm_pending && !alarm_received && size == 8 && p[0] == 1 && p[1] <= 1 &&
     p[7] == 0 && p[6] <= 2 && gateway_audio_u32(p + 2) == alarm_sequence &&
     (p[6] != 0 || (p[1] != 0) == alarm_target)) {
  alarm_received = true; alarm_ack_time = k_uptime_get();
  alarm_result = p[6] == 0 ? 0 : p[6] == 1 ? -ENOTSUP : -EIO;
  alarm_active = p[1] != 0; alarm_known = p[6] == 0;
  alarm_source = 2; alarm_output_status = p[6];
  if (p[6] == 0) { gateway_robot_alert(p[1] != 0); }
  k_sem_give(&alarm_reply_done);
 }
 k_spin_unlock(&alarm_lock, key); return BT_GATT_ITER_CONTINUE;
}
static void alarm_subscribed(struct bt_conn *conn, uint8_t error, struct bt_gatt_subscribe_params *params)
{
 ARG_UNUSED(params);
 k_spinlock_key_t key = k_spin_lock(&alarm_lock);
 if (conn == alarm_peer) {
  alarm_subscribing = false; alarm_setup_error = alarm_closed ? -ENOTCONN : error ? -EPROTO : 0;
  if (error) { alarm_live = false; }
  k_sem_give(&alarm_setup_done);
 }
 k_spin_unlock(&alarm_lock, key);
}
int gateway_alarm_prepare(struct bt_conn *conn)
{
 k_spinlock_key_t key = k_spin_lock(&alarm_lock);
 alarm_peer = conn; alarm_closed = false; alarm_ready = alarm_known = false;
 if (alarm_generation != UINT32_MAX) { alarm_generation++; }
 alarm_start = alarm_end = alarm_control_handle = alarm_ack_handle = alarm_ack_end = alarm_ccc = 0;
 k_spin_unlock(&alarm_lock, key);
 int rc = alarm_discover(conn, BT_GATT_DISCOVER_PRIMARY, &alarm_service.uuid, BT_ATT_FIRST_ATTRIBUTE_HANDLE, BT_ATT_LAST_ATTRIBUTE_HANDLE);
 if (rc) { return rc; }
 if (!alarm_start || alarm_start > alarm_end) { return -ENOTSUP; }
 rc = alarm_discover(conn, BT_GATT_DISCOVER_CHARACTERISTIC, NULL, alarm_start, alarm_end);
 if (rc) { return rc; }
 if (!alarm_control_handle || !alarm_ack_handle || alarm_ack_handle >= alarm_ack_end) { return -ENOTSUP; }
 rc = alarm_discover(conn, BT_GATT_DISCOVER_DESCRIPTOR, BT_UUID_GATT_CCC, alarm_ack_handle + 1, alarm_ack_end);
 if (rc) { return rc; }
 if (!alarm_ccc) { return -ENOENT; }
 k_sem_reset(&alarm_setup_done);
 key = k_spin_lock(&alarm_lock);
 memset(&alarm_subscription, 0, sizeof(alarm_subscription));
 alarm_subscription.notify = alarm_notification; alarm_subscription.subscribe = alarm_subscribed;
 alarm_subscription.value_handle = alarm_ack_handle; alarm_subscription.ccc_handle = alarm_ccc;
 alarm_subscription.value = BT_GATT_CCC_NOTIFY;
 atomic_set_bit(alarm_subscription.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);
 alarm_subscribing = alarm_live = true; alarm_setup_error = 0;
 k_spin_unlock(&alarm_lock, key);
 rc = bt_gatt_subscribe(conn, &alarm_subscription);
 if (rc) { key = k_spin_lock(&alarm_lock); alarm_subscribing = alarm_live = false; k_spin_unlock(&alarm_lock, key); return rc; }
 if (k_sem_take(&alarm_setup_done, K_MSEC(4000))) { return -ETIMEDOUT; }
 key = k_spin_lock(&alarm_lock);
 rc = alarm_closed || !alarm_live ? -ENOTCONN : alarm_setup_error; alarm_ready = rc == 0;
 k_spin_unlock(&alarm_lock, key); return rc;
}
void gateway_alarm_disconnected(struct bt_conn *conn)
{
 k_spinlock_key_t key = k_spin_lock(&alarm_lock);
 if (conn == alarm_peer) {
  alarm_closed = true; alarm_ready = alarm_known = false;
  if (!alarm_received) { alarm_result = -ENOTCONN; }
  k_sem_give(&alarm_reply_done); k_sem_give(&alarm_write_done); k_sem_give(&alarm_setup_done);
 }
 k_spin_unlock(&alarm_lock, key);
}
bool gateway_alarm_drained(void)
{
 k_spinlock_key_t key = k_spin_lock(&alarm_lock);
 bool value = !alarm_discovering && !alarm_subscribing && !alarm_live && !alarm_pending && !alarm_write_active;
 k_spin_unlock(&alarm_lock, key); return value;
}
static void alarm_written(struct bt_conn *conn, uint8_t error, struct bt_gatt_write_params *params)
{
 ARG_UNUSED(params);
 k_spinlock_key_t key = k_spin_lock(&alarm_lock);
 if (conn == alarm_peer) {
  alarm_write_active = false; alarm_write_error = alarm_closed ? -ENOTCONN : error ? -EIO : 0;
  k_sem_give(&alarm_write_done);
 }
 k_spin_unlock(&alarm_lock, key);
}
int gateway_alarm_set(bool active)
{
 if (active) { gateway_robot_alert(true); }
 k_spinlock_key_t key = k_spin_lock(&alarm_lock);
 if (!alarm_ready || alarm_closed || alarm_peer == NULL) { k_spin_unlock(&alarm_lock, key); return -ENOTSUP; }
 if (alarm_pending || alarm_write_active) { k_spin_unlock(&alarm_lock, key); return -EBUSY; }
 alarm_sequence++; if (!alarm_sequence) { alarm_sequence++; }
 alarm_pending = alarm_write_active = true; alarm_received = false; alarm_target = active; alarm_known = false;
 alarm_result = alarm_write_error = -EINPROGRESS;
 alarm_command[0] = 1; alarm_command[1] = active;
 for (size_t i = 0; i < 4; i++) { alarm_command[i+2] = alarm_sequence >> (8*i); }
 struct bt_conn *conn = bt_conn_ref(alarm_peer);
 alarm_write = (struct bt_gatt_write_params) {
  .func = alarm_written, .handle = alarm_control_handle, .data = alarm_command, .length = sizeof(alarm_command),
 };
 k_sem_reset(&alarm_write_done); k_sem_reset(&alarm_reply_done);
 k_spin_unlock(&alarm_lock, key);
 int64_t deadline = k_uptime_get() + 4000;
 int rc = bt_gatt_write(conn, &alarm_write);
 if (rc) { key = k_spin_lock(&alarm_lock); alarm_write_active = false; k_spin_unlock(&alarm_lock, key); }
 else if (k_sem_take(&alarm_write_done, K_MSEC(MAX(0, deadline-k_uptime_get())))) { rc = -ETIMEDOUT; }
 else { key = k_spin_lock(&alarm_lock); rc = alarm_write_error; k_spin_unlock(&alarm_lock, key); }
 if (rc == 0 && k_sem_take(&alarm_reply_done, K_MSEC(MAX(0, deadline-k_uptime_get())))) { rc = -ETIMEDOUT; }
 key = k_spin_lock(&alarm_lock);
 if (alarm_received && alarm_ack_time <= deadline) { rc = alarm_result; }
 bool disconnect = alarm_write_active || (!alarm_received && rc != 0);
 alarm_pending = false;
 if (disconnect) { alarm_ready = alarm_known = false; }
 k_spin_unlock(&alarm_lock, key);
 if (disconnect) { (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN); }
 bt_conn_unref(conn); return rc;
}
int gateway_alarm_status(char *json, size_t capacity)
{
 k_spinlock_key_t key = k_spin_lock(&alarm_lock);
 bool available = alarm_ready && !alarm_closed, known = alarm_known, active = alarm_active;
 uint32_t events = alarm_events, generation = alarm_generation;
 uint8_t source = alarm_source, status = alarm_output_status;
 k_spin_unlock(&alarm_lock, key);
 int n = snprintf(json, capacity, "{\"available\":%s,\"active\":%s,\"acknowledged\":%s,"
                  "\"eventCounter\":%lu,\"generation\":%lu,\"source\":\"%s\",\"outputStatus\":%u}",
                  available ? "true" : "false", known ? active ? "true" : "false" : "null", known ? "true" : "false",
                  (unsigned long)events, (unsigned long)generation, source == 1 ? "button" : source == 2 ? "command" : "none", status);
 return n < 0 || (size_t)n >= capacity ? -ENOSPC : n;
}
