/* SPDX-License-Identifier: Apache-2.0 */
/* Observe the UART stream before the vendor H4 event pool can discard it.
 * No packet payloads, handles, addresses, or peripheral identities are kept.
 * This file is specific to the pinned Ameba-D UART1 H4 transport. */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <zephyr/kernel.h>

#define AMEBA_HCI_UART_ADDRESS UINT32_C(0x4000a000)
/* Pinned amebad/source/fwlib/include/ameba_uart.h LSR flag definitions. */
#define UART_ERROR_OVERRUN (1U << 1)
#define UART_ERROR_PARITY  (1U << 2)
#define UART_ERROR_FRAMING (1U << 3)
#define UART_ERROR_BREAK   (1U << 4)

enum stream_phase { STREAM_TYPE, STREAM_HEADER, STREAM_BODY };
struct uart_stream_state {
	uint32_t bytes, frames, event_headers, le_headers, adv_headers, adv_frames;
	uint32_t unknown_bytes, invalid_lengths, read_errors;
	uint32_t overruns, parity, framing, breaks;
	enum stream_phase phase;
	uint32_t remaining;
	uint16_t body_length;
	uint8_t type, header_at, header_size;
	bool le_event, first_body, adv_event;
};
static struct uart_stream_state stream;
static struct k_spinlock stream_lock;

static void add_count(uint32_t *counter, uint32_t amount)
{
	*counter = amount > UINT32_MAX - *counter ? UINT32_MAX : *counter + amount;
}

static void finish_frame(void)
{
	add_count(&stream.frames, 1);
	if (stream.adv_event) { add_count(&stream.adv_frames, 1); }
	stream.phase = STREAM_TYPE;
	stream.remaining = 0;
}

/* The caller owns stream_lock. Only framing fields are inspected; whole body
 * spans are skipped without examining or copying their contents. A truncated
 * packet remains pending across reads; a new H4-looking payload byte never
 * forces resynchronization. UART errors make stream counts inconclusive, and
 * are reported separately rather than guessing a new packet boundary. */
static void observe_bytes(const uint8_t *bytes, size_t length)
{
	add_count(&stream.bytes, (uint32_t)length);
	while (length != 0) {
		if (stream.phase == STREAM_TYPE) {
			uint8_t type = *bytes++;
			length--;
			/* H4: command, ACL, SCO, event, ISO. */
			static const uint8_t header_sizes[] = {0, 3, 4, 3, 2, 4};
			if (type == 0 || type >= sizeof(header_sizes)) {
				add_count(&stream.unknown_bytes, 1);
				continue;
			}
			stream.type = type;
			stream.header_size = header_sizes[type];
			stream.header_at = 0;
			stream.body_length = 0;
			stream.le_event = stream.adv_event = false;
			stream.remaining = stream.header_size;
			stream.phase = STREAM_HEADER;
		} else if (stream.phase == STREAM_HEADER) {
			uint8_t value = *bytes++;
			length--;
			uint8_t at = stream.header_at++;
			stream.remaining--;
			if (stream.type == 4) {
				if (at == 0) { stream.le_event = value == 0x3e; }
				else { stream.body_length = value; }
			} else if (at == 2) {
				stream.body_length = value;
			} else if (at == 3) {
				stream.body_length |= (uint16_t)value << 8;
			}
			if (stream.remaining != 0) { continue; }
			if (stream.type == 5) { stream.body_length &= 0x3fff; }
			if (stream.type == 4) {
				add_count(&stream.event_headers, 1);
				if (stream.le_event) {
					add_count(&stream.le_headers, 1);
					if (stream.body_length == 0) { add_count(&stream.invalid_lengths, 1); }
				}
			}
			stream.remaining = stream.body_length;
			stream.first_body = true;
			stream.phase = STREAM_BODY;
			if (stream.remaining == 0) { finish_frame(); }
		} else {
			if (stream.first_body && stream.le_event) {
				/* LE legacy/extended advertising subevent, before H4 pool allocation. */
				stream.adv_event = *bytes == 0x02 || *bytes == 0x0d;
				if (stream.adv_event) { add_count(&stream.adv_headers, 1); }
			}
			stream.first_body = false;
			size_t take = length < stream.remaining ? length : stream.remaining;
			bytes += take;
			length -= take;
			stream.remaining -= take;
			if (stream.remaining == 0) { finish_frame(); }
		}
	}
}

