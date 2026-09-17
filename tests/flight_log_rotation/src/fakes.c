/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Fake filesystem, MCAP sink, subscriber bus, and boot clock for the flight-log
 * rotation suite. They let the production flight_log.c writer, ring accounting,
 * session rotation, and standing-reservation pool run on native_sim while the
 * card behaviour, including a configurable multi-second reservation stall, is
 * controlled from the test body.
 */

#include "harness.h"

#include "flight_log_fs.h"
#include "mcap_stream.h"

#include <errno.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>

#include <synapse/mcap.h>
#include <synapse/mcap_topics.h>

#include <interfaces/synapse_time_status.h>
#include <interfaces/zros_topics.h>

#include <zros/zros_node.h>
#include <zros/zros_sub.h>
#include <zros/zros_topic.h>

/* Bytes of framing the constant-memory MCAP writer adds per fixed record.
 * Combined with the 40-byte InertialSample payload this yields the recorded
 * 54 bytes per control_imu message. */
#define RDD2_FAKE_MCAP_RECORD_OVERHEAD 14U

volatile uint32_t rdd2_test_expand_stall_ms;
volatile bool rdd2_test_load_paused;
volatile uint32_t rdd2_test_prealloc_count;
volatile uint32_t rdd2_test_rename_count;

/* ---- topic handles ---- */

struct zros_topic topic_control_imu;
struct zros_topic topic_pwm_signal_outputs;
struct zros_topic topic_navigation_odometry;
struct zros_topic topic_attitude_estimate;
struct zros_topic topic_vehicle_health;
struct zros_topic topic_control_loop_metrics;
struct zros_topic topic_attitude_command;
struct zros_topic topic_rate_command;
struct zros_topic topic_manual_input;
struct zros_topic topic_optical_flow_vel;
struct zros_topic topic_optical_flow;
struct zros_topic topic_gnss_fix;

/* ---- boot clock ---- */

uint64_t synapse_time_boot_ns(void)
{
	return k_ticks_to_ns_floor64(k_uptime_ticks());
}

synapse_types_TimeStatus_enum_t synapse_time_status_resolve(bool *ever_synced, int64_t *offset_ns)
{
	if (ever_synced != NULL) {
		*ever_synced = false;
	}
	if (offset_ns != NULL) {
		*offset_ns = 0;
	}
	return synapse_types_TimeStatus_LocalFreerun;
}

uint64_t synapse_time_apply_offset(uint64_t boot_ns, int64_t offset_ns)
{
	return boot_ns + (uint64_t)offset_ns;
}

/* ---- subscriber bus ----
 *
 * The logger initialises one subscriber per source in the source-table order.
 * The recorded flight per-channel publish rates follow that same order. */
static const double g_source_rate_hz[] = {
	800.0, /* control_imu    */
	400.0, /* pwm            */
	100.0, /* odometry       */
	200.0, /* attitude       */
	193.0, /* vehicle health */
	200.0, /* loop metrics   */
	200.0, /* attitude cmd   */
	200.0, /* rate cmd       */
	380.0, /* manual         */
	34.0,  /* optical flow   */
	34.0,  /* optical flow raw */
	6.0,   /* gnss           */
};

static size_t g_sub_init_count;

void zros_node_init(struct zros_node *node, const char *name)
{
	if (node != NULL) {
		node->name = name;
	}
}

int zros_sub_init(struct zros_sub *sub, struct zros_node *node, struct zros_topic *topic,
		  void *data, double rate_limit_hz)
{
	ARG_UNUSED(node);
	ARG_UNUSED(topic);
	ARG_UNUSED(rate_limit_hz);

	if (sub == NULL) {
		return -EINVAL;
	}

	sub->source_index = (int)g_sub_init_count;
	sub->data = data;
	if (g_sub_init_count < ARRAY_SIZE(g_source_rate_hz)) {
		double hz = g_source_rate_hz[g_sub_init_count];

		sub->period_ns = hz > 0.0 ? (uint64_t)(1000000000.0 / hz) : 0U;
	} else {
		sub->period_ns = 0U;
	}
	sub->next_due_ns = 0U;
	g_sub_init_count++;
	return 0;
}

