/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include "../ack_gate.h"

int main(void)
{
	native_ack_gate gate;
	uint8_t ack[BLE_DISPLAY_ACK_SIZE];
	native_ack_begin(&gate, 513);
	ble_display_build_ack(513, BLE_DISPLAY_OK, ack);
	assert(!native_ack_accept(&gate, ack, sizeof(ack), 1));
	gate.final_started = true;
	assert(!native_ack_accept(&gate, ack, sizeof(ack) - 1, 2));
	ack[0] = 2;
	assert(!native_ack_accept(&gate, ack, sizeof(ack), 2));
	ack[0] = BLE_DISPLAY_VERSION;
	ack[4] = BLE_DISPLAY_BUSY + 1;
	assert(!native_ack_accept(&gate, ack, sizeof(ack), 2));
	ble_display_build_ack(512, BLE_DISPLAY_OK, ack);
	assert(!native_ack_accept(&gate, ack, sizeof(ack), 3));
	ble_display_build_ack(513, BLE_DISPLAY_BUSY, ack);
	assert(native_ack_accept(&gate, ack, sizeof(ack), 4));
	assert(!native_ack_success(&gate, true));
	ble_display_build_ack(513, BLE_DISPLAY_OK, ack);
	assert(!native_ack_accept(&gate, ack, sizeof(ack), 5));
	native_ack_begin(&gate, 514);
	gate.final_started = true;
	assert(!native_ack_accept(&gate, ack, sizeof(ack), 6));
	ble_display_build_ack(514, BLE_DISPLAY_OK, ack);
	assert(native_ack_accept(&gate, ack, sizeof(ack), 7));
	assert(!native_ack_success(&gate, false));
	assert(native_ack_success(&gate, true));
	gate.active = false;
	assert(!native_ack_accept(&gate, ack, sizeof(ack), 8));

	/* Encode the exact on-air binary format and reassemble across chunk edges. */
	const char text[] = "NU87 -> NU40\nCount: 1\nUptime: 2s\nBLE link ready";
	ble_display_rx rx;
	ble_display_rx_reset(&rx);
	for (uint8_t chunk = 0; chunk * BLE_DISPLAY_CHUNK < sizeof(text) - 1; ++chunk) {
		uint8_t frame[BLE_DISPLAY_FRAME_MAX];
		size_t size = ble_display_build_text(text, sizeof(text) - 1, 514, chunk, frame);
		assert(size > 6 && size <= 20);
		auto result = ble_display_receive(&rx, frame, size, chunk * 10);
		assert(result == ((chunk + 1) * BLE_DISPLAY_CHUNK >= sizeof(text) - 1 ?
				  BLE_DISPLAY_COMPLETE : BLE_DISPLAY_WAIT));
	}
	assert(strcmp(rx.text, text) == 0);
	return 0;
}
