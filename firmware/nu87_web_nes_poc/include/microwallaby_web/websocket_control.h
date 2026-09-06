/* SPDX-License-Identifier: Apache-2.0 */

#ifndef MICROWALLABY_WEB_WEBSOCKET_CONTROL_H_
#define MICROWALLABY_WEB_WEBSOCKET_CONTROL_H_

#include <stdbool.h>
#include <stdint.h>

int mw_websocket_control_frame_validate(bool final, bool close_frame,
					bool ping_frame, bool pong_frame,
					uint64_t remaining);

#endif /* MICROWALLABY_WEB_WEBSOCKET_CONTROL_H_ */