bool zros_sub_update_available(struct zros_sub *sub)
{
	if (sub == NULL || sub->period_ns == 0U || rdd2_test_load_paused) {
		return false;
	}
	return synapse_time_boot_ns() >= sub->next_due_ns;
}

int zros_sub_update(struct zros_sub *sub)
{
	uint64_t now;

	if (sub == NULL) {
		return -EINVAL;
	}

	now = synapse_time_boot_ns();
	sub->next_due_ns += sub->period_ns;
	/* Never let a scheduling gap turn into a catch-up burst: resume pacing
	 * from the present instead of replaying missed periods back to back. */
	if (sub->next_due_ns + sub->period_ns < now) {
		sub->next_due_ns = now + sub->period_ns;
	}
	return 0;
}

int zros_sub_wait_many(struct zros_sub *const *subs, size_t count, k_timeout_t timeout)
{
	ARG_UNUSED(subs);
	ARG_UNUSED(count);
	ARG_UNUSED(timeout);

	if (rdd2_test_load_paused) {
		k_sleep(K_MSEC(20));
		return -EAGAIN;
	}

	/* Advance the simulated clock one fine quantum, then report samples
	 * available so the capture loop drains every due source this quantum. */
	k_sleep(K_USEC(250));
	return 0;
}

/* ---- fake card filesystem ---- */

static K_MUTEX_DEFINE(g_card_lock);
static bool g_mounted;
static uint32_t g_mount_generation;
static uint32_t g_next_index;

/*
 * Fake card directory. Only what the session layer reads back is modelled: the
 * standing reservation pool, as a table of basename and size. Session files are
 * not modelled, since nothing looks one up, so renaming a reservation into a
 * session name simply drops its entry.
 */
#define FAKE_FILE_SLOTS 8

struct fake_file {
	char name[32];
	uint64_t size;
};

static struct fake_file g_files[FAKE_FILE_SLOTS];
static struct fake_file *g_building;

static const char *basename_of(const char *path)
{
	const char *slash = strrchr(path, '/');

	return slash != NULL ? slash + 1 : path;
}

static struct fake_file *fake_find(const char *path)
{
	const char *name = basename_of(path);

	for (size_t i = 0U; i < FAKE_FILE_SLOTS; ++i) {
		if (g_files[i].name[0] != '\0' && strcmp(g_files[i].name, name) == 0) {
			return &g_files[i];
		}
	}
	return NULL;
}

static struct fake_file *fake_create(const char *path)
{
	const char *name = basename_of(path);

	for (size_t i = 0U; i < FAKE_FILE_SLOTS; ++i) {
		if (g_files[i].name[0] == '\0') {
			strncpy(g_files[i].name, name, sizeof(g_files[i].name) - 1U);
			g_files[i].size = 0U;
			return &g_files[i];
		}
	}
	return NULL;
}

/* Basename of a pool slot, the same pattern the logger composes paths with. */
static void reserve_name(uint32_t slot, char *out, size_t cap)
{
	(void)snprintk(out, cap, RDD2_FLIGHT_LOG_RESERVE_PATTERN, (unsigned int)slot);
}

uint32_t rdd2_test_ready_reservations(void)
{
	uint32_t count = 0U;

	for (uint32_t slot = 0U; slot < RDD2_FLIGHT_LOG_RESERVE_SLOTS; ++slot) {
		char name[32];
		const struct fake_file *f;

		reserve_name(slot, name, sizeof(name));
		f = fake_find(name);
		if (f != NULL && f->size == (uint64_t)CONFIG_RDD2_FLIGHT_LOG_ROTATE_BYTES) {
			count++;
		}
	}
	return count;
}

