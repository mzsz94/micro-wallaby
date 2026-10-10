/* SPDX-License-Identifier: Apache-2.0 */
/* Optional MCUmgr transport only: policy, image signatures and explicit
 * test/reset/confirm actions belong to MCUboot and the host updater. */
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include "gateway_audio.h"
#include "gateway_smp.h"

static struct bt_uuid_128 smp_service = BT_UUID_INIT_128(BT_UUID_128_ENCODE(
 0x8d53dc1d, 0x1db7, 0x4cd3, 0x868b, 0x8a527460aa84));
static struct bt_uuid_128 smp_characteristic = BT_UUID_INIT_128(BT_UUID_128_ENCODE(
 0xda2e7828, 0xfbce, 0x4e01, 0xae9e, 0x261174997c48));
static struct k_spinlock smp_lock;
static struct bt_conn *smp_peer;
static struct bt_gatt_discover_params smp_discovery;
static struct bt_gatt_subscribe_params smp_subscription;
static uint16_t smp_start, smp_end, smp_handle, smp_ccc;
static bool smp_closed = true, smp_discovering, smp_subscribing, smp_live, smp_ready, smp_active;
static int smp_setup_result, smp_result;
static struct gateway_smp_rx smp_rx;
static struct bt_conn *smp_tx_conn;
static uint8_t smp_tx_data[244];
static uint16_t smp_tx_handle, smp_tx_length;
static bool smp_tx_pending;
static int smp_tx_result;
K_SEM_DEFINE(smp_setup_done, 0, 1);
K_SEM_DEFINE(smp_reply_done, 0, 1);
K_SEM_DEFINE(smp_tx_done, 0, 1);
K_SEM_DEFINE(smp_security_done, 0, 1);
static int smp_security_error;

static void smp_send_work(struct k_work *work)
{
 ARG_UNUSED(work);
 /* Zephyr's ATT allocator uses K_NO_WAIT on this system workqueue. A
  * direct call from HTTP would use K_FOREVER when the TX pool is full. */
 k_spinlock_key_t key = k_spin_lock(&smp_lock);
 struct bt_conn *conn = smp_tx_conn;
 bool send = smp_active && !smp_closed && smp_ready;
 k_spin_unlock(&smp_lock, key);
 int rc = send ? bt_gatt_write_without_response(conn, smp_tx_handle, smp_tx_data, smp_tx_length, false) : -ENOTCONN;
 key = k_spin_lock(&smp_lock);
 smp_tx_result = rc; smp_tx_pending = false; smp_tx_conn = NULL;
 k_sem_give(&smp_tx_done);
 k_spin_unlock(&smp_lock, key);
 bt_conn_unref(conn);
}
K_WORK_DEFINE(smp_send_job, smp_send_work);

static int smp_send_fragment(struct bt_conn *conn, uint16_t handle, const uint8_t *data, size_t size, int64_t deadline)
{
 if (size > sizeof(smp_tx_data) || k_uptime_get() >= deadline) { return -EMSGSIZE; }
 k_sem_reset(&smp_tx_done);
 k_spinlock_key_t key = k_spin_lock(&smp_lock);
 if (smp_tx_pending) { k_spin_unlock(&smp_lock, key); return -EBUSY; }
 smp_tx_pending = true; smp_tx_conn = bt_conn_ref(conn);
 smp_tx_handle = handle; smp_tx_length = size;
 memcpy(smp_tx_data, data, size);
 k_spin_unlock(&smp_lock, key);
 int rc = k_work_submit(&smp_send_job);
 if (rc < 0) {
  key = k_spin_lock(&smp_lock); smp_tx_pending = false; smp_tx_conn = NULL; k_spin_unlock(&smp_lock, key);
  bt_conn_unref(conn); return rc;
 }
 if (k_sem_take(&smp_tx_done, K_MSEC(MAX(0, deadline-k_uptime_get())))) { return -ETIMEDOUT; }
 key = k_spin_lock(&smp_lock); rc = smp_tx_result; k_spin_unlock(&smp_lock, key);
 return rc;
}

