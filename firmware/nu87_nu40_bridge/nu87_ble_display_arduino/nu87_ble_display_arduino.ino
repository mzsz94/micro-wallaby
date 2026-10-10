/* SPDX-License-Identifier: Apache-2.0 */
#include <Arduino.h>
#include "BLEDevice.h"
#include "ack_gate.h"
#include "scan_filter.h"
#include "ble_face.h"
#include "smp_relay.h"

extern "C" {
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
}

static const char SERVICE_UUID[] = "90e2c000-3c0d-4ef3-9d78-574cd1a64087";
static const char ACK_UUID[] = "90e2c001-3c0d-4ef3-9d78-574cd1a64087";
static const char TEXT_UUID[] = "90e2c002-3c0d-4ef3-9d78-574cd1a64087";
static const char SMP_SERVICE_UUID[] = "8d53dc1d-1db7-4cd3-868b-8a527460aa84";
static const char SMP_UUID[] = "da2e7828-fbce-4e01-ae9e-261174997c48";
static const uint32_t SCAN_MS = 5000;
static const uint32_t CONNECT_MS = 6000;
static const uint32_t DISCOVER_MS = 8000;

static SemaphoreHandle_t candidate_lock, candidate_event, remote_lock, ack_lock, ack_event;
static SemaphoreHandle_t smp_lock, smp_event;
static struct smp_relay_rx smp_rx;
static BLEAdvertData candidate;
static bool scanning, candidate_ready, initialized;
static BLEClient *peer;
static BLERemoteCharacteristic *ack_characteristic, *text_characteristic;
static BLERemoteCharacteristic *smp_characteristic;
static struct native_ack_gate ack_gate;
static uint16_t sequence;
static uint32_t counter;
static bool face_demo = true;
static bool demo_paused;
struct scan_counters {
	uint32_t callbacks, connectable, uuid_match, candidate_queued, candidate_dropped;
};
static struct scan_counters scan_counts;

static TickType_t ticks(uint32_t ms)
{
	TickType_t result = pdMS_TO_TICKS(ms);

	return result ? result : 1;
}

static void on_scan(T_LE_CB_DATA *data)
{
	taskENTER_CRITICAL();
	scan_counts.callbacks++;
	taskEXIT_CRITICAL();
	if (data == nullptr || data->p_le_scan_info == nullptr) {
		return;
	}
	const T_LE_SCAN_INFO *report = data->p_le_scan_info;
	if (report->adv_type != GAP_ADV_EVT_TYPE_UNDIRECTED &&
	    report->adv_type != GAP_ADV_EVT_TYPE_DIRECTED) {
		return;
	}
	taskENTER_CRITICAL();
	scan_counts.connectable++;
	taskEXIT_CRITICAL();
	BLEUUID wanted(SERVICE_UUID);
	uint8_t service_ad[NATIVE_SERVICE_AD_SIZE];
	if (!native_scan_sanitize(report->data, report->data_len, wanted.dataNative(), service_ad)) {
		return;
	}
	/* Preserve address/type/RSSI, but never expose arbitrary AD fields to the SDK. */
	T_LE_SCAN_INFO safe_report = *report;
	memset(safe_report.data, 0, sizeof(safe_report.data));
	memcpy(safe_report.data, service_ad, sizeof(service_ad));
	safe_report.data_len = sizeof(service_ad);
	T_LE_CB_DATA safe_callback = {};
	safe_callback.p_le_scan_info = &safe_report;
	BLEAdvertData found;
	found.parseScanInfo(&safe_callback);
	taskENTER_CRITICAL();
	scan_counts.uuid_match++;
	taskEXIT_CRITICAL();
	if (xSemaphoreTake(candidate_lock, 0) != pdTRUE) {
		taskENTER_CRITICAL();
		scan_counts.candidate_dropped++;
		taskEXIT_CRITICAL();
		return;
	}
	if (scanning && !candidate_ready) {
		candidate = found;
		candidate_ready = true;
		taskENTER_CRITICAL();
		scan_counts.candidate_queued++;
		taskEXIT_CRITICAL();
		xSemaphoreGive(candidate_event);
	} else {
		taskENTER_CRITICAL();
		scan_counts.candidate_dropped++;
		taskEXIT_CRITICAL();
	}
	xSemaphoreGive(candidate_lock);
}

