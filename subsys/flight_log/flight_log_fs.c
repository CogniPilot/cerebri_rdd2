/* SPDX-License-Identifier: Apache-2.0 */

#include "flight_log.h"
#include "flight_log_fs.h"

#include <errno.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/byteorder.h>

#include <ff.h>

LOG_MODULE_REGISTER(rdd2_flight_log_fs, CONFIG_RDD2_FLIGHT_LOG_LOG_LEVEL);

/* Geometry the logger formats a blank card to: FAT32 with 32 KiB clusters (the
 * largest cluster every host FAT32 tool accepts, and FatFs caps FAT32 at
 * 64 KiB), two FATs as host tools default to, and a data area aligned to 4 MiB
 * (8192 sectors) so every cluster write stays inside one card allocation unit.
 * The layout is a superfloppy: the volume spans the whole device with no MBR. */
#define FORMAT_CLUSTER_BYTES 32768U
#define FORMAT_ALIGN_SECTORS 8192U

/* The spare grows one step per flush cycle and each step must land on a cluster
 * boundary for a clean chain extension, so the step has to stay a whole number
 * of clusters of the geometry the logger formats to. */
BUILD_ASSERT(CONFIG_RDD2_FLIGHT_LOG_PREALLOC_STEP_BYTES % FORMAT_CLUSTER_BYTES == 0U);

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

/* Bumped on every successful mount. A caller that caches something about the
 * card's layout, such as the reservation layer's no-contiguous-run answer,
 * compares generations to notice that the volume it learned that from is gone:
 * a reinsert, an sd unmount/mount, or a bench format all land here. */
static uint32_t g_mount_generation;

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

/* Latched so a card whose geometry differs from the target but which carries
 * files is reported once rather than on every mount retry. Cleared when a mount
 * ends with the target geometry or the card is removed (init fails). */
static bool g_geometry_warned;

/*
 * Shared scratch buffer for the two passes that walk raw sectors: the format,
 * where f_mkfs clears the FAT one buffer load per write (Zephyr's fs_mkfs
 * wrapper passes 512 bytes and so issues one single-sector write per FAT
 * sector, while 4 KiB clears eight sectors a write), and the trim passes, which
 * read the FAT eight sectors at a time. Both run under the card lock and
 * never concurrently, so one buffer serves both. It is static because it is far
 * too large for a shell stack, and aligned to 32 bytes so the sdmmc read path
 * gives it to DMA directly instead of bouncing it through an internal copy.
 */
static uint8_t g_work[4096] __aligned(32);

/* FAT sectors read per disk_access_read call, which is the whole scratch buffer,
 * and the FAT32 entries a 512 byte sector holds. */
#define TRIM_FAT_SECTORS (sizeof(g_work) / 512U)
#define FAT32_ENTRIES_PER_SECTOR (512U / 4U)

/* Longest erase issued in one command, 256 MiB. The card accepts any range (the
 * sdmmc layer reports a one sector erase block), but a single command has to
 * finish inside the 5 s host busy timeout, so a long free run is split. This is
 * the same bound the FatFs trim glue uses. */
#define TRIM_CHUNK_SECTORS 524288U

void rdd2_flight_log_fs_lock(void)
{
	k_mutex_lock(&g_card_lock, K_FOREVER);
}

void rdd2_flight_log_fs_unlock(void)
{
	k_mutex_unlock(&g_card_lock);
}

/*
 * Everything after the card is initialised: mount the volume, settle the block
 * device reference count, and latch the mounted state. Runs with the card lock
 * held and with exactly one block-device reference outstanding, so both the
 * first mount and the remount after a format share it.
 */
static int mount_tail(void)
{
	int rc;

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
		return rc;
	}

	/* The mount holds its own block-device reference now (one CTRL_INIT via
	 * the FatFs disk_initialize path). Drop the disk_access_init probe
	 * reference with a non-forced CTRL_DEINIT so the FatFs reference is the
	 * only one held. fs_unmount then brings the refcount to zero and a
	 * reinserted card is re-probed by the next disk_access_init. */
	(void)disk_access_ioctl(RDD2_FLIGHT_LOG_DISK_NAME, DISK_IOCTL_CTRL_DEINIT, NULL);

	g_mounted = true;
	g_mount_generation++;
	g_mount_refused = false;
	LOG_INF("mounted %s on %s", RDD2_FLIGHT_LOG_DISK_NAME, RDD2_FLIGHT_LOG_MOUNT_POINT);
	return 0;
}

