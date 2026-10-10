/* SPDX-License-Identifier: Apache-2.0 */
#include "gateway_http.h"
#include <errno.h>
#include <zephyr/kernel.h>

/* This NU40 app has no BLE bootloader entry and uses a different memory map.
 * Keep the S140 legacy updater out of its gateway image entirely.
 */
int gateway_ota_start(uint32_t ipv4, const uint8_t descriptor[OTA_DESCRIPTOR_SIZE])
{
	ARG_UNUSED(ipv4);
	ARG_UNUSED(descriptor);
	return -ENOTSUP;
}
int gateway_ota_cancel(void) { return -ENOTSUP; }
bool gateway_ota_busy(void) { return false; }
void gateway_ota_status(char *buffer, size_t capacity)
{
	snprintk(buffer, capacity,
		 "{\"busy\":false,\"phase\":\"unsupported\",\"acknowledged\":0,\"size\":0,\"error\":0}");
}
