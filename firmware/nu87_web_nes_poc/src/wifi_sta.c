/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <microwallaby_web/wifi_sta.h>

LOG_MODULE_REGISTER(mw_wifi_sta, CONFIG_MW_WEB_LOG_LEVEL);

#define WIFI_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT)
#define IPV4_EVENTS (NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IPV4_ADDR_DEL)

enum wifi_phase {
	WIFI_PHASE_IDLE,
	WIFI_PHASE_RETRY_WAIT,
	WIFI_PHASE_CONNECTING,
	WIFI_PHASE_WAIT_DHCP,
	WIFI_PHASE_CANCEL_PENDING,
	WIFI_PHASE_DISCONNECTING,
	WIFI_PHASE_READY,
};

struct dhcp_address_search {
	bool found;
	char address[NET_IPV4_ADDR_LEN];
};

static struct net_if *sta_iface;
static struct net_mgmt_event_callback wifi_callback;
static struct net_mgmt_event_callback ipv4_callback;
static struct k_work_q wifi_work_queue;
static struct k_work_delayable reconnect_work;
static struct k_work disconnect_work;
static struct k_work_delayable connect_timeout_work;
static enum wifi_phase phase;
static uint32_t generation;
static uint32_t timeout_generation;
static int64_t timeout_deadline_ms;
static uint32_t cancel_generation;
static bool connect_result_ok;
static bool address_valid;
static atomic_t ipv4_ready;
static char ipv4_address[NET_IPV4_ADDR_LEN];

K_MUTEX_DEFINE(state_lock);
K_THREAD_STACK_DEFINE(wifi_work_stack, CONFIG_MW_WIFI_WORK_STACK_SIZE);

static enum wifi_security_type configured_security(void)
{
#if defined(CONFIG_MW_WIFI_SECURITY_OPEN)
	return WIFI_SECURITY_TYPE_NONE;
#else
	return WIFI_SECURITY_TYPE_PSK;
#endif
}

BUILD_ASSERT(!IS_ENABLED(CONFIG_MW_WIFI_SECURITY_PSK) ||
		     IS_ENABLED(CONFIG_MW_ALLOW_INSECURE_TEST_PSK),
		     "WPA2-PSK requires explicit CONFIG_MW_ALLOW_INSECURE_TEST_PSK=y");

static void clear_ipv4_locked(void)
{
	address_valid = false;
	ipv4_address[0] = '\0';
	atomic_clear(&ipv4_ready);
}

static bool mark_ready_if_complete_locked(void)
{
	bool became_ready = false;

	if (connect_result_ok && address_valid &&
	    (phase == WIFI_PHASE_CONNECTING || phase == WIFI_PHASE_WAIT_DHCP ||
	     phase == WIFI_PHASE_READY)) {
		became_ready = atomic_get(&ipv4_ready) == 0;
		phase = WIFI_PHASE_READY;
		atomic_set(&ipv4_ready, 1);
	}

	return became_ready;
}

static void schedule_connect(k_timeout_t delay)
{
	int ret = k_work_reschedule_for_queue(&wifi_work_queue, &reconnect_work, delay);

	if (ret < 0) {
		LOG_ERR("Failed to schedule Wi-Fi connection work: %d", ret);
	}
}

static void schedule_reconnect(void)
{
	schedule_connect(K_SECONDS(CONFIG_MW_WIFI_CONNECT_RETRY_SECONDS));
}

static int arm_connect_timeout_locked(void)
{
	timeout_generation = generation;
	timeout_deadline_ms =
		k_uptime_get() + (CONFIG_MW_WIFI_CONNECT_TIMEOUT_SECONDS * MSEC_PER_SEC);

	return k_work_reschedule(
		&connect_timeout_work, K_SECONDS(CONFIG_MW_WIFI_CONNECT_TIMEOUT_SECONDS));
}

static void log_ready_address(void)
{
	char address[NET_IPV4_ADDR_LEN];

	if (mw_wifi_sta_address(address, sizeof(address)) >= 0) {
		LOG_INF("Open http://%s:%d/ on a device connected to the same network", address,
			CONFIG_MW_WEB_HTTP_PORT);
	}
}

