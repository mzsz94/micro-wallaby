/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>

#include <microwallaby_web/input_protocol.h>

bool mw_input_sample_sanitize(struct mw_joystick_sample *sample, int64_t sent_ms,
			      uint32_t maximum_age_ms)
{
	bool stale;

	if (sample == NULL) {
		return true;
	}

	stale = sample->captured_ms < 0 || sent_ms < sample->captured_ms ||
		(sent_ms - sample->captured_ms) > maximum_age_ms;
	if (stale || !sample->input_ok) {
		sample->x_permille = 0;
		sample->y_permille = 0;
		sample->sw_pressed = false;
	}
	if (stale) {
		sample->input_ok = false;
	}

	return stale;
}

int mw_input_event_format(char *buffer, size_t capacity,
			  const struct mw_joystick_sample *sample,
			  uint32_t transmit_sequence, int64_t sent_ms)
{
	int written;

	if (buffer == NULL || capacity == 0U || sample == NULL) {
		return -EINVAL;
	}

	written = snprintf(buffer, capacity,
			   "{\"v\":%d,\"seq\":%" PRIu32 ",\"sample_seq\":%" PRIu32
			   ",\"sample_ms\":%" PRId64 ",\"sent_ms\":%" PRId64
			   ",\"x\":%d,\"y\":%d,\"sw\":%s,\"ok\":%s}",
			   MW_INPUT_PROTOCOL_VERSION, transmit_sequence, sample->sequence,
			   sample->captured_ms, sent_ms, sample->x_permille,
			   sample->y_permille, sample->sw_pressed ? "true" : "false",
			   sample->input_ok ? "true" : "false");
	if (written < 0) {
		return written;
	}
	if ((size_t)written >= capacity) {
		return -ENOSPC;
	}

	return written;
}
