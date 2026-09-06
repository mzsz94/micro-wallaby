/* SPDX-License-Identifier: Apache-2.0 */

#ifndef MICROWALLABY_WEB_WIFI_STA_H_
#define MICROWALLABY_WEB_WIFI_STA_H_

#include <stdbool.h>
#include <stddef.h>

int mw_wifi_sta_start(void);
bool mw_wifi_sta_ipv4_ready(void);
int mw_wifi_sta_address(char *buffer, size_t capacity);

#endif /* MICROWALLABY_WEB_WIFI_STA_H_ */