static void disconnect_work_handler(struct k_work *work)
{
	uint32_t work_generation;
	bool retry = false;
	int ret;

	ARG_UNUSED(work);

	k_mutex_lock(&state_lock, K_FOREVER);
	work_generation = cancel_generation;
	if (phase != WIFI_PHASE_CANCEL_PENDING || work_generation != generation) {
		k_mutex_unlock(&state_lock);
		return;
	}
	phase = WIFI_PHASE_DISCONNECTING;
	k_mutex_unlock(&state_lock);

	ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, sta_iface, NULL, 0);
	if (ret >= 0) {
		/* The disconnect-result event is the barrier that permits a retry. */
		return;
	}

	LOG_ERR("Wi-Fi disconnect request failed: %d", ret);
	k_mutex_lock(&state_lock, K_FOREVER);
	if (generation == work_generation && phase == WIFI_PHASE_DISCONNECTING) {
		phase = WIFI_PHASE_RETRY_WAIT;
		connect_result_ok = false;
		clear_ipv4_locked();
		retry = true;
	}
	k_mutex_unlock(&state_lock);

	if (retry) {
		schedule_reconnect();
	}
}

static void connect_timeout_handler(struct k_work *work)
{
	uint32_t work_generation;
	int64_t remaining_ms;
	bool cancel = false;
	bool rearmed = false;
	int ret = 0;

	ARG_UNUSED(work);

	k_mutex_lock(&state_lock, K_FOREVER);
	work_generation = timeout_generation;
	if (work_generation == generation &&
	    (phase == WIFI_PHASE_CONNECTING || phase == WIFI_PHASE_WAIT_DHCP)) {
		remaining_ms = timeout_deadline_ms - k_uptime_get();
		if (remaining_ms > 0) {
			/*
			 * A cancelled handler from an older generation may already be
			 * running. Honor the current generation's absolute deadline.
			 */
			ret = k_work_reschedule(&connect_timeout_work, K_MSEC(remaining_ms));
			rearmed = true;
		} else {
			phase = WIFI_PHASE_CANCEL_PENDING;
			cancel_generation = work_generation;
			connect_result_ok = false;
			clear_ipv4_locked();
			cancel = true;
		}
	}
	k_mutex_unlock(&state_lock);

	if (rearmed) {
		if (ret < 0) {
			LOG_ERR("Failed to rearm Wi-Fi timeout: %d", ret);
		}
		return;
	}

	if (!cancel) {
		return;
	}

	LOG_WRN("Wi-Fi/DHCP timed out after %d seconds; cancelling generation %u",
		CONFIG_MW_WIFI_CONNECT_TIMEOUT_SECONDS, work_generation);
	ret = k_work_submit_to_queue(&wifi_work_queue, &disconnect_work);
	if (ret < 0) {
		LOG_ERR("Failed to queue Wi-Fi disconnect work: %d", ret);
	}
}

static void connect_work_handler(struct k_work *work)
{
	struct wifi_connect_req_params params = {
		.ssid = (const uint8_t *)CONFIG_MW_WIFI_SSID,
		.ssid_length = sizeof(CONFIG_MW_WIFI_SSID) - 1U,
		.security = configured_security(),
		.channel = WIFI_CHANNEL_ANY,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
	};
	uint32_t work_generation;
	bool retry = false;
	int timeout_ret;
	int ret;

	ARG_UNUSED(work);

	k_mutex_lock(&state_lock, K_FOREVER);
	if (phase != WIFI_PHASE_IDLE && phase != WIFI_PHASE_RETRY_WAIT) {
		k_mutex_unlock(&state_lock);
		return;
	}
	work_generation = ++generation;
	phase = WIFI_PHASE_CONNECTING;
	connect_result_ok = false;
	clear_ipv4_locked();
	timeout_ret = arm_connect_timeout_locked();
	k_mutex_unlock(&state_lock);

	if (params.security != WIFI_SECURITY_TYPE_NONE) {
		params.psk = (const uint8_t *)CONFIG_MW_WIFI_PSK;
		params.psk_length = sizeof(CONFIG_MW_WIFI_PSK) - 1U;
	}

	if (timeout_ret < 0) {
		LOG_ERR("Failed to schedule Wi-Fi timeout: %d", timeout_ret);
	}

	LOG_INF("Connecting generation %u to configured isolated test AP (SSID length %zu)",
		work_generation, sizeof(CONFIG_MW_WIFI_SSID) - 1U);
	ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, sta_iface, &params, sizeof(params));

	k_mutex_lock(&state_lock, K_FOREVER);
	if (generation != work_generation) {
		k_mutex_unlock(&state_lock);
		return;
	}

	if (ret < 0 && phase == WIFI_PHASE_CANCEL_PENDING) {
		/* The synchronous connect failed after its timeout. No disconnect is needed. */
		phase = WIFI_PHASE_RETRY_WAIT;
		connect_result_ok = false;
		clear_ipv4_locked();
		retry = true;
	} else if (ret < 0 && phase == WIFI_PHASE_CONNECTING) {
		phase = WIFI_PHASE_RETRY_WAIT;
		connect_result_ok = false;
		clear_ipv4_locked();
		retry = true;
	} else if (ret >= 0 && phase == WIFI_PHASE_CONNECTING) {
		/* Ameba starts DHCP after its synchronous association call returns. */
		phase = WIFI_PHASE_WAIT_DHCP;
	}
	k_mutex_unlock(&state_lock);

	if (ret < 0) {
		LOG_ERR("Wi-Fi connect request failed: %d", ret);
	}
	if (retry) {
		(void)k_work_cancel_delayable(&connect_timeout_work);
		schedule_reconnect();
	}
}

