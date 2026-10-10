/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WALLABY_GATEWAY_SMP_H
#define WALLABY_GATEWAY_SMP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define GATEWAY_SMP_MAX 1024U
struct bt_conn;
struct gateway_smp_rx {
 uint8_t request[8], data[GATEWAY_SMP_MAX];
 size_t used, expected;
};
static inline size_t gateway_smp_length(const uint8_t *p)
{ return 8U + ((size_t)p[2] << 8) + p[3]; }
static inline bool gateway_smp_request_valid(const uint8_t *p, size_t size)
{
 return p != NULL && size >= 8 && size <= GATEWAY_SMP_MAX &&
  (p[0] == 0 || p[0] == 2) && p[1] == 0 && gateway_smp_length(p) == size;
}
/* 1 complete, 0 more required, 2 unrelated reply, -1 invalid framing. */
static inline int gateway_smp_receive(struct gateway_smp_rx *r, const uint8_t *p, size_t size)
{
 if (p == NULL || size == 0) { return -1; }
 if (r->used == 0) {
  if (size < 8) { return -1; }
  if (p[0] != r->request[0] + 1 || p[1] != 0 || memcmp(p + 4, r->request + 4, 4) != 0) { return 2; }
  r->expected = gateway_smp_length(p);
  if (r->expected > GATEWAY_SMP_MAX) { return -1; }
 }
 if (size > r->expected - r->used) { return -1; }
 memcpy(r->data + r->used, p, size); r->used += size;
 return r->used == r->expected ? 1 : 0;
}
int gateway_smp_prepare(struct bt_conn *conn);
int gateway_smp_auth_init(void);
void gateway_smp_disconnected(struct bt_conn *conn);
bool gateway_smp_drained(void);
bool gateway_smp_available(void);
bool gateway_smp_busy(void);
int gateway_smp_transaction(const uint8_t *request, size_t size, uint8_t *response, size_t capacity);
#endif