/* True when the mounted volume already carries the target geometry. Reads the
 * live FATFS the mount populated, so it is only meaningful while mounted. */
static bool geometry_ok(void)
{
	return (uint32_t)g_fat_fs.csize * 512U == FORMAT_CLUSTER_BYTES &&
	       (g_fat_fs.database % FORMAT_ALIGN_SECTORS) == 0U;
}

/*
 * 0 when the mounted root directory holds no content, -ENOTEMPTY when it does,
 * or the negative errno of the directory read. Any file or directory counts as
 * content, with one exception: Windows creates "System Volume Information" on
 * every insert and it holds only indexing metadata, so a card blanked on Windows
 * still reads as empty and auto-formats.
 */
static int root_empty_locked(void)
{
	struct fs_dir_t dir;
	struct fs_dirent entry;
	int rc;

	fs_dir_t_init(&dir);
	rc = fs_opendir(&dir, RDD2_FLIGHT_LOG_MOUNT_POINT);
	if (rc != 0) {
		return rc;
	}

	while (true) {
		rc = fs_readdir(&dir, &entry);
		if (rc != 0 || entry.name[0] == '\0') {
			break;
		}
		if (strcmp(entry.name, "System Volume Information") != 0) {
			rc = -ENOTEMPTY;
			break;
		}
	}

	(void)fs_closedir(&dir);
	return rc;
}

/*
 * Erase one contiguous run of free clusters, adding the erased size to *bytes.
 * Cluster n starts at sector database + (n - 2) * csize. Runs with the card lock
 * held and only while the volume is mounted, so the live FATFS geometry is valid.
 */
static int trim_cluster_run(uint32_t first_cluster, uint32_t clusters, uint64_t *bytes)
{
	uint64_t sector = (uint64_t)g_fat_fs.database +
			  (uint64_t)(first_cluster - 2U) * g_fat_fs.csize;
	uint64_t remaining = (uint64_t)clusters * g_fat_fs.csize;

	while (remaining > 0U) {
		uint32_t chunk = (uint32_t)MIN(remaining, (uint64_t)TRIM_CHUNK_SECTORS);
		uint64_t before = *bytes;
		int rc = disk_access_erase(RDD2_FLIGHT_LOG_DISK_NAME, (uint32_t)sector, chunk,
					   DISK_ACCESS_ERASE_PHYSICAL);

		if (rc != 0) {
			return rc;
		}
		sector += chunk;
		remaining -= chunk;
		*bytes += (uint64_t)chunk * 512U;
		/* Progress for the whole-card pass, which erases tens of GiB and
		 * runs for minutes. A chunk is 256 MiB, so this crosses at most one
		 * boundary per erase command; a per-file trim covers at most the
		 * rotation size and so logs a handful of lines. */
		if ((*bytes >> 30) != (before >> 30)) {
			LOG_INF("trimmed %llu GiB so far", (unsigned long long)(*bytes >> 30));
		}
	}

	return 0;
}

/*
 * Sliding window over the first FAT: which FAT-relative sector g_work holds and
 * how many are valid, count 0 meaning empty. The chain walk below keeps the
 * window across clusters and only re-reads when the needed sector leaves it, so
 * a contiguous extent costs one read per 1024 clusters: about 128 reads for a
 * 4095 MiB extent at 32 KiB clusters, against 131040 reads without the window.
 */
struct fat_window {
	uint32_t first;
	uint32_t count;
};

/*
 * Read the FAT32 entry of one cluster through the window, refilling it when the
 * wanted sector falls outside. Returns 0 with the 28-bit entry value in *entry,
 * or the negative errno of the read. Valid only while a FAT32 volume is mounted.
 */
static int fat_entry_read(struct fat_window *win, uint32_t cluster, uint32_t *entry)
{
	uint32_t sector = cluster / FAT32_ENTRIES_PER_SECTOR;

	/* The BPB comes off the card, so a FAT too short for the cluster count it
	 * claims is possible. Refuse rather than read past the first FAT. */
	if (sector >= g_fat_fs.fsize) {
		return -EINVAL;
	}

	if (win->count == 0U || sector < win->first || sector - win->first >= win->count) {
		uint32_t sectors = MIN((uint32_t)TRIM_FAT_SECTORS,
				       (uint32_t)(g_fat_fs.fsize - sector));
		int rc = disk_access_read(RDD2_FLIGHT_LOG_DISK_NAME, g_work,
					  (uint32_t)(g_fat_fs.fatbase + sector), sectors);

		if (rc != 0) {
			win->count = 0U;
			return rc;
		}
		win->first = sector;
		win->count = sectors;
	}

	*entry = sys_get_le32(&g_work[(sector - win->first) * 512U +
				      (cluster % FAT32_ENTRIES_PER_SECTOR) * 4U]) &
		 0x0FFFFFFFU;
	return 0;
}