static void wifi_event_handler(struct net_mgmt_event_callback *callback,
			       uint64_t event, struct net_if *iface)
{
	const struct wifi_status *status = callback->info;
	bool cancel_timeout = false;
	bool ready = false;
	bool retry = false;

	if (iface != sta_iface) {
		return;
	}

	if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
		int result = status != NULL ? status->status : -EIO;

		k_mutex_lock(&state_lock, K_FOREVER);
		if (phase != WIFI_PHASE_CONNECTING && phase != WIFI_PHASE_WAIT_DHCP) {
			uint32_t late_generation = generation;

			k_mutex_unlock(&state_lock);
			LOG_WRN("Ignoring late Wi-Fi result for generation %u", late_generation);
			return;
		}

		if (result != 0) {
			phase = WIFI_PHASE_RETRY_WAIT;
			connect_result_ok = false;
			clear_ipv4_locked();
			cancel_timeout = true;
			retry = true;
		} else {
			connect_result_ok = true;
			phase = WIFI_PHASE_WAIT_DHCP;
			ready = mark_ready_if_complete_locked();
			cancel_timeout = ready;
		}
		k_mutex_unlock(&state_lock);

		if (result != 0) {
			LOG_ERR("Wi-Fi association/DHCP failed: %d", result);
		} else if (!ready) {
			LOG_INF("Wi-Fi DHCP completed; waiting for the IPv4 address event");
		}
	} else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		k_mutex_lock(&state_lock, K_FOREVER);
		if (phase == WIFI_PHASE_DISCONNECTING || phase == WIFI_PHASE_WAIT_DHCP ||
		    phase == WIFI_PHASE_READY) {
			phase = WIFI_PHASE_RETRY_WAIT;
			connect_result_ok = false;
			clear_ipv4_locked();
			cancel_timeout = true;
			retry = true;
		}
		k_mutex_unlock(&state_lock);

		if (retry) {
			LOG_WRN("Wi-Fi disconnected; retrying in %d seconds",
				CONFIG_MW_WIFI_CONNECT_RETRY_SECONDS);
		}
	}

	if (cancel_timeout) {
		(void)k_work_cancel_delayable(&connect_timeout_work);
	}
	if (ready) {
		LOG_INF("Wi-Fi connected; Ameba DHCP negotiation completed");
		log_ready_address();
	}
	if (retry) {
		schedule_reconnect();
	}
}

static void find_dhcp_address(struct net_if *iface, struct net_if_addr *if_addr,
			      void *user_data)
{
	struct dhcp_address_search *search = user_data;

	ARG_UNUSED(iface);

	if (search->found || if_addr->addr_type != NET_ADDR_DHCP) {
		return;
	}

	if (net_addr_ntop(NET_AF_INET, &if_addr->address.in_addr, search->address,
			  sizeof(search->address)) != NULL) {
		search->found = true;
	}
}

static void ipv4_event_handler(struct net_mgmt_event_callback *callback,
			       uint64_t event, struct net_if *iface)
{
	bool cancel_timeout = false;
	bool ready = false;

	ARG_UNUSED(callback);

	if (iface != sta_iface) {
		return;
	}

	if (event == NET_EVENT_IPV4_ADDR_DEL) {
		bool restart_timeout = false;
		int timeout_ret = 0;

		k_mutex_lock(&state_lock, K_FOREVER);
		clear_ipv4_locked();
		if (phase == WIFI_PHASE_READY) {
			phase = WIFI_PHASE_WAIT_DHCP;
			connect_result_ok = false;
			timeout_ret = arm_connect_timeout_locked();
			restart_timeout = true;
		}
		k_mutex_unlock(&state_lock);

		if (restart_timeout && timeout_ret < 0) {
			LOG_ERR("Failed to schedule DHCP recovery timeout: %d", timeout_ret);
		}
		return;
	}

	if (event == NET_EVENT_IPV4_ADDR_ADD) {
		struct dhcp_address_search search = {0};

		net_if_ipv4_addr_foreach(iface, find_dhcp_address, &search);
		if (!search.found) {
			return;
		}

		k_mutex_lock(&state_lock, K_FOREVER);
		if (phase == WIFI_PHASE_CONNECTING || phase == WIFI_PHASE_WAIT_DHCP ||
		    phase == WIFI_PHASE_READY) {
			snprintk(ipv4_address, sizeof(ipv4_address), "%s", search.address);
			address_valid = true;
			ready = mark_ready_if_complete_locked();
			cancel_timeout = ready;
		}
		k_mutex_unlock(&state_lock);
	}

	if (cancel_timeout) {
		(void)k_work_cancel_delayable(&connect_timeout_work);
	}
	if (ready) {
		LOG_INF("Wi-Fi connected; Ameba DHCP negotiation completed");
		log_ready_address();
	}
}