static void on_ack(BLERemoteCharacteristic *characteristic, uint8_t *data, uint16_t length)
{
	xSemaphoreTake(ack_lock, portMAX_DELAY);
	if (characteristic == ack_characteristic &&
	    native_ack_accept(&ack_gate, data, length, millis())) {
		xSemaphoreGive(ack_event);
	}
	xSemaphoreGive(ack_lock);
}

static void on_disconnect(BLEClient *client)
{
	/* The SDK deletes remote objects after this callback returns. Hold deletion
	 * until a main-task GATT operation has completed its bounded SDK wait. */
	xSemaphoreTake(remote_lock, portMAX_DELAY);
	if (peer == client) {
		peer = nullptr;
		text_characteristic = nullptr;
		xSemaphoreTake(smp_lock, portMAX_DELAY);
		smp_characteristic = nullptr;
		smp_rx.active = false;
		smp_rx.failed = true;
		xSemaphoreGive(smp_event);
		xSemaphoreGive(smp_lock);
		xSemaphoreTake(ack_lock, portMAX_DELAY);
		ack_characteristic = nullptr;
		ack_gate.active = false;
		xSemaphoreGive(ack_event);
		xSemaphoreGive(ack_lock);
	}
	xSemaphoreGive(remote_lock);
	Serial.println("NU87|LINK state=disconnected");
}

static void on_smp(BLERemoteCharacteristic *characteristic, uint8_t *data, uint16_t length)
{
	xSemaphoreTake(smp_lock, portMAX_DELAY);
	if (characteristic == smp_characteristic &&
	    smp_relay_append(&smp_rx, data, length) != 0) {
		xSemaphoreGive(smp_event);
	}
	xSemaphoreGive(smp_lock);
}

static bool scan_peer(BLEAdvertData &target)
{
	while (xSemaphoreTake(candidate_event, 0) == pdTRUE) {}
	xSemaphoreTake(candidate_lock, portMAX_DELAY);
	candidate_ready = false;
	taskENTER_CRITICAL();
	memset(&scan_counts, 0, sizeof(scan_counts));
	taskEXIT_CRITICAL();
	scanning = true;
	xSemaphoreGive(candidate_lock);
	BLE.configScan()->startScan();
	(void)xSemaphoreTake(candidate_event, ticks(SCAN_MS));
	xSemaphoreTake(candidate_lock, portMAX_DELAY);
	scanning = false;
	bool found = candidate_ready;
	if (found) {
		target = candidate;
	}
	xSemaphoreGive(candidate_lock);
	BLE.configScan()->stopScan();
	delay(100);
	taskENTER_CRITICAL();
	struct scan_counters snapshot = scan_counts;
	taskEXIT_CRITICAL();
	char report[256];
	snprintf(report, sizeof(report),
		 "NU87|SCAN waiting=1 uptime_ms=%lu callbacks=%lu connectable=%lu "
		 "uuid_match=%lu candidate_queued=%lu candidate_dropped=%lu connected=0 ready=0",
		 (unsigned long)millis(), (unsigned long)snapshot.callbacks,
		 (unsigned long)snapshot.connectable, (unsigned long)snapshot.uuid_match,
		 (unsigned long)snapshot.candidate_queued,
		 (unsigned long)snapshot.candidate_dropped);
	Serial.println(report);
	return found;
}

static int connect_peer(BLEAdvertData &target, int8_t &connection)
{
	connection = -1;
	if (!BLE.configConnection()->connect(target, CONNECT_MS)) {
		return -1;
	}
	uint32_t started = millis();
	BLEAddr address = target.getAddr();
	do {
		/* The SDK's BLEAdvertData overload keeps a pointer into a temporary
		 * BLEAddr. Keep the address alive while looking up the connection. */
		connection = BLE.configConnection()->getConnId(address.data(),
							     target.getAddrType());
		if (connection >= 0 && BLE.connected(connection)) {
			break;
		}
		delay(20);
	} while ((uint32_t)(millis() - started) < CONNECT_MS + 500);
	if (connection < 0 || !BLE.connected(connection)) {
		return -2;
	}
	xSemaphoreTake(remote_lock, portMAX_DELAY);
	peer = BLE.addClient(connection);
	if (peer) {
		peer->setDisconnectCallback(on_disconnect);
		peer->discoverServices();
	}
	bool created = peer != nullptr;
	xSemaphoreGive(remote_lock);
	return created ? 0 : -3;
}