int rdd2_flight_log_fs_trim_file_range(struct fs_file_t *file, uint64_t from_byte,
				       uint64_t to_byte)
{
	struct fat_window win = { .first = 0U, .count = 0U };
	uint64_t cluster_bytes;
	uint64_t first_index;
	uint64_t last_index;
	uint64_t bytes = 0U;
	uint32_t run_first = 0U;
	uint32_t run_clusters = 0U;
	uint32_t cluster;
	int rc;

	if (file == NULL || file->filep == NULL || to_byte <= from_byte) {
		return -EINVAL;
	}
	if (!g_mounted) {
		return -ENODEV;
	}
	/* The entry walk decodes FAT32 only. Any other volume is left alone
	 * rather than guessed at. */
	if (g_fat_fs.fs_type != FS_FAT32) {
		return -ENOTSUP;
	}

	/* The chain is walked off the card, so the entries the f_expand or the
	 * growth lseek just created have to be on it: FatFs keeps the last FAT
	 * sector it touched in its own dirty window until something syncs. */
	rc = fs_sync(file);
	if (rc != 0) {
		return rc;
	}

	cluster_bytes = (uint64_t)g_fat_fs.csize * 512U;
	first_index = from_byte / cluster_bytes;
	last_index = (to_byte - 1U) / cluster_bytes;

	cluster = ((FIL *)file->filep)->obj.sclust;
	for (uint64_t index = 0U; index <= last_index; ++index) {
		uint32_t next;

		/* End of chain (>= 0x0FFFFFF8), a free entry, or a value past the
		 * last cluster: the chain stops short of the range. */
		if (cluster < 2U || cluster >= g_fat_fs.n_fatent) {
			break;
		}

		if (index >= first_index) {
			/* Coalesce consecutive clusters so a contiguous extent is
			 * erased by one command per 256 MiB rather than per cluster. */
			if (run_clusters != 0U && cluster != run_first + run_clusters) {
				rc = trim_cluster_run(run_first, run_clusters, &bytes);
				if (rc != 0) {
					return rc;
				}
				run_clusters = 0U;
			}
			if (run_clusters == 0U) {
				run_first = cluster;
			}
			++run_clusters;
		}

		rc = fat_entry_read(&win, cluster, &next);
		if (rc != 0) {
			return rc;
		}
		cluster = next;
	}

	return run_clusters != 0U ? trim_cluster_run(run_first, run_clusters, &bytes) : 0;
}

int rdd2_flight_log_fs_trim_file(struct fs_file_t *file, const char *name)
{
	int64_t start_ms = k_uptime_get();
	uint64_t size;
	int rc;

	if (file == NULL || file->filep == NULL || name == NULL) {
		return -EINVAL;
	}

	size = (uint64_t)((FIL *)file->filep)->obj.objsize;
	if (size == 0U) {
		return 0;
	}

	rc = rdd2_flight_log_fs_trim_file_range(file, 0U, size);
	if (rc != 0) {
		LOG_WRN("trim of %s stopped, error %d", name, rc);
		return rc;
	}

	LOG_INF("trimmed %llu MiB of %s in %lld ms", (unsigned long long)(size >> 20), name,
		(long long)(k_uptime_get() - start_ms));
	return 0;
}

