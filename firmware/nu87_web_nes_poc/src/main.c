/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <microwallaby_web/joystick.h>
#include <microwallaby_web/web_server.h>
#include <microwallaby_web/wifi_sta.h>

LOG_MODULE_REGISTER(mw_web_nes_main, CONFIG_MW_WEB_LOG_LEVEL);

int main(void)
{
	int ret;

	LOG_INF("microWallaby NU-87 Wi-Fi NES PoC starting");
	LOG_INF("No ROM is stored on or uploaded to this board");
	LOG_WRN("TEST ONLY: connect this firmware only to an isolated test network");

	ret = mw_joystick_start();
	if (ret < 0) {
		LOG_ERR("Joystick initialization failed: %d", ret);
	}

	ret = mw_web_server_start();
	if (ret < 0) {
		return ret;
	}

	ret = mw_wifi_sta_start();
	if (ret < 0) {
		LOG_ERR("Wi-Fi startup failed; Wi-Fi is disabled: %d", ret);
	}

	return 0;
}
