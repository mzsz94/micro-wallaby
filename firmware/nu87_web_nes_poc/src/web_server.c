/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/websocket.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/mem_stats.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/sys/util.h>

#include <microwallaby_web/input_protocol.h>
#include <microwallaby_web/joystick.h>
#include <microwallaby_web/web_server.h>
#include <microwallaby_web/websocket_control.h>
#include <microwallaby_web/wifi_sta.h>

LOG_MODULE_REGISTER(mw_web_server, CONFIG_MW_WEB_LOG_LEVEL);

#define WS_BUFFER_SIZE 256
#define WS_PAYLOAD_SIZE 192
#define WS_CONTROL_PAYLOAD_SIZE 125
#define STATUS_PAYLOAD_SIZE 448

extern struct k_heap _system_heap;

static const uint8_t index_html_gz[] = {
#include "mw_index.html.gz.inc"
};

static const uint8_t app_js_gz[] = {
#include "mw_app.js.gz.inc"
};

static const uint8_t controller_js_gz[] = {
#include "mw_controller.js.gz.inc"
};

static const uint8_t jsnes_js_gz[] = {
#include "mw_jsnes.min.js.gz.inc"
};

static struct http_resource_detail_static index_resource_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_STATIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_encoding = "gzip",
		.content_type = "text/html",
	},
	.static_data = index_html_gz,
	.static_data_len = sizeof(index_html_gz),
};

static struct http_resource_detail_static app_resource_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_STATIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_encoding = "gzip",
		.content_type = "text/javascript",
	},
	.static_data = app_js_gz,
	.static_data_len = sizeof(app_js_gz),
};

static struct http_resource_detail_static controller_resource_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_STATIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_encoding = "gzip",
		.content_type = "text/javascript",
	},
	.static_data = controller_js_gz,
	.static_data_len = sizeof(controller_js_gz),
};

static struct http_resource_detail_static jsnes_resource_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_STATIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_encoding = "gzip",
		.content_type = "text/javascript",
	},
	.static_data = jsnes_js_gz,
	.static_data_len = sizeof(jsnes_js_gz),
};

static int active_websocket = -1;
static atomic_t websocket_connected;
static atomic_t packets_sent;
static atomic_t send_errors;
static atomic_t stale_input_frames;

K_MUTEX_DEFINE(websocket_lock);
K_SEM_DEFINE(websocket_ready, 0, 1);

static int system_heap_stats_get(struct sys_memory_stats *stats)
{
	k_spinlock_key_t key = k_spin_lock(&_system_heap.lock);
	int ret = sys_heap_runtime_stats_get(&_system_heap.heap, stats);

	k_spin_unlock(&_system_heap.lock, key);
	return ret;
}

static int status_handler(struct http_client_ctx *client,
			  enum http_transaction_status status,
			  const struct http_request_ctx *request_ctx,
			  struct http_response_ctx *response_ctx, void *user_data)
{
	static char response[STATUS_PAYLOAD_SIZE];
	struct mw_joystick_stats input_stats;
	struct sys_memory_stats heap_stats = {0};
	struct mw_web_server_stats stats;
	size_t heap_min_free = 0U;
	int heap_result;
	int written;

	ARG_UNUSED(client);
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	mw_web_server_get_stats(&stats);
	mw_joystick_get_stats(&input_stats);
	heap_result = system_heap_stats_get(&heap_stats);
	if (heap_result == 0 &&
	    heap_stats.free_bytes + heap_stats.allocated_bytes >=
		    heap_stats.max_allocated_bytes) {
		heap_min_free = heap_stats.free_bytes + heap_stats.allocated_bytes -
			heap_stats.max_allocated_bytes;
	}
	written = snprintk(response, sizeof(response),
			    "{\"v\":1,\"uptime_ms\":%" PRId64
			    ",\"wifi_ipv4\":%s,\"joystick_ws\":%s,"
			    "\"packets_sent\":%" PRIu32 ",\"send_errors\":%" PRIu32
			    ",\"stale_input_frames\":%" PRIu32
			    ",\"input_samples\":%" PRIu32 ",\"input_errors\":%" PRIu32
			    ",\"deadline_misses\":%" PRIu32 ",\"heap_stats_ok\":%s"
			    ",\"heap_free_bytes\":%zu,\"heap_allocated_bytes\":%zu"
			    ",\"heap_peak_allocated_bytes\":%zu,\"heap_min_free_bytes\":%zu}",
			    k_uptime_get(), mw_wifi_sta_ipv4_ready() ? "true" : "false",
			    stats.client_connected ? "true" : "false", stats.packets_sent,
			    stats.send_errors, stats.stale_input_frames, input_stats.samples,
			    input_stats.input_errors,
			    input_stats.deadline_misses, heap_result == 0 ? "true" : "false",
			    heap_stats.free_bytes, heap_stats.allocated_bytes,
			    heap_stats.max_allocated_bytes, heap_min_free);
	if (written < 0 || (size_t)written >= sizeof(response)) {
		return -ENOSPC;
	}