int rdd2_flight_log_fs_trim_free(uint64_t *bytes_out)
{
	struct fat_window win = { .first = 0U, .count = 0U };
	uint64_t bytes = 0U;
	uint32_t run_first = 0U;
	uint32_t run_clusters = 0U;
	int64_t start_ms;
	int rc = 0;

	rdd2_flight_log_fs_lock();

	if (!g_mounted) {
		rdd2_flight_log_fs_unlock();
		return -ENODEV;
	}
	if (rdd2_flight_log_session_active()) {
		rdd2_flight_log_fs_unlock();
		return -EBUSY;
	}
	if (g_fat_fs.fs_type != FS_FAT32) {
		rdd2_flight_log_fs_unlock();
		return -ENOTSUP;
	}

	start_ms = k_uptime_get();

	/* FatFs keeps the free cluster count from FSInfo and maintains it, so the
	 * size of the job is known here without the second whole-FAT pass an
	 * f_getfree would cost. 0xFFFFFFFF means the count is not known. */
	if (g_fat_fs.free_clst != 0xFFFFFFFFU) {
		LOG_INF("trimming %llu MiB of free space on %s",
			(unsigned long long)(((uint64_t)g_fat_fs.free_clst * g_fat_fs.csize *
					      512U) >>
					     20),
			RDD2_FLIGHT_LOG_DISK_NAME);
	} else {
		LOG_INF("trimming the free space of %s", RDD2_FLIGHT_LOG_DISK_NAME);
	}

	/*
	 * The pass reads the whole first FAT and erases every free run it finds,
	 * whether or not that run is already erased, so it costs a few MB of
	 * reads and minutes of erase commands on a large mostly-empty card. That
	 * is why it runs from the format and by hand rather than at every mount.
	 */
	for (uint32_t cluster = 2U; cluster < g_fat_fs.n_fatent; ++cluster) {
		uint32_t entry;

		rc = fat_entry_read(&win, cluster, &entry);
		if (rc != 0) {
			break;
		}
		if (entry == 0U) {
			if (run_clusters == 0U) {
				run_first = cluster;
			}
			++run_clusters;
			continue;
		}
		/* An allocated cluster ends the run in progress. */
		if (run_clusters != 0U) {
			rc = trim_cluster_run(run_first, run_clusters, &bytes);
			run_clusters = 0U;
			if (rc != 0) {
				break;
			}
		}
	}

	/* A volume whose free space reaches the last cluster ends mid-run. */
	if (rc == 0 && run_clusters != 0U) {
		rc = trim_cluster_run(run_first, run_clusters, &bytes);
	}

	if (rc != 0) {
		LOG_WRN("free space trim of %s stopped after %llu MiB, error %d",
			RDD2_FLIGHT_LOG_DISK_NAME, (unsigned long long)(bytes >> 20), rc);
		rdd2_flight_log_fs_unlock();
		return rc;
	}

	LOG_INF("trimmed %llu MiB of free space in %lld ms", (unsigned long long)(bytes >> 20),
		(long long)(k_uptime_get() - start_ms));
	if (bytes_out != NULL) {
		*bytes_out = bytes;
	}

	rdd2_flight_log_fs_unlock();
	return 0;
}

int rdd2_flight_log_fs_largest_free_run(uint64_t *bytes_out)
{
	struct fat_window win = { .first = 0U, .count = 0U };
	uint32_t run_clusters = 0U;
	uint32_t best_clusters = 0U;
	int rc = 0;

	if (bytes_out == NULL) {
		return -EINVAL;
	}

	rdd2_flight_log_fs_lock();

	if (!g_mounted) {
		rdd2_flight_log_fs_unlock();
		return -ENODEV;
	}
	if (g_fat_fs.fs_type != FS_FAT32) {
		rdd2_flight_log_fs_unlock();
		return -ENOTSUP;
	}

	/* One pass over the first FAT, through the same 8-sector window the trim
	 * passes read with, tracking only the longest run of consecutive free
	 * clusters. This is the scan f_expand would do anyway, done once with an
	 * answer the caller can use: without it f_expand reads the whole FAT looking
	 * for a run of a size the card cannot offer and then fails, and the session
	 * falls back to grow-on-write, which fragments the free space further. */
	for (uint32_t cluster = 2U; cluster < g_fat_fs.n_fatent; ++cluster) {
		uint32_t entry;

		rc = fat_entry_read(&win, cluster, &entry);
		if (rc != 0) {
			break;
		}
		if (entry == 0U) {
			++run_clusters;
			best_clusters = MAX(best_clusters, run_clusters);
		} else {
			run_clusters = 0U;
		}
	}

	if (rc != 0) {
		rdd2_flight_log_fs_unlock();
		return rc;
	}

	*bytes_out = (uint64_t)best_clusters * g_fat_fs.csize * 512U;
	rdd2_flight_log_fs_unlock();
	return 0;
}