static bool private_passkey(unsigned *value)
{
 const char *code = CONFIG_GATEWAY_NU40_PASSKEY;
 if (strlen(code) != 6) { return false; }
 unsigned result = 0;
 for (size_t i = 0; i < 6; i++) {
  if (code[i] < '0' || code[i] > '9') { return false; }
  result = result * 10 + (code[i] - '0');
 }
 *value = result; return true;
}
static void pairing_passkey(struct bt_conn *conn)
{
 unsigned passkey;
 k_spinlock_key_t key = k_spin_lock(&smp_lock);
 bool selected = conn == smp_peer && !smp_closed;
 k_spin_unlock(&smp_lock, key);
 if (!selected || !private_passkey(&passkey)) { (void)bt_conn_auth_cancel(conn); return; }
 (void)bt_conn_auth_passkey_entry(conn, passkey);
}
static void pairing_cancel(struct bt_conn *conn)
{
 k_spinlock_key_t key = k_spin_lock(&smp_lock);
 if (conn == smp_peer) { smp_security_error = -EACCES; k_sem_give(&smp_security_done); }
 k_spin_unlock(&smp_lock, key);
}
static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err error)
{
 k_spinlock_key_t key = k_spin_lock(&smp_lock);
 if (conn == smp_peer) {
  smp_security_error = error == 0 && level >= BT_SECURITY_L3 ? 0 : -EACCES;
  if (smp_security_error) { smp_ready = false; }
  k_sem_give(&smp_security_done);
 }
 k_spin_unlock(&smp_lock, key);
}
BT_CONN_CB_DEFINE(smp_security_callbacks) = { .security_changed = security_changed };
static const struct bt_conn_auth_cb smp_auth = { .passkey_entry = pairing_passkey, .cancel = pairing_cancel };
int gateway_smp_auth_init(void) { return bt_conn_auth_cb_register(&smp_auth); }

static int require_authenticated_link(struct bt_conn *conn)
{
 unsigned passkey;
 if (!private_passkey(&passkey)) { return -EACCES; }
 if (bt_conn_get_security(conn) >= BT_SECURITY_L3) { return 0; }
 k_sem_reset(&smp_security_done);
 k_spinlock_key_t key = k_spin_lock(&smp_lock); smp_security_error = -EINPROGRESS; k_spin_unlock(&smp_lock, key);
 int rc = bt_conn_set_security(conn, BT_SECURITY_L3);
 if (rc) { return rc; }
 if (k_sem_take(&smp_security_done, K_MSEC(10000))) { return -ETIMEDOUT; }
 key = k_spin_lock(&smp_lock); rc = smp_closed ? -ENOTCONN : smp_security_error; k_spin_unlock(&smp_lock, key);
 return rc == 0 && bt_conn_get_security(conn) < BT_SECURITY_L3 ? -EACCES : rc;
}

static uint8_t smp_discovered(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             struct bt_gatt_discover_params *params)
{
 k_spinlock_key_t key = k_spin_lock(&smp_lock);
 uint8_t action = BT_GATT_ITER_CONTINUE;
 if (conn != smp_peer || smp_closed) { attr = NULL; smp_setup_result = -ENOTCONN; }
 if (attr == NULL) { smp_discovering = false; action = BT_GATT_ITER_STOP; }
 else if (params->type == BT_GATT_DISCOVER_PRIMARY) {
  const struct bt_gatt_service_val *service = attr->user_data;
  smp_start = attr->handle + 1; smp_end = service->end_handle;
  smp_discovering = false; action = BT_GATT_ITER_STOP;
 } else if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
  const struct bt_gatt_chrc *ch = attr->user_data;
  if (smp_handle && attr->handle > smp_handle) { smp_end = attr->handle - 1; smp_discovering = false; action = BT_GATT_ITER_STOP; }
  else if (bt_uuid_cmp(ch->uuid, &smp_characteristic.uuid) == 0) {
   if (!(ch->properties & BT_GATT_CHRC_NOTIFY) || !(ch->properties & BT_GATT_CHRC_WRITE_WITHOUT_RESP)) { smp_setup_result = -EPROTO; }
   smp_handle = ch->value_handle;
  }
 } else { smp_ccc = attr->handle; smp_discovering = false; action = BT_GATT_ITER_STOP; }
 if (!smp_discovering) { k_sem_give(&smp_setup_done); }
 k_spin_unlock(&smp_lock, key);
 return action;
}

