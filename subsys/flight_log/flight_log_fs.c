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

/*
 * FatFs is built non-reentrant on this target: CONFIG_FS_FATFS_REENTRANT is
 * incompatible with the LFN BSS working-buffer mode, and that mode keeps one
 * shared static name buffer. Two threads inside FatFs at once corrupt it, so a
 * single card lock serializes every in-process filesystem touch: the writer
 * batch, and every sd/log shell command that reaches the volume. The mcumgr
 * retrieval path runs its read outside this process and cannot hold this lock,
 * so it is gated separately by the session-active denial in the fs_mgmt hook.
 */
static K_MUTEX_DEFINE(g_card_lock);

/* Latched so the unformatted or foreign card refusal is logged once per state
 * change rather than on every low-rate retry. Cleared when the card is removed
 * (init fails) or a volume mounts, so a fresh insert reports once more. */
static bool g_mount_refused;

void rdd2_flight_log_fs_lock(void)
{
	k_mutex_lock(&g_card_lock, K_FOREVER);
}

void rdd2_flight_log_fs_unlock(void)
{
	k_mutex_unlock(&g_card_lock);
}

int rdd2_flight_log_fs_mount(void)
{
	int rc;

	rdd2_flight_log_fs_lock();

	if (g_mounted) {
		rdd2_flight_log_fs_unlock();
		return 0;
	}

	/* Bring the card up. An absent or not-ready card returns non-zero and
	 * the logger treats that as a no-op it retries at low rate. This is the
	 * probe reference: disk_access_init takes one block-device reference. */
	rc = disk_access_init(RDD2_FLIGHT_LOG_DISK_NAME);
	if (rc != 0) {
		g_mount_refused = false;
		rdd2_flight_log_fs_unlock();
		return rc;
	}

	/* Mount the existing volume. Auto-format is never enabled, so a card
	 * with no FAT filesystem is refused here rather than being wiped. */
	rc = fs_mount(&g_mount);
	if (rc != 0) {
		/* FatFs called disk_initialize before it found no volume and does
		 * not release that reference on FR_NO_FILESYSTEM
		 * (modules/fs/fatfs/ff.c mount_volume), and the disk_access_init
		 * probe reference is still held too. Force the refcount to zero
		 * (zephyr/subsys/disk/disk_access.c DISK_IOCTL_CTRL_DEINIT force
		 * branch) so retries do not accumulate references and a reformatted
		 * or reinserted card is genuinely re-initialized. */
		bool force = true;

		(void)disk_access_ioctl(RDD2_FLIGHT_LOG_DISK_NAME, DISK_IOCTL_CTRL_DEINIT,
					&force);
		if (!g_mount_refused) {
			LOG_WRN("%s present but no mountable FAT volume, logging stays off",
				RDD2_FLIGHT_LOG_DISK_NAME);
			g_mount_refused = true;
		}
		rdd2_flight_log_fs_unlock();
		return rc;
	}

	/* The mount holds its own block-device reference now (one CTRL_INIT via
	 * the FatFs disk_initialize path). Drop the disk_access_init probe
	 * reference with a non-forced CTRL_DEINIT so the FatFs reference is the
	 * only one held. fs_unmount then brings the refcount to zero and a
	 * reinserted card is re-probed by the next disk_access_init. */
	(void)disk_access_ioctl(RDD2_FLIGHT_LOG_DISK_NAME, DISK_IOCTL_CTRL_DEINIT, NULL);

	g_mounted = true;
	g_mount_refused = false;
	LOG_INF("mounted %s on %s", RDD2_FLIGHT_LOG_DISK_NAME, RDD2_FLIGHT_LOG_MOUNT_POINT);
	rdd2_flight_log_fs_unlock();
	return 0;
}

int rdd2_flight_log_fs_unmount(void)
{
	int rc;

	rdd2_flight_log_fs_lock();

	if (!g_mounted) {
		rdd2_flight_log_fs_unlock();
		return 0;
	}

	/* fs_unmount powers the block device off through CTRL_DEINIT. Only the
	 * FatFs reference remains at this point, so this drops the refcount to
	 * zero and the card is genuinely deinitialized. */
	rc = fs_unmount(&g_mount);
	if (rc != 0) {
		rdd2_flight_log_fs_unlock();
		return rc;
	}

	g_mounted = false;
	rdd2_flight_log_fs_unlock();
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

static int next_index_locked(uint32_t *index_out)
{
	struct fs_dir_t dir;
	struct fs_dirent entry;
	uint32_t highest = 0U;
	bool any = false;
	int rc;

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
		/* Skip the background spare explicitly. parse_session_index already
		 * rejects it (its name carries no four-digit field), but naming it
		 * here keeps the scan correct even if the spare name ever changes. */
		if (strcmp(entry.name, RDD2_FLIGHT_LOG_SPARE_NAME) == 0) {
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

int rdd2_flight_log_fs_next_index(uint32_t *index_out)
{
	int rc;

	if (index_out == NULL) {
		return -EINVAL;
	}

	rdd2_flight_log_fs_lock();
	rc = next_index_locked(index_out);
	rdd2_flight_log_fs_unlock();
	return rc;
}

int rdd2_flight_log_fs_session_path(uint32_t index, char *out, size_t cap)
{
	int written;

	if (out == NULL) {
		return -EINVAL;
	}

	/* The index never wraps: the caller refuses to start once the scan would
	 * pass 9999, so %04u always renders a four-digit basename here. */
	written = snprintk(out, cap, "%s/%s%04u%s", RDD2_FLIGHT_LOG_MOUNT_POINT,
			   RDD2_FLIGHT_LOG_FILE_PREFIX, (unsigned int)index,
			   RDD2_FLIGHT_LOG_FILE_SUFFIX);
	if (written < 0 || (size_t)written >= cap) {
		return -ENAMETOOLONG;
	}
	return written;
}