int rdd2_flight_log_fs_format(bool force)
{
	static const MKFS_PARM parm = {
		.fmt = FM_FAT32 | FM_SFD,
		.n_fat = 2,
		.align = FORMAT_ALIGN_SECTORS,
		.n_root = 0,
		.au_size = FORMAT_CLUSTER_BYTES,
	};
	FRESULT fres;
	int rc;

	rdd2_flight_log_fs_lock();

	if (!g_mounted) {
		rdd2_flight_log_fs_unlock();
		return -ENODEV;
	}
	if (rdd2_flight_log_session_active()) {
		rdd2_flight_log_fs_unlock();
		return -EBUSY;
	}

	/* force skips the empty-root check only: the not-mounted and session-active
	 * refusals above still stand, because neither is about what is on the card. */
	if (!force) {
		rc = root_empty_locked();
		if (rc != 0) {
			rdd2_flight_log_fs_unlock();
			return rc;
		}
	}

	/* fs_unmount drops the only block-device reference and powers the card
	 * off. f_mkfs brings it back up through the FatFs disk_initialize path,
	 * which leaves exactly the one reference mount_tail expects. */
	rc = fs_unmount(&g_mount);
	if (rc != 0) {
		rdd2_flight_log_fs_unlock();
		return rc;
	}
	g_mounted = false;

	/* f_mkfs clears the FAT, which is tens of seconds on a large card with no
	 * output of its own. */
	LOG_INF("formatting blank %s ...", RDD2_FLIGHT_LOG_DISK_NAME);
	fres = f_mkfs(RDD2_FLIGHT_LOG_DISK_NAME ":", &parm, g_work, sizeof(g_work));
	if (fres != FR_OK) {
		/* No volume and no owner of the reference f_mkfs took: force the
		 * refcount to zero so the next mount re-probes the card. */
		bool force = true;

		(void)disk_access_ioctl(RDD2_FLIGHT_LOG_DISK_NAME, DISK_IOCTL_CTRL_DEINIT,
					&force);
		LOG_ERR("format of %s failed, FRESULT %d", RDD2_FLIGHT_LOG_DISK_NAME,
			(int)fres);
		rdd2_flight_log_fs_unlock();
		return -EIO;
	}

	LOG_INF("formatted blank %s: FAT32, %u byte clusters, data area on a %u sector boundary",
		RDD2_FLIGHT_LOG_DISK_NAME, FORMAT_CLUSTER_BYTES, FORMAT_ALIGN_SECTORS);
	rc = mount_tail();
	if (rc == 0) {
		/* f_mkfs writes a fresh FAT but erases nothing, so erase the free
		 * space now: a formatted card starts out as known-erased space. The
		 * lock is recursive and no session can be active on this path. */
		(void)rdd2_flight_log_fs_trim_free(NULL);
	}
	rdd2_flight_log_fs_unlock();
	return rc;
}

bool rdd2_flight_log_fs_geometry_ok(void)
{
	return g_mounted && geometry_ok();
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
		g_geometry_warned = false;
		rdd2_flight_log_fs_unlock();
		return rc;
	}

	rc = mount_tail();
	if (rc != 0) {
		rdd2_flight_log_fs_unlock();
		return rc;
	}

	if (!geometry_ok()) {
		/* A blank card gets the target geometry now, while nothing is
		 * recorded. A session can never be open on this path, so the only
		 * refusal that matters here is a card that carries content. */
		rc = rdd2_flight_log_fs_format(false);
		if (rc == -ENOTEMPTY) {
			if (!g_geometry_warned) {
				LOG_WRN("%s geometry mismatch: %u byte clusters, data area %s; card has files, left as is",
					RDD2_FLIGHT_LOG_DISK_NAME,
					(unsigned int)g_fat_fs.csize * 512U,
					(g_fat_fs.database % FORMAT_ALIGN_SECTORS) == 0U
						? "aligned"
						: "unaligned");
				g_geometry_warned = true;
			}
		} else if (rc != 0) {
			rdd2_flight_log_fs_unlock();
			return rc;
		} else {
			g_geometry_warned = false;
		}
	} else {
		g_geometry_warned = false;
	}

	/* No free-space erase here. The space that matters is the space about to be
	 * written, and that is erased as it is reserved: the session extent right
	 * after its f_expand and each spare growth step right after it lands.
	 * Erasing all free space instead meant reading the whole FAT at every boot
	 * for blocks that would never be written. A card that arrived through a
	 * format was erased whole by the format, and `sd trim` remains for a manual
	 * full pass. */

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

