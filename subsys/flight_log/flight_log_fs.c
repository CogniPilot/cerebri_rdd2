/* SPDX-License-Identifier: Apache-2.0 */

#include "flight_log_fs.h"

#include <errno.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/disk_access.h>

#include <ff.h>

LOG_MODULE_REGISTER(rdd2_flight_log_fs, CONFIG_RDD2_FLIGHT_LOG_LOG_LEVEL);

static FATFS g_fat_fs;
static struct fs_mount_t g_mount = {
	.type = FS_FATFS,
	.fs_data = &g_fat_fs,
	.mnt_point = RDD2_FLIGHT_LOG_MOUNT_POINT,
	.storage_dev = (void *)RDD2_FLIGHT_LOG_DISK_NAME,
	/* USE_DISK_ACCESS routes FatFs through the block device. NO_FORMAT is
	 * mandatory: it forbids fs_mount from formatting a card that carries no
	 * FAT volume, so an unformatted or foreign card is refused instead of
	 * being wiped. */
	.flags = FS_MOUNT_FLAG_USE_DISK_ACCESS | FS_MOUNT_FLAG_NO_FORMAT,
};
static bool g_mounted;

int rdd2_flight_log_fs_mount(void)
{
	int rc;

	if (g_mounted) {
		return 0;
	}

	/* Bring the card up. An absent or not-ready card returns non-zero and
	 * the logger treats that as a no-op it retries at low rate. */
	rc = disk_access_init(RDD2_FLIGHT_LOG_DISK_NAME);
	if (rc != 0) {
		return rc;
	}

	/* Mount the existing volume. Auto-format is never enabled, so a card
	 * with no FAT filesystem is refused here rather than being wiped. */
	rc = fs_mount(&g_mount);
	if (rc != 0) {
		return rc;
	}

	g_mounted = true;
	LOG_INF("mounted %s on %s", RDD2_FLIGHT_LOG_DISK_NAME, RDD2_FLIGHT_LOG_MOUNT_POINT);
	return 0;
}

int rdd2_flight_log_fs_unmount(void)
{
	int rc;

	if (!g_mounted) {
		return 0;
	}

	rc = fs_unmount(&g_mount);
	if (rc != 0) {
		return rc;
	}

	g_mounted = false;
	return 0;
}

bool rdd2_flight_log_fs_mounted(void)
{
	return g_mounted;
}

/*
 * Parse "flightNNNN.mcap" and return the numeric index. Returns false for any
 * name that does not match the exact per-boot session pattern, so unrelated
 * files on a shared card are ignored by the rotation index scan.
 */
static bool parse_session_index(const char *name, uint32_t *index_out)
{
	const size_t prefix_len = sizeof(RDD2_FLIGHT_LOG_FILE_PREFIX) - 1U;
	const size_t suffix_len = sizeof(RDD2_FLIGHT_LOG_FILE_SUFFIX) - 1U;
	const size_t digits = 4U;
	uint32_t value = 0U;

	if (strncmp(name, RDD2_FLIGHT_LOG_FILE_PREFIX, prefix_len) != 0) {
		return false;
	}
	if (strlen(name) != prefix_len + digits + suffix_len) {
		return false;
	}
	for (size_t i = 0U; i < digits; ++i) {
		char c = name[prefix_len + i];

		if (c < '0' || c > '9') {
			return false;
		}
		value = value * 10U + (uint32_t)(c - '0');
	}
	if (strcmp(name + prefix_len + digits, RDD2_FLIGHT_LOG_FILE_SUFFIX) != 0) {
		return false;
	}

	*index_out = value;
	return true;
}

int rdd2_flight_log_fs_next_index(uint32_t *index_out)
{
	struct fs_dir_t dir;
	struct fs_dirent entry;
	uint32_t highest = 0U;
	bool any = false;
	int rc;

	if (index_out == NULL) {
		return -EINVAL;
	}
	if (!g_mounted) {
		return -ENODEV;
	}

	fs_dir_t_init(&dir);
	rc = fs_opendir(&dir, RDD2_FLIGHT_LOG_MOUNT_POINT);
	if (rc != 0) {
		return rc;
	}

	while (true) {
		uint32_t index;

		rc = fs_readdir(&dir, &entry);
		if (rc != 0) {
			(void)fs_closedir(&dir);
			return rc;
		}
		if (entry.name[0] == '\0') {
			break;
		}
		if (entry.type != FS_DIR_ENTRY_FILE) {
			continue;
		}
		if (!parse_session_index(entry.name, &index)) {
			continue;
		}
		if (!any || index > highest) {
			highest = index;
			any = true;
		}
	}

	(void)fs_closedir(&dir);
	*index_out = any ? highest + 1U : 0U;
	return 0;
}

int rdd2_flight_log_fs_session_path(uint32_t index, char *out, size_t cap)
{
	int written;

	if (out == NULL) {
		return -EINVAL;
	}

	written = snprintk(out, cap, "%s/%s%04u%s", RDD2_FLIGHT_LOG_MOUNT_POINT,
			   RDD2_FLIGHT_LOG_FILE_PREFIX, (unsigned int)(index % 10000U),
			   RDD2_FLIGHT_LOG_FILE_SUFFIX);
	if (written < 0 || (size_t)written >= cap) {
		return -ENAMETOOLONG;
	}
	return written;
}
