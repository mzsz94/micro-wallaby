/* SPDX-License-Identifier: Apache-2.0 */
#include "gateway_http.h"
#include "gateway_usb_protocol.h"
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>

extern int wifi_is_running(unsigned char index);
static const struct device *const console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
RING_BUF_DECLARE(rx_ring, 4096);
K_SEM_DEFINE(rx_ready, 0, 1);
static atomic_t rx_overflow;
static uint8_t encoded[1536], decoded[1536];
static uint8_t tx_packet[USB_RPC_HEADER + USB_RPC_BODY_MAX + 4];
static uint8_t tx_wire[USB_RPC_WIRE_MAX];
static struct usb_rpc_session session;

static int defer_console_irq(void)
{
	/* ROM leaves console interrupt sources enabled. Until the shared RF's
	 * polling startup prompt is consumed, the Zephyr ISR has no RX owner. */
	irq_disable(DT_IRQN(DT_CHOSEN(zephyr_console)));
	return 0;
}
SYS_INIT(defer_console_irq, PRE_KERNEL_2, 0);

static void receive(const struct device *dev, void *user)
{
	ARG_UNUSED(user);
	uint8_t data[64];
	uart_irq_update(dev);
	while (uart_irq_rx_ready(dev)) {
		int count = uart_fifo_read(dev, data, sizeof(data));
		if (count <= 0) { break; }
		if (ring_buf_put(&rx_ring, data, (uint32_t)count) != (uint32_t)count) {
			atomic_set(&rx_overflow, 1);
		}
		k_sem_give(&rx_ready);
		uart_irq_update(dev);
	}
}

static void reply(void *user, int code, const void *body, size_t length, const char *type)
{
	const struct usb_rpc_request *request = user;
	ARG_UNUSED(type);
	if (length > USB_RPC_BODY_MAX) { code = 500; body = ""; length = 0; }
	memset(tx_packet, 0, USB_RPC_HEADER);
	memcpy(tx_packet, "MW\x01\x02", 4);
	usb_rpc_put32(tx_packet + 4, request->session);
	usb_rpc_put32(tx_packet + 8, request->sequence);
	usb_rpc_put16(tx_packet + 12, (uint16_t)code);
	usb_rpc_put16(tx_packet + 16, (uint16_t)length);
	if (length) { memcpy(tx_packet + USB_RPC_HEADER, body, length); }
	size_t size = USB_RPC_HEADER + length;
	usb_rpc_put32(tx_packet + size, usb_rpc_crc(tx_packet, size));
	size_t wire_size = usb_rpc_encode(tx_packet, size + 4, tx_wire, sizeof(tx_wire));
	if (!wire_size) { return; }
	uart_poll_out(console, 0);
	for (size_t i = 0; i < wire_size; i++) { uart_poll_out(console, tx_wire[i]); }
	uart_poll_out(console, 0);
}

static void execute(size_t length)
{
	struct usb_rpc_request request;
	if (!usb_rpc_parse(decoded, length, &request)) { return; }
	if (!gateway_api_authenticated(request.token)) {
		reply(&request, 401, "unauthorized", 12, "text/plain"); return;
	}
	if (!usb_rpc_accept(&session, request.session, request.sequence,
			    request.method == USB_RPC_HELLO)) {
		reply(&request, 409, "session conflict", 16, "text/plain"); return;
	}
	if (request.method == USB_RPC_HELLO) {
		/* A new owner authenticates explicitly. It cannot replay a sequence
		 * from the same session, including an ambiguous timed-out mutation. */
		static const char hello[] = "{\"protocol\":\"wallaby-usb\",\"version\":1}";
		reply(&request, 200, hello, sizeof(hello) - 1, "application/json"); return;
	}
	gateway_api_execute(request.method == USB_RPC_GET ? "GET" : "POST", request.path,
			    request.body, request.length, reply, &request);
}

void gateway_usb_run(void)
{
	if (!device_is_ready(console)) { return; }
	/* The vendor's RF initialization owns the console until its empty-EFUSE
	 * prompt completes. The host acknowledges that exact RAM prompt once.
	 * Association and DHCP are unnecessary for this transport. */
	while (wifi_is_running(0) <= 0) { k_sleep(K_MSEC(20)); }
	log_flush();
	const struct log_backend *backend = log_backend_get_by_name("log_backend_uart");
	if (backend) { log_backend_disable(backend); }
	if (uart_irq_callback_user_data_set(console, receive, NULL) != 0) { return; }
	uart_irq_rx_enable(console);
	irq_enable(DT_IRQN(DT_CHOSEN(zephyr_console)));
	static const char ready[] = "\r\nMWUSB|READY version=1 transport=usb\r\n";
	for (size_t i = 0; i < sizeof(ready) - 1; i++) { uart_poll_out(console, ready[i]); }
	size_t used = 0;
	bool discard = false;
	for (;;) {
		(void)k_sem_take(&rx_ready, K_MSEC(20));
		if (atomic_set(&rx_overflow, 0)) { used = 0; discard = true; }
		uint8_t byte;
		while (ring_buf_get(&rx_ring, &byte, 1) == 1) {
			if (byte == 0) {
				if (used && !discard) {
					size_t count = usb_rpc_decode(encoded, used, decoded, sizeof(decoded));
					if (count) { execute(count); }
				}
				used = 0; discard = false;
			} else if (!discard) {
				if (used == sizeof(encoded)) { discard = true; used = 0; }
				else { encoded[used++] = byte; }
			}
		}
	}
}