uint32_t rdd2_flight_log_fs_mount_generation(void)
{
	return g_mount_generation;
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

/*
 * Parse "reserveNN.pre" and return the slot number. Returns false for any other
 * name, including a reservation file left by an older naming.
 */
static bool parse_reserve_slot(const char *name, uint32_t *slot_out)
{
	const size_t prefix_len = sizeof(RDD2_FLIGHT_LOG_RESERVE_PREFIX) - 1U;
	uint32_t value = 0U;
	size_t digits = 0U;

	if (strncmp(name, RDD2_FLIGHT_LOG_RESERVE_PREFIX, prefix_len) != 0) {
		return false;
	}
	name += prefix_len;
	while (name[digits] >= '0' && name[digits] <= '9') {
		value = value * 10U + (uint32_t)(name[digits] - '0');
		digits++;
	}
	if (digits == 0U || strcmp(&name[digits], RDD2_FLIGHT_LOG_RESERVE_SUFFIX) != 0) {
		return false;
	}

	*slot_out = value;
	return true;
}

/* Reservation basename prefix of the single-file naming this pool replaced. Such
 * a file is never renamed into a session, so its clusters are dead space: the
 * scan collects one per pass and unlinks it once the directory is closed. */
#define RESERVE_LEGACY_PREFIX "flightspare"

/* The slot bitmaps below carry one bit per pool slot. */
BUILD_ASSERT(RDD2_FLIGHT_LOG_RESERVE_SLOTS <= 32U,
	     "the reservation pool holds at most 32 slots");

static int scan_locked(struct rdd2_flight_log_scan *out, char *stale, size_t stale_cap)
{
	struct fs_dir_t dir;
	struct fs_dirent entry;
	uint32_t ready_mask = 0U;
	uint32_t present_mask = 0U;
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
		uint32_t value;

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
		if (parse_reserve_slot(entry.name, &value)) {
			if (value < RDD2_FLIGHT_LOG_RESERVE_SLOTS) {
				present_mask |= BIT(value);
				if ((uint64_t)entry.size ==
				    (uint64_t)CONFIG_RDD2_FLIGHT_LOG_ROTATE_BYTES) {
					ready_mask |= BIT(value);
				}
			}
			continue;
		}
		if (strncmp(entry.name, RESERVE_LEGACY_PREFIX,
			    sizeof(RESERVE_LEGACY_PREFIX) - 1U) == 0 &&
		    strlen(entry.name) < stale_cap) {
			strcpy(stale, entry.name);
			continue;
		}
		if (!parse_session_index(entry.name, &value)) {
			continue;
		}
		if (!any || value > highest) {
			highest = value;
			any = true;
		}
	}

	(void)fs_closedir(&dir);

	out->next_index = any ? highest + 1U : 0U;
	out->ready_count = 0U;
	out->ready_slot = -1;
	out->free_slot = -1;
	out->free_slot_used = false;
	for (uint32_t slot = 0U; slot < RDD2_FLIGHT_LOG_RESERVE_SLOTS; ++slot) {
		if ((ready_mask & BIT(slot)) != 0U) {
			out->ready_count++;
			if (out->ready_slot < 0) {
				out->ready_slot = (int)slot;
			}
		} else if (out->free_slot < 0) {
			out->free_slot = (int)slot;
			out->free_slot_used = (present_mask & BIT(slot)) != 0U;
		}
	}
	return 0;
}

int rdd2_flight_log_fs_scan(struct rdd2_flight_log_scan *out)
{
	/* Long enough for any name the legacy prefix can carry; a longer one is
	 * not one of ours and is left alone. */
	char stale[32];
	char path[sizeof(RDD2_FLIGHT_LOG_MOUNT_POINT) + sizeof(stale)];
	int rc;

	if (out == NULL) {
		return -EINVAL;
	}

	stale[0] = '\0';
	rdd2_flight_log_fs_lock();
	rc = scan_locked(out, stale, sizeof(stale));
	/* Unlink only now, with the directory closed: removing an entry while a
	 * readdir walks it would leave the walk on a stale position. */
	if (rc == 0 && stale[0] != '\0' &&
	    snprintk(path, sizeof(path), "%s/%s", RDD2_FLIGHT_LOG_MOUNT_POINT, stale) > 0) {
		LOG_INF("removing leftover reservation %s", stale);
		(void)fs_unlink(path);
	}
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
