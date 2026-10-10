/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_heap.h>
#include "wifi_intf_drv_to_app_basic.h"
#include "gateway_link.h"

LOG_MODULE_REGISTER(gateway_radio_diag, LOG_LEVEL_INF);

/* This pinned Zephyr defines _system_heap as a synchronized k_heap, not a
 * sys_heap. Snapshot its inner allocator under the same lock as k_malloc;
 * logging must happen after releasing that lock.
 */
extern struct k_heap _system_heap;

static struct k_spinlock radio_diagnostic_lock;
enum diagnostic_command { CMD_RESET, CMD_MASK, CMD_LE_MASK, CMD_SCAN_PARAMS, CMD_SCAN_ENABLE, CMD_COUNT };
struct command_diagnostic { uint32_t count; int status; };
struct radio_diagnostic_state {
	uint32_t rx_packets, rx_events, le_events, adv_events, malformed_events;
	uint32_t rfk_calls;
	int last_rfk_type, last_rfk_result;
	struct command_diagnostic commands[CMD_COUNT];
	int meta_mask, adv_mask, scan_type, interval, window, own_address, filter_policy;
	int scan_enable, duplicates;
};
static struct radio_diagnostic_state radio_diagnostics = {
	.last_rfk_type = -1, .last_rfk_result = -1,
	.commands = { {0, -1}, {0, -1}, {0, -1}, {0, -1}, {0, -1} },
	.meta_mask = -1, .adv_mask = -1, .scan_type = -1, .interval = -1, .window = -1,
	.own_address = -1, .filter_policy = -1, .scan_enable = -1, .duplicates = -1,
};

static int command_index(uint16_t opcode)
{
	switch (opcode) {
	case 0x0c03: return CMD_RESET;
	case 0x0c01: return CMD_MASK;
	case 0x2001: return CMD_LE_MASK;
	case 0x200b: return CMD_SCAN_PARAMS;
	case 0x200c: return CMD_SCAN_ENABLE;
	default: return -1;
	}
}

static void count_event(uint32_t *counter)
{
	if (*counter != UINT32_MAX) { ++*counter; }
}

/* Strong replacement for the pinned H4 driver's weak, empty vendor TX hook.
 * Observe scan commands for the asynchronous coexistence reporter as well as
 * diagnostics. Neither observer alters the original HCI packet.
 */
void bt_vendor_process_tx_frame(uint8_t type, uint8_t *data, uint16_t length)
{
	gateway_coex_scan_tx(type, data, length);
	if (type != 0x01 || data == NULL || length < 3 || data[2] > length - 3) { return; }
	int index = command_index((uint16_t)data[0] | ((uint16_t)data[1] << 8));
	if (index < 0) { return; }
	static const uint8_t parameter_lengths[CMD_COUNT] = {0, 8, 8, 7, 2};
	if (data[2] != parameter_lengths[index]) { return; }
	k_spinlock_key_t key = k_spin_lock(&radio_diagnostic_lock);
	count_event(&radio_diagnostics.commands[index].count);
	radio_diagnostics.commands[index].status = -1;
	switch (index) {
	case CMD_RESET:
		/* A new reset makes previously observed radio settings uncertain. */
		radio_diagnostics.meta_mask = radio_diagnostics.adv_mask = -1;
		radio_diagnostics.scan_enable = -1;
		break;
	case CMD_MASK:
		/* LE Meta Event is bit61 of the eight-byte little-endian mask. */
		radio_diagnostics.meta_mask = (data[10] >> 5) & 1;
		break;
	case CMD_LE_MASK:
		/* Legacy Advertising Report is bit1 of the LE event mask. */
		radio_diagnostics.adv_mask = (data[3] >> 1) & 1;
		break;
	case CMD_SCAN_PARAMS:
		radio_diagnostics.scan_type = data[3];
		radio_diagnostics.interval = (uint16_t)data[4] | ((uint16_t)data[5] << 8);
		radio_diagnostics.window = (uint16_t)data[6] | ((uint16_t)data[7] << 8);
		radio_diagnostics.own_address = data[8];
		radio_diagnostics.filter_policy = data[9];
		break;
	case CMD_SCAN_ENABLE:
		radio_diagnostics.scan_enable = data[3];
		radio_diagnostics.duplicates = data[4];
		break;
	}
	k_spin_unlock(&radio_diagnostic_lock, key);
}