static int smp_discover(struct bt_conn *conn, uint8_t type, const struct bt_uuid *uuid, uint16_t first, uint16_t last)
{
 k_sem_reset(&smp_setup_done);
 k_spinlock_key_t key = k_spin_lock(&smp_lock);
 smp_discovery = (struct bt_gatt_discover_params) {
  .uuid = uuid, .func = smp_discovered, .start_handle = first, .end_handle = last, .type = type,
 };
 smp_discovering = true; smp_setup_result = 0;
 k_spin_unlock(&smp_lock, key);
 int rc = bt_gatt_discover(conn, &smp_discovery);
 if (rc) { key = k_spin_lock(&smp_lock); smp_discovering = false; k_spin_unlock(&smp_lock, key); return rc; }
 if (k_sem_take(&smp_setup_done, K_MSEC(4000))) { return -ETIMEDOUT; }
 key = k_spin_lock(&smp_lock); rc = smp_closed ? -ENOTCONN : smp_setup_result; k_spin_unlock(&smp_lock, key);
 return rc;
}

static uint8_t smp_notification(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                                const void *data, uint16_t size)
{
 ARG_UNUSED(params);
 k_spinlock_key_t key = k_spin_lock(&smp_lock);
 if (conn != smp_peer) { k_spin_unlock(&smp_lock, key); return BT_GATT_ITER_STOP; }
 if (data == NULL) {
  smp_subscribing = smp_live = smp_ready = false;
  if (smp_active) { smp_result = -ENOTCONN; k_sem_give(&smp_reply_done); }
  k_sem_give(&smp_setup_done);
  k_spin_unlock(&smp_lock, key); return BT_GATT_ITER_STOP;
 }
 if (!smp_closed && smp_active && smp_result == -EINPROGRESS) {
  int rc = gateway_smp_receive(&smp_rx, data, size);
  if (rc == 1 || rc < 0) { smp_result = rc == 1 ? 0 : -EPROTO; k_sem_give(&smp_reply_done); }
 }
 k_spin_unlock(&smp_lock, key); return BT_GATT_ITER_CONTINUE;
}

static void smp_subscribed(struct bt_conn *conn, uint8_t error, struct bt_gatt_subscribe_params *params)
{
 ARG_UNUSED(params);
 k_spinlock_key_t key = k_spin_lock(&smp_lock);
 if (conn == smp_peer) {
  smp_subscribing = false; smp_setup_result = smp_closed ? -ENOTCONN : error ? -EPROTO : 0;
  if (error) { smp_live = false; }
  k_sem_give(&smp_setup_done);
 }
 k_spin_unlock(&smp_lock, key);
}

int gateway_smp_prepare(struct bt_conn *conn)
{
 k_spinlock_key_t key = k_spin_lock(&smp_lock);
 smp_peer = conn; smp_closed = false; smp_ready = false;
 smp_start = smp_end = smp_handle = smp_ccc = 0;
 k_spin_unlock(&smp_lock, key);
 int rc = smp_discover(conn, BT_GATT_DISCOVER_PRIMARY, &smp_service.uuid,
                       BT_ATT_FIRST_ATTRIBUTE_HANDLE, BT_ATT_LAST_ATTRIBUTE_HANDLE);
 if (rc) { return rc; }
 if (!smp_start || smp_start > smp_end) { return -ENOTSUP; }
 rc = smp_discover(conn, BT_GATT_DISCOVER_CHARACTERISTIC, NULL, smp_start, smp_end);
 if (rc) { return rc; }
 if (!smp_handle || smp_handle >= smp_end) { return -ENOENT; }
 rc = smp_discover(conn, BT_GATT_DISCOVER_DESCRIPTOR, BT_UUID_GATT_CCC, smp_handle + 1, smp_end);
 if (rc) { return rc; }
 if (!smp_ccc) { return -ENOENT; }
 rc = require_authenticated_link(conn);
 if (rc) { return rc; }
 k_sem_reset(&smp_setup_done);
 key = k_spin_lock(&smp_lock);
 memset(&smp_subscription, 0, sizeof(smp_subscription));
 smp_subscription.notify = smp_notification; smp_subscription.subscribe = smp_subscribed;
 smp_subscription.value_handle = smp_handle; smp_subscription.ccc_handle = smp_ccc;
 smp_subscription.value = BT_GATT_CCC_NOTIFY;
 atomic_set_bit(smp_subscription.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);
 smp_subscribing = smp_live = true; smp_setup_result = 0;
 k_spin_unlock(&smp_lock, key);
 rc = bt_gatt_subscribe(conn, &smp_subscription);
 if (rc) { key = k_spin_lock(&smp_lock); smp_subscribing = smp_live = false; k_spin_unlock(&smp_lock, key); return rc; }
 if (k_sem_take(&smp_setup_done, K_MSEC(4000))) { return -ETIMEDOUT; }
 key = k_spin_lock(&smp_lock);
 rc = smp_closed || !smp_live ? -ENOTCONN : smp_setup_result;
 smp_ready = rc == 0;
 k_spin_unlock(&smp_lock, key);
 return rc;
}

