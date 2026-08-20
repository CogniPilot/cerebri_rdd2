/* SPDX-License-Identifier: Apache-2.0 */

#include "flight_log.h"
#include "flight_log_fs.h"

#include <string.h>

#include <zephyr/init.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/fs_mgmt/fs_mgmt_callbacks.h>

/*
 * Confine mcumgr file access to the microSD mount point. Every file read and
 * write request routed through the FS management group passes through here, and
 * any path that is not under RDD2_FLIGHT_LOG_MOUNT_POINT is denied. This keeps
 * OTA reflash paths and the internal flash out of reach of a log retrieval
 * client, which only ever needs the session files on the card.
 */
static enum mgmt_cb_return fs_access_cb(uint32_t event, enum mgmt_cb_return prev_status,
					int32_t *rc, uint16_t *group, bool *abort_more,
					void *data, size_t data_size)
{
	const struct fs_mgmt_file_access *access = data;
	const size_t prefix_len = sizeof(RDD2_FLIGHT_LOG_MOUNT_POINT) - 1U;

	ARG_UNUSED(prev_status);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);

	if (event != MGMT_EVT_OP_FS_MGMT_FILE_ACCESS) {
		return MGMT_CB_OK;
	}

	if (access == NULL || data_size < sizeof(*access) || access->filename == NULL ||
	    strncmp(access->filename, RDD2_FLIGHT_LOG_MOUNT_POINT, prefix_len) != 0) {
		*rc = MGMT_ERR_EACCESSDENIED;
		return MGMT_CB_ERROR_RC;
	}

	/* The mcumgr read runs later in the fs_mgmt handler, outside the writer's
	 * card lock, so it cannot be serialized against the writer by that lock.
	 * FatFs is non-reentrant here, so instead of locking we refuse access
	 * while a session is open: the operator runs flightlog stop (or rotate to
	 * close the current file) before retrieving, and downloads then target the
	 * closed files while no writer is touching the card. This event fires only
	 * for the FS group, so the img group and OTA are unaffected. */
	if (rdd2_flight_log_session_active()) {
		*rc = MGMT_ERR_EBUSY;
		return MGMT_CB_ERROR_RC;
	}

	return MGMT_CB_OK;
}

static struct mgmt_callback fs_access_callback = {
	.callback = fs_access_cb,
	.event_id = MGMT_EVT_OP_FS_MGMT_FILE_ACCESS,
};

static int register_fs_access_hook(void)
{
	mgmt_callback_register(&fs_access_callback);
	return 0;
}

SYS_INIT(register_fs_access_hook, APPLICATION, 99);
