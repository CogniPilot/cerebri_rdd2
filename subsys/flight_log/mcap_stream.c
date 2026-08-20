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
	stream->bytes_written = 0U;
	stream->block_fill = 0U;
	return 0;
}

int mcap_stream_close_file(struct mcap_stream *stream)
{
	int rc;

	if (stream == NULL || !stream->file_open) {
		return 0;
	}

	if (!stream->write_failed) {
		if (flush_block(stream) == 0) {
			(void)fs_sync(&stream->file);
		}
	}

	rc = fs_close(&stream->file);
	stream->file_open = false;
	return rc;
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