int mw_wifi_sta_start(void)
{
	const size_t ssid_length = sizeof(CONFIG_MW_WIFI_SSID) - 1U;
	const size_t psk_length = sizeof(CONFIG_MW_WIFI_PSK) - 1U;
	const struct k_work_queue_config queue_config = {
		.name = "mw_wifi",
	};
	enum wifi_security_type security = configured_security();

	if (ssid_length == 0U) {
		LOG_ERR("CONFIG_MW_WIFI_SSID is empty; copy wifi.example.conf to "
			"wifi.conf and set local credentials");
		return -EINVAL;
	}
	if (ssid_length > WIFI_SSID_MAX_LEN) {
		LOG_ERR("Wi-Fi SSID is too long (%zu > %d)", ssid_length,
			WIFI_SSID_MAX_LEN);
		return -EINVAL;
	}
	if (security != WIFI_SECURITY_TYPE_NONE &&
	    (psk_length < WIFI_PSK_MIN_LEN || psk_length > WIFI_PSK_MAX_LEN)) {
		LOG_ERR("Wi-Fi password length must be %d..%d for WPA2-PSK",
			WIFI_PSK_MIN_LEN, WIFI_PSK_MAX_LEN);
		return -EINVAL;
	}
	if (security == WIFI_SECURITY_TYPE_PSK) {
		LOG_WRN("TEST ONLY: WPA2 uses an unverified AmebaD random source; "
			"use a disposable credential on an isolated AP");
	} else {
		LOG_WRN("TEST ONLY: open Wi-Fi and plain HTTP provide no confidentiality");
	}

	sta_iface = net_if_get_wifi_sta();
	if (sta_iface == NULL) {
		LOG_ERR("No Wi-Fi STA interface; verify both wifi0 and wifi1 are enabled "
			"and Realtek blobs are present");
		return -ENODEV;
	}

	k_work_queue_init(&wifi_work_queue);
	k_work_queue_start(&wifi_work_queue, wifi_work_stack,
			   K_THREAD_STACK_SIZEOF(wifi_work_stack),
			   K_PRIO_PREEMPT(CONFIG_MW_WIFI_WORK_PRIORITY), &queue_config);
	k_work_init_delayable(&reconnect_work, connect_work_handler);
	k_work_init(&disconnect_work, disconnect_work_handler);
	k_work_init_delayable(&connect_timeout_work, connect_timeout_handler);

	net_mgmt_init_event_callback(&wifi_callback, wifi_event_handler, WIFI_EVENTS);
	net_mgmt_add_event_callback(&wifi_callback);
	net_mgmt_init_event_callback(&ipv4_callback, ipv4_event_handler, IPV4_EVENTS);
	net_mgmt_add_event_callback(&ipv4_callback);

	k_mutex_lock(&state_lock, K_FOREVER);
	phase = WIFI_PHASE_RETRY_WAIT;
	connect_result_ok = false;
	clear_ipv4_locked();
	k_mutex_unlock(&state_lock);
	schedule_connect(K_MSEC(CONFIG_MW_WIFI_CONNECT_DELAY_MS));

	return 0;
}

bool mw_wifi_sta_ipv4_ready(void)
{
	return atomic_get(&ipv4_ready) != 0;
}

int mw_wifi_sta_address(char *buffer, size_t capacity)
{
	int written;

	if (buffer == NULL || capacity == 0U) {
		return -EINVAL;
	}

	k_mutex_lock(&state_lock, K_FOREVER);
	if (phase != WIFI_PHASE_READY || !address_valid || atomic_get(&ipv4_ready) == 0) {
		k_mutex_unlock(&state_lock);
		return -ENETDOWN;
	}
	written = snprintf(buffer, capacity, "%s", ipv4_address);
	k_mutex_unlock(&state_lock);
	if (written < 0) {
		return written;
	}
	if ((size_t)written >= capacity) {
		return -ENOSPC;
	}

	return written;
}
