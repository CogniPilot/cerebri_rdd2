/* SPDX-License-Identifier: Apache-2.0 */

#include "mcap_stream.h"
#include "flight_log_fs.h"

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

/* Native FatFs handle and calls. For an FS_FATFS mount the VFS stores the
 * underlying FIL in fs_file_t.filep (zephyr/subsys/fs/fat_fs.c fatfs_open sets
 * zfp->filep to a FIL from the fatfs_filep_pool slab). Reaching it lets the
 * session layer call f_expand and f_truncate, which have no VFS equivalent. */
#include <ff.h>

static int write_all(struct mcap_stream *stream, const uint8_t *buf, size_t len)
{
	size_t offset = 0U;

	while (offset < len) {
		ssize_t written = fs_write(&stream->file, buf + offset, len - offset);

		if (written < 0) {
			stream->write_failed = true;
			return (int)written;
		}
		if (written == 0) {
			stream->write_failed = true;
			return -EIO;
		}
		offset += (size_t)written;
	}
	return 0;
}

/* Card sector size. CONFIG_RDD2_FLIGHT_LOG_BLOCK_BYTES is a multiple of it. */
#define SECTOR_BYTES 512U

/*
 * Write the first n bytes of the staging block and shift whatever is left down
 * to the front. n must not exceed block_fill. Callers pass the whole fill to
 * empty the block, or a sector multiple to keep the file offset 512-aligned.
 */
static int flush_block(struct mcap_stream *stream, size_t n)
{
	int rc;

	if (n == 0U) {
		return 0;
	}
	rc = write_all(stream, stream->block, n);
	if (rc != 0) {
		stream->block_fill = 0U;
		return rc;
	}
	stream->block_fill -= n;
	if (stream->block_fill != 0U) {
		memmove(stream->block, stream->block + n, stream->block_fill);
	}
	return 0;
}

