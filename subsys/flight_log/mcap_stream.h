/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_FLIGHT_LOG_MCAP_STREAM_H_
#define RDD2_FLIGHT_LOG_MCAP_STREAM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/fs/fs.h>

#include <synapse/mcap.h>

/* 32 lowercase hex characters plus a terminating NUL. */
#define RDD2_FLIGHT_LOG_SESSION_ID_LEN 33U

/*
 * FatFs-backed complete-write sink for the synapse/1 MCAP writer.
 *
 * The writer emits variable-length byte runs. This adapter accumulates them
 * into a sector-aligned block and pushes whole blocks to the open file, so the
 * card sees aligned writes rather than one write per short record. A flush
 * pushes the partial remainder and issues an f_sync, bounding how much trailing
 * data a power loss can cost. All memory is caller-owned and there is no
 * dynamic allocation: only the writer's storage thread ever calls into the
 * sink, so blocking on the card here is by design.
 *
 * When preallocated is set the session file was expanded to a contiguous extent
 * at open (see mcap_stream_preallocate), so the streaming writes fill the
 * reserved clusters in place and never grow the FAT. Close truncates the unused
 * tail back to bytes_written so the reservation does not leave phantom space.
 */
struct mcap_stream {
	struct fs_file_t file;
	bool file_open;
	bool write_failed;
	bool last_sync_ok;
	bool preallocated;      /* session file holds a reserved extent */
	uint64_t reserved_bytes; /* size of the reserved extent, 0 when grow-on-write */
	uint64_t bytes_written; /* payload bytes handed to FatFs */
	size_t block_fill;
	uint8_t block[CONFIG_RDD2_FLIGHT_LOG_BLOCK_BYTES];
};

/*
 * Background pre-allocated spare file. While a session streams, the writer grows
 * this reserved file one small step per flush cycle (mcap_stream_spare_grow) so
 * the next session's cluster extent is fully built before rotation, moving the
 * f_expand burst out of the mid-flight rotation path.
 *
 * The spare is disposable and holds no logged data: growth only stretches the
 * FAT cluster chain over newly seeked ranges, it never writes file content. Its
 * name is ignored by the session-index scan, and because the active session
 * file's extent is already fully built, a card yank during a growth step can
 * damage only this spare's chain, never the streaming session.
 */
struct prealloc_spare {
	struct fs_file_t file;
	bool open;
	uint64_t reserved; /* bytes of chain allocated so far */
	uint64_t target;   /* final reservation, the rotation size */
};

/*
 * Open path for writing, creating and truncating it. Returns 0 on success or a
 * negative errno. The stream must be zeroed by the caller before first use.
 */
int mcap_stream_open_file(struct mcap_stream *stream, const char *path);

/*
 * Reserve a contiguous size_bytes extent for the freshly opened session file so
 * the streaming writes fill it in place and touch no FAT allocation metadata.
 * Must be called immediately after mcap_stream_open_file and before any bytes
 * are written, because the underlying f_expand only accepts an empty file. On
 * success sets stream->preallocated and returns 0. Returns -ENOSPC when no
 * contiguous extent that large is free, or another negative errno on failure:
 * preallocation is an optimization, so the caller continues with grow-on-write
 * on any failure. Runs under the card lock like the rest of the session layer.
 */
int mcap_stream_preallocate(struct mcap_stream *stream, uint64_t size_bytes);

/*
 * Open an existing file for writing without truncating it, positioned at offset
 * 0, so a pre-built cluster chain (a spare renamed into this session) is reused
 * in place: the streaming writes overwrite the reserved clusters from the top and
 * never grow the FAT. Marks the stream preallocated with the given reserved size.
 * Must not pass a truncating or appending open, both of which would defeat the
 * reservation. Returns 0 on success or a negative errno.
 */
int mcap_stream_open_existing(struct mcap_stream *stream, const char *path,
			      uint64_t reserved_bytes);

/*
 * Flush any buffered remainder, sync, and close the file. Returns 0 on success.
 * Safe to call on an already-closed stream. Truncates a preallocated extent back
 * to bytes_written so the unused tail frees: this is the stop and fallback path.
 */
int mcap_stream_close_file(struct mcap_stream *stream);

/*
 * Close for size-triggered rotation, where the file streamed past its full
 * reservation. When bytes_written has reached the reserved extent the unused
 * tail is empty, so the truncate is skipped and the pre-built chain is left
 * intact for the reader (a truncate here would be a needless metadata write at
 * the very moment rotation must stay cheap). Falls back to a truncating close if
 * the file somehow closed short of its reservation. Returns 0 on success.
 */
int mcap_stream_close_file_full(struct mcap_stream *stream);

/* Complete-write sink callback: 0 means every byte was accepted. */
int mcap_stream_sink_write(void *context, const uint8_t *data, size_t size);

/* Sink flush callback: writes the remainder and syncs the file. */
int mcap_stream_sink_flush(void *context);

/* Build a synapse_mcap_sink_t bound to this stream. */
synapse_mcap_sink_t mcap_stream_sink(struct mcap_stream *stream);

/*
 * Fill out (RDD2_FLIGHT_LOG_SESSION_ID_LEN bytes) with a 32-hex session id.
 * Folds a cryptographic random draw (or the non-cryptographic generator when
 * entropy is unavailable) with the SoC unique id and a monotonic boot counter,
 * so ids do not repeat across boots or across boards.
 */
void mcap_stream_session_id(char *out);

/*
 * Open (creating, truncating any leftover) the spare at path and set its growth
 * target. reserved starts at 0. Returns 0 on success or a negative errno.
 */
int mcap_stream_spare_open(struct prealloc_spare *spare, const char *path,
			   uint64_t target);

/*
 * Grow the spare by at most step_bytes using the FatFs write-mode lseek idiom,
 * which stretches the file's cluster chain over the newly seeked range and
 * writes only FAT metadata, no file data. Syncs after the step so the on-disk
 * chain and directory size stay consistent and a yank leaves a well-formed
 * partial spare. Returns 1 when the target is reached (the spare is synced and
 * closed, ready to rename), 0 when more steps remain, or a negative errno on
 * failure (-ENOSPC when the card cannot hold the full reservation alongside the
 * live session). The caller discards the spare on any negative return.
 */
int mcap_stream_spare_grow(struct prealloc_spare *spare, uint64_t step_bytes);

/* Close the spare file if open. Does not remove it from the card. */
void mcap_stream_spare_close(struct prealloc_spare *spare);

#endif /* RDD2_FLIGHT_LOG_MCAP_STREAM_H_ */