static int subscribe_peer(void)
{
	uint32_t started = millis();
	bool done = false;

	while ((uint32_t)(millis() - started) < DISCOVER_MS) {
		xSemaphoreTake(remote_lock, portMAX_DELAY);
		bool connected = peer && peer->connected();
		done = connected && peer->discoveryDone();
		xSemaphoreGive(remote_lock);
		if (!connected || done) {
			break;
		}
		delay(20);
	}
	if (!done) {
		return -4;
	}
	xSemaphoreTake(remote_lock, portMAX_DELAY);
	int result = -5;
	if (peer && peer->connected()) {
		BLERemoteService *service = peer->getService(SERVICE_UUID);
		BLERemoteCharacteristic *ack = service ?
			service->getCharacteristic(ACK_UUID) : nullptr;
		text_characteristic = service ? service->getCharacteristic(TEXT_UUID) : nullptr;
		if (ack && ack->canNotify() && text_characteristic &&
		    (text_characteristic->getProperties() & GATT_CHAR_PROP_WRITE)) {
			BLERemoteDescriptor *cccd = ack->getDescriptor("2902");
			if (cccd) {
				ack->setBufferLen(BLE_DISPLAY_ACK_SIZE);
				ack->setNotifyCallback(on_ack);
				xSemaphoreTake(ack_lock, portMAX_DELAY);
				ack_characteristic = ack;
				xSemaphoreGive(ack_lock);
				uint8_t notify[] = {1, 0};
				result = cccd->setData(notify, sizeof(notify)) ? 0 : -6;
				/* OTA is optional: the face demo still supports older receivers. */
				BLERemoteService *smp_service = peer->getService(SMP_SERVICE_UUID);
				BLERemoteCharacteristic *smp = smp_service ?
					smp_service->getCharacteristic(SMP_UUID) : nullptr;
				BLERemoteDescriptor *smp_cccd = smp ? smp->getDescriptor("2902") :
					nullptr;
				if (result == 0 && smp && smp->canNotify() && smp->canWrite() &&
				    smp_cccd) {
					smp->setBufferLen(SMP_RELAY_MAX);
					smp->setNotifyCallback(on_smp);
					xSemaphoreTake(smp_lock, portMAX_DELAY);
					smp_characteristic = smp;
					xSemaphoreGive(smp_lock);
					if (!smp_cccd->setData(notify, sizeof(notify))) {
						xSemaphoreTake(smp_lock, portMAX_DELAY);
						smp_characteristic = nullptr;
						xSemaphoreGive(smp_lock);
					}
				}
			}
		}
	}
	xSemaphoreGive(remote_lock);
	return result;
}

static void relay_smp(uint8_t *request, size_t length)
{
	static char response[2 * SMP_RELAY_MAX + 64];
	while (xSemaphoreTake(smp_event, 0) == pdTRUE) {}
	xSemaphoreTake(smp_lock, portMAX_DELAY);
	bool valid = smp_relay_begin(&smp_rx, request, length);
	xSemaphoreGive(smp_lock);
	bool writes = valid;
	for (size_t offset = 0; writes && offset < length; offset += 20) {
		size_t count = length - offset < 20 ? length - offset : 20;

		xSemaphoreTake(remote_lock, portMAX_DELAY);
		writes = peer && peer->connected() && smp_characteristic &&
			smp_characteristic->setData(request + offset, count, GATT_WRITE_TYPE_CMD);
		xSemaphoreGive(remote_lock);
		if (writes) {
			delay(5);
		}
	}
	if (writes) {
		(void)xSemaphoreTake(smp_event, ticks(10000));
	}
	xSemaphoreTake(smp_lock, portMAX_DELAY);
	bool complete = writes && smp_rx.complete && !smp_rx.failed;
	int result = complete ? 0 : (valid ? -2 : -1);
	int used = snprintf(response, sizeof(response), "NU87|SMP result=%d data=", result);
	if (complete) {
		static const char hex[] = "0123456789abcdef";
		for (size_t i = 0; i < smp_rx.used; i++) {
			response[used++] = hex[smp_rx.data[i] >> 4];
			response[used++] = hex[smp_rx.data[i] & 15];
		}
	}
	response[used] = '\0';
	smp_rx.active = false;
	xSemaphoreGive(smp_lock);
	Serial.println(response);
}

