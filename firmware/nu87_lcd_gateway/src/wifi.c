/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "ameba_wifi.h"
#include "gateway_link.h"

LOG_MODULE_REGISTER(gateway_wifi_sta, CONFIG_GATEWAY_LOG_LEVEL);

#define WIFI_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT)
#define IPV4_EVENTS (NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IPV4_ADDR_DEL)
#define DISCONNECT_POLL_MS 250
#define DISCONNECT_TIMEOUT_MS 10000

/* Values from the pinned lib_rtw_event.a rtw_join_status enum. The public
 * wifi_get_join_status() query returns these actual radio states; the
 * Zephyr driver's cached RTK_STA_STOPPED is set before disassociation ends.
 */
enum ameba_join_status {
	AMEBA_JOIN_SUCCESS = 9,
	AMEBA_JOIN_FAIL = 10,
	AMEBA_JOIN_DISCONNECT = 11,
};

enum wifi_phase {
	WIFI_PHASE_IDLE,
	WIFI_PHASE_RETRY_WAIT,
	WIFI_PHASE_CONNECTING,
	WIFI_PHASE_WAIT_DHCP,
	WIFI_PHASE_CANCEL_PENDING,
	WIFI_PHASE_DISCONNECTING,
	WIFI_PHASE_RETRY_BLOCKED,
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
static struct k_work_delayable disconnect_check_work;
static struct k_work_delayable connect_timeout_work;
static enum wifi_phase phase;
static uint32_t generation;
static uint32_t timeout_generation;
static int64_t timeout_deadline_ms;
static uint32_t cancel_generation;
static int64_t disconnect_deadline_ms;
static unsigned int disconnect_idle_samples;
static bool association_complete;
static bool connect_result_ok;
static bool channel_hint_tried;
static bool address_valid;
static atomic_t ipv4_ready;
static char ipv4_address[NET_IPV4_ADDR_LEN];
static struct net_eth_addr sta_runtime_mac;

K_MUTEX_DEFINE(state_lock);
K_THREAD_STACK_DEFINE(wifi_work_stack, CONFIG_GATEWAY_WIFI_WORK_STACK_SIZE);

static enum wifi_security_type configured_security(void)
{
#if defined(CONFIG_GATEWAY_WIFI_SECURITY_OPEN)
	return WIFI_SECURITY_TYPE_NONE;
#else
	return WIFI_SECURITY_TYPE_PSK;
#endif
}

BUILD_ASSERT(!IS_ENABLED(CONFIG_GATEWAY_WIFI_SECURITY_PSK) ||
		     IS_ENABLED(CONFIG_GATEWAY_ALLOW_INSECURE_TEST_PSK),
		     "WPA2-PSK requires explicit CONFIG_GATEWAY_ALLOW_INSECURE_TEST_PSK=y");

BUILD_ASSERT(sizeof(struct _rtw_mac_t) == NET_ETH_ADDR_LEN,
	     "Pinned Ameba MAC ABI must contain exactly six bytes");

static int prepare_sta_link_address(void)
{
	struct _rtw_mac_t radio_mac = {0};
	struct net_eth_addr candidate;
	struct net_linkaddr *current;
	int ret;

	/* efuse=0 asks the driver for the address actually used on air. This
	 * vendor call requires an initialized adapter; do not call it early.
	 */
	if (wifi_is_running(0) <= 0) {
		LOG_WRN("Wi-Fi radio is not initialized yet; retrying after startup");
		return -EAGAIN;
	}
	ret = wifi_get_mac_address(0, &radio_mac, 0);
	if (ret != 0) {
		LOG_ERR("Could not read the active Wi-Fi STA MAC address: %d", ret);
		return -EIO;
	}
	memcpy(candidate.addr, radio_mac.octet, sizeof(candidate.addr));
	if (!net_eth_is_addr_valid(&candidate)) {
		LOG_ERR("Active Wi-Fi STA MAC address is invalid; refusing interface startup");
		return -EINVAL;
	}
	current = net_if_get_link_addr(sta_iface);
	if (current->len == sizeof(candidate.addr) &&
	    memcmp(current->addr, candidate.addr, sizeof(candidate.addr)) == 0) {
		return 0;
	}
	/* The pinned driver initially reads eFuse before asynchronous radio
	 * initialization finishes. A board using a runtime fallback MAC must
	 * publish that same address to Zephyr before Ethernet validates it.
	 */
	if (net_if_is_admin_up(sta_iface)) {
		ret = net_if_down(sta_iface);
		if (ret < 0 && ret != -EALREADY) {
			LOG_ERR("Could not pause STA to synchronize its MAC address: %d", ret);
			return ret;
		}
	}
	sta_runtime_mac = candidate;
	ret = net_if_set_link_addr(sta_iface, sta_runtime_mac.addr,
				  sizeof(sta_runtime_mac.addr), NET_LINK_ETHERNET);
	if (ret < 0) {
		LOG_ERR("Could not synchronize the active STA MAC with Zephyr: %d", ret);
		return ret;
	}
	LOG_INF("Wi-Fi STA link address synchronized with the active radio");
	return 0;
}

static void clear_ipv4_locked(void)
{
	address_valid = false;
	ipv4_address[0] = '\0';
	atomic_clear(&ipv4_ready);
}

static bool mark_ready_if_complete_locked(void)
{
	struct net_in_addr address;
	uint8_t join_status = 0;
	bool became_ready = false;

	/* Events have no attempt identifier. Corroborate them against the current
	 * completed association and live DHCP lease before publishing an address.
	 */
	if (association_complete && connect_result_ok && address_valid &&
	    (phase == WIFI_PHASE_CONNECTING || phase == WIFI_PHASE_WAIT_DHCP ||
	     phase == WIFI_PHASE_READY) &&
	    sta_iface->config.dhcpv4.state == NET_DHCPV4_BOUND &&
	    wifi_get_join_status(&join_status) == 0 && join_status == AMEBA_JOIN_SUCCESS &&
	    net_addr_pton(NET_AF_INET, ipv4_address, &address) == 0 &&
	    memcmp(&address, &sta_iface->config.dhcpv4.requested_ip, sizeof(address)) == 0) {
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
	schedule_connect(K_SECONDS(CONFIG_GATEWAY_WIFI_CONNECT_RETRY_SECONDS));
}

static int arm_connect_timeout_locked(void)
{
	timeout_generation = generation;
	timeout_deadline_ms =
		k_uptime_get() + (CONFIG_GATEWAY_WIFI_CONNECT_TIMEOUT_SECONDS * MSEC_PER_SEC);

	return k_work_reschedule(
		&connect_timeout_work, K_SECONDS(CONFIG_GATEWAY_WIFI_CONNECT_TIMEOUT_SECONDS));
}

static void log_ready_address(void)
{
	char address[NET_IPV4_ADDR_LEN];

	if (gateway_wifi_address(address, sizeof(address)) >= 0) {
		LOG_INF("Open http://%s:%d/ on a device connected to the same network", address,
			CONFIG_GATEWAY_HTTP_PORT);
	}
}

static void schedule_disconnect_check(void)
{
	int ret = k_work_reschedule_for_queue(&wifi_work_queue, &disconnect_check_work,
					      K_MSEC(DISCONNECT_POLL_MS));

	if (ret < 0) {
		LOG_ERR("Could not schedule disconnect confirmation: %d; reset the board", ret);
	}
}

static void disconnect_check_handler(struct k_work *work)
{
	uint8_t join_status = 0;
	uint32_t work_generation;
	bool confirmed = false;
	bool expired = false;
	int ret;

	ARG_UNUSED(work);

	k_mutex_lock(&state_lock, K_FOREVER);
	work_generation = cancel_generation;
	if (phase != WIFI_PHASE_DISCONNECTING || work_generation != generation) {
		k_mutex_unlock(&state_lock);
		return;
	}
	k_mutex_unlock(&state_lock);

	ret = wifi_get_join_status(&join_status);
	k_mutex_lock(&state_lock, K_FOREVER);
	if (phase != WIFI_PHASE_DISCONNECTING || work_generation != generation) {
		k_mutex_unlock(&state_lock);
		return;
	}
	/* FAIL can also be set by a timed-out blocking API while the radio is
	 * still linking. Only the driver's completed disconnect indication is a
	 * safe cancellation barrier; ambiguous failures deliberately fail closed.
	 */
	if (ret == 0 && join_status == AMEBA_JOIN_DISCONNECT) {
		disconnect_idle_samples++;
	} else {
		disconnect_idle_samples = 0;
	}
	confirmed = disconnect_idle_samples >= 2U;
	if (!confirmed && k_uptime_get() >= disconnect_deadline_ms) {
		phase = WIFI_PHASE_RETRY_BLOCKED;
		expired = true;
	}
	k_mutex_unlock(&state_lock);

	if (confirmed) {
		/* Synchronously retire the lease before opening the next generation.
		 * This work shares the connect queue, so the old blocking association
		 * call and disconnect request have both returned before this barrier.
		 */
		net_dhcpv4_stop(sta_iface);
		net_eth_carrier_off(sta_iface);
		k_mutex_lock(&state_lock, K_FOREVER);
		phase = WIFI_PHASE_RETRY_WAIT;
		association_complete = false;
		connect_result_ok = false;
		clear_ipv4_locked();
		k_mutex_unlock(&state_lock);
		LOG_INF("Wi-Fi disconnect confirmed by radio state %u; retrying in %d seconds",
			join_status, CONFIG_GATEWAY_WIFI_CONNECT_RETRY_SECONDS);
		schedule_reconnect();
	} else if (expired) {
		LOG_ERR("Wi-Fi disconnect not confirmed within %d seconds "
			"(query %d, radio state %u); reset the board before retrying",
			DISCONNECT_TIMEOUT_MS / MSEC_PER_SEC, ret, join_status);
	} else {
		schedule_disconnect_check();
	}
}

static void disconnect_work_handler(struct k_work *work)
{
	uint32_t work_generation;
	int ret;

	ARG_UNUSED(work);

	k_mutex_lock(&state_lock, K_FOREVER);
	work_generation = cancel_generation;
	if (phase != WIFI_PHASE_CANCEL_PENDING || work_generation != generation) {
		k_mutex_unlock(&state_lock);
		return;
	}
	phase = WIFI_PHASE_DISCONNECTING;
	disconnect_idle_samples = 0;
	disconnect_deadline_ms = k_uptime_get() + DISCONNECT_TIMEOUT_MS;
	k_mutex_unlock(&state_lock);

	ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, sta_iface, NULL, 0);
	if (ret < 0) {
		LOG_WRN("Wi-Fi disconnect request returned %d; checking the radio state", ret);
	}
	/* This SDK's single-core event bridge omits DISCONNECT_RESULT. A successful
	 * request also only queues disassociation. Neither is a completion barrier.
	 */
	schedule_disconnect_check();
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
		CONFIG_GATEWAY_WIFI_CONNECT_TIMEOUT_SECONDS, work_generation);
	ret = k_work_submit_to_queue(&wifi_work_queue, &disconnect_work);
	if (ret < 0) {
		LOG_ERR("Failed to queue Wi-Fi disconnect work: %d", ret);
	}
}

