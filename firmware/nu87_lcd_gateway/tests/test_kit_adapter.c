/* SPDX-License-Identifier: Apache-2.0 */
/* Host callback-order and scan-selection tests; includes the actual adapter.
 * See kit_mock/kit_mock.h for the explicit scope of the host model. */
#include <assert.h>
#include <stdlib.h>
#include "../src/ble_kit.c"

enum delivery { ACK_FIRST, WRITE_FIRST, WRONG_SEQUENCE, BAD_ACK, OLED_ERROR,
	BUSY_ACK, BAD_TEXT_ACK, ATT_ERROR_AFTER_ACK, DISCONNECT_AFTER_ACK,
	DISCONNECT_BEFORE_ACK, NO_RESPONSE, ACK_WITHOUT_ATT_RESPONSE, NO_ACK,
	ATT_REJECT, ENQUEUE_ERROR };
static enum delivery delivery;
static struct bt_conn peripheral;
static struct ble_display_rx receiver;
static int writes, disconnects;
static size_t transferred;
static bool mock_wifi_ready;
static bool mock_radio_running;
static unsigned mock_diagnostics_calls;
static unsigned mock_coex_diagnostics_calls;
static bool mock_smp_busy;
static uint8_t mock_audio_error;
void gateway_smp_disconnected(struct bt_conn *conn) { ARG_UNUSED(conn); }
bool gateway_smp_drained(void) { return true; }
bool gateway_smp_busy(void) { return mock_smp_busy; }
int gateway_smp_prepare(struct bt_conn *conn) { ARG_UNUSED(conn); return -ENOTSUP; }
int gateway_smp_auth_init(void) { return 0; }
void gateway_alarm_disconnected(struct bt_conn *conn) { ARG_UNUSED(conn); }
bool gateway_alarm_drained(void) { return true; }
int gateway_alarm_prepare(struct bt_conn *conn) { ARG_UNUSED(conn); return -ENOTSUP; }
int gateway_robot_command(const char *command) { assert(!strcmp(command, "stop")); return 0; }

int wifi_is_running(unsigned char index) { ARG_UNUSED(index); return mock_radio_running; }
int gateway_wifi_start(void) { return 0; }
bool gateway_wifi_ipv4_ready(void) { return mock_wifi_ready; }
int gateway_wifi_address(char *out, size_t capacity) { if (capacity) { out[0] = '\0'; } return -ENOTCONN; }
void gateway_radio_diagnostics(const char *stage)
{
	if (mock_diagnostics_calls == 0) {
		assert(strcmp(stage, "before_bt_enable") == 0 && mock_enable_calls == 0);
	} else {
		assert(mock_diagnostics_calls == 1);
		assert(strcmp(stage, "after_bt_enable") == 0 && mock_enable_calls == 1);
	}
	mock_diagnostics_calls++;
}
void gateway_radio_coex_diagnostics(void) { mock_coex_diagnostics_calls++; }

int bt_conn_disconnect(struct bt_conn *conn, uint8_t reason)
{
	disconnects++;
	disconnected(conn, reason);
	return 0;
}

int bt_gatt_write(struct bt_conn *conn, struct bt_gatt_write_params *params)
{
	writes++;
	if (params->handle == 15) {
		assert(params->length == 6);
		const uint8_t *control = params->data;
		assert(control[0] == 1 && control[1] <= 1);
		params->func(conn, mock_audio_error, params);
		return 0;
	}
	assert(params->handle == 9);
	assert(params->length >= 7 && params->length <= 20);
	if (delivery == ENQUEUE_ERROR) { return -ENOMEM; }
	if (delivery == ATT_REJECT) { params->func(conn, 0x13, params); return 0; }
	transferred += params->length - 6;
	enum ble_display_result result = ble_display_receive(&receiver, params->data,
		params->length, (uint32_t)mock_time);
	assert(result >= 0);
	if (result != BLE_DISPLAY_COMPLETE) { params->func(conn, 0, params); return 0; }
	uint8_t ack[BLE_DISPLAY_ACK_SIZE];
	uint16_t seq = receiver.sequence + (delivery == WRONG_SEQUENCE ? 1 : 0);
	uint8_t status = delivery == OLED_ERROR ? BLE_DISPLAY_OLED_ERROR :
		delivery == BUSY_ACK ? BLE_DISPLAY_BUSY : delivery == BAD_TEXT_ACK ? BLE_DISPLAY_BAD_TEXT : 0;
	ble_display_build_ack(seq, status, ack);
	if (delivery == BAD_ACK) { ack[1] = BLE_DISPLAY_TEXT; }
	if (delivery == NO_RESPONSE) { return 0; }
	if (delivery == WRITE_FIRST || delivery == NO_ACK) { params->func(conn, 0, params); }
	if (delivery == DISCONNECT_BEFORE_ACK) { (void)bt_conn_disconnect(conn, 19); }
	if (delivery != NO_ACK) { notification(conn, &subscription, ack, sizeof(ack)); }
	if (delivery == DISCONNECT_AFTER_ACK) { (void)bt_conn_disconnect(conn, 19); }
	if (delivery != WRITE_FIRST && delivery != ACK_WITHOUT_ATT_RESPONSE && delivery != NO_ACK) {
		params->func(conn, delivery == ATT_ERROR_AFTER_ACK ? 14 : 0, params);
	}
	return 0;
}

