/* SPDX-License-Identifier: Apache-2.0 */
/* NU87 Wallaby role: bounded command state, with simulated actuators only.
 * No physical motor driver or autonomous motion is enabled by this module. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include "gateway_robot.h"
static const char *const robot_commands[] = { "stop", "dock", "greet", "forward", "back", "left", "right" };
static struct k_spinlock robot_lock;
static unsigned robot_command;
static uint32_t robot_sequence;
static int64_t robot_until;
static bool robot_alert;

static void robot_tick(void)
{
 if (robot_command && k_uptime_get() >= robot_until) { robot_command = 0; robot_until = 0; }
}
int gateway_robot_command(const char *command)
{
 if (command == NULL) { return -EINVAL; }
 unsigned index;
 for (index = 0; index < ARRAY_SIZE(robot_commands); index++) {
  if (!strcmp(command, robot_commands[index])) { break; }
 }
 if (index == ARRAY_SIZE(robot_commands)) { return -EINVAL; }
 k_spinlock_key_t key = k_spin_lock(&robot_lock);
 robot_tick();
 if (robot_alert && index != 0) { k_spin_unlock(&robot_lock, key); return -EPERM; }
 robot_command = index;
 robot_until = index ? k_uptime_get() + (index == 2 ? 3000 : 900) : 0;
 if (robot_sequence != UINT32_MAX) { robot_sequence++; }
 k_spin_unlock(&robot_lock, key); return 0;
}
void gateway_robot_alert(bool active)
{
 k_spinlock_key_t key = k_spin_lock(&robot_lock);
 robot_alert = active; robot_command = 0; robot_until = 0;
 if (robot_sequence != UINT32_MAX) { robot_sequence++; }
 k_spin_unlock(&robot_lock, key);
}
int gateway_robot_status(char *json, size_t capacity, bool accepted)
{
 k_spinlock_key_t key = k_spin_lock(&robot_lock);
 robot_tick();
 unsigned command = robot_command;
 bool alert = robot_alert;
 uint32_t sequence = robot_sequence;
 k_spin_unlock(&robot_lock, key);
 const char *state = alert ? "alert" : command == 0 ? "idle" : command == 1 ? "docking" : command == 2 ? "greeting" : "moving";
 int n = snprintf(json, capacity, "{\"accepted\":%s,\"command\":\"%s\",\"state\":\"%s\","
                  "\"sequence\":%lu,\"alert\":%s,\"simulated\":true}",
                  accepted ? "true" : "false", robot_commands[command], state,
                  (unsigned long)sequence, alert ? "true" : "false");
 return n < 0 || (size_t)n >= capacity ? -ENOSPC : n;
}
