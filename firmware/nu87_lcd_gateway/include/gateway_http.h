/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GATEWAY_HTTP_H
#define GATEWAY_HTTP_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define HTTP_HEADER_CAPACITY 1536
#define OTA_DESCRIPTOR_SIZE 80
struct http_request {
	char method[8];
	char path[96];
	char bearer[129];
	size_t content_length;
	bool keep_alive;
};
/* Strict request framing; bounded sequential keep-alive, no chunking. */
int http_parse_request(char *header, struct http_request *request);
int http_send_all(int socket, const void *data, size_t length);
int http_read_exact(int socket, void *data, size_t length);
int http_read_before(int socket, void *data, size_t length, int64_t deadline);
int http_fetch_image(uint32_t ipv4, uint16_t port, const char *id,
		     uint32_t offset, uint32_t total, uint8_t *data, size_t length);
void gateway_http_run(void);
int gateway_ota_start(uint32_t ipv4, const uint8_t descriptor[OTA_DESCRIPTOR_SIZE]);
int gateway_ota_cancel(void);
bool gateway_ota_busy(void);
void gateway_ota_status(char *buffer, size_t capacity);
#endif
