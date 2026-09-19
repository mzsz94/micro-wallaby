/* SPDX-License-Identifier: Apache-2.0 */
#include "imu_calibration.h"
#include "imu_storage.h"

#include <errno.h>
#include <math.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>

#define CAL_KEY 0x1701
#define CAL_SIZE 24
#define CAL_MAGIC 0x4d574943U

static struct nvs_fs fs;
static bool mounted;

int imu_storage_init(void)
{
	const struct flash_area *area;
	struct flash_pages_info page;
	int rc = flash_area_open(PARTITION_ID(storage_partition), &area);

	if (rc != 0) {
		return rc;
	}
	fs.flash_device = area->fa_dev;
	fs.offset = area->fa_off;
	rc = flash_get_page_info_by_offs(fs.flash_device, fs.offset, &page);
	if (rc == 0 && (page.size == 0 || page.size > UINT16_MAX ||
	    area->fa_off % page.size || area->fa_size % page.size ||
	    area->fa_size / page.size < 2)) {
		rc = -EINVAL;
	}
	if (rc == 0) {
		fs.sector_size = page.size;
		fs.sector_count = area->fa_size / page.size;
		rc = nvs_mount(&fs);
	}
	flash_area_close(area);
	mounted = rc == 0;
	/* Never erase the partition as an automatic mount-error recovery. */
	return rc;
}

int imu_storage_load(uint8_t who, double bias[3])
{
	uint8_t record[CAL_SIZE];
	double next[3];
	ssize_t rc;

	if (!mounted) {
		return -ENODEV;
	}
	rc = nvs_read(&fs, CAL_KEY, record, sizeof(record));
	if (rc < 0) {
		return rc;
	}
	if (rc != sizeof(record) || sys_get_le32(record) != CAL_MAGIC ||
	    sys_get_le32(record + 4) != 1 || sys_get_le32(record + 8) != who) {
		return -EBADMSG;
	}
	for (int i = 0; i < 3; ++i) {
		uint32_t bits = sys_get_le32(record + 12 + i * 4);
		int64_t value = bits <= INT32_MAX ? (int64_t)bits : (int64_t)bits - 4294967296LL;
		next[i] = value / 1000000.0;
	}
	if (!imu_bias_valid(next)) {
		return -ERANGE;
	}
	for (int i = 0; i < 3; ++i) {
		bias[i] = next[i];
	}
	return 0;
}

int imu_storage_save(uint8_t who, const double bias[3])
{
	uint8_t record[CAL_SIZE];
	ssize_t rc;

	if (!mounted) {
		return -ENODEV;
	}
	if (!imu_bias_valid(bias)) {
		return -ERANGE;
	}
	sys_put_le32(CAL_MAGIC, record);
	sys_put_le32(1, record + 4);
	sys_put_le32(who, record + 8);
	for (int i = 0; i < 3; ++i) {
		sys_put_le32((uint32_t)(int32_t)lround(bias[i] * 1000000), record + 12 + i * 4);
	}
	rc = nvs_write(&fs, CAL_KEY, record, sizeof(record));
	/* nvs_write returns zero when the exact record is already stored. */
	return rc == 0 || rc == sizeof(record) ? 0 : (rc < 0 ? (int)rc : -EIO);
}

int imu_storage_clear(void)
{
	return mounted ? nvs_delete(&fs, CAL_KEY) : -ENODEV;
}