static void reset(enum delivery behavior)
{
	memset(&state, 0, sizeof(state));
	memset(&peripheral, 0, sizeof(peripheral));
	memset(candidates, 0, sizeof(candidates));
	memset(&scan_counts, 0, sizeof(scan_counts));
	memset(&kit_diag, 0, sizeof(kit_diag));
	kit_diag.disconnect_reason = -1;
	scan_active = false;
	requested_scan_type = active_scan_type = BT_LE_SCAN_TYPE_ACTIVE;
	scan_generation = 0;
	peripheral.refs = 1;
	(void)bt_addr_le_from_str("F1:22:33:44:55:66", "random", &peripheral.address);
	peer = &peripheral;
	selected = peripheral.address;
	state.ble_connected = state.lcd_ready = state.ble_configured = true;
	enabled = true;
	start_requested = initialization_started = true;
	connecting = closed = false;
	discovery_active = subscribe_active = write_active = false;
	subscription_live = true;
	command_pending = ack_received = final_chunk_submitted = false;
	operation_lock.held = false;
	text_handle = 9;
	sequence = 0;
	mock_time = 100;
	k_sem_reset(&write_done); k_sem_reset(&command_done); k_sem_reset(&setup_done); k_sem_reset(&link_event);
	ble_display_rx_reset(&receiver);
	delivery = behavior;
	writes = disconnects = 0;
	transferred = 0;
	memset(&audio_ring, 0, sizeof(audio_ring));
	audio_ctrl_handle = 15;
	audio_subscribe_active = audio_subscription_live = audio_mtu_active = false;
	mock_smp_busy = false; mock_audio_error = 0;
	mock_wifi_ready = true;
	mock_radio_running = true;
	mock_scan_starts = mock_scan_stops = mock_enable_calls = 0;
	mock_diagnostics_calls = mock_coex_diagnostics_calls = 0;
	mock_scan_start_result = mock_scan_stop_result = mock_enable_result = 0;
}

static void test_ack_orders(void)
{
	const enum delivery successes[] = { ACK_FIRST, WRITE_FIRST, ATT_ERROR_AFTER_ACK, DISCONNECT_AFTER_ACK };
	for (size_t i = 0; i < ARRAY_SIZE(successes); i++) {
		reset(successes[i]);
		assert(gateway_display_text((const uint8_t *)"hello", 5, 5000) == 0);
		assert(writes == 1 && transferred == 5 && peripheral.refs == 1);
	}
	reset(DISCONNECT_BEFORE_ACK);
	assert(gateway_display_text((const uint8_t *)"hello", 5, 5000) == -ENOTCONN);
	assert(!ack_received);
}

static void test_errors_and_timeouts(void)
{
	const enum delivery modes[] = { WRONG_SEQUENCE, BAD_ACK, OLED_ERROR, BUSY_ACK, BAD_TEXT_ACK, NO_RESPONSE, NO_ACK };
	const int errors[] = { -ETIMEDOUT, -ETIMEDOUT, -EIO, -EBUSY, -EINVAL, -ETIMEDOUT, -ETIMEDOUT };
	for (size_t i = 0; i < ARRAY_SIZE(modes); i++) {
		reset(modes[i]);
		assert(gateway_display_text((const uint8_t *)"hello", 5, 5000) == errors[i]);
		assert(peripheral.refs == 1);
		if (modes[i] == WRONG_SEQUENCE || modes[i] == BAD_ACK || modes[i] == NO_ACK || modes[i] == NO_RESPONSE) {
			assert(disconnects != 0 && !state.lcd_ready);
		} else { assert(disconnects == 0); }
	}
	reset(NO_RESPONSE);
	assert(gateway_display_text((const uint8_t *)"hello", 5, 5000) == -ETIMEDOUT);
	assert(write_active); /* Static request storage stays owned until callback. */
	assert(gateway_display_text((const uint8_t *)"next", 4, 5000) == -ENOTCONN);
	assert(writes == 1);
	written(&peripheral, 14, &write_params);
	assert(!write_active);
	reset(ACK_WITHOUT_ATT_RESPONSE);
	assert(gateway_display_text((const uint8_t *)"hello", 5, 5000) == 0);
	assert(write_active && !state.lcd_ready && disconnects != 0);
	written(&peripheral, 14, &write_params);
	assert(!write_active);
}

