/* SPDX-License-Identifier: Apache-2.0 */
/* Deliberately synchronous host model for deterministic callback-order tests.
 * It does not emulate a controller, radio, RTOS scheduler, or OTA transport. */
#ifndef KIT_MOCK_H
#define KIT_MOCK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define CONFIG_GATEWAY_LOG_LEVEL 3
#define CONFIG_GATEWAY_WIFI_SSID "test"
#define CONFIG_GATEWAY_NU40_PASSKEY "314159" /* Unit-test fixture only. */
#define ARG_UNUSED(x) (void)(x)
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define K_MSEC(x) (x)
#define K_SECONDS(x) ((x) * 1000)
#define K_FOREVER (-1)
#define K_NO_WAIT 0
#define LOG_MODULE_REGISTER(...)
#define LOG_INF(...)
#define LOG_ERR(...)
#define K_THREAD_DEFINE(...)
struct k_work { void (*handler)(struct k_work *); };
#define K_WORK_DEFINE(name, callback) static struct k_work name = { callback }
static inline int k_work_submit(struct k_work *work) { work->handler(work); return 1; }
struct k_spinlock { int unused; };
typedef int k_spinlock_key_t;
static inline int k_spin_lock(struct k_spinlock *lock) { ARG_UNUSED(lock); return 0; }
static inline void k_spin_unlock(struct k_spinlock *lock, int key) { ARG_UNUSED(lock); ARG_UNUSED(key); }
struct k_mutex { bool held; };
#define K_MUTEX_DEFINE(name) static struct k_mutex name
static inline int k_mutex_lock(struct k_mutex *mutex, int wait) { ARG_UNUSED(wait); if (mutex->held) { return -EBUSY; } mutex->held = true; return 0; }
static inline void k_mutex_unlock(struct k_mutex *mutex) { mutex->held = false; }
struct k_sem { int count; };
#define K_SEM_DEFINE(name,initial,limit) static struct k_sem name = { initial }
static int64_t mock_time = 100;
static inline int64_t k_uptime_get(void) { return mock_time; }
static inline void k_sleep(int64_t delay) { mock_time += delay; }
static inline void k_sem_reset(struct k_sem *sem) { sem->count = 0; }
static inline void k_sem_give(struct k_sem *sem) { sem->count = 1; }
static inline int k_sem_take(struct k_sem *sem, int64_t delay) { if (sem->count) { sem->count = 0; return 0; } if (delay > 0) { mock_time += delay; } return -EAGAIN; }
struct bt_uuid { uint8_t type; };
struct bt_uuid_128 { struct bt_uuid uuid; uint8_t val[16]; };
#define BT_UUID_128_ENCODE(a,b,c,d,e) (a)&255,((a)>>8)&255,((a)>>16)&255,((a)>>24)&255,(b)&255,((b)>>8)&255,(c)&255,((c)>>8)&255,(d)&255,((d)>>8)&255,(e)&255,((e)>>8)&255,((e)>>16)&255,((e)>>24)&255,((e)>>32)&255,((e)>>40)&255
#define BT_UUID_INIT_128(...) { .uuid = { 128 }, .val = { __VA_ARGS__ } }
static struct bt_uuid mock_ccc_uuid = { 16 };
#define BT_UUID_GATT_CCC (&mock_ccc_uuid)
static inline int bt_uuid_cmp(const struct bt_uuid *a, const struct bt_uuid *b) { if (a->type != b->type) { return 1; } return a->type == 128 ? memcmp(((const struct bt_uuid_128 *)a)->val, ((const struct bt_uuid_128 *)b)->val, 16) : a != b; }
typedef struct { uint8_t val[6]; } bt_addr_t;
typedef struct { uint8_t type; bt_addr_t a; } bt_addr_le_t;
#define BT_ADDR_LE_PUBLIC 0
#define BT_ADDR_LE_RANDOM 1
#define BT_ADDR_STR_LEN 18
static inline int bt_addr_le_cmp(const bt_addr_le_t *a, const bt_addr_le_t *b) { return memcmp(a,b,sizeof(*a)); }
static inline bool bt_addr_le_is_identity(const bt_addr_le_t *a) { return a->type == 0 || (a->type == 1 && (a->a.val[5] & 0xc0) == 0xc0); }
static inline int bt_addr_le_from_str(const char *s, const char *type, bt_addr_le_t *a) { unsigned x[6]; char tail; if (sscanf(s,"%2x:%2x:%2x:%2x:%2x:%2x%c", &x[5],&x[4],&x[3],&x[2],&x[1],&x[0],&tail) != 6) { return -EINVAL; } a->type = strcmp(type,"public") == 0 ? 0 : 1; for (int i=0;i<6;i++) { a->a.val[i]=x[i]; } return 0; }
static inline int bt_addr_to_str(const bt_addr_t *a, char *out, size_t n) { return snprintf(out,n,"%02X:%02X:%02X:%02X:%02X:%02X",a->val[5],a->val[4],a->val[3],a->val[2],a->val[1],a->val[0]); }
static inline int bt_addr_le_to_str(const bt_addr_le_t *a, char *out, size_t n) { return bt_addr_to_str(&a->a,out,n); }
struct bt_conn { bt_addr_le_t address; int refs; };
static inline const bt_addr_le_t *bt_conn_get_dst(struct bt_conn *c) { return &c->address; }
static inline struct bt_conn *bt_conn_ref(struct bt_conn *c) { c->refs++; return c; }
static inline void bt_conn_unref(struct bt_conn *c) { c->refs--; }
typedef uint8_t bt_security_t;
enum bt_security_err { BT_SECURITY_ERR_SUCCESS, BT_SECURITY_ERR_AUTH_FAIL };
#define BT_SECURITY_L3 3
static bt_security_t mock_security = BT_SECURITY_L3;
struct bt_conn_cb { void (*connected)(struct bt_conn *,uint8_t); void (*disconnected)(struct bt_conn *,uint8_t); void (*security_changed)(struct bt_conn *,bt_security_t,enum bt_security_err); };
struct bt_conn_auth_cb { void (*passkey_entry)(struct bt_conn *); void (*cancel)(struct bt_conn *); };
static inline int bt_conn_auth_cb_register(const struct bt_conn_auth_cb *c) { ARG_UNUSED(c); return 0; }
static inline int bt_conn_auth_cancel(struct bt_conn *c) { ARG_UNUSED(c); return 0; }
static inline int bt_conn_auth_passkey_entry(struct bt_conn *c,unsigned p) { ARG_UNUSED(c); ARG_UNUSED(p); return 0; }
static inline bt_security_t bt_conn_get_security(const struct bt_conn *c) { ARG_UNUSED(c); return mock_security; }
static inline int bt_conn_set_security(struct bt_conn *c,bt_security_t l) { ARG_UNUSED(c); ARG_UNUSED(l); return 0; }
#define BT_CONN_CB_DEFINE(name) static struct bt_conn_cb name
#define BT_HCI_ERR_REMOTE_USER_TERM_CONN 19
#define BT_CONN_LE_CREATE_CONN NULL
#define BT_LE_CONN_PARAM_DEFAULT NULL
#define BT_GAP_ADV_TYPE_ADV_IND 0
#define BT_GAP_ADV_TYPE_SCAN_RSP 4
#define BT_DATA_UUID128_ALL 7
#define BT_DATA_UUID128_SOME 6
#define BT_DATA_NAME_COMPLETE 9
#define BT_LE_SCAN_TYPE_ACTIVE 1
#define BT_LE_SCAN_TYPE_PASSIVE 0
#define BT_GAP_SCAN_FAST_INTERVAL 96
#define BT_GAP_SCAN_FAST_WINDOW 48
struct bt_le_scan_param { uint8_t type; uint32_t options; uint16_t interval, window; };
#define BT_LE_SCAN_PARAM(t,o,i,w) (&(struct bt_le_scan_param){ (t),(o),(i),(w) })
static unsigned mock_scan_starts, mock_scan_stops, mock_enable_calls;
static int mock_scan_start_result, mock_scan_stop_result, mock_enable_result;
static struct bt_le_scan_param mock_scan_parameters;
struct bt_data { uint8_t type, data_len; const uint8_t *data; };
struct net_buf_simple { const uint8_t *data; size_t len; };
static inline void bt_data_parse(struct net_buf_simple *buf, bool (*cb)(struct bt_data *,void *),void *arg) { size_t at=0; while (at<buf->len) { size_t size=buf->data[at++]; if (!size || size>buf->len-at) { return; } struct bt_data d={buf->data[at],size-1,buf->data+at+1}; if (!cb(&d,arg)) { return; } at+=size; } }
static inline int bt_le_scan_start(const struct bt_le_scan_param *param, void (*cb)(const bt_addr_le_t *,int8_t,uint8_t,struct net_buf_simple *)) { mock_scan_starts++; mock_scan_parameters = *param; ARG_UNUSED(cb); return mock_scan_start_result; }
static inline int bt_le_scan_stop(void) { mock_scan_stops++; return mock_scan_stop_result; }
static inline int bt_conn_le_create(const bt_addr_le_t *a,const void *p,const void *q,struct bt_conn **c) { ARG_UNUSED(a);ARG_UNUSED(p);ARG_UNUSED(q);ARG_UNUSED(c);return -ENOTSUP; }
static inline int bt_enable(void *cb) { ARG_UNUSED(cb); mock_enable_calls++; return mock_enable_result; }
void gateway_radio_diagnostics(const char *stage);
void gateway_radio_coex_diagnostics(void);
int bt_conn_disconnect(struct bt_conn *conn, uint8_t reason);
struct bt_gatt_attr { uint16_t handle; const void *user_data; };
struct bt_gatt_service_val { uint16_t end_handle; };
struct bt_gatt_chrc { const struct bt_uuid *uuid; uint16_t value_handle; uint8_t properties; };
struct bt_gatt_discover_params { const struct bt_uuid *uuid; uint8_t (*func)(struct bt_conn *,const struct bt_gatt_attr *,struct bt_gatt_discover_params *); uint16_t start_handle,end_handle; uint8_t type; };
struct bt_gatt_subscribe_params { uint8_t (*notify)(struct bt_conn *,struct bt_gatt_subscribe_params *,const void *,uint16_t); void (*subscribe)(struct bt_conn *,uint8_t,struct bt_gatt_subscribe_params *); uint16_t value_handle,ccc_handle,value; unsigned flags[1]; };
struct bt_gatt_write_params { void (*func)(struct bt_conn *,uint8_t,struct bt_gatt_write_params *); uint16_t handle; const void *data; uint16_t length; };
struct bt_gatt_exchange_params { void (*func)(struct bt_conn *,uint8_t,struct bt_gatt_exchange_params *); };
static uint16_t mock_att_mtu = 247;
static inline uint16_t bt_gatt_get_mtu(struct bt_conn *c) { ARG_UNUSED(c); return mock_att_mtu; }
static inline int bt_gatt_exchange_mtu(struct bt_conn *c, struct bt_gatt_exchange_params *p) { p->func(c, 0, p); return 0; }
#define BT_GATT_ITER_STOP 0
#define BT_GATT_ITER_CONTINUE 1
#define BT_GATT_DISCOVER_PRIMARY 1
#define BT_GATT_DISCOVER_CHARACTERISTIC 2
#define BT_GATT_DISCOVER_DESCRIPTOR 3
#define BT_GATT_CHRC_WRITE 8
#define BT_GATT_CHRC_WRITE_WITHOUT_RESP 4
#define BT_GATT_CHRC_NOTIFY 16
#define BT_GATT_CCC_NOTIFY 1
#define BT_GATT_SUBSCRIBE_FLAG_VOLATILE 0
#define BT_ATT_FIRST_ATTRIBUTE_HANDLE 1
#define BT_ATT_LAST_ATTRIBUTE_HANDLE 65535
static inline void atomic_set_bit(unsigned *value, unsigned bit) { *value |= 1U << bit; }
static inline int bt_gatt_discover(struct bt_conn *c,struct bt_gatt_discover_params *p) { ARG_UNUSED(c);ARG_UNUSED(p);return -ENOTSUP; }
static inline int bt_gatt_subscribe(struct bt_conn *c,struct bt_gatt_subscribe_params *p) { ARG_UNUSED(c);ARG_UNUSED(p);return -ENOTSUP; }
int bt_gatt_write(struct bt_conn *c,struct bt_gatt_write_params *p);
int bt_gatt_write_without_response(struct bt_conn *,uint16_t,const void *,uint16_t,bool);
#endif
