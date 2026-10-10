/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WALLABY_GATEWAY_ROBOT_H
#define WALLABY_GATEWAY_ROBOT_H
#include <stdbool.h>
#include <stddef.h>
int gateway_robot_command(const char *command);
int gateway_robot_status(char *json, size_t capacity, bool accepted);
void gateway_robot_alert(bool active);
#endif