static void test_fragmentation_and_validation(void)
{
	char text[84]; memset(text, 'A', sizeof(text));
	reset(ACK_FIRST);
	assert(gateway_display_text((const uint8_t *)text, sizeof(text), 5000) == 0);
	assert(writes == 6 && transferred == sizeof(text));
	assert(memcmp(receiver.text, text, sizeof(text)) == 0);
	assert(receiver.sequence == 1);
	assert(gateway_display_text((const uint8_t *)"next", 4, 5000) == 0);
	assert(receiver.sequence == 2);
	reset(ACK_FIRST);
	assert(gateway_display_text((const uint8_t *)"\n\n\n\nX", 5, 5000) == -EINVAL);
	assert(gateway_display_text((const uint8_t *)"\xc3\xa9", 2, 5000) == -EINVAL);
	assert(gateway_display_text(NULL, 0, 5000) == -EINVAL);
	assert(writes == 0);
	operation_lock.held = true;
	assert(gateway_display_text((const uint8_t *)"busy", 4, 5000) == -EBUSY);
	assert(writes == 0);
}

static void test_retained_link_diagnostics(void)
{
	char json[768];
	reset(ATT_REJECT);
	assert(gateway_display_text((const uint8_t *)"hello", 5, 5000) == -EIO);
	assert(kit_diag.commands == 1 && kit_diag.failures == 1);
	assert(kit_diag.failure_att == 0x13 && kit_diag.failure_enqueue == 0);
	assert(!kit_diag.failure_ack && kit_diag.failure_fragment == 0);
	assert(kit_diag.disconnects == 1 && kit_diag.disconnect_reason == 19);
	/* Recovery must not erase the cause before the host can read it. */
	delivery = ACK_FIRST;
	closed = false;
	state.ble_connected = state.lcd_ready = true;
	connected(&peripheral, 0);
	assert(gateway_display_text((const uint8_t *)"next", 4, 5000) == 0);
	assert(kit_diag.connections == 1 && kit_diag.commands == 2 && kit_diag.failures == 1);
	assert(kit_diag.failure == -EIO && kit_diag.failure_att == 0x13);
	int size = gateway_kit_diagnostics(json, sizeof(json));
	assert(size > 0 && size == (int)strlen(json));
	assert(strstr(json, "\"attError\":19") && strstr(json, "\"ackReceived\":false"));
	assert(strstr(json, "hello") == NULL && strstr(json, KIT_NAME) == NULL);
	assert(gateway_kit_diagnostics(json, (size_t)size + 1) == size);
	assert(gateway_kit_diagnostics(json, (size_t)size) == -ENOSPC && !json[0]);
	assert(gateway_kit_diagnostics(NULL, sizeof(json)) == -EINVAL);
	assert(gateway_kit_diagnostics(json, 0) == -EINVAL);
	reset(OLED_ERROR);
	assert(gateway_display_text((const uint8_t *)"hello", 5, 5000) == -EIO);
	assert(kit_diag.failure_ack && kit_diag.failure_att == 0 && kit_diag.disconnects == 0);
	reset(ENQUEUE_ERROR);
	assert(gateway_display_text((const uint8_t *)"hello", 5, 5000) == -ENOMEM);
	assert(kit_diag.failure_enqueue == -ENOMEM && kit_diag.failure_att == 0);
	memset(&kit_diag, 0xff, sizeof(kit_diag));
	kit_diag.failure_ack = true;
	connected(&peripheral, 0);
	disconnected(&peripheral, 8);
	assert(kit_diag.connections == UINT32_MAX && kit_diag.disconnects == UINT32_MAX);
	assert(gateway_kit_diagnostics(json, sizeof(json)) > 0);
}