static void connect_work_handler(struct k_work *work)
{
	struct wifi_connect_req_params params = {
		.ssid = (const uint8_t *)CONFIG_GATEWAY_WIFI_SSID,
		.ssid_length = sizeof(CONFIG_GATEWAY_WIFI_SSID) - 1U,
		.security = configured_security(),
		.channel = WIFI_CHANNEL_ANY,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
	};
	uint32_t work_generation;
	bool retry = false;
	bool ready = false;
	int timeout_ret;
	int ret;

	ARG_UNUSED(work);

	k_mutex_lock(&state_lock, K_FOREVER);
	if (phase != WIFI_PHASE_IDLE && phase != WIFI_PHASE_RETRY_WAIT) {
		k_mutex_unlock(&state_lock);
		return;
	}
	/* A phone hotspot can change channel whenever it restarts. Use the
	 * configured hint once; subsequent attempts must search the whole band.
	 */
	if (CONFIG_GATEWAY_WIFI_CHANNEL && !channel_hint_tried) {
		params.channel = CONFIG_GATEWAY_WIFI_CHANNEL;
	}
	channel_hint_tried = true;
	work_generation = ++generation;
	phase = WIFI_PHASE_CONNECTING;
	association_complete = false;
	connect_result_ok = false;
	clear_ipv4_locked();
	timeout_ret = arm_connect_timeout_locked();
	k_mutex_unlock(&state_lock);

	if (params.security != WIFI_SECURITY_TYPE_NONE) {
		params.psk = (const uint8_t *)CONFIG_GATEWAY_WIFI_PSK;
		params.psk_length = sizeof(CONFIG_GATEWAY_WIFI_PSK) - 1U;
	}

	if (timeout_ret < 0) {
		LOG_ERR("Failed to schedule Wi-Fi timeout: %d", timeout_ret);
	}

	LOG_INF("Connecting generation %u to configured test AP (SSID length %zu, channel %u)",
		work_generation, sizeof(CONFIG_GATEWAY_WIFI_SSID) - 1U,
		(unsigned int)params.channel);
	/* A Wi-Fi interface is not necessarily auto-started. wifi_mgmt rejects
	 * an administratively down interface before calling the Ameba driver.
	 * Recheck on each retry so a previous disconnect cannot strand the STA.
	 */
	ret = prepare_sta_link_address();
	if (ret >= 0 && !net_if_is_admin_up(sta_iface)) {
		ret = net_if_up(sta_iface);
		if (ret == -EALREADY) {
			ret = 0;
		}
		if (ret < 0) {
			LOG_ERR("Could not bring Wi-Fi STA interface up: %d; "
				"check radio initialization and device readiness", ret);
		}
	}
	if (ret >= 0) {
		ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, sta_iface, &params, sizeof(params));
	}

	k_mutex_lock(&state_lock, K_FOREVER);
	if (generation != work_generation) {
		k_mutex_unlock(&state_lock);
		return;
	}

	if (ret < 0 && phase == WIFI_PHASE_CANCEL_PENDING) {
		/* A negative return after timeout does not prove the radio stopped.
		 * Preserve cancellation so the already queued disconnect work runs.
		 */
	} else if (ret < 0 && phase == WIFI_PHASE_CONNECTING) {
		phase = WIFI_PHASE_RETRY_WAIT;
		connect_result_ok = false;
		clear_ipv4_locked();
		retry = true;
	} else if (ret >= 0 && (phase == WIFI_PHASE_CONNECTING ||
			       phase == WIFI_PHASE_WAIT_DHCP)) {
		/* Ameba starts DHCP after its synchronous association call returns. */
		association_complete = true;
		phase = WIFI_PHASE_WAIT_DHCP;
		ready = mark_ready_if_complete_locked();
	}
	k_mutex_unlock(&state_lock);

	if (ret < 0) {
		LOG_ERR("Wi-Fi connect request failed: %d", ret);
	}
	if (retry) {
		(void)k_work_cancel_delayable(&connect_timeout_work);
		schedule_reconnect();
	}
	if (ready) {
		(void)k_work_cancel_delayable(&connect_timeout_work);
		LOG_INF("Wi-Fi connected; Ameba DHCP negotiation completed");
		log_ready_address();
	}
}