extern uint16_t __real_hci_uart_read(uint8_t *buffer, uint16_t length);

uint16_t __wrap_hci_uart_read(uint8_t *buffer, uint16_t length)
{
	/* The pinned function calls UART_ReceiveDataTO(..., Times=0), and is
	 * already called from the UART ISR. It neither waits nor takes a mutex.
	 * Keep read+observation together so ISR and H4 worker recovery cannot
	 * reorder bytes between the hardware read and the diagnostic parser. */
	k_spinlock_key_t key = k_spin_lock(&stream_lock);
	uint16_t received = __real_hci_uart_read(buffer, length);
	if (received > length || (received != 0 && buffer == NULL)) {
		add_count(&stream.read_errors, 1);
	} else if (received != 0) {
		observe_bytes(buffer, received);
	}
	k_spin_unlock(&stream_lock, key);
	return received;
}

/* ABI-compatible with the ROM's u32 UART_LineStatusGet(UART_TypeDef *).
 * Avoid the vendor include's conflicting Zephyr IRQ declarations. */
extern uint32_t __real_UART_LineStatusGet(void *uart);

uint32_t __wrap_UART_LineStatusGet(void *uart)
{
	if ((uintptr_t)uart != AMEBA_HCI_UART_ADDRESS) {
		return __real_UART_LineStatusGet(uart);
	}
	k_spinlock_key_t key = k_spin_lock(&stream_lock);
	/* Read exactly once, where the driver already requested it. Status bits
	 * may clear on read; observing the returned value adds no register read. */
	uint32_t status = __real_UART_LineStatusGet(uart);
	if (status & UART_ERROR_OVERRUN) { add_count(&stream.overruns, 1); }
	if (status & UART_ERROR_PARITY) { add_count(&stream.parity, 1); }
	if (status & UART_ERROR_FRAMING) { add_count(&stream.framing, 1); }
	if (status & UART_ERROR_BREAK) { add_count(&stream.breaks, 1); }
	k_spin_unlock(&stream_lock, key);
	return status;
}

int gateway_hci_transport_diagnostics(char *json, size_t capacity)
{
	if (json == NULL || capacity == 0) { return -EINVAL; }
	k_spinlock_key_t key = k_spin_lock(&stream_lock);
	struct uart_stream_state snapshot = stream;
	k_spin_unlock(&stream_lock, key);
	int count = snprintf(json, capacity,
		"{\"uartBytes\":%lu,\"frames\":%lu,\"eventHeaders\":%lu,\"leHeaders\":%lu,"
		"\"advHeaders\":%lu,\"advFrames\":%lu,\"unknownBytes\":%lu,\"invalidLengths\":%lu,"
		"\"readErrors\":%lu,\"uartOverruns\":%lu,\"uartParity\":%lu,\"uartFraming\":%lu,"
		"\"uartBreak\":%lu,\"inFrame\":%s,\"pendingBytes\":%lu}",
		(unsigned long)snapshot.bytes, (unsigned long)snapshot.frames,
		(unsigned long)snapshot.event_headers, (unsigned long)snapshot.le_headers,
		(unsigned long)snapshot.adv_headers, (unsigned long)snapshot.adv_frames,
		(unsigned long)snapshot.unknown_bytes, (unsigned long)snapshot.invalid_lengths,
		(unsigned long)snapshot.read_errors, (unsigned long)snapshot.overruns,
		(unsigned long)snapshot.parity, (unsigned long)snapshot.framing,
		(unsigned long)snapshot.breaks, snapshot.phase != STREAM_TYPE ? "true" : "false",
		(unsigned long)snapshot.remaining);
	if (count < 0 || (size_t)count >= capacity) { json[0] = '\0'; return -ENOSPC; }
	return count;
}