static void test_connection_supervision_negotiation(void)
{
	reset(ACK_FIRST);
	struct bt_le_conn_param params = {
		.interval_min = 24, .interval_max = 40, .latency = 0, .timeout = 42,
	};
	assert(kit_connections.le_param_req(&peripheral, &params));
	assert(params.timeout == 400 && params.interval_min == 24 && params.interval_max == 40);
	assert(kit_diag.param_requests == 1 && kit_diag.short_timeout_requests == 1);
	assert(kit_diag.requested_timeout == 42 && !kit_diag.params_known);
	kit_connections.le_param_updated(&peripheral, 40, 0, 400);
	assert(kit_diag.params_known && kit_diag.interval == 40 && kit_diag.timeout == 400);
	char json[768];
	assert(gateway_kit_diagnostics(json, sizeof(json)) > 0);
	assert(strstr(json, "\"params\":[40,0,400]") && strstr(json, "\"requestedTimeout\":42"));
	params.timeout = 600;
	assert(kit_connections.le_param_req(&peripheral, &params) && params.timeout == 600);
	assert(kit_diag.param_requests == 2 && kit_diag.short_timeout_requests == 1);
	struct bt_conn other = {0};
	params.timeout = 42;
	assert(kit_connections.le_param_req(&other, &params) && params.timeout == 42);
	kit_connections.le_param_updated(&other, 80, 0, 42);
	assert(kit_diag.param_requests == 2 && kit_diag.param_updates == 1);
	disconnected(&peripheral, 8);
	assert(!kit_diag.params_known && kit_diag.timeout == 400);
}

static void advertisement(const bt_addr_le_t *address, bool name, const char *text)
{
	uint8_t bytes[32];
	size_t length = name ? strlen(text) : sizeof(service_uuid.val);
	bytes[0] = length + 1;
	bytes[1] = name ? BT_DATA_NAME_COMPLETE : BT_DATA_UUID128_ALL;
	memcpy(bytes + 2, name ? (const uint8_t *)text : service_uuid.val, length);
	struct net_buf_simple ad = { bytes, length + 2 };
	scanned(address, -48, name ? BT_GAP_ADV_TYPE_SCAN_RSP : BT_GAP_ADV_TYPE_ADV_IND, &ad);
}

static void test_explicit_selection_and_freshness(void)
{
	char json[1024];
	reset(ACK_FIRST); peer = NULL; state.ble_configured = false;
	advertisement(&peripheral.address, false, NULL);
	assert(gateway_peer_candidates(json, sizeof(json)) == 2 && strcmp(json,"[]") == 0);
	assert(gateway_select_peer("F1:22:33:44:55:66", "random") == -ENOENT);
	advertisement(&peripheral.address, true, KIT_NAME);
	assert(!state.ble_configured); /* Discovery never authorizes connecting. */
	assert(gateway_peer_candidates(json, sizeof(json)) > 2);
	assert(strstr(json, "F1:22:33:44:55:66") != NULL && strstr(json, KIT_NAME) != NULL);
	assert(gateway_select_peer("F1:22:33:44:55:66", "public") == -ENOENT);
	assert(gateway_select_peer("F1:22:33:44:55:66", "random") == 0);
	assert(state.ble_configured);
	mock_time += KIT_FRESH_MS;
	assert(gateway_peer_candidates(json, sizeof(json)) == 2);
	assert(gateway_select_peer("F1:22:33:44:55:66", "random") == -ENOENT);
	advertisement(&peripheral.address, false, NULL);
	assert(gateway_peer_candidates(json, sizeof(json)) == 2); /* Name stale independently. */
	advertisement(&peripheral.address, true, KIT_NAME);
	assert(gateway_peer_candidates(json, 3) == -ENOSPC);
	assert(gateway_peer_candidates(json, sizeof(json)) > 2);
	advertisement(&peripheral.address, true, "NU40-OTHER");
	assert(gateway_peer_candidates(json, sizeof(json)) == 2);
	assert(gateway_select_peer("F1:22:33:44:55:66", "random") == -ENOENT);
}