void rdd2_flight_log_fs_lock(void)
{
	k_mutex_lock(&g_card_lock, K_FOREVER);
}

void rdd2_flight_log_fs_unlock(void)
{
	k_mutex_unlock(&g_card_lock);
}

int rdd2_flight_log_fs_mount(void)
{
	g_mounted = true;
	g_mount_generation++;
	return 0;
}

uint32_t rdd2_flight_log_fs_mount_generation(void)
{
	return g_mount_generation;
}

int rdd2_flight_log_fs_unmount(void)
{
	g_mounted = false;
	return 0;
}

bool rdd2_flight_log_fs_mounted(void)
{
	return g_mounted;
}

/* The session layer erases its reservation before streaming into it. Nothing in
 * the rotation behaviour under test depends on that erase, so the fake card just
 * accepts it. */
int rdd2_flight_log_fs_trim_file(struct fs_file_t *file, const char *name)
{
	ARG_UNUSED(file);
	ARG_UNUSED(name);
	return 0;
}

/* A defragmented card: the whole free space is one run, so the reservation is
 * never clamped below the rotation size. */
int rdd2_flight_log_fs_largest_free_run(uint64_t *bytes_out)
{
	if (bytes_out == NULL) {
		return -EINVAL;
	}
	*bytes_out = 512ULL << 20;
	return 0;
}

/* The production one-pass directory read, over the fake directory table. */
int rdd2_flight_log_fs_scan(struct rdd2_flight_log_scan *out)
{
	if (out == NULL) {
		return -EINVAL;
	}
	if (!g_mounted) {
		return -ENODEV;
	}

	out->next_index = g_next_index;
	out->ready_count = 0U;
	out->ready_slot = -1;
	out->free_slot = -1;
	out->free_slot_used = false;

	for (uint32_t slot = 0U; slot < RDD2_FLIGHT_LOG_RESERVE_SLOTS; ++slot) {
		char name[32];
		const struct fake_file *f;

		reserve_name(slot, name, sizeof(name));
		f = fake_find(name);
		if (f != NULL && f->size == (uint64_t)CONFIG_RDD2_FLIGHT_LOG_ROTATE_BYTES) {
			out->ready_count++;
			if (out->ready_slot < 0) {
				out->ready_slot = (int)slot;
			}
		} else if (out->free_slot < 0) {
			out->free_slot = (int)slot;
			out->free_slot_used = f != NULL;
		}
	}
	return 0;
}

int rdd2_flight_log_fs_session_path(uint32_t index, char *out, size_t cap)
{
	int written;

	if (out == NULL) {
		return -EINVAL;
	}
	written = snprintk(out, cap, "%s/%s%04u%s", RDD2_FLIGHT_LOG_MOUNT_POINT,
			   RDD2_FLIGHT_LOG_FILE_PREFIX, (unsigned int)index,
			   RDD2_FLIGHT_LOG_FILE_SUFFIX);
	if (written < 0 || (size_t)written >= cap) {
		return -ENAMETOOLONG;
	}
	return written;
}

/* Raw fs entry points the logger calls directly. */

int fs_statvfs(const char *path, struct fs_statvfs *stat)
{
	ARG_UNUSED(path);
	if (stat == NULL) {
		return -EINVAL;
	}
	/* Roomy card: 512 MiB free against a scaled 256 KiB rotation size and a
	 * 1 MiB pool, so the session reservation keeps its full size and every pool
	 * build passes the free-space gate. */
	stat->f_bsize = 512U;
	stat->f_frsize = 512U;
	stat->f_blocks = 1U << 20;
	stat->f_bfree = 1U << 20;
	return 0;
}

int fs_unlink(const char *path)
{
	struct fake_file *f = fake_find(path);

	if (f == NULL) {
		return -ENOENT;
	}
	if (f == g_building) {
		g_building = NULL;
	}
	f->name[0] = '\0';
	return 0;
}

