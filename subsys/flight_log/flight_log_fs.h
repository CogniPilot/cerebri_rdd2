/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_FLIGHT_LOG_FS_H_
#define RDD2_FLIGHT_LOG_FS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/fs/fs.h>
#include <zephyr/sys/util.h>

/* Disk name published by the board devicetree sdmmc-disk child, and the FAT
 * mount point derived from it. The FatFs volume string is generated from the
 * disk name, so the mount point is "/<disk>:". */
#define RDD2_FLIGHT_LOG_DISK_NAME "SD"
#define RDD2_FLIGHT_LOG_MOUNT_POINT "/SD:"

/* Per-boot session file basename pattern: flightNNNN.mcap, NNNN zero-padded. */
#define RDD2_FLIGHT_LOG_FILE_PREFIX "flight"
#define RDD2_FLIGHT_LOG_FILE_SUFFIX ".mcap"

/* Basename pattern of one standing reservation, the slot number substituted:
 * reserve00.pre and up. Deliberately not an .mcap and not a flightNNNN name: the
 * session-index scan ignores these and a decoder never mistakes one for a
 * recording. The writer grows a reservation to the rotation size in the
 * background, then renames it into the next session on rotation or at boot, so
 * the extent is built ahead of time rather than at open. */
#define RDD2_FLIGHT_LOG_RESERVE_PATTERN "reserve%02u.pre"
#define RDD2_FLIGHT_LOG_RESERVE_PREFIX "reserve"
#define RDD2_FLIGHT_LOG_RESERVE_SUFFIX ".pre"

/*
 * Slots in the standing reservation pool: the reserve divided by one session
 * extent, at least one. The writer keeps this many full-size reservations on the
 * card when free space allows, so every rotation and every boot renames one in
 * instead of reserving inline.
 */
#define RDD2_FLIGHT_LOG_RESERVE_SLOTS                                                              \
	((uint32_t)MAX(1LL, (long long)CONFIG_RDD2_FLIGHT_LOG_RESERVE_BYTES /                      \
				    (long long)CONFIG_RDD2_FLIGHT_LOG_ROTATE_BYTES))

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
 * Counter incremented on every successful mount, so a caller that cached
 * something it learned about the card can tell whether it still applies. Any
 * remount is a different card as far as such a cache is concerned, including
 * the internal remount a bench format does. Zero before the first mount.
 */
uint32_t rdd2_flight_log_fs_mount_generation(void);

/*
 * Format the card to the logger's target geometry: FAT32, 32 KiB clusters, a
 * 4 MiB-aligned data area, no partition table, and an erase of the free space
 * the fresh FAT exposes, so a formatted card is known-erased space. Without
 * force, only a mounted FAT volume whose root directory holds no content is ever
 * formatted, so a card carrying files and a card that does not mount are both
 * left untouched; force skips that check alone and wipes a card with files.
 * Returns 0 with the volume remounted, -ENODEV when no volume is mounted,
 * -EBUSY while a logging session is active, -ENOTEMPTY when the root holds
 * content and force is not set, -EIO when the format itself fails, or another
 * negative errno from the unmount or the directory scan. Takes the card lock,
 * which is recursive, so the mount path may call this with the lock already
 * held.
 */
int rdd2_flight_log_fs_format(bool force);

/* True while the mounted volume carries the target geometry. Meaningful only
 * while mounted: returns false otherwise. */
bool rdd2_flight_log_fs_geometry_ok(void);

/*
 * Walk the FAT of the mounted volume and erase every free cluster, so free space
 * is known-erased space and a new session does not make the card controller
 * erase and collect garbage under the writer. Host FAT drivers do not trim SD
 * cards, so a card whose flights were deleted on a host needs this. It is a
 * whole-FAT read plus an erase of everything free, minutes of work on a large
 * card, so it runs from the format and by hand with `sd trim` rather than at
 * mount: the space the logger is about to write is erased as it is reserved by
 * the two calls below.
 * Progress is logged every 1 GiB. Returns 0 with the erased byte count in
 * *bytes_out (which may be NULL), -ENODEV when no volume is mounted, -EBUSY
 * while a logging session is active, -ENOTSUP when the volume is not FAT32, or
 * the negative errno of the read or erase that stopped the pass. Takes the card
 * lock.
 */
int rdd2_flight_log_fs_trim_free(uint64_t *bytes_out);

/*
 * Erase the clusters of an open file's chain that cover [from_byte, to_byte), so
 * the reserved space the logger is about to stream into is known-erased space
 * and the card controller does not have to erase and collect garbage under the
 * writer. The file is synced first, so the FAT entries a just-finished f_expand
 * or growth lseek created are on the card before the chain is walked off it.
 * Erasing discards whatever those clusters hold, so this is only for a range
 * that carries no data yet. Returns 0, -EINVAL on a bad argument or an empty
 * range, -ENODEV when no volume is mounted, -ENOTSUP when the volume is not
 * FAT32, or the negative errno of the sync, read, or erase that stopped it. The
 * caller must hold the card lock.
 */
int rdd2_flight_log_fs_trim_file_range(struct fs_file_t *file, uint64_t from_byte,
				       uint64_t to_byte);

/*
 * Erase every cluster of an open file's current chain, name being the file name
 * for the log line. Same contract as the range call above, of which this is the
 * whole-file case: an empty file is a no-op. Returns 0 or a negative errno, and
 * logs the size trimmed and how long it took.
 */
int rdd2_flight_log_fs_trim_file(struct fs_file_t *file, const char *name);

/*
 * Walk the first FAT of the mounted volume and report the size in bytes of its
 * longest run of consecutive free clusters, 0 when there is none. This is what
 * a contiguous reservation can actually be, which on a fragmented card is far
 * less than the free total, so the reservation is sized to it rather than
 * letting f_expand scan the whole FAT for a size the card can never satisfy.
 * One FAT read per 8 sectors, around a second on a large card. Returns 0 with
 * the size in *bytes_out, -EINVAL on a NULL argument, -ENODEV when no volume is
 * mounted, -ENOTSUP when the volume is not FAT32, or the negative errno of the
 * read that stopped the pass. Takes the card lock, which is recursive, so a
 * locked caller may use it.
 */
int rdd2_flight_log_fs_largest_free_run(uint64_t *bytes_out);

/* What one pass over the mount point tells the session layer. */
struct rdd2_flight_log_scan {
	uint32_t next_index;    /* highest flightNNNN seen + 1, 0 when none exist */
	uint32_t ready_count;   /* slots holding a full-size standing reservation */
	int ready_slot;         /* lowest such slot, -1 when none is ready */
	int free_slot;          /* lowest slot to build in, -1 when the pool is full */
	bool free_slot_used;    /* that slot holds a wrong-sized file to remove first */
};

/*
 * One directory pass over the mount point, answering everything a session open
 * and the reservation builder need: the next free session index, the standing
 * reservations, and the first pool slot free to build in. The directory entries
 * carry both name and size, so no per-slot stat is needed and a boot open costs
 * one directory read, one rename, and one file open. Reservation files left by
 * an older naming are unlinked afterwards, once the directory is closed. Returns
 * 0 on success or a negative errno. Takes the card lock.
 */
int rdd2_flight_log_fs_scan(struct rdd2_flight_log_scan *out);

/*
 * Compose the absolute session path for a given index into out (capacity cap).
 * Returns the number of characters written, or a negative value on truncation.
 */
int rdd2_flight_log_fs_session_path(uint32_t index, char *out, size_t cap);

#endif /* RDD2_FLIGHT_LOG_FS_H_ */