static void test_wifi_discovery_scheduling(void)
{
	char json[1024];
	bool scanning = false;
	reset(ACK_FIRST);
	peer = NULL;
	state.ble_connected = state.lcd_ready = state.ble_configured = false;
	mock_wifi_ready = false;
	/* Waiting several minutes for DHCP is nonterminal and leaves the one-time
	 * HCI initialization state intact. No scan starts before IPv4 readiness. */
	for (int i = 0; i < 6; i++) {
		mock_time += 60000;
		assert(!schedule_discovery(&scanning));
		assert(enabled && initialization_started && state.last_error == 0);
	}
	assert(!scanning && mock_scan_starts == 0 && mock_enable_calls == 0);
	advertisement(&peripheral.address, false, NULL);
	advertisement(&peripheral.address, true, KIT_NAME);
	assert(gateway_peer_candidates(json, sizeof(json)) == 2);
	mock_wifi_ready = true;
	assert(schedule_discovery(&scanning));
	assert(scanning && mock_scan_starts == 1);
	assert(scan_active);
	assert(mock_scan_parameters.type == BT_LE_SCAN_TYPE_ACTIVE);
	assert(mock_scan_parameters.options == 0);
	assert(mock_scan_parameters.interval == BT_GAP_SCAN_FAST_INTERVAL);
	assert(mock_scan_parameters.window == BT_GAP_SCAN_FAST_WINDOW);
	assert(schedule_discovery(&scanning) && mock_scan_starts == 1);
	advertisement(&peripheral.address, false, NULL);
	advertisement(&peripheral.address, true, KIT_NAME);
	assert(gateway_select_peer("F1:22:33:44:55:66", "random") == 0);
	mock_wifi_ready = false;
	assert(!schedule_discovery(&scanning));
	assert(!scanning && mock_scan_stops == 1);
	assert(!scan_active);
	assert(state.ble_configured && bt_addr_le_cmp(&selected, &peripheral.address) == 0);
	assert(gateway_peer_candidates(json, sizeof(json)) == 2);
	assert(!schedule_discovery(&scanning) && mock_scan_stops == 1);
	mock_wifi_ready = true;
	assert(schedule_discovery(&scanning) && mock_scan_starts == 2);
	assert(gateway_peer_candidates(json, sizeof(json)) == 2); /* Old sightings stay expired. */
	assert(mock_enable_calls == 0);
	/* A failed stop stays tracked and is retried, never treated as stopped. */
	mock_wifi_ready = false;
	mock_scan_stop_result = -EIO;
	assert(!schedule_discovery(&scanning) && scanning);
	assert(scan_active);
	assert(state.last_error == -EIO && mock_scan_stops == 2);
	mock_scan_stop_result = 0;
	assert(!schedule_discovery(&scanning) && !scanning && mock_scan_stops == 3);
	assert(!scan_active);
	/* Existing BLE links are outside discovery scheduling and survive Wi-Fi
	 * loss; scheduling never disconnects them or triggers HCI reinitializing. */
	peer = &peripheral;
	state.ble_connected = state.lcd_ready = true;
	assert(!schedule_discovery(&scanning));
	assert(peer == &peripheral && state.ble_connected && state.lcd_ready);
	assert(disconnects == 0 && mock_enable_calls == 0 && mock_scan_stops == 3);
	peer = NULL;
	mock_wifi_ready = true;
	mock_scan_start_result = -EIO;
	assert(!schedule_discovery(&scanning) && !scanning);
	mock_scan_start_result = 0;
	assert(schedule_discovery(&scanning) && scanning && mock_scan_starts == 4);
}

static void test_controller_initialization_after_ipv4(void)
{
	reset(ACK_FIRST);
	peer = NULL;
	state.ble_connected = state.lcd_ready = state.ble_configured = false;
	enabled = initialization_started = false;
	mock_wifi_ready = false;
	for (int i = 0; i < 6; i++) {
		mock_time += 60000;
		initialize_controller_when_ready();
		assert(!enabled && !initialization_started && state.last_error == 0);
	}
	assert(mock_enable_calls == 0 && mock_diagnostics_calls == 0);
	mock_wifi_ready = true;
	mock_radio_running = false;
	initialize_controller_when_ready();
	assert(!initialization_started && mock_enable_calls == 0);
	mock_radio_running = true;
	start_requested = false;
	initialize_controller_when_ready();
	assert(!initialization_started && mock_enable_calls == 0);
	start_requested = true;
	initialize_controller_when_ready();
	assert(enabled && initialization_started && mock_enable_calls == 1);
	assert(mock_diagnostics_calls == 2);
	initialize_controller_when_ready();
	mock_wifi_ready = false;
	initialize_controller_when_ready();
	mock_time += 60000;
	mock_wifi_ready = true;
	initialize_controller_when_ready();
	assert(enabled && mock_enable_calls == 1 && mock_diagnostics_calls == 2);
	/* A real failed HCI attempt remains consumed: Wi-Fi recovery must not
	 * overlap or implicitly retry a partially initialized controller. */
	reset(ACK_FIRST);
	peer = NULL;
	enabled = initialization_started = false;
	mock_enable_result = -EIO;
	initialize_controller_when_ready();
	assert(!enabled && initialization_started && state.last_error == -EIO);
	mock_wifi_ready = false;
	initialize_controller_when_ready();
	mock_wifi_ready = true;
	initialize_controller_when_ready();
	assert(mock_enable_calls == 1 && mock_diagnostics_calls == 2);
}

