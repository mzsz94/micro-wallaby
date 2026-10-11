/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WALLABY_GATEWAY_USB_PROTOCOL_H
#define WALLABY_GATEWAY_USB_PROTOCOL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* COBS-delimited frames coexist with boot text. CRC covers the whole decoded
 * message. A fresh authenticated HELLO owns a session; mutations are never
 * retried. The largest response is 32 complete 176-byte microphone frames. */
#define USB_RPC_HEADER 20U
#define USB_RPC_BODY_MAX 5632U
#define USB_RPC_REQUEST_MAX 1024U
#define USB_RPC_WIRE_MAX 6144U
#define USB_RPC_REQUEST 1U
#define USB_RPC_RESPONSE 2U
#define USB_RPC_HELLO 0U
#define USB_RPC_GET 1U
#define USB_RPC_POST 2U

struct usb_rpc_session {
	uint32_t owner;
	uint32_t sequence;
};

/* Call only AFTER authentication. Consume a new sequence before executing a
 * mutation, so losing its response cannot cause duplicate execution. */
static inline bool usb_rpc_accept(struct usb_rpc_session *session, uint32_t owner,
				  uint32_t sequence, bool hello)
{
	if (!owner || !sequence) { return false; }
	if (hello) {
		if (sequence != 1 || session->owner == owner) { return false; }
		session->owner = owner;
	} else if (session->owner != owner || sequence <= session->sequence) {
		return false;
	}
	session->sequence = sequence;
	return true;
}

static inline uint16_t usb_rpc_u16(const uint8_t *p)
{
	return p[0] | (uint16_t)p[1] << 8;
}

static inline uint32_t usb_rpc_u32(const uint8_t *p)
{
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline void usb_rpc_put16(uint8_t *p, uint16_t value)
{
	p[0] = value; p[1] = value >> 8;
}

static inline void usb_rpc_put32(uint8_t *p, uint32_t value)
{
	p[0] = value; p[1] = value >> 8; p[2] = value >> 16; p[3] = value >> 24;
}

static inline uint32_t usb_rpc_crc(const uint8_t *data, size_t length)
{
	uint32_t crc = UINT32_MAX;
	for (size_t i = 0; i < length; i++) {
		crc ^= data[i];
		for (unsigned bit = 0; bit < 8; bit++) {
			crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
		}
	}
	return ~crc;
}

static inline size_t usb_rpc_encode(const uint8_t *data, size_t length,
				    uint8_t *out, size_t capacity)
{
	if (capacity < 1) { return 0; }
	size_t code_at = 0, used = 1;
	uint8_t code = 1;
	for (size_t i = 0; i < length; i++) {
		if (data[i] == 0) {
			out[code_at] = code;
			if (used >= capacity) { return 0; }
			code_at = used++; code = 1;
		} else {
			if (used >= capacity) { return 0; }
			out[used++] = data[i];
			if (++code == 255) {
				out[code_at] = code;
				if (used >= capacity) { return 0; }
				code_at = used++; code = 1;
			}
		}
	}
	out[code_at] = code;
	return used;
}

static inline size_t usb_rpc_decode(const uint8_t *data, size_t length,
				    uint8_t *out, size_t capacity)
{
	size_t at = 0, used = 0;
	while (at < length) {
		uint8_t code = data[at++];
		if (!code || (size_t)code - 1 > length - at ||
		    (size_t)code - 1 > capacity - used) { return 0; }
		for (unsigned i = 1; i < code; i++) {
			if (!data[at]) { return 0; }
			out[used++] = data[at++];
		}
		if (code != 255 && at < length) {
			if (used >= capacity) { return 0; }
			out[used++] = 0;
		}
	}
	return used;
}

struct usb_rpc_request {
	uint32_t session, sequence;
	uint16_t method;
	char path[96], token[129];
	const uint8_t *body;
	size_t length;
};

static inline bool usb_rpc_parse(const uint8_t *data, size_t length,
				 struct usb_rpc_request *request)
{
	if (length < USB_RPC_HEADER + 4 || memcmp(data, "MW\x01\x01", 4) ||
	    data[18] || data[19] || !usb_rpc_u32(data + 4) || !usb_rpc_u32(data + 8)) {
		return false;
	}
	uint16_t method = usb_rpc_u16(data + 12), size = usb_rpc_u16(data + 16);
	size_t path = data[14], token = data[15];
	if (method > USB_RPC_POST || path >= sizeof(request->path) || token < 16 ||
	    token >= sizeof(request->token) || size > USB_RPC_REQUEST_MAX ||
	    length != USB_RPC_HEADER + path + token + size + 4 ||
	    usb_rpc_u32(data + length - 4) != usb_rpc_crc(data, length - 4)) { return false; }
	if ((method == USB_RPC_HELLO && (path || size)) ||
	    (method != USB_RPC_HELLO && (path < 4 || memcmp(data + USB_RPC_HEADER, "/v1/", 4)))) {
		return false;
	}
	for (size_t i = 0; i < path + token; i++) {
		uint8_t byte = data[USB_RPC_HEADER + i];
		if (byte < 33 || byte > 126) { return false; }
	}
	request->session = usb_rpc_u32(data + 4);
	request->sequence = usb_rpc_u32(data + 8);
	request->method = method;
	memcpy(request->path, data + USB_RPC_HEADER, path); request->path[path] = 0;
	memcpy(request->token, data + USB_RPC_HEADER + path, token); request->token[token] = 0;
	request->body = data + USB_RPC_HEADER + path + token;
	request->length = size;
	return true;
}

#endif
