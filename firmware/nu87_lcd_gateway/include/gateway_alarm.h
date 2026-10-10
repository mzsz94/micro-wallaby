/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WALLABY_GATEWAY_ALARM_H
#define WALLABY_GATEWAY_ALARM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct bt_conn;
int gateway_alarm_prepare(struct bt_conn *conn);
void gateway_alarm_disconnected(struct bt_conn *conn);
bool gateway_alarm_drained(void);
int gateway_alarm_set(bool active);
int gateway_alarm_status(char *json, size_t capacity);
#endif