static void test_scan_mode_transitions(void)
{
	char json[512];
	bool scanning = false;
	reset(ACK_FIRST);
	assert(gateway_scan_mode(NULL) == -EINVAL);
	assert(gateway_scan_mode("") == -EINVAL);
	assert(gateway_scan_mode("PASSIVE") == -EINVAL);
	assert(gateway_scan_mode("passive\n") == -EINVAL);
	assert(gateway_scan_mode("active") == -EBUSY);
	assert(gateway_scan_mode("passive") == -EBUSY);
	peer = NULL;
	connecting = true;
	assert(gateway_scan_mode("passive") == -EBUSY);
	assert(requested_scan_type == BT_LE_SCAN_TYPE_ACTIVE);
	connecting = false;
	state.ble_connected = state.lcd_ready = state.ble_configured = false;
	assert(schedule_discovery(&scanning));
	assert(scan_generation == 1 && mock_coex_diagnostics_calls == 1);
	assert(gateway_scan_mode("passive") == 0);
	/* HTTP only queues; the real scan remains active until the worker runs. */
	assert(mock_scan_starts == 1 && mock_scan_stops == 0);
	assert(active_scan_type == BT_LE_SCAN_TYPE_ACTIVE && scan_active);
	assert(gateway_scan_diagnostics(json, sizeof(json)) > 0);
	assert(strstr(json, "\"requestedMode\":\"passive\""));
	assert(strstr(json, "\"activeMode\":\"active\"") && strstr(json, "\"generation\":1"));
	advertisement(&peripheral.address, false, NULL);
	advertisement(&peripheral.address, true, KIT_NAME);
	assert(gateway_peer_candidates(json, sizeof(json)) > 2);
	mock_scan_stop_result = -EIO;
	assert(!schedule_discovery(&scanning));
	assert(scanning && scan_active && active_scan_type == BT_LE_SCAN_TYPE_ACTIVE);
	assert(mock_scan_starts == 1 && mock_scan_stops == 1 && scan_generation == 1);
	assert(scan_counts.errors == 1 && state.last_error == -EIO);
	assert(gateway_peer_candidates(json, sizeof(json)) == 2);
	assert(!schedule_discovery(&scanning));
	assert(scan_counts.errors == 2 && mock_scan_stops == 2 && mock_scan_starts == 1);
	/* A successful retry stops before starting with the requested type. */
	mock_scan_stop_result = 0;
	assert(schedule_discovery(&scanning));
	assert(mock_scan_stops == 3 && mock_scan_starts == 2 && scan_generation == 2);
	assert(mock_scan_parameters.type == BT_LE_SCAN_TYPE_PASSIVE);
	assert(mock_scan_parameters.options == 0 && mock_scan_parameters.interval == BT_GAP_SCAN_FAST_INTERVAL);
	assert(mock_scan_parameters.window == BT_GAP_SCAN_FAST_WINDOW);
	assert(active_scan_type == BT_LE_SCAN_TYPE_PASSIVE && mock_coex_diagnostics_calls == 2);
	assert(gateway_scan_diagnostics(json, sizeof(json)) > 0);
	assert(strstr(json, "\"activeMode\":\"passive\"") && strstr(json, "\"scanErrors\":2"));
	assert(gateway_scan_mode("passive") == 0 && schedule_discovery(&scanning));
	assert(mock_scan_starts == 2 && mock_scan_stops == 3 && scan_generation == 2);
	/* Stop success plus start failure leaves no actual mode and retries only
	 * the new start. It never claims the requested mode is already running. */
	assert(gateway_scan_mode("active") == 0);
	mock_scan_start_result = -EIO;
	assert(!schedule_discovery(&scanning));
	assert(!scan_active && !scanning && scan_generation == 2);
	assert(mock_scan_starts == 3 && mock_scan_stops == 4 && scan_counts.errors == 3);
	assert(gateway_scan_diagnostics(json, sizeof(json)) > 0 && strstr(json, "\"activeMode\":\"none\""));
	mock_scan_start_result = 0;
	assert(schedule_discovery(&scanning));
	assert(mock_scan_starts == 4 && mock_scan_stops == 4 && scan_generation == 3);
	assert(active_scan_type == BT_LE_SCAN_TYPE_ACTIVE && mock_coex_diagnostics_calls == 3);
	/* Changing modes while DHCP is absent is an unbounded pending request;
	 * it neither scans prematurely nor reinitializes Bluetooth. */
	mock_wifi_ready = false;
	assert(!schedule_discovery(&scanning));
	assert(!scanning && mock_scan_stops == 5);
	assert(gateway_scan_mode("passive") == 0);
	for (int i = 0; i < 6; i++) {
		mock_time += 60000;
		assert(!schedule_discovery(&scanning));
		initialize_controller_when_ready();
	}
	assert(mock_scan_starts == 4 && mock_scan_stops == 5 && scan_generation == 3);
	assert(gateway_scan_diagnostics(json, sizeof(json)) > 0);
	assert(strstr(json, "\"requestedMode\":\"passive\"") && strstr(json, "\"activeMode\":\"none\""));
	mock_wifi_ready = true;
	initialize_controller_when_ready();
	assert(schedule_discovery(&scanning));
	assert(active_scan_type == BT_LE_SCAN_TYPE_PASSIVE && scan_generation == 4);
	assert(mock_scan_starts == 5 && mock_scan_stops == 5 && mock_enable_calls == 0);
	assert(mock_coex_diagnostics_calls == 4);
}