int mcap_stream_open_file(struct mcap_stream *stream, const char *path)
{
	int rc;

	if (stream == NULL || path == NULL) {
		return -EINVAL;
	}

	/* FS_O_TRUNC is load-bearing: if a same-named file already exists (an
	 * index reused after files were pruned off-vehicle), truncation drops any
	 * stale tail so the session never carries bytes from a prior recording. */
	fs_file_t_init(&stream->file);
	rc = fs_open(&stream->file, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (rc != 0) {
		return rc;
	}

	stream->file_open = true;
	stream->write_failed = false;
	stream->last_sync_ok = true;
	stream->preallocated = false;
	stream->reserved_bytes = 0U;
	stream->bytes_written = 0U;
	stream->block_fill = 0U;
	return 0;
}

int mcap_stream_open_existing(struct mcap_stream *stream, const char *path,
			      uint64_t reserved_bytes)
{
	int rc;

	if (stream == NULL || path == NULL) {
		return -EINVAL;
	}

	/* No FS_O_TRUNC and no FS_O_APPEND: truncation would free the pre-built
	 * chain this session is meant to reuse, and append would force every write
	 * to end-of-file instead of overwriting the reserved clusters from the top.
	 * FatFs opens the file with the pointer at 0; seek to 0 anyway so the first
	 * write lands at the start of the reserved extent regardless. */
	fs_file_t_init(&stream->file);
	rc = fs_open(&stream->file, path, FS_O_WRITE);
	if (rc != 0) {
		return rc;
	}

	rc = fs_seek(&stream->file, 0, FS_SEEK_SET);
	if (rc != 0) {
		(void)fs_close(&stream->file);
		return rc;
	}

	stream->file_open = true;
	stream->write_failed = false;
	stream->last_sync_ok = true;
	stream->preallocated = true;
	stream->reserved_bytes = reserved_bytes;
	stream->bytes_written = 0U;
	stream->block_fill = 0U;
	return 0;
}

int mcap_stream_preallocate(struct mcap_stream *stream, uint64_t size_bytes)
{
	FIL *fil;
	FRESULT fr;

	if (stream == NULL || !stream->file_open || stream->file.filep == NULL ||
	    size_bytes == 0U) {
		return -EINVAL;
	}

	/* Allocate the contiguous extent now (opt = 1). f_expand requires the file
	 * to be empty, which holds here because this runs right after open and
	 * before the first write. It sets the file size to the full extent but
	 * leaves the write pointer at 0, so the sequential writes that follow fill
	 * the reserved clusters in place without stretching the FAT. */
	fil = (FIL *)stream->file.filep;

	/* f_expand scans the FAT in a single circular pass that starts at the
	 * volume's suggested next-free cluster and gives up when it comes back to
	 * it, so a free run straddling that point counts as two shorter runs. The
	 * hint is read back from FSInfo at mount, and after a power cut or reset
	 * that left the volume mounted it points into the middle of the free space:
	 * an almost empty card then denies a reservation it has room for twice
	 * over. Point the scan at the start of the volume so it walks the FAT in
	 * layout order and measures the free run the same way the caller's
	 * largest-free-run check does. f_expand replaces the hint with the end of
	 * what it allocates, and a failed expand leaves it at the volume start,
	 * which only makes the next chain allocation search from there. */
	fil->obj.fs->last_clst = 2U;

	fr = f_expand(fil, (FSIZE_t)size_bytes, 1);
	if (fr != FR_OK) {
		/* FR_DENIED here means no contiguous free block of that size exists
		 * (fragmented or nearly full card). Report it as no-space so the
		 * caller falls back to grow-on-write. */
		return fr == FR_DENIED ? -ENOSPC : -EIO;
	}

	stream->preallocated = true;
	stream->reserved_bytes = size_bytes;
	return 0;
}

static int mcap_stream_close_common(struct mcap_stream *stream, bool truncate_tail)
{
	int rc;

	if (stream == NULL || !stream->file_open) {
		return 0;
	}

	if (!stream->write_failed) {
		/* Full remainder, sub-sector tail included: one unaligned write at
		 * close is the price of landing the MCAP footer. */
		if (flush_block(stream, stream->block_fill) == 0) {
			/* Give back the unused tail of a preallocated extent so the
			 * volume is not left holding phantom clusters. This frees FAT
			 * clusters only and writes no erase, so the tail is erased again
			 * when it is next reserved. After flush_block the FatFs write
			 * pointer sits at the real end, so seek to the streamed byte
			 * count and truncate there to free every reserved cluster past
			 * it. Best-effort: a card pulled mid-session sets
			 * write_failed and skips this path entirely, and a stale
			 * full-size directory entry with a garbage tail is acceptable
			 * because the MCAP reader stops at the first invalid record.
			 *
			 * truncate_tail is cleared on size-triggered rotation, where the
			 * stream ran past its whole reservation: the tail is already
			 * empty so there is nothing to free, and skipping the truncate
			 * keeps that rotation off the FAT. Fall back to truncating if the
			 * file somehow closed short of its reservation. */
			bool tail_present = stream->bytes_written < stream->reserved_bytes;

			if (stream->preallocated && stream->file.filep != NULL &&
			    (truncate_tail || tail_present)) {
				FIL *fil = (FIL *)stream->file.filep;

				if (f_lseek(fil, (FSIZE_t)stream->bytes_written) == FR_OK) {
					(void)f_truncate(fil);
				}
			}
			(void)fs_sync(&stream->file);
		}
	}

	rc = fs_close(&stream->file);
	stream->file_open = false;
	return rc;
}

int mcap_stream_close_file(struct mcap_stream *stream)
{
	return mcap_stream_close_common(stream, true);
}

int mcap_stream_close_file_full(struct mcap_stream *stream)
{
	return mcap_stream_close_common(stream, false);
}

int mcap_stream_sink_write(void *context, const uint8_t *data, size_t size)
{
	struct mcap_stream *stream = context;
	size_t offset = 0U;

	if (stream == NULL || !stream->file_open || stream->write_failed) {
		return -1;
	}

	while (offset < size) {
		size_t room = sizeof(stream->block) - stream->block_fill;
		size_t chunk = MIN(room, size - offset);

		memcpy(stream->block + stream->block_fill, data + offset, chunk);
		stream->block_fill += chunk;
		offset += chunk;

		if (stream->block_fill == sizeof(stream->block)) {
			if (flush_block(stream, stream->block_fill) != 0) {
				return -1;
			}
		}
	}

	stream->bytes_written += size;
	return 0;
}

int mcap_stream_sink_flush(void *context)
{
	struct mcap_stream *stream = context;
	int64_t start_ms;
	int rc;

	if (stream == NULL || !stream->file_open || stream->write_failed) {
		if (stream != NULL) {
			stream->last_sync_ok = false;
		}
		return -1;
	}

	/* Whole sectors only: handing FatFs a sub-sector tail would leave the file
	 * offset mid-sector, and every later 4 KiB block write would then split
	 * into a read-modify-write of the shared sector plus a short direct write
	 * from an odd buffer offset. The held-back tail (under one sector) goes out
	 * with the next flush, or in full at close.
	 *
	 * The offset is sector-aligned but not block-aligned, so a 4 KiB write
	 * that straddles a cluster boundary still splits into two aligned
	 * multi-sector writes (about one in eight at 32 KiB clusters). Holding
	 * back the whole sub-4096 remainder instead would remove that split at
	 * the cost of up to 4 KiB of power-cut exposure rather than 511 bytes.
	 */
	if (flush_block(stream, (stream->block_fill / SECTOR_BYTES) * SECTOR_BYTES) != 0) {
		stream->last_sync_ok = false;
		return -1;
	}

	/* While a preallocated extent streams, f_sync has nothing left to do: the
	 * directory entry already carries the reserved size and the timestamp is a
	 * fixed constant, the chain is fully built, and the flush above hands FatFs
	 * only whole sectors, so FatFs holds no dirty file data. An f_sync would
	 * therefore only rewrite an unchanged directory sector every flush. What
	 * still matters for durability is waiting for the card to finish the last
	 * write, so sync the disk instead. The grow-on-write fallback keeps f_sync
	 * because its FAT chain really does change. Close and rotate still go
	 * through fs_sync/fs_close, which land the true size. */
	start_ms = k_uptime_get();
	rc = stream->preallocated
		     ? disk_access_ioctl(RDD2_FLIGHT_LOG_DISK_NAME, DISK_IOCTL_CTRL_SYNC, NULL)
		     : fs_sync(&stream->file);
	stream->last_sync_ms = (uint32_t)(k_uptime_get() - start_ms);
	if (stream->last_sync_ms > stream->max_sync_ms) {
		stream->max_sync_ms = stream->last_sync_ms;
	}
	stream->last_sync_ok = (rc == 0);
	return rc == 0 ? 0 : -1;
}

synapse_mcap_sink_t mcap_stream_sink(struct mcap_stream *stream)
{
	return (synapse_mcap_sink_t){
		.write = mcap_stream_sink_write,
		.flush = mcap_stream_sink_flush,
		.context = stream,
	};
}

void mcap_stream_session_id(char *out)
{
	static const char hex_digits[] = "0123456789abcdef";
	static atomic_t boot_counter;
	uint8_t seed[16] = {0};
	uint8_t uid[16] = {0};
	ssize_t uid_len;
	uint32_t counter;

	if (out == NULL) {
		return;
	}

	counter = (uint32_t)atomic_inc(&boot_counter);

	if (sys_csrand_get(seed, sizeof(seed)) != 0) {
		/* No entropy source: the non-cryptographic generator still gives
		 * a per-boot varying seed, which the unique id and counter make
		 * collision-resistant enough for a log session tag. */
		sys_rand_get(seed, sizeof(seed));
	}

	uid_len = hwinfo_get_device_id(uid, sizeof(uid));
	if (uid_len > 0) {
		for (size_t i = 0U; i < sizeof(seed); ++i) {
			seed[i] ^= uid[i % (size_t)uid_len];
		}
	}

	seed[0] ^= (uint8_t)counter;
	seed[1] ^= (uint8_t)(counter >> 8);
	seed[2] ^= (uint8_t)(counter >> 16);
	seed[3] ^= (uint8_t)(counter >> 24);

	for (size_t i = 0U; i < sizeof(seed); ++i) {
		out[i * 2U] = hex_digits[seed[i] >> 4];
		out[i * 2U + 1U] = hex_digits[seed[i] & 0x0FU];
	}
	out[32] = '\0';
}

int mcap_stream_reservation_open(struct prealloc_reservation *reservation, const char *path,
				 uint64_t target)
{
	int rc;

	if (reservation == NULL || path == NULL || target == 0U) {
		return -EINVAL;
	}

	/* FS_O_TRUNC resets any leftover partial file so growth starts from an
	 * empty chain at a known zero size. */
	fs_file_t_init(&reservation->file);
	rc = fs_open(&reservation->file, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (rc != 0) {
		return rc;
	}

	reservation->open = true;
	reservation->reserved = 0U;
	reservation->target = target;
	return 0;
}

int mcap_stream_reservation_grow(struct prealloc_reservation *reservation, uint64_t step_bytes)
{
	FIL *fil;
	uint64_t previous;
	uint64_t next;
	FRESULT fr;

	if (reservation == NULL || !reservation->open || reservation->file.filep == NULL ||
	    step_bytes == 0U) {
		return -EINVAL;
	}

	previous = reservation->reserved;
	next = previous + step_bytes;
	if (next > reservation->target) {
		next = reservation->target;
	}

	/* Classic FatFs grow idiom: with FA_WRITE set, seeking past the current file
	 * size stretches the cluster chain over the range. In ff.c f_lseek the
	 * cluster-follow loop calls create_chain with forced stretch for every
	 * cluster it crosses in write mode, allocating clusters and writing FAT
	 * entries but no file data. f_expand cannot be used per step because it
	 * requires an empty file (objsize == 0) and so cannot extend an already
	 * partly grown file. This is the only FAT allocation the writer performs
	 * during a live session, and it lands entirely on this disposable file, not
	 * the active session whose extent is already fully built and thus quiet. */
	fil = (FIL *)reservation->file.filep;
	fr = f_lseek(fil, (FSIZE_t)next);
	if (fr != FR_OK) {
		return -EIO;
	}

	/* f_lseek reports FR_OK even when a full card clips the grow, so the
	 * reached offset, not the return code, is the truth. */
	reservation->reserved = (uint64_t)f_tell(fil);
	if (reservation->reserved < next) {
		/* create_chain clipped on a full card: this one cannot reach target. */
		return -ENOSPC;
	}

	/* Erase the clusters this step just added, while they still hold nothing,
	 * so the session that inherits this chain at rotation streams into
	 * known-erased blocks. The range walk reads the chain off the card and so
	 * syncs the file first, which also lands the FAT entries the lseek above
	 * created and the directory size; a yank therefore leaves a well-formed
	 * partial file. Best-effort: a failed erase costs throughput, not data. */
	(void)rdd2_flight_log_fs_trim_file_range(&reservation->file, previous,
						 reservation->reserved);

	if (reservation->reserved >= reservation->target) {
		if (f_sync(fil) != FR_OK) {
			return -EIO;
		}
		mcap_stream_reservation_close(reservation);
		return 1;
	}
	return 0;
}

void mcap_stream_reservation_close(struct prealloc_reservation *reservation)
{
	if (reservation == NULL || !reservation->open) {
		return;
	}
	(void)fs_close(&reservation->file);
	reservation->open = false;
}
