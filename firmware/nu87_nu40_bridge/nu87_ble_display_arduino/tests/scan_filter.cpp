/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "../scan_filter.h"

static const char SERVICE_UUID[] = "90e2c000-3c0d-4ef3-9d78-574cd1a64087";
static const uint8_t target_uuid[] = {
	0x87, 0x40, 0xa6, 0xd1, 0x4c, 0x57, 0x78, 0x9d,
	0xf3, 0x4e, 0x0d, 0x3c, 0x00, 0xc0, 0xe2, 0x90,
};
#define GAP_ADV_EVT_TYPE_UNDIRECTED 0
#define GAP_ADV_EVT_TYPE_DIRECTED 1
#define pdTRUE 1
#define taskENTER_CRITICAL() do {} while (0)
#define taskEXIT_CRITICAL() do {} while (0)

struct T_LE_SCAN_INFO {
	uint8_t bd_addr[6], remote_addr_type, adv_type;
	int8_t rssi;
	uint8_t data_len, data[31];
};
union T_LE_CB_DATA { T_LE_SCAN_INFO *p_le_scan_info; };
static unsigned int parser_calls, events;
class BLEUUID {
public:
	explicit BLEUUID(const char *text) { assert(strcmp(text, SERVICE_UUID) == 0); }
	const uint8_t *dataNative() { return target_uuid; }
};
class BLEAdvertData {
public:
	T_LE_SCAN_INFO report = {};
	void parseScanInfo(T_LE_CB_DATA *callback)
	{
		parser_calls++;
		report = *callback->p_le_scan_info;
		assert(report.data_len == 18 && report.data[0] == 17 && report.data[1] == 7);
		assert(memcmp(report.data + 2, target_uuid, 16) == 0);
		for (size_t i = 18; i < sizeof(report.data); i++) {
			assert(report.data[i] == 0);
		}
	}
};
static BLEAdvertData candidate;
static bool scanning = true, candidate_ready, lock_available = true;
static int candidate_lock = 1, candidate_event = 2;
static struct {
	unsigned int callbacks, connectable, uuid_match, candidate_queued, candidate_dropped;
} scan_counts;
static int xSemaphoreTake(int semaphore, int timeout)
{
	assert(semaphore == candidate_lock && timeout == 0);
	return lock_available ? pdTRUE : 0;
}
static void xSemaphoreGive(int semaphore)
{
	if (semaphore == candidate_event) {
		events++;
	} else {
		assert(semaphore == candidate_lock);
	}
}

#include "app_on_scan.h"

static void service_ad(uint8_t *data, uint8_t type = 7)
{
	data[0] = 17;
	data[1] = type;
	memcpy(data + 2, target_uuid, sizeof(target_uuid));
}

static void test_filter(void)
{
	uint8_t data[31] = {2, 1, 6}, output[18];

	service_ad(data + 3);
	assert(native_scan_sanitize(data, 21, target_uuid, output));
	assert(output[0] == 17 && output[1] == 7 && memcmp(output + 2, target_uuid, 16) == 0);
	service_ad(data, 6);
	assert(native_scan_sanitize(data, 18, target_uuid, output));
	data[2] ^= 1;
	memset(output, 0x5a, sizeof(output));
	assert(!native_scan_sanitize(data, 18, target_uuid, output));
	assert(output[0] == 0x5a);
	assert(!native_scan_sanitize(nullptr, 1, target_uuid, output));
	assert(!native_scan_sanitize(data, 1, nullptr, output));
	assert(!native_scan_sanitize(data, 1, target_uuid, nullptr));
	assert(!native_scan_sanitize(data, 0, target_uuid, output));
	assert(!native_scan_sanitize(data, 32, target_uuid, output));
	service_ad(data);
	assert(!native_scan_sanitize(data, 17, target_uuid, output));
	data[0] = 16;
	assert(!native_scan_sanitize(data, 17, target_uuid, output));
	service_ad(data);
	data[18] = 12;
	assert(!native_scan_sanitize(data, 20, target_uuid, output));
	data[18] = 0;
	assert(native_scan_sanitize(data, 19, target_uuid, output));
}

static void test_callback(void)
{
	T_LE_SCAN_INFO report = {{1, 2, 3, 4, 5, 6}, 1, 3, -42, 31, {0}};
	T_LE_CB_DATA callback = {};
	callback.p_le_scan_info = &report;
	/* This valid eight-UUID list exceeds the SDK parser's seven-entry array. */
	report.data[0] = 17;
	report.data[1] = 3;
	on_scan(&callback);
	assert(parser_calls == 0 && scan_counts.connectable == 0);
	report.adv_type = GAP_ADV_EVT_TYPE_UNDIRECTED;
	report.data_len = 18;
	on_scan(&callback);
	assert(parser_calls == 0 && scan_counts.connectable == 1);
	/* A malformed manufacturer field is never passed through to the SDK. */
	report.data[0] = 2;
	report.data[1] = 0xff;
	report.data[2] = 0x12;
	service_ad(report.data + 3);
	report.data_len = 21;
	T_LE_SCAN_INFO original = report;
	on_scan(&callback);
	assert(parser_calls == 1 && events == 1 && candidate_ready);
	assert(scan_counts.uuid_match == 1 && scan_counts.candidate_queued == 1);
	assert(memcmp(candidate.report.bd_addr, report.bd_addr, 6) == 0);
	assert(candidate.report.remote_addr_type == 1 && candidate.report.rssi == -42);
	assert(candidate.report.adv_type == GAP_ADV_EVT_TYPE_UNDIRECTED);
	assert(memcmp(original.data, report.data, sizeof(report.data)) == 0);
	assert(report.data_len == original.data_len);
	lock_available = false;
	on_scan(&callback);
	assert(parser_calls == 2 && events == 1 && scan_counts.candidate_dropped == 1);
	lock_available = true;
	on_scan(&callback);
	assert(parser_calls == 3 && events == 1 && scan_counts.candidate_dropped == 2);
	report.adv_type = GAP_ADV_EVT_TYPE_DIRECTED;
	candidate_ready = false;
	on_scan(&callback);
	assert(parser_calls == 4 && events == 2 && candidate.report.adv_type == 1);
	report.data_len = 32;
	on_scan(&callback);
	assert(parser_calls == 4);
	on_scan(nullptr);
	callback.p_le_scan_info = nullptr;
	on_scan(&callback);
	assert(parser_calls == 4);
}

int main(void)
{
	test_filter();
	test_callback();
	return 0;
}
