/* SPDX-License-Identifier: Apache-2.0 */

#include "flight_log_fs.h"

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

	fs_dir_t_init(&dir);
	rc = fs_opendir(&dir, RDD2_FLIGHT_LOG_MOUNT_POINT);
	if (rc != 0) {
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
	return rc;
}

static int cmd_sd_info(const struct shell *sh, size_t argc, char **argv)
{
	struct fs_statvfs stat;
	uint32_t next_index;
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!rdd2_flight_log_fs_mounted()) {
		shell_error(sh, "not mounted");
		return -ENODEV;
	}

	rc = fs_statvfs(RDD2_FLIGHT_LOG_MOUNT_POINT, &stat);
	if (rc != 0) {
		shell_error(sh, "statvfs failed: %d", rc);
		return rc;
	}

	shell_print(sh, "block=%lu total=%lu free=%lu bytes_free=%llu", stat.f_frsize,
		    stat.f_blocks, stat.f_bfree,
		    (unsigned long long)stat.f_bfree * (unsigned long long)stat.f_frsize);

	rc = rdd2_flight_log_fs_next_index(&next_index);
	if (rc == 0) {
		shell_print(sh, "next session index=%u", (unsigned int)next_index);
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_sd,
	SHELL_CMD(mount, NULL, "Mount the microSD FAT volume.", cmd_sd_mount),
	SHELL_CMD(unmount, NULL, "Unmount the microSD FAT volume.", cmd_sd_unmount),
	SHELL_CMD(ls, NULL, "List files on the mounted volume.", cmd_sd_ls),
	SHELL_CMD(info, NULL, "Show free space and next session index.", cmd_sd_info),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(sd, &sub_sd, "microSD card bench utilities.", NULL);