static void begin_send(void)
{
	while (xSemaphoreTake(ack_event, 0) == pdTRUE) {}
	xSemaphoreTake(ack_lock, portMAX_DELAY);
	native_ack_begin(&ack_gate, ++sequence);
	xSemaphoreGive(ack_lock);
}

static int finish_send(bool writes)
{
	if (writes) {
		(void)xSemaphoreTake(ack_event, ticks(BLE_DISPLAY_TIMEOUT_MS));
	}
	xSemaphoreTake(ack_lock, portMAX_DELAY);
	struct native_ack_gate received = ack_gate;
	ack_gate.active = false;
	xSemaphoreGive(ack_lock);
	if (received.received) {
		Serial.print("NU87|ACK seq=");
		Serial.print(received.sequence);
		Serial.print(" status=");
		Serial.print(received.status);
		Serial.print(" at_ms=");
		Serial.println(received.at_ms);
	}
	if (native_ack_success(&received, writes)) {
		return 0;
	}
	return !writes ? -8 : (received.received ? -9 : -10);
}

static bool write_frame(uint8_t *frame, size_t length, bool final)
{
	if (final) {
		/* A notification may arrive before the final write returns. */
		xSemaphoreTake(ack_lock, portMAX_DELAY);
		ack_gate.final_started = true;
		xSemaphoreGive(ack_lock);
	}
	xSemaphoreTake(remote_lock, portMAX_DELAY);
	bool writes = peer && peer->connected() && text_characteristic &&
		text_characteristic->setData(frame, length, GATT_WRITE_TYPE_REQ);
	xSemaphoreGive(remote_lock);
	return writes;
}

static int send_text(const char *text)
{
	size_t length = strlen(text);
	char lines[BLE_DISPLAY_ROWS][BLE_DISPLAY_COLUMNS + 1];

	if (!ble_display_split_lines(text, length, lines)) {
		return -7;
	}
	begin_send();
	Serial.print("NU87|TEXT seq=");
	Serial.print(sequence);
	Serial.print(" length=");
	Serial.print(length);
	Serial.println(" attempt=1");
	bool writes = true;
	uint32_t started = millis();
	for (uint8_t chunk = 0; (size_t)chunk * BLE_DISPLAY_CHUNK < length; ++chunk) {
		uint8_t frame[BLE_DISPLAY_FRAME_MAX];
		size_t frame_length = ble_display_build_text(text, length, sequence, chunk, frame);

		if (!frame_length || (uint32_t)(millis() - started) >= BLE_DISPLAY_TIMEOUT_MS) {
			writes = false;
			break;
		}
		writes = write_frame(frame, frame_length,
			((size_t)chunk + 1) * BLE_DISPLAY_CHUNK >= length);
		if (!writes) {
			break;
		}
	}
	return finish_send(writes);
}

static int send_face(void)
{
	struct ble_face face = ble_face_demo(millis());
	uint8_t frame[BLE_FACE_FRAME_SIZE];

	begin_send();
	size_t length = ble_face_build(&face, sequence, frame);
	Serial.print("NU87|FACE seq=");
	Serial.print(sequence);
	Serial.print(" expression=");
	Serial.print(face.expression);
	Serial.print(" openness=");
	Serial.print(face.openness);
	Serial.print(" gaze_x=");
	Serial.print(face.gaze_x);
	Serial.print(" gaze_y=");
	Serial.println(face.gaze_y);
	return finish_send(length && write_frame(frame, length, true));
}

