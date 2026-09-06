/* SPDX-License-Identifier: Apache-2.0 */

#ifndef MICROWALLABY_WEB_INPUT_PROTOCOL_H_
#define MICROWALLABY_WEB_INPUT_PROTOCOL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <microwallaby_web/joystick.h>

#define MW_INPUT_PROTOCOL_VERSION 1

bool mw_input_sample_sanitize(struct mw_joystick_sample *sample, int64_t sent_ms,
			      uint32_t maximum_age_ms);
int mw_input_event_format(char *buffer, size_t capacity,
			  const struct mw_joystick_sample *sample,
			  uint32_t transmit_sequence, int64_t sent_ms);

#endif /* MICROWALLABY_WEB_INPUT_PROTOCOL_H_ */
