/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_FLIGHT_LOG_FS_H_
#define RDD2_FLIGHT_LOG_FS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Disk name published by the board devicetree sdmmc-disk child, and the FAT
 * mount point derived from it. The FatFs volume string is generated from the
 * disk name, so the mount point is "/<disk>:". */
#define RDD2_FLIGHT_LOG_DISK_NAME "SD"
#define RDD2_FLIGHT_LOG_MOUNT_POINT "/SD:"

/* Per-boot session file basename pattern: flightNNNN.mcap, NNNN zero-padded. */
#define RDD2_FLIGHT_LOG_FILE_PREFIX "flight"
#define RDD2_FLIGHT_LOG_FILE_SUFFIX ".mcap"

/* Reserved basename for the background pre-allocated spare file. Deliberately
 * not an .mcap and not a flightNNNN name: the session-index scan ignores it and
 * a decoder never mistakes it for a recording. The writer grows this file to the
 * rotation size while a session streams, then renames it into the next session
 * on rotation so the extent is built ahead of time rather than at rotation. */
#define RDD2_FLIGHT_LOG_SPARE_NAME "flightspare.pre"

/* Highest four-digit session index. The logger refuses to start a new session
 * once the next index would exceed this rather than wrapping onto flight0000. */
#define RDD2_FLIGHT_LOG_MAX_SESSION_INDEX 9999U

/*
 * Serialize in-process filesystem access to the card. FatFs is non-reentrant on
 * this target, so the writer batch and every shell command that touches the
 * volume must bracket their filesystem calls with these. The lock is recursive,
 * so a locked caller may nest the fs helpers below. The mcumgr retrieval path
 * cannot hold this lock and is gated by the fs_mgmt hook instead.
 */
void rdd2_flight_log_fs_lock(void);
void rdd2_flight_log_fs_unlock(void);

/*
 * Initialise the card and mount the existing FAT volume. Returns 0 on success.
 * A negative value means the card is absent, not ready, or carries no mountable
 * FAT volume: the caller treats that as a clean no-op and retries later. The
 * volume is never formatted, so an unformatted card is refused rather than
 * silently wiped.
 */
int rdd2_flight_log_fs_mount(void);

/* Unmount the volume if mounted. Safe to call when already unmounted. */
int rdd2_flight_log_fs_unmount(void);

/* True while the FAT volume is mounted. */
bool rdd2_flight_log_fs_mounted(void);

/*
 * Scan the mounted volume for existing flightNNNN.mcap files and return the
 * next free index (max existing + 1, or 0 when none exist). Returns 0 on
 * success or a negative errno.
 */
int rdd2_flight_log_fs_next_index(uint32_t *index_out);

/*
 * Compose the absolute session path for a given index into out (capacity cap).
 * Returns the number of characters written, or a negative value on truncation.
 */
int rdd2_flight_log_fs_session_path(uint32_t index, char *out, size_t cap);

#endif /* RDD2_FLIGHT_LOG_FS_H_ */