void gateway_smp_disconnected(struct bt_conn *conn)
{
 k_spinlock_key_t key = k_spin_lock(&smp_lock);
 if (conn == smp_peer) {
  smp_closed = true; smp_ready = false; smp_result = -ENOTCONN;
  k_sem_give(&smp_reply_done); k_sem_give(&smp_setup_done); k_sem_give(&smp_security_done);
 }
 k_spin_unlock(&smp_lock, key);
}

bool gateway_smp_drained(void)
{
 k_spinlock_key_t key = k_spin_lock(&smp_lock);
 bool value = !smp_discovering && !smp_subscribing && !smp_live && !smp_active && !smp_tx_pending;
 k_spin_unlock(&smp_lock, key); return value;
}
bool gateway_smp_available(void)
{
 k_spinlock_key_t key = k_spin_lock(&smp_lock); bool value = smp_ready && !smp_closed;
 k_spin_unlock(&smp_lock, key); return value;
}
bool gateway_smp_busy(void)
{
 k_spinlock_key_t key = k_spin_lock(&smp_lock); bool value = smp_active;
 k_spin_unlock(&smp_lock, key); return value;
}

int gateway_smp_transaction(const uint8_t *request, size_t size, uint8_t *response, size_t capacity)
{
 if (!gateway_smp_request_valid(request, size) || response == NULL) { return -EINVAL; }
 k_spinlock_key_t key = k_spin_lock(&smp_lock);
 if (!smp_ready || smp_closed || smp_peer == NULL) { k_spin_unlock(&smp_lock, key); return -ENOTSUP; }
 if (smp_active) { k_spin_unlock(&smp_lock, key); return -EBUSY; }
 smp_active = true; smp_result = -EINPROGRESS;
 memcpy(smp_rx.request, request, 8); smp_rx.used = smp_rx.expected = 0;
 struct bt_conn *conn = bt_conn_ref(smp_peer);
 uint16_t handle = smp_handle;
 k_sem_reset(&smp_reply_done);
 k_spin_unlock(&smp_lock, key);
 int rc = gateway_audio_pause();
 if (rc == 0 && bt_conn_get_security(conn) < BT_SECURITY_L3) { rc = -EACCES; }
 int64_t deadline = k_uptime_get() + 4000;
 size_t mtu = bt_gatt_get_mtu(conn);
 if (mtu < 23) { rc = -EMSGSIZE; }
 for (size_t offset = 0; rc == 0 && offset < size;) {
  size_t count = MIN(size - offset, MIN(mtu - 3, sizeof(smp_tx_data)));
  if (k_uptime_get() >= deadline) { rc = -ETIMEDOUT; break; }
  rc = smp_send_fragment(conn, handle, request + offset, count, deadline);
  if (rc == -ENOMEM || rc == -EAGAIN) { rc = 0; k_sleep(K_MSEC(10)); continue; }
  if (rc == 0) { offset += count; }
 }
 if (rc == 0 && k_sem_take(&smp_reply_done, K_MSEC(MAX(0, deadline - k_uptime_get())))) { rc = -ETIMEDOUT; }
 key = k_spin_lock(&smp_lock);
 if (rc == 0) { rc = smp_result; }
 if (rc == 0 && smp_rx.used > capacity) { rc = -ENOSPC; }
 if (rc == 0) { memcpy(response, smp_rx.data, smp_rx.used); rc = (int)smp_rx.used; }
 smp_active = false;
 /* A timeout can leave a partial request or a late response on the peer.
  * Tear down before another request can reuse an 8-bit SMP sequence. */
 if (rc < 0) { smp_ready = false; }
 k_spin_unlock(&smp_lock, key);
 if (rc < 0) { (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN); }
 bt_conn_unref(conn);
 return rc;
}