int fs_rename(const char *from, const char *to)
{
	struct fake_file *f = fake_find(from);

	ARG_UNUSED(to);
	if (f == NULL) {
		return -ENOENT;
	}
	/* Directory-entry-only move: the pre-built extent becomes the session, and
	 * session files are not modelled, so the entry simply goes away. */
	f->name[0] = '\0';
	g_next_index++;
	rdd2_test_rename_count++;
	return 0;
}

/* ---- fake MCAP sink / session stream ---- */

int mcap_stream_open_file(struct mcap_stream *stream, const char *path)
{
	ARG_UNUSED(path);
	if (stream == NULL) {
		return -EINVAL;
	}
	/* A new session file appears in the directory, so the next scan reports the
	 * following index. The rename path does the same in fs_rename. */
	g_next_index++;
	stream->file_open = true;
	stream->write_failed = false;
	stream->last_sync_ok = true;
	stream->preallocated = false;
	stream->reserved_bytes = 0U;
	stream->bytes_written = 0U;
	stream->block_fill = 0U;
	return 0;
}

int mcap_stream_preallocate(struct mcap_stream *stream, uint64_t size_bytes)
{
	if (stream == NULL || !stream->file_open || size_bytes == 0U) {
		return -EINVAL;
	}
	/* The inline f_expand burst: a multi-second allocation stall on the
	 * card. The writer holds the card lock across it; capture keeps
	 * enqueuing into the ring throughout. */
	if (rdd2_test_expand_stall_ms > 0U) {
		k_sleep(K_MSEC((int32_t)rdd2_test_expand_stall_ms));
	}
	rdd2_test_prealloc_count++;
	stream->preallocated = true;
	stream->reserved_bytes = size_bytes;
	return 0;
}

int mcap_stream_open_existing(struct mcap_stream *stream, const char *path, uint64_t reserved_bytes)
{
	ARG_UNUSED(path);
	if (stream == NULL) {
		return -EINVAL;
	}
	/* Reopen of a renamed pre-built extent: no reservation stall. */
	stream->file_open = true;
	stream->write_failed = false;
	stream->last_sync_ok = true;
	stream->preallocated = true;
	stream->reserved_bytes = reserved_bytes;
	stream->bytes_written = 0U;
	stream->block_fill = 0U;
	return 0;
}

static int mcap_stream_close_common(struct mcap_stream *stream)
{
	if (stream == NULL || !stream->file_open) {
		return 0;
	}
	stream->block_fill = 0U;
	stream->file_open = false;
	return 0;
}

int mcap_stream_close_file(struct mcap_stream *stream)
{
	return mcap_stream_close_common(stream);
}

int mcap_stream_close_file_full(struct mcap_stream *stream)
{
	return mcap_stream_close_common(stream);
}

int mcap_stream_sink_write(void *context, const uint8_t *data, size_t size)
{
	struct mcap_stream *stream = context;

	ARG_UNUSED(data);
	if (stream == NULL || !stream->file_open || stream->write_failed) {
		return -1;
	}
	stream->bytes_written += size;
	return 0;
}

int mcap_stream_sink_flush(void *context)
{
	struct mcap_stream *stream = context;

	if (stream == NULL || !stream->file_open || stream->write_failed) {
		if (stream != NULL) {
			stream->last_sync_ok = false;
		}
		return -1;
	}
	stream->last_sync_ok = true;
	return 0;
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
	if (out == NULL) {
		return;
	}
	memset(out, '0', RDD2_FLIGHT_LOG_SESSION_ID_LEN - 1U);
	out[RDD2_FLIGHT_LOG_SESSION_ID_LEN - 1U] = '\0';
}

