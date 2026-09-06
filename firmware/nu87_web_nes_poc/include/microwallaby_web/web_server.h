/* SPDX-License-Identifier: Apache-2.0 */

#ifndef MICROWALLABY_WEB_WEB_SERVER_H_
#define MICROWALLABY_WEB_WEB_SERVER_H_

#include <stdbool.h>
#include <stdint.h>

struct mw_web_server_stats {
	uint32_t packets_sent;
	uint32_t send_errors;
	uint32_t stale_input_frames;
	bool client_connected;
};

int mw_web_server_start(void);
void mw_web_server_get_stats(struct mw_web_server_stats *stats);

#endif /* MICROWALLABY_WEB_WEB_SERVER_H_ */
