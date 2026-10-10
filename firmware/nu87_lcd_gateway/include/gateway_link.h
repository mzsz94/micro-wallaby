/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MZSZ_GATEWAY_LINK_H
#define MZSZ_GATEWAY_LINK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lcd_protocol.h"

struct gateway_state {
	bool wifi_connected;
	char wifi_ipv4[16];
	bool ble_configured;
	bool ble_connected;
	bool lcd_ready;
	bool dfu_ready;
	char peer_addr[32];
	int last_error;
};

/* RX-context callback: copy/queue the data and return; never block here. */
typedef void (*gateway_dfu_notify_fn)(const uint8_t *data, size_t len, void *user);
int gateway_radio_init(void);
/* Diagnostic stage must be a non-sensitive, static string. */
void gateway_radio_diagnostics(const char *stage);
void gateway_radio_coex_diagnostics(void);
int gateway_hci_diagnostics(char *json, size_t capacity);
int gateway_hci_transport_diagnostics(char *json, size_t capacity);
int gateway_coex_scan_diagnostics(char *json, size_t capacity);
/* Observe HCI framing only; vendor coexistence work runs on its own thread. */
void gateway_coex_scan_tx(uint8_t type, const uint8_t *data, uint16_t length);
void gateway_coex_scan_rx(uint8_t type, const uint8_t *data, uint16_t length);
/* H4 RX thread only: serialize native coexistence processing with reports. */
void gateway_coex_scan_rx_begin(void);
void gateway_coex_scan_rx_end(void);
void gateway_get_state(struct gateway_state *state);
/* One blocking request at a time. Success means a matching completion status
 * arrived; status[4] still carries the peripheral's LCD_RESULT_* result.
 * No automatic retry or replay is performed on timeout/disconnect.
 */
int gateway_lcd_command(const uint8_t *data, size_t len,
			uint8_t status[LCD_STATUS_SIZE], int timeout_ms);

/* NU40-KIT-DISPLAY mode: bounded discovery and explicit, fresh peer selection.
 * Candidates returns a JSON array's byte count or a negative error. Selection
 * starts connection asynchronously; display succeeds only after matching ACK.
 */
#if defined(CONFIG_GATEWAY_NU40_KIT)
int gateway_peer_candidates(char *json, size_t capacity);
int gateway_scan_diagnostics(char *json, size_t capacity);
/* Queue an active/passive scan-mode change; never interrupt an active peer. */
int gateway_scan_mode(const char *mode);
int gateway_select_peer(const char *address, const char *type);
int gateway_display_text(const uint8_t *text, size_t len, int timeout_ms);
#endif
int gateway_dfu_enter(void);
int gateway_dfu_control(const uint8_t *data, size_t len);
int gateway_dfu_packet(const uint8_t *data, size_t len);
void gateway_dfu_set_notify_callback(gateway_dfu_notify_fn callback, void *user);
/* Leave DFU transport mode after a finished/failed transfer. No device reset
 * or activation command is sent. Subsequent scanning targets the LCD identity.
 */
void gateway_dfu_leave(void);

/* Internal Wi-Fi module interface; also useful to the HTTP listener. */
int gateway_wifi_start(void);
bool gateway_wifi_ipv4_ready(void);
int gateway_wifi_address(char *buffer, size_t capacity);
#endif
