/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NU87_NATIVE_ACK_GATE_H_
#define NU87_NATIVE_ACK_GATE_H_

#include "ble_display_protocol.h"

/* Protected by ack_lock in the sketch; this part also runs in host tests. */
struct native_ack_gate {
	uint16_t sequence;
	uint8_t status;
	uint32_t at_ms;
	bool active;
	bool final_started;
	bool received;
};

static inline void native_ack_begin(struct native_ack_gate *gate, uint16_t sequence)
{
	memset(gate, 0, sizeof(*gate));
	gate->sequence = sequence;
	gate->active = true;
}

static inline bool native_ack_accept(struct native_ack_gate *gate, const uint8_t *data,
				     size_t length, uint32_t at_ms)
{
	uint16_t sequence;
	uint8_t status;

	if (!gate->active || !gate->final_started || gate->received ||
	    !ble_display_parse_ack(data, length, &sequence, &status) ||
	    sequence != gate->sequence) {
		return false;
	}
	gate->received = true;
	gate->status = status;
	gate->at_ms = at_ms;
	return true;
}

static inline bool native_ack_success(const struct native_ack_gate *gate, bool all_writes)
{
	return all_writes && gate->received && gate->status == BLE_DISPLAY_OK;
}

#endif