int mcap_stream_reservation_open(struct prealloc_reservation *reservation, const char *path,
				 uint64_t target)
{
	struct fake_file *f;

	if (reservation == NULL || path == NULL || target == 0U) {
		return -EINVAL;
	}
	f = fake_find(path);
	if (f == NULL) {
		f = fake_create(path);
		if (f == NULL) {
			return -ENOSPC;
		}
	}
	f->size = 0U;
	g_building = f;
	reservation->open = true;
	reservation->reserved = 0U;
	reservation->target = target;
	return 0;
}

int mcap_stream_reservation_grow(struct prealloc_reservation *reservation, uint64_t step_bytes)
{
	if (reservation == NULL || !reservation->open || step_bytes == 0U ||
	    g_building == NULL) {
		return -EINVAL;
	}
	reservation->reserved += step_bytes;
	if (reservation->reserved >= reservation->target) {
		reservation->reserved = reservation->target;
		reservation->open = false;
		g_building->size = reservation->target;
		g_building = NULL;
		return 1;
	}
	g_building->size = reservation->reserved;
	return 0;
}

void mcap_stream_reservation_close(struct prealloc_reservation *reservation)
{
	if (reservation == NULL || !reservation->open) {
		return;
	}
	reservation->open = false;
}

/* ---- MCAP writer ---- */

int synapse_mcap_open(synapse_mcap_writer_t *writer, synapse_mcap_sink_t sink,
		      uint8_t *output_buffer, size_t output_buffer_size, const char *library,
		      const char *session_id, const char *source, int time_mode)
{
	ARG_UNUSED(output_buffer);
	ARG_UNUSED(output_buffer_size);
	ARG_UNUSED(library);
	ARG_UNUSED(session_id);
	ARG_UNUSED(source);
	ARG_UNUSED(time_mode);
	if (writer == NULL) {
		return -EINVAL;
	}
	writer->sink = sink;
	writer->sticky_error = SYNAPSE_MCAP_OK;
	writer->open = 1;
	return SYNAPSE_MCAP_OK;
}

int synapse_mcap_add_topic(synapse_mcap_writer_t *writer, const synapse_mcap_topic_t *topic,
			   const char *key, synapse_mcap_channel_t *channel)
{
	ARG_UNUSED(writer);
	ARG_UNUSED(key);
	if (topic == NULL || channel == NULL) {
		return -EINVAL;
	}
	channel->topic_id = topic->topic_id;
	channel->payload_size = topic->payload_size;
	channel->fixed_layout = topic->fixed_layout;
	return SYNAPSE_MCAP_OK;
}

int synapse_mcap_write_fixed(synapse_mcap_writer_t *writer, synapse_mcap_channel_t *channel,
			     uint64_t log_time_ns, uint64_t publish_time_ns, const void *payload,
			     size_t payload_size)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(log_time_ns);
	ARG_UNUSED(publish_time_ns);
	ARG_UNUSED(payload);
	if (writer == NULL || !writer->open) {
		return -EINVAL;
	}
	/* Account the framed record through the sink so the session byte count
	 * that arms rotation reflects the real streamed volume. */
	(void)writer->sink.write(writer->sink.context, NULL,
				 payload_size + RDD2_FAKE_MCAP_RECORD_OVERHEAD);
	return SYNAPSE_MCAP_OK;
}

int synapse_mcap_flush(synapse_mcap_writer_t *writer)
{
	if (writer == NULL || !writer->open) {
		return -EINVAL;
	}
	return writer->sink.flush(writer->sink.context) == 0 ? SYNAPSE_MCAP_OK : -EIO;
}

int synapse_mcap_close(synapse_mcap_writer_t *writer)
{
	if (writer == NULL) {
		return -EINVAL;
	}
	writer->open = 0;
	return SYNAPSE_MCAP_OK;
}

int synapse_mcap_error(const synapse_mcap_writer_t *writer)
{
	if (writer == NULL) {
		return -EINVAL;
	}
	return writer->sticky_error;
}
