/* SPDX-License-Identifier: Apache-2.0 */
#include "gateway_http.h"
#include <zephyr/net/socket.h>
#include <zephyr/kernel.h>
#include <zephyr/posix/fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

static int wait_before(int fd, short event, int64_t deadline)
{
	int64_t remaining = deadline - k_uptime_get();
	if (remaining <= 0) return -ETIMEDOUT;
	struct zsock_pollfd p = { .fd = fd, .events = event };
	int rc = zsock_poll(&p, 1, (int)remaining);
	return rc > 0 && (p.revents & event) ? 0 : -ETIMEDOUT;
}
static int send_before(int fd, const void *data, size_t length, int64_t deadline)
{
	const uint8_t *p = data;
	while (length) {
		if (wait_before(fd, ZSOCK_POLLOUT, deadline)) return -ETIMEDOUT;
		int n = zsock_send(fd, p, length, 0);
		if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
		if (n <= 0) return -EIO;
		p += n; length -= n;
	}
	return 0;
}
int http_send_all(int fd, const void *data, size_t length)
{
	return send_before(fd, data, length, k_uptime_get() + 5000);
}
int http_read_before(int fd, void *data, size_t length, int64_t deadline)
{
	uint8_t *p = data;
	while (length) {
		if (wait_before(fd, ZSOCK_POLLIN, deadline)) return -ETIMEDOUT;
		int n = zsock_recv(fd, p, length, 0);
		if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
		if (n <= 0) return -EIO;
		p += n; length -= n;
	}
	return 0;
}
int http_read_exact(int fd, void *data, size_t length)
{
	return http_read_before(fd, data, length, k_uptime_get() + 5000);
}
/* Each cache block uses a fresh, finite HTTP Range request. The source is the
 * requesting control host, never a caller-provided URL or DNS name. */
int http_fetch_image(uint32_t ipv4, uint16_t port, const char *id,
		     uint32_t offset, uint32_t total, uint8_t *data, size_t length)
{
	if (!length || offset > total || length > total - offset) return -EINVAL;
	int fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) return -errno;
	int64_t deadline = k_uptime_get() + 5000;
	if (zsock_fcntl(fd, F_SETFL, O_NONBLOCK) < 0) { zsock_close(fd); return -EIO; }
	struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(port),
		.sin_addr.s_addr = ipv4 };
	int result = -EIO;
	if (zsock_connect(fd, (struct sockaddr *)&address, sizeof(address))) {
		if (errno != EINPROGRESS || wait_before(fd, ZSOCK_POLLOUT, deadline)) goto done;
		int error = 0; socklen_t size = sizeof(error);
		if (zsock_getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) || error) goto done;
	}
	char host[INET_ADDRSTRLEN], request[256];
	zsock_inet_ntop(AF_INET, &ipv4, host, sizeof(host));
	int n = snprintf(request, sizeof(request),
		"GET /firmware/%s.bin HTTP/1.1\r\nHost: %s:%u\r\nRange: bytes=%u-%u\r\nConnection: close\r\n\r\n",
		id, host, port, (unsigned)offset, (unsigned)(offset + length - 1));
	if (n <= 0 || n >= sizeof(request) || send_before(fd, request, n, deadline)) goto done;
	char header[768]; size_t used = 0;
	while (used < sizeof(header) - 1) {
		if (http_read_before(fd, header + used, 1, deadline)) goto done;
		used++; header[used] = 0;
		if (used >= 4 && !memcmp(header + used - 4, "\r\n\r\n", 4)) break;
	}
	if (used == sizeof(header) - 1) goto done;
	/* Our companion source produces one exact canonical response. Reject any
 * redirect, transfer encoding, unexpected range or length. */
	unsigned long a, b, count, body;
	int end = 0;
	if (sscanf(header, "HTTP/1.1 206 Partial Content\r\nContent-Length: %lu\r\nContent-Range: bytes %lu-%lu/%lu\r\nConnection: close\r\n\r\n%n",
		&body, &a, &b, &count, &end) != 4 || end != used || body != length ||
		a != offset || b != offset + length - 1 || count != total) goto done;
	result = http_read_before(fd, data, length, deadline);
done:
	zsock_close(fd);
	return result;
}
