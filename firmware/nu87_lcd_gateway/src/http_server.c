/* SPDX-License-Identifier: Apache-2.0 */
#include "gateway_http.h"
#include "gateway_link.h"
#include "gateway_imu.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/fcntl.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(gateway_http, CONFIG_GATEWAY_LOG_LEVEL);
#define DISPLAY_TEXT_MAX 84U

struct http_client {
	int fd;
	bool keep_alive;
	bool responded;
	int64_t session_deadline;
};

static void respond(struct http_client *client, int code, const void *body,
		    size_t length, const char *type)
{
	/* The listener is serial. Combine small JSON replies into one write to
	 * avoid a second Wi-Fi packet/delayed ACK for every IMU status request. */
	static uint8_t wire[768];
	client->keep_alive = client->keep_alive && code < 400 &&
		k_uptime_get() < client->session_deadline;
	int n = snprintk((char *)wire, 256,
		"HTTP/1.1 %d Result\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
		"Connection: %s\r\nCache-Control: no-store\r\n\r\n",
		code, type, (unsigned)length, client->keep_alive ? "keep-alive" : "close");
	if (n < 0 || n >= 256) { client->keep_alive = false; return; }
	int rc;
	if (length <= sizeof(wire) - (size_t)n) {
		memcpy(wire + n, body, length);
		rc = http_send_all(client->fd, wire, (size_t)n + length);
	} else {
		rc = http_send_all(client->fd, wire, (size_t)n);
		if (!rc) rc = http_send_all(client->fd, body, length);
	}
	client->responded = rc == 0;
	if (rc) client->keep_alive = false;
}
static bool authenticated(const char *token)
{
	size_t n = strlen(CONFIG_GATEWAY_API_TOKEN);
	if (n < 16 || n > 128 || strlen(token) != n) return false;
	unsigned difference = 0;
	for (size_t i = 0; i < n; i++) difference |= token[i] ^ CONFIG_GATEWAY_API_TOKEN[i];
	return difference == 0;
}
static void route(struct http_client *client, uint32_t source_ip,
		  struct http_request request, int64_t deadline)
{
	int fd = client->fd;
	if (!strcmp(request.method, "GET") && !strcmp(request.path, "/v1/imu/status") &&
	    !request.content_length) {
		char result[512];
		int n = gateway_imu_status(result, sizeof(result));
		if (n < 0 || (size_t)n >= sizeof(result)) {
			respond(client, 503, "IMU unavailable", 15, "text/plain"); return;
		}
		respond(client, 200, result, (size_t)n, "application/json"); return;
	}
#if defined(CONFIG_GATEWAY_NU40_KIT)
	if (!strcmp(request.method, "GET") && !strcmp(request.path, "/v1/robot/status") && !request.content_length) {
		char result[256];
		int n = gateway_robot_status(result, sizeof(result), false);
		if (n < 0 || (size_t)n >= sizeof(result)) { respond(client, 503, "robot unavailable", 17, "text/plain"); return; }
		respond(client, 200, result, (size_t)n, "application/json"); return;
	}
	if (!strcmp(request.method, "GET") && !strcmp(request.path, "/v1/alarm/status") && !request.content_length) {
		char result[256];
		int n = gateway_alarm_status(result, sizeof(result));
		if (n < 0 || (size_t)n >= sizeof(result)) { respond(client, 503, "alarm unavailable", 17, "text/plain"); return; }
		respond(client, 200, result, (size_t)n, "application/json"); return;
	}
	if (!strcmp(request.method, "GET") && !strcmp(request.path, "/v1/smp/status") && !request.content_length) {
		const char *result = gateway_smp_available() ? "{\"available\":true}" : "{\"available\":false}";
		respond(client, 200, result, strlen(result), "application/json"); return;
	}
	if (!strcmp(request.method, "POST") && !strcmp(request.path, "/v1/smp")) {
		/* Request and response storage is bounded and private to the serial
		 * HTTP handler. No image policy or automatic boot command lives here. */
		static uint8_t packet[GATEWAY_SMP_MAX], reply[GATEWAY_SMP_MAX];
		if (request.content_length < 8 || request.content_length > sizeof(packet)) {
			respond(client, 400, "invalid SMP size", 16, "text/plain"); return;
		}
		if (http_read_before(fd, packet, request.content_length, deadline)) { return; }
		int n = gateway_smp_transaction(packet, request.content_length, reply, sizeof(reply));
		if (n < 0) { respond(client, n == -EBUSY ? 409 : n == -ENOTSUP ? 503 : 422, "SMP relay failed", 16, "text/plain"); return; }
		respond(client, 200, reply, (size_t)n, "application/octet-stream"); return;
	}
	if (!strcmp(request.method, "GET") && !strcmp(request.path, "/v1/audio/status") && !request.content_length) {
		char result[320];
		int n = gateway_audio_status(result, sizeof(result));
		if (n < 0 || (size_t)n >= sizeof(result)) { respond(client, 503, "audio unavailable", 17, "text/plain"); return; }
		respond(client, 200, result, n, "application/json"); return;
	}
	if (!strcmp(request.method, "GET") && !strcmp(request.path, "/v1/audio/chunk") && !request.content_length) {
		/* The HTTP listener is serial. Avoid putting this bounded drain buffer
		 * on the main stack alongside vendor Wi-Fi send stack usage. */
		static uint8_t audio[GATEWAY_AUDIO_FRAME_SIZE * GATEWAY_AUDIO_DRAIN_FRAMES];
		size_t n = gateway_audio_chunk(audio, sizeof(audio));
		respond(client, 200, audio, n, "application/octet-stream"); return;
	}
	if (!strcmp(request.method, "GET") && !strcmp(request.path, "/v1/scan") && !request.content_length) {
		/* Format directly into one buffer: diagnostics must not multiply the
		 * main thread's peak stack usage with per-section temporary copies. */
		char result[2304];
		const struct {
			const char *name;
			int (*format)(char *, size_t);
		} sections[] = {
			{"adapter", gateway_scan_diagnostics},
			{"link", gateway_kit_diagnostics},
			{"hci", gateway_hci_diagnostics},
			{"transport", gateway_hci_transport_diagnostics},
			{"coex", gateway_coex_scan_diagnostics},
		};
		size_t offset = 0;
		for (size_t i = 0; i < sizeof(sections) / sizeof(sections[0]); i++) {
			int n = snprintk(result + offset, sizeof(result) - offset,
				"%s\"%s\":", i == 0 ? "{" : ",", sections[i].name);
			if (n < 0 || (size_t)n >= sizeof(result) - offset) {
				respond(client, 503, "scan unavailable", 16, "text/plain"); return;
			}
			offset += (size_t)n;
			n = sections[i].format(result + offset, sizeof(result) - offset);
			if (n < 0 || (size_t)n >= sizeof(result) - offset) {
				respond(client, 503, "scan unavailable", 16, "text/plain"); return;
			}
			offset += (size_t)n;
		}
		if (offset >= sizeof(result) - 1) {
			respond(client, 503, "scan unavailable", 16, "text/plain"); return;
		}
		result[offset++] = '}';
		respond(client, 200, result, offset, "application/json"); return;
	}
	if (!strcmp(request.method, "GET") && !strcmp(request.path, "/v1/peers") && !request.content_length) {
		char peers[768];
		memcpy(peers, "{\"peers\":", 9);
		int n = gateway_peer_candidates(peers + 9, sizeof(peers) - 11);
		if (n < 0) { respond(client, 503, "scan unavailable", 16, "text/plain"); return; }
		peers[9 + n] = '}';
		respond(client, 200, peers, 10 + n, "application/json"); return;
	}
#endif
	if (!strcmp(request.method, "GET") && !strcmp(request.path, "/v1/status") && !request.content_length) {
		struct gateway_state state; gateway_get_state(&state);
		char ota[256], body[896]; gateway_ota_status(ota, sizeof(ota));
#if defined(CONFIG_GATEWAY_NU40_KIT)
		char capabilities[192];
		snprintf(capabilities, sizeof(capabilities),
			"\"protocol\":\"nu40-kit-display\",\"columns\":21,\"rows\":4,"
			"\"capabilities\":{\"text\":true,\"power\":false,\"ota\":false,\"microphone\":%s,\"smp\":%s}",
			gateway_audio_available() ? "true" : "false", gateway_smp_available() ? "true" : "false");
#else
		const char *capabilities = "\"protocol\":\"mzsz-lcd\",\"columns\":16,\"rows\":4,"
			"\"capabilities\":{\"text\":true,\"power\":true,\"ota\":true}";
#endif
		int n = snprintk(body, sizeof(body),
			"{\"wifi\":%s,\"ip\":\"%s\",\"configured\":%s,\"connected\":%s,\"lcdReady\":%s,\"dfuReady\":%s,\"peer\":\"%s\",\"error\":%d,\"ota\":%s,%s}",
			state.wifi_connected ? "true" : "false", state.wifi_ipv4,
			state.ble_configured ? "true" : "false", state.ble_connected ? "true" : "false",
			state.lcd_ready ? "true" : "false", state.dfu_ready ? "true" : "false", state.peer_addr, state.last_error, ota, capabilities);
		if (n < 0 || (size_t)n >= sizeof(body)) { respond(client, 500, "status overflow", 15, "text/plain"); return; }
		respond(client, 200, body, n, "application/json"); return;
	}
	if (strcmp(request.method, "POST") || request.content_length > DISPLAY_TEXT_MAX) {
		respond(client, 400, "invalid request", 15, "text/plain"); return;
	}
	uint8_t body[DISPLAY_TEXT_MAX];
	if (http_read_before(fd, body, request.content_length, deadline)) return;
	int rc = -EINVAL;
#if defined(CONFIG_GATEWAY_NU40_KIT)
	if (!strcmp(request.path, "/v1/robot/command") && request.content_length > 0 && request.content_length <= 7 &&
	    memchr(body, '\0', request.content_length) == NULL) {
		char command[8]; memcpy(command, body, request.content_length); command[request.content_length] = '\0';
		rc = gateway_robot_command(command);
		if (rc == 0) {
			char result[256]; int n = gateway_robot_status(result, sizeof(result), true);
			if (n < 0 || (size_t)n >= sizeof(result)) { respond(client, 503, "robot unavailable", 17, "text/plain"); return; }
			respond(client, 200, result, (size_t)n, "application/json"); return;
		}
	} else if (!strcmp(request.path, "/v1/alarm") &&
	    ((request.content_length == 2 && !memcmp(body, "on", 2)) ||
	     (request.content_length == 3 && !memcmp(body, "off", 3)))) {
		bool active = request.content_length == 2;
		rc = gateway_alarm_set(active);
		if (rc == 0) {
			const char *result = active ? "{\"available\":true,\"active\":true,\"acknowledged\":true}" :
				"{\"available\":true,\"active\":false,\"acknowledged\":true}";
			respond(client, 200, result, strlen(result), "application/json"); return;
		}
	} else if (!strcmp(request.path, "/v1/audio/start") && request.content_length > 0 && request.content_length <= 10) {
		uint32_t session = 0;
		bool valid = true;
		for (size_t i = 0; i < request.content_length; i++) {
			if (body[i] < '0' || body[i] > '9' || session > (UINT32_MAX - (body[i] - '0')) / 10) {
				valid = false; break;
			}
			session = session * 10 + (body[i] - '0');
		}
		if (valid && session != 0) { rc = gateway_audio_start(session); }
	} else if (!strcmp(request.path, "/v1/audio/stop") && request.content_length == 4 && !memcmp(body, "stop", 4)) {
		rc = gateway_audio_stop();
	} else if (!strcmp(request.path, "/v1/scan")) {
		if (request.content_length == 6 && !memcmp(body, "active", 6)) {
			rc = gateway_scan_mode("active");
		} else if (request.content_length == 7 && !memcmp(body, "passive", 7)) {
			rc = gateway_scan_mode("passive");
		}
	} else if (!strcmp(request.path, "/v1/display") && request.content_length > 0) {
		rc = gateway_display_text(body, request.content_length, 4000);
		if (!rc) {
			static const char result[] = "{\"ready\":true,\"columns\":21,\"rows\":4,\"dfuCapable\":false}";
			respond(client, 200, result, sizeof(result) - 1, "application/json"); return;
		}
	} else if (!strcmp(request.path, "/v1/peer") && request.content_length == 24 &&
		   body[17] == ' ' && memchr(body, '\0', 24) == NULL) {
		char address[18], type[7];
		memcpy(address, body, 17); address[17] = '\0';
		memcpy(type, body + 18, 6); type[6] = '\0';
		if (!strcmp(type, "public") || !strcmp(type, "random")) {
			rc = gateway_select_peer(address, type);
			if (!rc) {
				respond(client, 202, "{\"accepted\":true}", 17, "application/json"); return;
			}
		}
	} else if (!strcmp(request.path, "/v1/lcd") || !strcmp(request.path, "/v1/ota") ||
		   !strcmp(request.path, "/v1/ota/cancel")) {
		static const char unsupported[] = "unsupported by NU40-KIT firmware";
		respond(client, 405, unsupported, sizeof(unsupported) - 1, "text/plain"); return;
	}
#else
	if (!strcmp(request.path, "/v1/lcd") && request.content_length >= 4 && request.content_length <= LCD_COMMAND_MAX_SIZE) {
		if (gateway_ota_busy()) { respond(client, 409, "OTA busy", 8, "text/plain"); return; }
		/* Entry is only available through the fully preflighted OTA operation. */
		if (body[1] == LCD_OP_ENTER_DFU) { respond(client, 400, "use OTA API", 11, "text/plain"); return; }
		uint8_t status[LCD_STATUS_SIZE];
		rc = gateway_lcd_command(body, request.content_length, status, 4000);
		if (!rc) { respond(client, 200, status, sizeof(status), "application/octet-stream"); return; }
	} else if (!strcmp(request.path, "/v1/ota") && request.content_length == OTA_DESCRIPTOR_SIZE) {
		rc = gateway_ota_start(source_ip, body);
	} else if (!strcmp(request.path, "/v1/ota/cancel") && !request.content_length) {
		rc = gateway_ota_cancel();
	}
#endif
	char result[64]; int n = snprintk(result, sizeof(result), "{\"error\":%d}", rc);
	respond(client, rc ? (rc == -EBUSY ? 409 : rc == -ENOTSUP ? 405 : 422) : 202, result, n, "application/json");
}