	response_ctx->body = (const uint8_t *)response;
	response_ctx->body_len = written;
	response_ctx->final_chunk = true;
	return 0;
}

static struct http_resource_detail_dynamic status_resource_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_type = "application/json",
	},
	.cb = status_handler,
};

static uint8_t websocket_buffer[WS_BUFFER_SIZE];

static int websocket_setup(int socket, struct http_request_ctx *request_ctx,
			   void *user_data)
{
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	k_mutex_lock(&websocket_lock, K_FOREVER);
	if (active_websocket >= 0) {
		k_mutex_unlock(&websocket_lock);
		LOG_WRN("Rejecting a second joystick WebSocket client");
		return -EBUSY;
	}
	active_websocket = socket;
	atomic_set(&websocket_connected, 1);
	k_mutex_unlock(&websocket_lock);

	k_sem_give(&websocket_ready);
	LOG_INF("Joystick WebSocket client connected");
	return 0;
}

static struct http_resource_detail_websocket websocket_resource_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_WEBSOCKET,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
	},
	.cb = websocket_setup,
	.data_buffer = websocket_buffer,
	.data_buffer_len = sizeof(websocket_buffer),
};

static int current_websocket(void)
{
	int socket;

	k_mutex_lock(&websocket_lock, K_FOREVER);
	socket = active_websocket;
	k_mutex_unlock(&websocket_lock);
	return socket;
}

static void close_websocket(int socket)
{
	bool owned = false;

	k_mutex_lock(&websocket_lock, K_FOREVER);
	if (active_websocket == socket) {
		active_websocket = -1;
		atomic_clear(&websocket_connected);
		owned = true;
	}
	k_mutex_unlock(&websocket_lock);

	if (owned) {
		(void)websocket_unregister(socket);
	}
}

static int service_websocket_rx(int socket)
{
	uint8_t payload[WS_CONTROL_PAYLOAD_SIZE];
	uint32_t message_type = 0U;
	uint64_t remaining = 0U;
	int ret = websocket_recv_msg(socket, payload, sizeof(payload), &message_type,
				     &remaining, 0);
	int validation_ret;

	if (ret == -EAGAIN) {
		return 0;
	}
	if (ret < 0) {
		return ret;
	}
	validation_ret = mw_websocket_control_frame_validate(
		(message_type & WEBSOCKET_FLAG_FINAL) != 0U,
		(message_type & WEBSOCKET_FLAG_CLOSE) != 0U,
		(message_type & WEBSOCKET_FLAG_PING) != 0U,
		(message_type & WEBSOCKET_FLAG_PONG) != 0U, remaining);
	if (validation_ret < 0) {
		return validation_ret;
	}
	if ((message_type & WEBSOCKET_FLAG_CLOSE) != 0U) {
		return -ENOTCONN;
	}
	if ((message_type & WEBSOCKET_FLAG_PING) != 0U) {
		ret = websocket_send_msg(socket, payload, (size_t)ret,
					 WEBSOCKET_OPCODE_PONG, false, true,
					 CONFIG_MW_WEB_SEND_TIMEOUT_MS);
		return ret < 0 ? ret : 0;
	}
	if ((message_type & WEBSOCKET_FLAG_PONG) != 0U) {
		return 0;
	}

	return -EPROTO;
}