static void test_scan_diagnostic_counters(void)
{
	char json[512], peers[1024];
	reset(ACK_FIRST); peer = NULL; state.ble_configured = false;
	advertisement(&peripheral.address, false, NULL);
	advertisement(&peripheral.address, true, KIT_NAME);
	advertisement(&peripheral.address, false, NULL);
	assert(scan_counts.total == 3 && scan_counts.adv_ind == 2 && scan_counts.scan_rsp == 1);
	assert(scan_counts.service == 2 && scan_counts.name == 1 && scan_counts.merged == 1);
	bt_addr_le_t rejected = peripheral.address;
	rejected.a.val[5] = 0x41; /* Private random address is not a stable identity. */
	advertisement(&rejected, false, NULL);
	assert(scan_counts.identity_reject == 1 && scan_counts.service == 3);
	struct net_buf_simple empty = { NULL, 0 };
	scanned(&peripheral.address, -50, 2, &empty);
	assert(scan_counts.other == 1);
	mock_wifi_ready = false;
	scanned(&peripheral.address, -50, BT_GAP_ADV_TYPE_ADV_IND, &empty);
	assert(scan_counts.wifi_down == 1);
	mock_wifi_ready = true;
	const uint8_t bad[] = { 17, BT_DATA_UUID128_ALL, 0 };
	struct net_buf_simple malformed = { bad, sizeof(bad) };
	scanned(&peripheral.address, -50, BT_GAP_ADV_TYPE_ADV_IND, &malformed);
	assert(scan_counts.total == 7 && scan_counts.adv_ind == 5);
	assert(scan_counts.service == 3 && scan_counts.name == 1 && scan_counts.merged == 1);
	for (uint8_t i = 1; i <= KIT_CANDIDATES; i++) {
		bt_addr_le_t another = peripheral.address;
		another.a.val[0] += i;
		advertisement(&another, false, NULL);
		advertisement(&another, true, KIT_NAME);
	}
	assert(scan_counts.total == 15 && scan_counts.adv_ind == 9 && scan_counts.scan_rsp == 5);
	assert(scan_counts.service == 7 && scan_counts.name == 5 && scan_counts.merged == 4);
	assert(scan_counts.full_table == 2); /* Both fragments of the fifth identity rejected. */
	assert(!state.ble_configured);
	assert(gateway_select_peer("F1:22:33:44:55:6A", "random") == -ENOENT);
	assert(gateway_peer_candidates(peers, sizeof(peers)) > 2);
	assert(strstr(peers, "41:22:33:44:55:66") == NULL);
	int size = gateway_scan_diagnostics(json, sizeof(json));
	assert(size > 0 && size == (int)strlen(json));
	assert(strstr(json, "\"total\":15") && strstr(json, "\"identityReject\":1"));
	assert(strstr(json, "\"service\":7") && strstr(json, "\"name\":5"));
	assert(strstr(json, "\"merged\":4") && strstr(json, "\"fullTable\":2"));
	assert(strstr(json, "\"enabled\":true") && strstr(json, "\"initializationStarted\":true"));
	assert(strstr(json, "\"scanning\":false"));
	assert(strstr(json, "F1:22") == NULL && strstr(json, KIT_NAME) == NULL);
	bool scanning = false;
	assert(schedule_discovery(&scanning));
	assert(gateway_scan_diagnostics(json, sizeof(json)) > 0 && strstr(json, "\"scanning\":true"));
	/* Maximum-width counters remain within the HTTP endpoint's 512-byte
	 * buffer; saturation and undersized buffers must never corrupt memory. */
	memset(&scan_counts, 0xff, sizeof(scan_counts));
	scan_generation = UINT32_MAX;
	requested_scan_type = active_scan_type = BT_LE_SCAN_TYPE_PASSIVE;
	scanned(&peripheral.address, -50, BT_GAP_ADV_TYPE_ADV_IND, &malformed);
	assert(scan_counts.total == UINT32_MAX && scan_counts.adv_ind == UINT32_MAX);
	size = gateway_scan_diagnostics(json, sizeof(json));
	assert(size > 0 && size < (int)sizeof(json));
	assert(strstr(json, "4294967295"));
	assert(gateway_scan_diagnostics(json, (size_t)size + 1) == size);
	assert(gateway_scan_diagnostics(json, (size_t)size) == -ENOSPC && json[0] == '\0');
	struct { unsigned char before; char text[1]; unsigned char after; } guard = { 0x5a, { 'x' }, 0xa5 };
	assert(gateway_scan_diagnostics(guard.text, sizeof(guard.text)) == -ENOSPC);
	assert(guard.text[0] == '\0' && guard.before == 0x5a && guard.after == 0xa5);
	assert(gateway_scan_diagnostics(NULL, 512) == -EINVAL);
	assert(gateway_scan_diagnostics(json, 0) == -EINVAL);
}