static void poll_mode(void)
{
	static char command[2 * SMP_RELAY_MAX + 5];
	static size_t used;
	static bool overflow;

	for (int i = 0; i < (int)sizeof(command) && Serial.available(); i++) {
		char c = Serial.read();

		if (c == '\n' || c == '\r') {
			command[used] = '\0';
			if (!overflow && (!strcmp(command, "face") || !strcmp(command, "text") ||
					  !strcmp(command, "pause"))) {
				face_demo = !strcmp(command, "face");
				demo_paused = !strcmp(command, "pause");
				Serial.print("NU87|MODE value=");
				Serial.println(command);
			} else if (!overflow && !strcmp(command, "relay-status")) {
				char status[128];
				xSemaphoreTake(remote_lock, portMAX_DELAY);
				bool smp_ready = smp_characteristic != nullptr;
				xSemaphoreGive(remote_lock);

				snprintf(status, sizeof(status),
					 "NU87|RELAY firmware=3 smp_ready=%u paused=%u",
					 smp_ready, demo_paused);
				Serial.println(status);
			} else if (!overflow && !strncmp(command, "smp ", 4)) {
				uint8_t request[SMP_RELAY_MAX];
				size_t chars = used - 4;
				bool valid = chars != 0 && chars % 2 == 0;

				for (size_t j = 0; valid && j < chars; j++) {
					char c = command[j + 4];
					int nibble = c >= '0' && c <= '9' ? c - '0' :
						c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
					valid = nibble >= 0;
					if (valid && j % 2 == 0) {
						request[j / 2] = nibble << 4;
					} else if (valid) {
						request[j / 2] |= nibble;
					}
				}
				if (valid) {
					relay_smp(request, chars / 2);
				} else {
					Serial.println("NU87|SMP result=-1 data=");
				}
			}
			used = 0;
			overflow = false;
		} else if (used < sizeof(command) - 1) {
			command[used++] = c;
		} else {
			overflow = true;
		}
	}
}

void setup(void)
{
	Serial.begin(1500000);
	Serial.println("NU87|BOOT app=ble_display_native firmware=3 protocol=1 "
		       "baud=1500000 diagnostic=arduino-3.1.9 demo=face features=smp_relay");
	candidate_lock = xSemaphoreCreateMutex();
	candidate_event = xSemaphoreCreateBinary();
	remote_lock = xSemaphoreCreateMutex();
	ack_lock = xSemaphoreCreateMutex();
	ack_event = xSemaphoreCreateBinary();
	smp_lock = xSemaphoreCreateMutex();
	smp_event = xSemaphoreCreateBinary();
	if (!candidate_lock || !candidate_event || !remote_lock || !ack_lock || !ack_event ||
	    !smp_lock || !smp_event) {
		Serial.println("NU87|READY result=-11");
		return;
	}
	BLE.init();
	BLE.setScanCallback(on_scan);
	BLE.beginCentral(1);
	BLE.configClient();
	BLE.configScan()->setScanMode(GAP_SCAN_MODE_PASSIVE);
	BLE.configScan()->setScanInterval(60);
	BLE.configScan()->setScanWindow(30);
	BLE.configScan()->setScanDuplicateFilter(false);
	BLE.configScan()->updateScanParams();
	initialized = true;
	Serial.println("NU87|BLE enable_result=0");
}

void loop(void)
{
	if (!initialized) {
		delay(1000);
		return;
	}
	BLEAdvertData target;
	if (!scan_peer(target)) {
		delay(1000);
		return;
	}
	int8_t connection;
	int result = connect_peer(target, connection);
	Serial.print("NU87|CONNECT result=");
	Serial.println(result);
	if (!result) {
		result = subscribe_peer();
		Serial.print("NU87|SUBSCRIBE result=");
		Serial.println(result);
	}
	Serial.print("NU87|READY result=");
	Serial.println(result);
	while (!result) {
		poll_mode();
		xSemaphoreTake(remote_lock, portMAX_DELAY);
		bool connected = peer && peer->connected();
		xSemaphoreGive(remote_lock);
		if (!connected) {
			result = -13;
			break;
		}
		if (demo_paused) {
			delay(20);
			continue;
		}
		if (face_demo) {
			result = send_face();
		} else {
			char text[BLE_DISPLAY_MAX_TEXT + 1];

			snprintf(text, sizeof(text),
				 "NU87 -> NU40\nCount: %lu\nUptime: %lus\nBLE link ready",
				 (unsigned long)counter++, (unsigned long)(millis() / 1000));
			result = send_text(text);
		}
		Serial.print("NU87|SEND result=");
		Serial.println(result);
		delay(face_demo ? 120 : 1000);
	}
	if (connection >= 0 && BLE.connected(connection)) {
		BLE.configConnection()->disconnect(connection);
		uint32_t started = millis();
		while (BLE.connected(connection) && (uint32_t)(millis() - started) < 6000) {
			delay(20);
		}
		/* Never begin another connection while cleanup is outstanding. */
		if (BLE.connected(connection)) {
			initialized = false;
			Serial.println("NU87|READY result=-12");
		}
	}
	delay(1000);
}