static void websocket_sender(void *unused1, void *unused2, void *unused3)
{
	char payload[WS_PAYLOAD_SIZE];
	uint32_t transmit_sequence = 0U;

	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	while (true) {
		int socket;

		k_sem_take(&websocket_ready, K_FOREVER);
		while ((socket = current_websocket()) >= 0) {
			struct mw_joystick_sample sample;
			int64_t sent_ms;
			int length;
			int ret;

			ret = service_websocket_rx(socket);
			if (ret < 0) {
				if (ret == -ENOTCONN) {
					LOG_INF("Joystick WebSocket peer closed");
				} else {
					atomic_inc(&send_errors);
					LOG_WRN("WebSocket receive/control error: %d", ret);
				}
				close_websocket(socket);
				break;
			}

			if (!mw_joystick_latest(&sample)) {
				k_sleep(K_MSEC(CONFIG_MW_WEB_INPUT_PERIOD_MS));
				continue;
			}

			sent_ms = k_uptime_get();
			if (mw_input_sample_sanitize(&sample, sent_ms,
						     CONFIG_MW_INPUT_STALE_TIMEOUT_MS)) {
				atomic_inc(&stale_input_frames);
			}

			/* Advance for every emission attempt. If formatting or sending fails,
			 * the next frame lets a connected/reconnected browser observe the gap.
			 */
			transmit_sequence++;
			length = mw_input_event_format(payload, sizeof(payload), &sample,
						       transmit_sequence, sent_ms);
			if (length < 0) {
				atomic_inc(&send_errors);
				LOG_ERR("Input event formatting failed: %d", length);
				k_sleep(K_MSEC(CONFIG_MW_WEB_INPUT_PERIOD_MS));
				continue;
			}

			ret = websocket_send_msg(socket, (const uint8_t *)payload, length,
						 WEBSOCKET_OPCODE_DATA_TEXT, false, true,
						 CONFIG_MW_WEB_SEND_TIMEOUT_MS);
			if (ret < 0) {
				atomic_inc(&send_errors);
				LOG_WRN("Joystick WebSocket closed: %d", ret);
				close_websocket(socket);
				break;
			}

			atomic_inc(&packets_sent);
			k_sleep(K_MSEC(CONFIG_MW_WEB_INPUT_PERIOD_MS));
		}
	}
}

K_THREAD_DEFINE(websocket_sender_thread, CONFIG_MW_WEB_SOCKET_STACK_SIZE,
		websocket_sender, NULL, NULL, NULL, CONFIG_MW_WEB_SOCKET_PRIORITY, 0, 0);

static uint16_t http_port = CONFIG_MW_WEB_HTTP_PORT;
HTTP_SERVICE_DEFINE(mw_http_service, NULL, &http_port,
		    CONFIG_HTTP_SERVER_MAX_CLIENTS, 2, NULL, NULL, NULL);

HTTP_RESOURCE_DEFINE(index_resource, mw_http_service, "/", &index_resource_detail);
HTTP_RESOURCE_DEFINE(app_resource, mw_http_service, "/app.js", &app_resource_detail);
HTTP_RESOURCE_DEFINE(controller_resource, mw_http_service, "/controller.js",
		     &controller_resource_detail);
HTTP_RESOURCE_DEFINE(jsnes_resource, mw_http_service, "/jsnes.min.js",
		     &jsnes_resource_detail);
HTTP_RESOURCE_DEFINE(status_resource, mw_http_service, "/api/status",
		     &status_resource_detail);
HTTP_RESOURCE_DEFINE(websocket_resource, mw_http_service, "/ws/input",
		     &websocket_resource_detail);

int mw_web_server_start(void)
{
	int ret = http_server_start();

	if (ret < 0) {
		LOG_ERR("HTTP server failed to start: %d", ret);
		return ret;
	}

	LOG_INF("HTTP/WebSocket server listening on port %u", http_port);
	return 0;
}

void mw_web_server_get_stats(struct mw_web_server_stats *stats)
{
	if (stats == NULL) {
		return;
	}

	*stats = (struct mw_web_server_stats) {
		.packets_sent = (uint32_t)atomic_get(&packets_sent),
		.send_errors = (uint32_t)atomic_get(&send_errors),
		.stale_input_frames = (uint32_t)atomic_get(&stale_input_frames),
		.client_connected = atomic_get(&websocket_connected) != 0,
	};
}