static void test_audio_control_and_disconnect(void)
{
	reset(ACK_FIRST);
	assert(gateway_audio_start(17) == -ENOTSUP && writes == 0);
	audio_ring.available = audio_subscription_live = true;
	assert(gateway_audio_start(0) == -EINVAL && writes == 0);
	assert(gateway_audio_start(17) == 0 && audio_ring.running && audio_ring.received == 0);
	assert(gateway_audio_start(18) == -EBUSY && writes == 1);
	uint8_t frame[176] = { 'W', 'A', 1, 0, 17 }, out[176];
	assert(audio_notification(&peripheral, &audio_subscription, frame, sizeof(frame)) == BT_GATT_ITER_CONTINUE);
	assert(audio_ring.received == 1 && audio_ring.queued == 1);
	assert(gateway_audio_stop() == 0 && !audio_ring.running);
	assert(gateway_audio_chunk(out, sizeof(out)) == 176 && memcmp(frame, out, 176) == 0);
	assert(gateway_audio_start(17) == -EINVAL); /* Session IDs cannot be reused. */
	assert(gateway_audio_start(18) == 0);
	frame[4] = 18;
	(void)audio_notification(&peripheral, &audio_subscription, frame, sizeof(frame));
	disconnected(&peripheral, 19);
	assert(!audio_ring.available && !audio_ring.running && audio_ring.queued == 0);
	assert(gateway_audio_chunk(out, sizeof(out)) == 0);
	(void)audio_notification(&peripheral, &audio_subscription, frame, sizeof(frame));
	assert(audio_ring.queued == 0);
	reset(ACK_FIRST); audio_ring.available = true; mock_audio_error = 14;
	assert(gateway_audio_start(19) == -EIO && !audio_ring.running && !audio_ring.queued);
	reset(ACK_FIRST); audio_ring.available = true; mock_smp_busy = true;
	assert(gateway_audio_start(20) == -EBUSY && writes == 0);
	mock_smp_busy = false;
	audio_ctrl_handle = audio_data_handle = 0;
	assert(setup_audio(&peripheral) == -ENOTSUP && state.lcd_ready);
}

int main(void)
{
	test_audio_control_and_disconnect();
	test_ack_orders();
	test_errors_and_timeouts();
	test_fragmentation_and_validation();
	test_retained_link_diagnostics();
	test_connection_supervision_negotiation();
	test_explicit_selection_and_freshness();
	test_wifi_discovery_scheduling();
	test_controller_initialization_after_ipv4();
	test_scan_mode_transitions();
	test_scan_diagnostic_counters();
	puts("PASS kit adapter: ACK ordering, errors, timeout ownership, fragmentation, explicit fresh selection, Wi-Fi scan scheduling, HCI initialization after IPv4, async scan modes, bounded scan diagnostics");
	return 0;
}
