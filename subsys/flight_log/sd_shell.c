/* SPDX-License-Identifier: Apache-2.0 */

#include "flight_log.h"
#include "flight_log_fs.h"

#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

static int cmd_sd_mount(const struct shell *sh, size_t argc, char **argv)
{
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rc = rdd2_flight_log_fs_mount();
	if (rc != 0) {
		shell_error(sh, "mount failed: %d (card absent or unformatted)", rc);
		return rc;
	}
	shell_print(sh, "mounted %s", RDD2_FLIGHT_LOG_MOUNT_POINT);
	return 0;
}

static int cmd_sd_unmount(const struct shell *sh, size_t argc, char **argv)
{
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	/* The writer owns the volume while a session is open. Refuse rather than
	 * pull the mount out from under it. Stop logging first. */
	if (rdd2_flight_log_session_active()) {
		shell_error(sh, "logging session active, run flightlog stop first");
		return -EBUSY;
	}

	rc = rdd2_flight_log_fs_unmount();
	if (rc != 0) {
		shell_error(sh, "unmount failed: %d", rc);
		return rc;
	}
	shell_print(sh, "unmounted");
	return 0;
}

static int cmd_sd_ls(const struct shell *sh, size_t argc, char **argv)
{
	struct fs_dir_t dir;
	struct fs_dirent entry;
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!rdd2_flight_log_fs_mounted()) {
		shell_error(sh, "not mounted");
		return -ENODEV;
	}

	/* Hold the card lock across the directory walk so it never runs inside
	 * non-reentrant FatFs concurrently with the writer batch. */
	rdd2_flight_log_fs_lock();
	fs_dir_t_init(&dir);
	rc = fs_opendir(&dir, RDD2_FLIGHT_LOG_MOUNT_POINT);
	if (rc != 0) {
		rdd2_flight_log_fs_unlock();
		shell_error(sh, "opendir failed: %d", rc);
		return rc;
	}

	while (true) {
		rc = fs_readdir(&dir, &entry);
		if (rc != 0 || entry.name[0] == '\0') {
			break;
		}
		if (entry.type == FS_DIR_ENTRY_DIR) {
			shell_print(sh, "  <dir>  %s", entry.name);
		} else {
			shell_print(sh, "  %8zu  %s", entry.size, entry.name);
		}
	}

	(void)fs_closedir(&dir);
	rdd2_flight_log_fs_unlock();
	return rc;
}

static int cmd_sd_info(const struct shell *sh, size_t argc, char **argv)
{
	struct fs_statvfs stat;
	struct rdd2_flight_log_scan scan;
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!rdd2_flight_log_fs_mounted()) {
		shell_error(sh, "not mounted");
		return -ENODEV;
	}

	rdd2_flight_log_fs_lock();
	rc = fs_statvfs(RDD2_FLIGHT_LOG_MOUNT_POINT, &stat);
	if (rc != 0) {
		rdd2_flight_log_fs_unlock();
		shell_error(sh, "statvfs failed: %d", rc);
		return rc;
	}

	shell_print(sh, "block=%lu total=%lu free=%lu bytes_free=%llu", stat.f_frsize,
		    stat.f_blocks, stat.f_bfree,
		    (unsigned long long)stat.f_bfree * (unsigned long long)stat.f_frsize);
	shell_print(sh, "cluster=%lu bytes geometry=%s", stat.f_frsize,
		    rdd2_flight_log_fs_geometry_ok() ? "ok" : "mismatch");

	rc = rdd2_flight_log_fs_scan(&scan);
	rdd2_flight_log_fs_unlock();
	if (rc == 0) {
		shell_print(sh, "next session index=%u reservations=%u/%u",
			    (unsigned int)scan.next_index, (unsigned int)scan.ready_count,
			    (unsigned int)RDD2_FLIGHT_LOG_RESERVE_SLOTS);
	}
	return 0;
}

static int cmd_sd_format(const struct shell *sh, size_t argc, char **argv)
{
	bool force = argc == 2U && strcmp(argv[1], "force") == 0;
	int rc;

	if (argc > 1U && !force) {
		shell_error(sh, "usage: sd format [force]");
		return -EINVAL;
	}

	rc = rdd2_flight_log_fs_format(force);
	if (rc == -ENODEV) {
		shell_error(sh, "not mounted");
	} else if (rc == -EBUSY) {
		shell_error(sh, "logging session active, run flightlog stop first");
	} else if (rc == -ENOTEMPTY) {
		shell_error(sh, "card has files, refusing to format (sd format force overrides)");
	} else if (rc != 0) {
		shell_error(sh, "format failed: %d", rc);
	} else {
		shell_print(sh, "formatted: FAT32, 32768 byte clusters, data area on a "
				"4 MiB boundary");
	}
	return rc;
}

static int cmd_sd_trim(const struct shell *sh, size_t argc, char **argv)
{
	uint64_t bytes = 0U;
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rc = rdd2_flight_log_fs_trim_free(&bytes);
	if (rc == -ENODEV) {
		shell_error(sh, "not mounted");
	} else if (rc == -EBUSY) {
		shell_error(sh, "logging session active, run flightlog stop first");
	} else if (rc == -ENOTSUP) {
		shell_error(sh, "not a FAT32 volume");
	} else if (rc != 0) {
		shell_error(sh, "trim failed: %d", rc);
	} else {
		shell_print(sh, "trimmed %llu MiB of free space",
			    (unsigned long long)(bytes >> 20));
	}
	return rc;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_sd,
	SHELL_CMD(mount, NULL, "Mount the microSD FAT volume.", cmd_sd_mount),
	SHELL_CMD(unmount, NULL, "Unmount the microSD FAT volume.", cmd_sd_unmount),
	SHELL_CMD(ls, NULL, "List files on the mounted volume.", cmd_sd_ls),
	SHELL_CMD(info, NULL, "Show free space and next session index.", cmd_sd_info),
	SHELL_CMD(format, NULL,
		  "Format an empty card to the logger geometry; `force` formats a card with files.",
		  cmd_sd_format),
	SHELL_CMD(trim, NULL, "Erase every free cluster so free space is known-erased space.",
		  cmd_sd_trim),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(sd, &sub_sd, "microSD card bench utilities.", NULL);
