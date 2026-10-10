/* SPDX-License-Identifier: Apache-2.0 */
#include "gateway_http.h"
#include "gateway_link.h"
#include "legacy_dfu_client.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct ota_job { uint32_t host; uint16_t port; char id[65]; legacy_dfu_image_t image; };
struct notification { uint8_t data[8]; uint8_t size; };
K_MSGQ_DEFINE(jobs, sizeof(struct ota_job), 1, 4);
K_MSGQ_DEFINE(notifications, sizeof(struct notification), 16, 4);
K_MUTEX_DEFINE(status_lock);
static atomic_t busy, cancelled, overflow, activation_started;
static char phase[32] = "idle";
static uint32_t acknowledged, image_size;
static int error_code;
static legacy_dfu_client_t client;
static struct ota_job current;
static uint8_t cache[4096];
static uint32_t cache_offset, cache_length;
static bool completed_write, write_result;

static void set_phase(const char *value, int error)
{
	k_mutex_lock(&status_lock, K_FOREVER);
	snprintk(phase, sizeof(phase), "%s", value);
	error_code = error;
	acknowledged = client.acknowledged_bytes;
	image_size = current.image.image_size;
	k_mutex_unlock(&status_lock);
}
bool gateway_ota_busy(void) { return atomic_get(&busy) != 0; }
void gateway_ota_status(char *buffer, size_t capacity)
{
	k_mutex_lock(&status_lock, K_FOREVER);
	snprintk(buffer, capacity,
		"{\"busy\":%s,\"phase\":\"%s\",\"acknowledged\":%u,\"size\":%u,\"error\":%d}",
		gateway_ota_busy() ? "true" : "false", phase, acknowledged, image_size, error_code);
	k_mutex_unlock(&status_lock);
}
int gateway_ota_start(uint32_t ipv4, const uint8_t p[OTA_DESCRIPTOR_SIZE])
{
	if (memcmp(p, "LCD1", 4)) return -EINVAL;
	struct ota_job job = { .host = ipv4, .port = sys_get_le16(p + 14),
		.image = { .image_address = LEGACY_DFU_APP_START,
			.image_size = sys_get_le32(p + 4), .app_version = sys_get_le32(p + 8),
			.image_crc16 = sys_get_le16(p + 12), .sd_fwid = LEGACY_DFU_SD_FWID } };
	if (job.port < 1024 || job.image.image_size < 8 || job.image.image_size > LEGACY_DFU_APP_MAX_SIZE ||
	    (job.image.image_size & 3)) return -EINVAL;
	for (size_t i = 0; i < 64; i++) {
		if (!((p[16 + i] >= '0' && p[16 + i] <= '9') || (p[16 + i] >= 'a' && p[16 + i] <= 'f'))) return -EINVAL;
		job.id[i] = p[16 + i];
	}
	job.id[64] = 0;
	if (!atomic_cas(&busy, 0, 1)) return -EBUSY;
	atomic_clear(&cancelled);
	atomic_clear(&activation_started);
	k_mutex_lock(&status_lock, K_FOREVER);
	strcpy(phase, "queued"); acknowledged = 0; image_size = job.image.image_size; error_code = 0;
	k_mutex_unlock(&status_lock);
	if (k_msgq_put(&jobs, &job, K_NO_WAIT)) { atomic_clear(&busy); return -EBUSY; }
	return 0;
}
int gateway_ota_cancel(void)
{
	if (!gateway_ota_busy() || atomic_get(&activation_started)) return -EALREADY;
	atomic_set(&cancelled, 1);
	return 0;
}
static bool read_image(void *context, uint32_t offset, uint8_t *dst, size_t length)
{
	ARG_UNUSED(context);
	if (atomic_get(&cancelled) || offset > current.image.image_size || length > current.image.image_size - offset) return false;
	if (offset < cache_offset || offset + length > cache_offset + cache_length) {
		cache_offset = offset;
		cache_length = MIN(sizeof(cache), current.image.image_size - offset);
		if (http_fetch_image(current.host, current.port, current.id, offset,
				current.image.image_size, cache, cache_length)) { cache_length = 0; return false; }
	}
	memcpy(dst, cache + offset - cache_offset, length);
	return true;
}
static legacy_dfu_io_result_t write_ble(void *context, legacy_dfu_channel_t channel,
				       const uint8_t *data, size_t length)
{
	ARG_UNUSED(context);
	if (channel == LEGACY_DFU_CONTROL && length == 1 && data[0] == 5)
		atomic_set(&activation_started, 1);
	int rc = channel == LEGACY_DFU_CONTROL ? gateway_dfu_control(data, length) : gateway_dfu_packet(data, length);
	/* These functions wait for ATT response / local packet TX completion. */
	write_result = rc == 0;
	completed_write = true;
	return LEGACY_DFU_IO_ACCEPTED;
}
static void close_ble(void *context) { ARG_UNUSED(context); gateway_dfu_leave(); }
static void receive_notification(const uint8_t *data, size_t length, void *user)
{
	ARG_UNUSED(user);
	struct notification item;
	if (!length || length > sizeof(item.data)) { atomic_set(&overflow, 1); return; }
	item.size = length; memcpy(item.data, data, length);
	if (k_msgq_put(&notifications, &item, K_NO_WAIT)) atomic_set(&overflow, 1);
}
static void ota_worker(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		k_msgq_get(&jobs, &current, K_FOREVER);
		cache_length = 0; cache_offset = 0; completed_write = false;
		k_msgq_purge(&notifications); atomic_clear(&overflow);
		legacy_dfu_transport_t transport = { .read = read_image, .write = write_ble, .close = close_ble };
		if (!legacy_dfu_begin(&client, &transport, &current.image, NULL, k_uptime_get_32())) {
			set_phase("invalid_image", client.error); goto finish;
		}
		set_phase("checking_image", 0);
		/* No write may occur until the full-image CRC and vector checks pass. */
		while (client.state == LEGACY_DFU_PREFLIGHT) {
			legacy_dfu_poll(&client, k_uptime_get_32()); k_yield();
		}
		if (atomic_get(&cancelled)) { legacy_dfu_cancel(&client); set_phase("cancelled", 0); goto finish; }
		if (client.state != LEGACY_DFU_START) { set_phase("preflight_failed", client.error); goto finish; }
		set_phase("entering_bootloader", 0);
		gateway_dfu_set_notify_callback(receive_notification, NULL);
		int rc = gateway_dfu_enter();
		if (rc) { set_phase("bootloader_unavailable", rc); goto leave; }
		/* DFU entry can take longer than a normal response timeout. */
		legacy_dfu_transport_ready(&client, k_uptime_get_32());
		while (!legacy_dfu_terminal(&client)) {
			uint32_t now = k_uptime_get_32();
			if (atomic_get(&cancelled)) legacy_dfu_cancel(&client);
			if (atomic_get(&overflow)) { legacy_dfu_write_complete(&client, false, now); legacy_dfu_disconnected(&client, now); }
			struct notification item;
			while (k_msgq_get(&notifications, &item, K_NO_WAIT) == 0)
				legacy_dfu_notification(&client, item.data, item.size, now);
			struct gateway_state state;
			gateway_get_state(&state);
			if (!state.dfu_ready) legacy_dfu_disconnected(&client, now);
			legacy_dfu_poll(&client, now);
			if (completed_write) {
				completed_write = false;
				legacy_dfu_write_complete(&client, write_result, k_uptime_get_32());
			}
			set_phase(client.state >= LEGACY_DFU_VALIDATE ? "validating_rebooting" : "transferring", client.error);
			k_sleep(K_MSEC(1));
		}
		if (client.state == LEGACY_DFU_REBOOT_OBSERVED) {
			gateway_dfu_leave(); set_phase("waiting_for_app", 0);
			uint32_t start = k_uptime_get_32(); bool responded = false;
			while ((uint32_t)(k_uptime_get_32() - start) < 45000) {
				struct gateway_state state; gateway_get_state(&state);
				if (state.ble_connected && state.lcd_ready) {
					uint8_t command[4] = {LCD_PROTOCOL_VERSION, LCD_OP_STATUS, 0xFF, 0xFE}, status[8];
					if (!gateway_lcd_command(command, sizeof(command), status, 3000) && status[4] == LCD_RESULT_OK) {
						responded = true; break;
					}
				}
				k_sleep(K_MSEC(500));
			}
			set_phase(responded ? "app_responding" : "app_not_confirmed", responded ? 0 : -ETIMEDOUT);
		} else {
			set_phase(client.state == LEGACY_DFU_CANCELLED ? "cancelled" : "transfer_failed", client.error);
		}
leave:
		gateway_dfu_set_notify_callback(NULL, NULL); gateway_dfu_leave();
finish:
		atomic_clear(&busy);
	}
}
K_THREAD_DEFINE(ota_thread, 8192, ota_worker, NULL, NULL, NULL, 6, 0, 0);