/* ABI declarations avoid the vendor header's incompatible IRQ declarations.
 * The pinned implementations of the four configuration getters only read
 * saved bytes/bits. state_get reads TDMA state and prints its numeric values.
 * Call from a worker once per scan transition, never from the RX callback.
 */
extern bool rtk_coex_com_coex_is_enabled(void);
extern void rtk_coex_com_state_get(void);
extern uint8_t rtk_coex_wl_get_rfe_type(void);
extern uint8_t rtk_coex_wl_get_ant_div(void);
extern uint8_t rtk_coex_wl_get_bt_pos(void);
extern uint8_t rtk_coex_wl_get_ant_type(void);

void gateway_radio_coex_diagnostics(void)
{
	bool enabled = rtk_coex_com_coex_is_enabled();
	uint8_t rfe = rtk_coex_wl_get_rfe_type();
	uint8_t diversity = rtk_coex_wl_get_ant_div();
	uint8_t position = rtk_coex_wl_get_bt_pos();
	uint8_t antenna = rtk_coex_wl_get_ant_type();
	LOG_INF("Coex enabled=%u rfe=%u ant_div=%u bt_pos=%u ant_type=%u",
		(unsigned int)enabled, (unsigned int)rfe, (unsigned int)diversity,
		(unsigned int)position, (unsigned int)antenna);
	rtk_coex_com_state_get();
}

void gateway_radio_diagnostics(const char *stage)
{
	struct sys_memory_stats stats;
	k_spinlock_key_t key = k_spin_lock(&_system_heap.lock);
	int result = sys_heap_runtime_stats_get(&_system_heap.heap, &stats);

	k_spin_unlock(&_system_heap.lock, key);
	if (result != 0) {
		LOG_WRN("Heap %s: stats error %d", stage != NULL ? stage : "unknown", result);
		return;
	}
	LOG_INF("Heap %s: free=%zu allocated=%zu max=%zu",
		stage != NULL ? stage : "unknown", stats.free_bytes,
		stats.allocated_bytes, stats.max_allocated_bytes);
}

extern int __real_wifi_connect_zephyr(uint8_t *ssid, uint8_t ssid_len,
		uint8_t *psk, uint8_t psk_len, uint8_t channel);

int __wrap_wifi_connect_zephyr(uint8_t *ssid, uint8_t ssid_len,
		uint8_t *psk, uint8_t psk_len, uint8_t channel)
{
	gateway_radio_diagnostics("before_wifi_connect");
	int result = __real_wifi_connect_zephyr(ssid, ssid_len, psk, psk_len, channel);
	uint8_t join_status = UINT8_MAX;
	int status_result = wifi_get_join_status(&join_status);

	gateway_radio_diagnostics("after_wifi_connect");
	/* The Zephyr driver collapses every vendor failure to -EAGAIN. Preserve
	 * the underlying numeric result without exposing SSID/password contents,
	 * lengths, or pointers in the diagnostics.
	 */
	LOG_INF("Wi-Fi vendor result=%d join_status=%u status_result=%d",
		result, (unsigned int)join_status, status_result);
	return result;
}

extern int __real_rtk_coex_btc_bt_rfk(void *parameter, uint16_t length);

int __wrap_rtk_coex_btc_bt_rfk(void *parameter, uint16_t length)
{
	int type = -1;
	if (parameter != NULL && length >= 1) {
		/* The pinned HCI object's DWARF confirms a one-byte bt_rfk_type at
		 * offset zero in the five-byte bt_rfk_param (-fshort-enums). Its
		 * SDK header imports IRQ declarations incompatible with Zephyr, so
		 * read only this byte, without inspecting calibration payloads.
		 */
		type = *(const uint8_t *)parameter;
	}
	int result = __real_rtk_coex_btc_bt_rfk(parameter, length);
	k_spinlock_key_t key = k_spin_lock(&radio_diagnostic_lock);
	count_event(&radio_diagnostics.rfk_calls);
	radio_diagnostics.last_rfk_type = type;
	radio_diagnostics.last_rfk_result = result;
	k_spin_unlock(&radio_diagnostic_lock, key);
	LOG_INF("BT RF calibration type=%d result=%d", type, result);
	return result;
}

extern void __real_bt_coex_process_rx_frame(uint8_t type, uint8_t *data, uint16_t length);

