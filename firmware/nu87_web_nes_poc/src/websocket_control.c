/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>

#include <microwallaby_web/websocket_control.h>

int mw_websocket_control_frame_validate(bool final, bool close_frame,
					bool ping_frame, bool pong_frame,
					uint64_t remaining)
{
	unsigned int control_type_count = (close_frame ? 1U : 0U) +
		(ping_frame ? 1U : 0U) + (pong_frame ? 1U : 0U);

	if (remaining != 0U) {
		return -EMSGSIZE;
	}
	if (!final || control_type_count != 1U) {
		return -EPROTO;
	}

	return 0;
}
