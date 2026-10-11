/* SPDX-License-Identifier: Apache-2.0 */
#include "gateway_link.h"
#include "gateway_http.h"
#include "gateway_imu.h"
#include "gateway_usb.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(lcd_gateway, LOG_LEVEL_INF);
int main(void)
{
	LOG_INF("NU87 Wi-Fi/BLE LCD gateway: isolated test LAN only");
	gateway_imu_start();
	int rc = gateway_radio_init();
	if (rc) { LOG_ERR("Radio initialization failed: %d", rc); return rc; }
#if defined(CONFIG_GATEWAY_USB_HOST)
	gateway_usb_run();
#else
	gateway_http_run();
#endif
	return 0;
}