static bool handle(int fd, uint32_t source_ip, bool allow_keep_alive,
		   int64_t idle_deadline, int64_t session_deadline)
{
	struct http_client state = { .fd = fd, .session_deadline = session_deadline };
	struct http_client *client = &state;
	char header[HTTP_HEADER_CAPACITY];
	size_t used = 0;
	int64_t deadline = k_uptime_get() + 5000;
	while (used < sizeof(header) - 1) {
		if (http_read_before(fd, header + used, 1,
				     used ? deadline : idle_deadline)) return false;
		used++;
		header[used] = 0;
		if (used >= 4 && !memcmp(header + used - 4, "\r\n\r\n", 4)) break;
	}
	struct http_request request;
	if (used == sizeof(header) - 1 || http_parse_request(header, &request)) {
		respond(client, 400, "invalid request", 15, "text/plain");
		return false;
	}
	/* Authenticate every request, even on a previously authenticated socket.
	 * Errors always close: an unread rejected body cannot become a new header. */
	if (!authenticated(request.bearer)) {
		respond(client, 401, "unauthorized", 12, "text/plain");
		return false;
	}
	client->keep_alive = allow_keep_alive && request.keep_alive;
	route(client, source_ip, request, deadline);
	return client->responded && client->keep_alive;
}

static void serve_client(int fd, uint32_t source_ip)
{
	int64_t session_deadline = k_uptime_get() + 4000;
	/* Bound ownership of the serial listener, including an abandoned socket.
	 * Old clients retain the original one-request/Connection: close behavior. */
	for (unsigned i = 0; i < 8; i++) {
		int64_t idle_deadline = k_uptime_get() + (i ? 750 : 5000);
		if (!handle(fd, source_ip, i < 7, idle_deadline, session_deadline)) break;
	}
}
void gateway_http_run(void)
{
	bool stack_reported = false;
	while (!gateway_wifi_ipv4_ready()) k_sleep(K_SECONDS(1));
	int listener = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener < 0) return;
	struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(CONFIG_GATEWAY_HTTP_PORT),
		.sin_addr.s_addr = INADDR_ANY };
	if (zsock_bind(listener, (struct sockaddr *)&address, sizeof(address)) || zsock_listen(listener, 2)) {
		zsock_close(listener); return;
	}
	char gateway_ip[16];
	if (gateway_wifi_address(gateway_ip, sizeof(gateway_ip)) >= 0) {
		/* Report again after the listener starts. Vendor startup prints can
		 * interleave with the earlier DHCP log on the shared console. */
		LOG_INF("NU87 Wi-Fi LCD gateway: http://%s:%d/", gateway_ip,
			CONFIG_GATEWAY_HTTP_PORT);
	}
	for (;;) {
		struct sockaddr_in peer; socklen_t length = sizeof(peer);
		int fd = zsock_accept(listener, (struct sockaddr *)&peer, &length);
		if (fd < 0) { k_sleep(K_MSEC(100)); continue; }
		if (zsock_fcntl(fd, F_SETFL, O_NONBLOCK) < 0) { zsock_close(fd); continue; }
		struct timeval timeout = { .tv_sec = 5 };
		zsock_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
		zsock_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
		int no_delay = 1;
		zsock_setsockopt(fd, IPPROTO_TCP, ZSOCK_TCP_NODELAY, &no_delay, sizeof(no_delay));
		serve_client(fd, peer.sin_addr.s_addr);
		zsock_close(fd);
		if (!stack_reported) {
			size_t unused;
			if (k_thread_stack_space_get(k_current_get(), &unused) == 0) {
				LOG_INF("First HTTP request complete; main stack unused: %zu bytes", unused);
				stack_reported = true;
			}
		}
	}
}
