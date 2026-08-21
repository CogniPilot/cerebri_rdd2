/* SPDX-License-Identifier: Apache-2.0 */

#include "mcap_stream.h"

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
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

static int flush_block(struct mcap_stream *stream)
{
	int rc;

	if (stream->block_fill == 0U) {
		return 0;
	}
	rc = write_all(stream, stream->block, stream->block_fill);
	stream->block_fill = 0U;
	return rc;
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
		if (flush_block(stream) == 0) {
			/* Give back the unused tail of a preallocated extent so the
			 * card is not left holding phantom clusters. After flush_block
			 * the FatFs write pointer sits at the real end, so seek to the
			 * streamed byte count and truncate there to free every reserved
			 * cluster past it. Best-effort: a card pulled mid-session sets
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
			if (flush_block(stream) != 0) {
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
	int rc;

	if (stream == NULL || !stream->file_open || stream->write_failed) {
		if (stream != NULL) {
			stream->last_sync_ok = false;
		}
		return -1;
	}

	if (flush_block(stream) != 0) {
		stream->last_sync_ok = false;
		return -1;
	}

	rc = fs_sync(&stream->file);
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

int mcap_stream_spare_open(struct prealloc_spare *spare, const char *path, uint64_t target)
{
	int rc;

	if (spare == NULL || path == NULL || target == 0U) {
		return -EINVAL;
	}

	/* FS_O_TRUNC resets any leftover partial spare so growth starts from an
	 * empty chain at a known zero size. */
	fs_file_t_init(&spare->file);
	rc = fs_open(&spare->file, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (rc != 0) {
		return rc;
	}

	spare->open = true;
	spare->reserved = 0U;
	spare->target = target;
	return 0;
}

int mcap_stream_spare_grow(struct prealloc_spare *spare, uint64_t step_bytes)
{
	FIL *fil;
	uint64_t next;
	FRESULT fr;

	if (spare == NULL || !spare->open || spare->file.filep == NULL || step_bytes == 0U) {
		return -EINVAL;
	}

	next = spare->reserved + step_bytes;
	if (next > spare->target) {
		next = spare->target;
	}

	/* Classic FatFs grow idiom: with FA_WRITE set, seeking past the current file
	 * size stretches the cluster chain over the range. In ff.c f_lseek the
	 * cluster-follow loop calls create_chain with forced stretch for every
	 * cluster it crosses in write mode, allocating clusters and writing FAT
	 * entries but no file data. f_expand cannot be used per step because it
	 * requires an empty file (objsize == 0) and so cannot extend an already
	 * partly grown spare. This is the only FAT allocation the writer performs
	 * during a live session, and it lands entirely on this disposable file, not
	 * the active session whose extent is already fully built and thus quiet. */
	fil = (FIL *)spare->file.filep;
	fr = f_lseek(fil, (FSIZE_t)next);
	if (fr != FR_OK) {
		return -EIO;
	}

	/* No per-step sync: the spare is disposable, so durability of a partial
	 * build is worthless, and skipping the sync lets the FatFs sector window
	 * coalesce the FAT writes of many consecutive steps into the rare flush
	 * when the window moves. The one durable sync happens at completion via
	 * the close below, which also lands the directory size. f_lseek reports
	 * FR_OK even when a full card clips the grow, so the reached offset, not
	 * the return code, is the truth. */
	spare->reserved = (uint64_t)f_tell(fil);
	if (spare->reserved < next) {
		/* create_chain clipped on a full card: the spare cannot reach target. */
		return -ENOSPC;
	}
	if (spare->reserved >= spare->target) {
		if (f_sync(fil) != FR_OK) {
			return -EIO;
		}
		mcap_stream_spare_close(spare);
		return 1;
	}
	return 0;
}

void mcap_stream_spare_close(struct prealloc_spare *spare)
{
	if (spare == NULL || !spare->open) {
		return;
	}
	(void)fs_close(&spare->file);
	spare->open = false;
}