static void wifi_event_handler(struct net_mgmt_event_callback *callback,
			       uint64_t event, struct net_if *iface)
{
	const struct wifi_status *status = callback->info;
	bool cancel_timeout = false;
	bool ready = false;
	bool retry = false;
	bool cancel = false;

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
		uint8_t join_status = 0;

		/* A queued event from an older attempt cannot cancel a current link. */
		if (wifi_get_join_status(&join_status) != 0 ||
		    (join_status != AMEBA_JOIN_FAIL && join_status != AMEBA_JOIN_DISCONNECT)) {
			return;
		}
		k_mutex_lock(&state_lock, K_FOREVER);
		if (phase == WIFI_PHASE_WAIT_DHCP || phase == WIFI_PHASE_READY) {
			phase = WIFI_PHASE_CANCEL_PENDING;
			cancel_generation = generation;
			connect_result_ok = false;
			clear_ipv4_locked();
			cancel_timeout = true;
			cancel = true;
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
	if (retry) {
		schedule_reconnect();
	}
	if (cancel) {
		(void)k_work_submit_to_queue(&wifi_work_queue, &disconnect_work);
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

int gateway_wifi_start(void)
{
	const size_t ssid_length = sizeof(CONFIG_GATEWAY_WIFI_SSID) - 1U;
	const size_t psk_length = sizeof(CONFIG_GATEWAY_WIFI_PSK) - 1U;
	const struct k_work_queue_config queue_config = {
		.name = "gateway_wifi",
	};
	enum wifi_security_type security = configured_security();

	if (ssid_length == 0U) {
		LOG_ERR("CONFIG_GATEWAY_WIFI_SSID is empty; copy local.example.conf to "
			"local.conf and set local credentials");
		return -EINVAL;
	}
	if (ssid_length > WIFI_SSID_MAX_LEN) {
		LOG_ERR("Wi-Fi SSID is too long (%zu > %d)", ssid_length,
			WIFI_SSID_MAX_LEN);
		return -EINVAL;
	}
	if (security == WIFI_SECURITY_TYPE_NONE && psk_length != 0U) {
		LOG_ERR("Wi-Fi password is set while OPEN security is selected; "
			"select WPA2-PSK or clear CONFIG_GATEWAY_WIFI_PSK");
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
			   K_PRIO_PREEMPT(CONFIG_GATEWAY_WIFI_WORK_PRIORITY), &queue_config);
	k_work_init_delayable(&reconnect_work, connect_work_handler);
	k_work_init(&disconnect_work, disconnect_work_handler);
	k_work_init_delayable(&disconnect_check_work, disconnect_check_handler);
	k_work_init_delayable(&connect_timeout_work, connect_timeout_handler);

	net_mgmt_init_event_callback(&wifi_callback, wifi_event_handler, WIFI_EVENTS);
	net_mgmt_add_event_callback(&wifi_callback);
	net_mgmt_init_event_callback(&ipv4_callback, ipv4_event_handler, IPV4_EVENTS);
	net_mgmt_add_event_callback(&ipv4_callback);

	k_mutex_lock(&state_lock, K_FOREVER);
	phase = WIFI_PHASE_RETRY_WAIT;
	connect_result_ok = false;
	channel_hint_tried = false;
	clear_ipv4_locked();
	k_mutex_unlock(&state_lock);
	schedule_connect(K_MSEC(CONFIG_GATEWAY_WIFI_CONNECT_DELAY_MS));

	return 0;
}

bool gateway_wifi_ipv4_ready(void)
{
	return atomic_get(&ipv4_ready) != 0;
}

int gateway_wifi_address(char *buffer, size_t capacity)
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