void __wrap_bt_coex_process_rx_frame(uint8_t type, uint8_t *data, uint16_t length)
{
	/* H4 packet type 4 is an event. Its first two bytes are event code and
	 * parameter length; LE meta-event parameters begin with a subevent code.
	 * Count complete frames only, without reading addresses or AD payloads.
	 */
	bool event = type == 0x04;
	bool valid = event && data != NULL && length >= 2 && data[1] <= length - 2;
	bool le_event = valid && data[0] == 0x3e && data[1] >= 1;
	bool adv_event = le_event && data[1] >= 2 && data[2] == 0x02;
	k_spinlock_key_t key = k_spin_lock(&radio_diagnostic_lock);
	count_event(&radio_diagnostics.rx_packets);
	if (event) { count_event(&radio_diagnostics.rx_events); }
	if (event && (!valid || (data[0] == 0x3e && data[1] == 0))) {
		count_event(&radio_diagnostics.malformed_events);
	}
	if (le_event) { count_event(&radio_diagnostics.le_events); }
	if (adv_event) { count_event(&radio_diagnostics.adv_events); }
	if (valid && data[1] >= 4) {
		int index = -1, status = -1;
		if (data[0] == 0x0e) { /* Command Complete */
			index = command_index((uint16_t)data[3] | ((uint16_t)data[4] << 8));
			status = data[5];
		} else if (data[0] == 0x0f) { /* Command Status, including controller rejections */
			index = command_index((uint16_t)data[4] | ((uint16_t)data[5] << 8));
			status = data[2];
		}
		if (index >= 0) { radio_diagnostics.commands[index].status = status; }
	}
	k_spin_unlock(&radio_diagnostic_lock, key);
	/* The diagnostic must not drop, retain, mutate, or replace vendor data. */
	gateway_coex_scan_rx_begin();
	__real_bt_coex_process_rx_frame(type, data, length);
	/* Preserve native coexistence/profile processing first. The scan observer
	 * commits successful commands and schedules vendor reporting off RX.
	 */
	gateway_coex_scan_rx(type, data, length);
	gateway_coex_scan_rx_end();
}

int gateway_hci_diagnostics(char *json, size_t capacity)
{
	if (json == NULL || capacity == 0) { return -EINVAL; }
	k_spinlock_key_t key = k_spin_lock(&radio_diagnostic_lock);
	struct radio_diagnostic_state snapshot = radio_diagnostics;
	k_spin_unlock(&radio_diagnostic_lock, key);
	/* Each command pair is [TX count, last controller status]; -1 is unknown
	 * or awaiting completion. Scan settings are observed TX values, whose
	 * controller acceptance is shown by the matching command status.
	 */
	int length = snprintf(json, capacity,
		"{\"rx_packets\":%u,\"rx_events\":%u,\"le_events\":%u,\"adv_events\":%u,"
		"\"malformed_events\":%u,\"rfk_calls\":%u,\"last_rfk_type\":%d,\"last_rfk_result\":%d,"
		"\"commands\":{\"reset\":[%u,%d],\"mask\":[%u,%d],\"le_mask\":[%u,%d],"
		"\"scan_params\":[%u,%d],\"scan_enable\":[%u,%d]},"
		"\"scan\":{\"meta_mask\":%d,\"adv_mask\":%d,\"type\":%d,\"interval\":%d,\"window\":%d,"
		"\"own_addr\":%d,\"filter\":%d,\"enable\":%d,\"duplicates\":%d}}",
		(unsigned int)snapshot.rx_packets, (unsigned int)snapshot.rx_events,
		(unsigned int)snapshot.le_events, (unsigned int)snapshot.adv_events,
		(unsigned int)snapshot.malformed_events, (unsigned int)snapshot.rfk_calls,
		snapshot.last_rfk_type, snapshot.last_rfk_result,
		(unsigned int)snapshot.commands[CMD_RESET].count, snapshot.commands[CMD_RESET].status,
		(unsigned int)snapshot.commands[CMD_MASK].count, snapshot.commands[CMD_MASK].status,
		(unsigned int)snapshot.commands[CMD_LE_MASK].count, snapshot.commands[CMD_LE_MASK].status,
		(unsigned int)snapshot.commands[CMD_SCAN_PARAMS].count, snapshot.commands[CMD_SCAN_PARAMS].status,
		(unsigned int)snapshot.commands[CMD_SCAN_ENABLE].count, snapshot.commands[CMD_SCAN_ENABLE].status,
		snapshot.meta_mask, snapshot.adv_mask, snapshot.scan_type, snapshot.interval, snapshot.window,
		snapshot.own_address, snapshot.filter_policy, snapshot.scan_enable, snapshot.duplicates);
	if (length < 0 || (size_t)length >= capacity) { json[0] = '\0'; return -ENOSPC; }
	return length;
}
