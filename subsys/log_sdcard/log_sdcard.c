/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ff.h>

#include <zephyr/fs/fs.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <zros/zros_node.h>
#include <zros/zros_sub.h>

#include "rdd2_log_schema_blobs.h"
#include "synapse/synapse_log_builder.h"
#include "topic_bus.h"

LOG_MODULE_REGISTER(rdd2_log_sdcard, CONFIG_RDD2_LOG_SDCARD_LOG_LEVEL);

#define RDD2_LOG_FORMAT_VERSION 1U
#define RDD2_LOG_SYNC_INTERVAL_MS 4000
#define RDD2_LOG_FILE_NAME_LEN 64

enum rdd2_log_schema_id {
	RDD2_LOG_SCHEMA_ID_LOG = 1U,
	RDD2_LOG_SCHEMA_ID_TOPICS = 2U,
	RDD2_LOG_SCHEMA_ID_MOCAP = 3U,
};

enum rdd2_log_topic_id {
	RDD2_LOG_TOPIC_ID_FLIGHT_STATE = 1U,
	RDD2_LOG_TOPIC_ID_MOTOR_OUTPUT = 2U,
	RDD2_LOG_TOPIC_ID_MOCAP_FRAME = 3U,
};

struct rdd2_log_schema_descriptor {
	uint64_t schema_id;
	const char *name;
	const char *root_type;
	const char *file_identifier;
	const uint8_t *fbs;
	size_t fbs_len;
	const char *fbs_sha256;
	const uint8_t *bfbs;
	size_t bfbs_len;
};

struct rdd2_log_topic_descriptor {
	uint32_t topic_id;
	const char *name;
	uint64_t schema_id;
	const char *encoding;
};

static const struct rdd2_log_schema_descriptor g_schema_records[] = {
	{
		.schema_id = RDD2_LOG_SCHEMA_ID_LOG,
		.name = "synapse_log.fbs",
		.root_type = "synapse.log.LogRecord",
		.file_identifier = "SYLG",
		.fbs = rdd2_schema_synapse_log_fbs,
		.fbs_len = RDD2_SCHEMA_SYNAPSE_LOG_FBS_LEN,
		.fbs_sha256 = RDD2_SCHEMA_SYNAPSE_LOG_FBS_SHA256,
		.bfbs = rdd2_schema_synapse_log_bfbs,
		.bfbs_len = RDD2_SCHEMA_SYNAPSE_LOG_BFBS_LEN,
	},
	{
		.schema_id = RDD2_LOG_SCHEMA_ID_TOPICS,
		.name = "synapse_topics.fbs",
		.root_type = "synapse.topic",
		.file_identifier = "",
		.fbs = rdd2_schema_synapse_topics_fbs,
		.fbs_len = RDD2_SCHEMA_SYNAPSE_TOPICS_FBS_LEN,
		.fbs_sha256 = RDD2_SCHEMA_SYNAPSE_TOPICS_FBS_SHA256,
		.bfbs = rdd2_schema_synapse_topics_bfbs,
		.bfbs_len = RDD2_SCHEMA_SYNAPSE_TOPICS_BFBS_LEN,
	},
	{
		.schema_id = RDD2_LOG_SCHEMA_ID_MOCAP,
		.name = "synapse_mocap.fbs",
		.root_type = "synapse.topic",
		.file_identifier = "",
		.fbs = rdd2_schema_synapse_mocap_fbs,
		.fbs_len = RDD2_SCHEMA_SYNAPSE_MOCAP_FBS_LEN,
		.fbs_sha256 = RDD2_SCHEMA_SYNAPSE_MOCAP_FBS_SHA256,
		.bfbs = rdd2_schema_synapse_mocap_bfbs,
		.bfbs_len = RDD2_SCHEMA_SYNAPSE_MOCAP_BFBS_LEN,
	},
};

static const struct rdd2_log_topic_descriptor g_topic_records[] = {
	{
		.topic_id = RDD2_LOG_TOPIC_ID_FLIGHT_STATE,
		.name = "flight_state",
		.schema_id = RDD2_LOG_SCHEMA_ID_TOPICS,
		.encoding = "flatbuffers:synapse.topic.FlightSnapshot",
	},
	{
		.topic_id = RDD2_LOG_TOPIC_ID_MOTOR_OUTPUT,
		.name = "motor_output",
		.schema_id = RDD2_LOG_SCHEMA_ID_TOPICS,
		.encoding = "flatbuffers:synapse.topic.MotorOutput",
	},
#if IS_ENABLED(CONFIG_RDD2_LOG_SDCARD_LOG_MOCAP)
	{
		.topic_id = RDD2_LOG_TOPIC_ID_MOCAP_FRAME,
		.name = "mocap_frame",
		.schema_id = RDD2_LOG_SCHEMA_ID_MOCAP,
		.encoding = "flatbuffers:synapse.topic.MocapFrame",
	},
#endif
};

struct rdd2_log_context {
	atomic_t running;
	atomic_t stop_requested;
	struct k_thread thread;
	struct k_mutex lock;
	struct fs_file_t file;
	struct fs_mount_t mount;
	FATFS fat_fs;
	flatbuffers_builder_t builder;
	struct zros_node node;
	struct zros_sub flight_state_sub;
	struct zros_sub motor_output_sub;
	rdd2_topic_flight_state_blob_t flight_state;
	rdd2_topic_motor_output_blob_t motor_output;
#if IS_ENABLED(CONFIG_RDD2_LOG_SDCARD_LOG_MOCAP)
	struct zros_sub mocap_frame_sub;
	struct rdd2_topic_mocap_frame mocap_frame;
#endif
	char path[RDD2_LOG_FILE_NAME_LEN];
	size_t bytes_written;
	size_t frames_written;
	size_t dropped_frames;
};

static K_THREAD_STACK_DEFINE(g_rdd2_log_stack, CONFIG_RDD2_LOG_SDCARD_STACK_SIZE);
static struct rdd2_log_context g_ctx = {
	.running = ATOMIC_INIT(0),
	.stop_requested = ATOMIC_INIT(0),
	.lock = Z_MUTEX_INITIALIZER(g_ctx.lock),
};

static int rdd2_log_write_all(struct fs_file_t *file, const void *data, size_t len)
{
	const uint8_t *cursor = data;
	size_t remaining = len;

	while (remaining > 0U) {
		ssize_t written = fs_write(file, cursor, remaining);

		if (written < 0) {
			return (int)written;
		}
		if (written == 0) {
			return -EIO;
		}

		cursor += written;
		remaining -= (size_t)written;
	}

	return 0;
}

static int rdd2_log_write_delimited_record(struct rdd2_log_context *ctx, const void *data,
					   size_t len)
{
	uint8_t prefix[sizeof(uint32_t)];
	int rc;

	if (len > UINT32_MAX) {
		return -E2BIG;
	}

	sys_put_le32((uint32_t)len, prefix);
	rc = rdd2_log_write_all(&ctx->file, prefix, sizeof(prefix));
	if (rc != 0) {
		return rc;
	}

	rc = rdd2_log_write_all(&ctx->file, data, len);
	if (rc != 0) {
		return rc;
	}

	ctx->bytes_written += sizeof(prefix) + len;
	return 0;
}

static int rdd2_log_finish_record(struct rdd2_log_context *ctx, uint8_t **data_out,
				  size_t *len_out)
{
	void *data;

	data = flatcc_builder_finalize_buffer(&ctx->builder, len_out);
	if (data == NULL || *len_out == 0U) {
		return -ENOMEM;
	}

	*data_out = data;
	return 0;
}

static int rdd2_log_write_built_record(struct rdd2_log_context *ctx)
{
	uint8_t *data = NULL;
	size_t len = 0U;
	int rc;

	rc = rdd2_log_finish_record(ctx, &data, &len);
	if (rc == 0) {
		rc = rdd2_log_write_delimited_record(ctx, data, len);
	}

	if (data != NULL) {
		flatcc_builder_free(data);
	}
	return rc;
}

static int rdd2_log_builder_reset(struct rdd2_log_context *ctx)
{
	return flatcc_builder_reset(&ctx->builder) == 0 ? 0 : -ENOMEM;
}

static int rdd2_log_write_header(struct rdd2_log_context *ctx)
{
	flatbuffers_string_ref_t source;
	flatbuffers_string_ref_t description;
	synapse_log_LogFileHeader_ref_t header;

	if (rdd2_log_builder_reset(ctx) != 0) {
		return -ENOMEM;
	}

	source = flatbuffers_string_create_str(&ctx->builder, "cerebri_rdd2");
	description = flatbuffers_string_create_str(
		&ctx->builder,
		"Length-delimited synapse.log.LogRecord stream with embedded BFBS schemas");
	if (source == 0 || description == 0) {
		return -ENOMEM;
	}

	header = synapse_log_LogFileHeader_create(&ctx->builder, RDD2_LOG_FORMAT_VERSION, 0U,
						 source, description);
	if (header == 0) {
		return -ENOMEM;
	}

	if (synapse_log_LogRecord_create_as_root(
		    &ctx->builder, synapse_log_RecordPayload_as_LogFileHeader(header)) == 0) {
		return -ENOMEM;
	}

	return rdd2_log_write_built_record(ctx);
}

static int rdd2_log_write_schema_record(struct rdd2_log_context *ctx,
					const struct rdd2_log_schema_descriptor *schema)
{
	flatbuffers_string_ref_t name;
	flatbuffers_string_ref_t root_type;
	flatbuffers_string_ref_t file_identifier;
	flatbuffers_string_ref_t fbs_text;
	flatbuffers_string_ref_t fbs_sha256;
	flatbuffers_uint8_vec_ref_t bfbs;
	synapse_log_SchemaRecord_ref_t record;

	if (rdd2_log_builder_reset(ctx) != 0) {
		return -ENOMEM;
	}

	name = flatbuffers_string_create_str(&ctx->builder, schema->name);
	root_type = flatbuffers_string_create_str(&ctx->builder, schema->root_type);
	file_identifier = flatbuffers_string_create_str(&ctx->builder, schema->file_identifier);
	fbs_text = flatbuffers_string_create(&ctx->builder, (const char *)schema->fbs,
					     schema->fbs_len);
	fbs_sha256 = flatbuffers_string_create_str(&ctx->builder, schema->fbs_sha256);
	bfbs = flatbuffers_uint8_vec_create(&ctx->builder, schema->bfbs, schema->bfbs_len);
	if (name == 0 || root_type == 0 || file_identifier == 0 || fbs_text == 0 ||
	    fbs_sha256 == 0 || bfbs == 0) {
		return -ENOMEM;
	}

	record = synapse_log_SchemaRecord_create(&ctx->builder, schema->schema_id, name,
						 root_type, file_identifier, fbs_text,
						 fbs_sha256, bfbs);
	if (record == 0) {
		return -ENOMEM;
	}

	if (synapse_log_LogRecord_create_as_root(
		    &ctx->builder, synapse_log_RecordPayload_as_SchemaRecord(record)) == 0) {
		return -ENOMEM;
	}

	return rdd2_log_write_built_record(ctx);
}

static int rdd2_log_write_topic_record(struct rdd2_log_context *ctx,
				       const struct rdd2_log_topic_descriptor *topic)
{
	flatbuffers_string_ref_t name;
	flatbuffers_string_ref_t encoding;
	synapse_log_TopicRecord_ref_t record;

	if (rdd2_log_builder_reset(ctx) != 0) {
		return -ENOMEM;
	}

	name = flatbuffers_string_create_str(&ctx->builder, topic->name);
	encoding = flatbuffers_string_create_str(&ctx->builder, topic->encoding);
	if (name == 0 || encoding == 0) {
		return -ENOMEM;
	}

	record = synapse_log_TopicRecord_create(&ctx->builder, topic->topic_id, name,
						topic->schema_id, encoding);
	if (record == 0) {
		return -ENOMEM;
	}

	if (synapse_log_LogRecord_create_as_root(
		    &ctx->builder, synapse_log_RecordPayload_as_TopicRecord(record)) == 0) {
		return -ENOMEM;
	}

	return rdd2_log_write_built_record(ctx);
}

static int rdd2_log_write_frame(struct rdd2_log_context *ctx, uint32_t topic_id,
				const uint8_t *payload, size_t payload_len)
{
	flatbuffers_uint8_vec_ref_t payload_vec;
	synapse_log_LogFrame_ref_t frame;

	if (payload == NULL || payload_len == 0U) {
		return -EINVAL;
	}

	if (rdd2_log_builder_reset(ctx) != 0) {
		return -ENOMEM;
	}

	payload_vec = flatbuffers_uint8_vec_create(&ctx->builder, payload, payload_len);
	if (payload_vec == 0) {
		return -ENOMEM;
	}

	frame = synapse_log_LogFrame_create(&ctx->builder, (uint64_t)k_ticks_to_us_floor64(
							    k_uptime_ticks()),
					   topic_id, payload_vec);
	if (frame == 0) {
		return -ENOMEM;
	}

	if (synapse_log_LogRecord_create_as_root(
		    &ctx->builder, synapse_log_RecordPayload_as_LogFrame(frame)) == 0) {
		return -ENOMEM;
	}

	return rdd2_log_write_built_record(ctx);
}

static int rdd2_log_write_preamble(struct rdd2_log_context *ctx)
{
	int rc;

	rc = rdd2_log_write_header(ctx);
	if (rc != 0) {
		return rc;
	}

	for (size_t i = 0U; i < ARRAY_SIZE(g_schema_records); i++) {
		rc = rdd2_log_write_schema_record(ctx, &g_schema_records[i]);
		if (rc != 0) {
			return rc;
		}
	}

	for (size_t i = 0U; i < ARRAY_SIZE(g_topic_records); i++) {
		rc = rdd2_log_write_topic_record(ctx, &g_topic_records[i]);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

static int rdd2_log_find_next_sequence(void)
{
	struct fs_dir_t dir;
	struct fs_dirent entry;
	int max_seq = 0;

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, CONFIG_RDD2_LOG_SDCARD_PATH) != 0) {
		return 1;
	}

	while (fs_readdir(&dir, &entry) == 0 && entry.name[0] != '\0') {
		int seq;

		if (sscanf(entry.name, "rdd2_%d.sylg", &seq) == 1) {
			max_seq = MAX(max_seq, seq);
		}
	}

	fs_closedir(&dir);
	return max_seq + 1;
}

static int rdd2_log_mount(struct rdd2_log_context *ctx)
{
	int rc;
	struct fs_statvfs stat;

	ctx->mount.type = FS_FATFS;
	ctx->mount.fs_data = &ctx->fat_fs;
	ctx->mount.mnt_point = CONFIG_RDD2_LOG_SDCARD_PATH;

	rc = fs_statvfs(CONFIG_RDD2_LOG_SDCARD_PATH, &stat);
	if (rc == 0) {
		return 0;
	}

	rc = fs_mount(&ctx->mount);
	if (rc != 0) {
		LOG_ERR("failed to mount %s: %d", CONFIG_RDD2_LOG_SDCARD_PATH, rc);
		return rc;
	}

	rc = fs_statvfs(CONFIG_RDD2_LOG_SDCARD_PATH, &stat);
	if (rc != 0) {
		LOG_ERR("mounted %s but statvfs failed: %d", CONFIG_RDD2_LOG_SDCARD_PATH, rc);
		(void)fs_unmount(&ctx->mount);
		return rc;
	}

	LOG_INF("mounted %s: %u MiB", CONFIG_RDD2_LOG_SDCARD_PATH,
		(uint32_t)((stat.f_bsize * stat.f_blocks) >> 20));
	return 0;
}

static int rdd2_log_open(struct rdd2_log_context *ctx)
{
	int seq;
	int rc;

	rc = rdd2_log_mount(ctx);
	if (rc != 0) {
		return rc;
	}

	seq = rdd2_log_find_next_sequence();
	snprintk(ctx->path, sizeof(ctx->path), "%s/rdd2_%04d.sylg", CONFIG_RDD2_LOG_SDCARD_PATH,
		 seq);

	fs_file_t_init(&ctx->file);
	rc = fs_open(&ctx->file, ctx->path, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);
	if (rc != 0) {
		LOG_ERR("failed to open %s: %d", ctx->path, rc);
		return rc;
	}

	ctx->bytes_written = 0U;
	ctx->frames_written = 0U;
	ctx->dropped_frames = 0U;
	LOG_INF("opened %s", ctx->path);
	return 0;
}

static int rdd2_log_init_subscriptions(struct rdd2_log_context *ctx)
{
	double rate_hz = (double)CONFIG_RDD2_LOG_SDCARD_TOPIC_RATE_HZ;
	int rc;

	zros_node_init(&ctx->node, "rdd2_log_sdcard");

	rc = zros_sub_init(&ctx->flight_state_sub, &ctx->node, &topic_flight_state,
			   &ctx->flight_state, rate_hz);
	if (rc != 0) {
		return rc;
	}

	rc = zros_sub_init(&ctx->motor_output_sub, &ctx->node, &topic_motor_output,
			   &ctx->motor_output, rate_hz);
	if (rc != 0) {
		zros_sub_fini(&ctx->flight_state_sub);
		return rc;
	}

#if IS_ENABLED(CONFIG_RDD2_LOG_SDCARD_LOG_MOCAP)
	rc = zros_sub_init(&ctx->mocap_frame_sub, &ctx->node, &topic_mocap_frame,
			   &ctx->mocap_frame, rate_hz);
	if (rc != 0) {
		zros_sub_fini(&ctx->motor_output_sub);
		zros_sub_fini(&ctx->flight_state_sub);
		return rc;
	}
#endif

	return 0;
}

static void rdd2_log_fini_subscriptions(struct rdd2_log_context *ctx)
{
#if IS_ENABLED(CONFIG_RDD2_LOG_SDCARD_LOG_MOCAP)
	zros_sub_fini(&ctx->mocap_frame_sub);
#endif
	zros_sub_fini(&ctx->motor_output_sub);
	zros_sub_fini(&ctx->flight_state_sub);
	zros_node_fini(&ctx->node);
}

static void rdd2_log_try_write_frame(struct rdd2_log_context *ctx, uint32_t topic_id,
				     const uint8_t *payload, size_t payload_len)
{
	int rc;

	rc = rdd2_log_write_frame(ctx, topic_id, payload, payload_len);
	if (rc == 0) {
		ctx->frames_written++;
	} else {
		ctx->dropped_frames++;
		LOG_WRN_RATELIMIT("failed to log topic %u: %d", topic_id, rc);
	}
}

static void rdd2_log_poll_topics(struct rdd2_log_context *ctx)
{
	if (zros_sub_update_available(&ctx->flight_state_sub) &&
	    zros_sub_update(&ctx->flight_state_sub) == 0) {
		rdd2_log_try_write_frame(ctx, RDD2_LOG_TOPIC_ID_FLIGHT_STATE,
					 ctx->flight_state, sizeof(ctx->flight_state));
	}

	if (zros_sub_update_available(&ctx->motor_output_sub) &&
	    zros_sub_update(&ctx->motor_output_sub) == 0) {
		rdd2_log_try_write_frame(ctx, RDD2_LOG_TOPIC_ID_MOTOR_OUTPUT,
					 ctx->motor_output, sizeof(ctx->motor_output));
	}

#if IS_ENABLED(CONFIG_RDD2_LOG_SDCARD_LOG_MOCAP)
	if (zros_sub_update_available(&ctx->mocap_frame_sub) &&
	    zros_sub_update(&ctx->mocap_frame_sub) == 0 && ctx->mocap_frame.payload_len > 0U &&
	    ctx->mocap_frame.payload_len <= sizeof(ctx->mocap_frame.payload)) {
		rdd2_log_try_write_frame(ctx, RDD2_LOG_TOPIC_ID_MOCAP_FRAME,
					 ctx->mocap_frame.payload, ctx->mocap_frame.payload_len);
	}
#endif
}

static void rdd2_log_thread(void *p0, void *p1, void *p2)
{
	struct rdd2_log_context *ctx = p0;
	int64_t last_sync_ms;
	int rc;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);

	atomic_set(&ctx->running, 1);

	rc = flatcc_builder_init(&ctx->builder);
	if (rc != 0) {
		LOG_ERR("flatbuffer builder init failed: %d", rc);
		goto out_not_started;
	}

	rc = rdd2_log_open(ctx);
	if (rc != 0) {
		goto out_builder;
	}

	rc = rdd2_log_write_preamble(ctx);
	if (rc != 0) {
		LOG_ERR("failed to write log preamble: %d", rc);
		goto out_file;
	}

	rc = rdd2_log_init_subscriptions(ctx);
	if (rc != 0) {
		LOG_ERR("failed to initialize log subscriptions: %d", rc);
		goto out_file;
	}

	last_sync_ms = k_uptime_get();
	while (atomic_get(&ctx->stop_requested) == 0) {
		rdd2_log_poll_topics(ctx);

		if (k_uptime_get() - last_sync_ms >= RDD2_LOG_SYNC_INTERVAL_MS) {
			(void)fs_sync(&ctx->file);
			last_sync_ms = k_uptime_get();
		}

		k_sleep(K_MSEC(2));
	}

	rdd2_log_fini_subscriptions(ctx);

out_file:
	(void)fs_sync(&ctx->file);
	(void)fs_close(&ctx->file);
	LOG_INF("closed %s: frames=%zu dropped=%zu bytes=%zu", ctx->path, ctx->frames_written,
		ctx->dropped_frames, ctx->bytes_written);
out_builder:
	flatcc_builder_clear(&ctx->builder);
out_not_started:
	atomic_clear(&ctx->running);
	atomic_clear(&ctx->stop_requested);
}

static int rdd2_log_start(void)
{
	k_tid_t tid;

	if (!atomic_cas(&g_ctx.running, 0, 1)) {
		return -EALREADY;
	}

	atomic_clear(&g_ctx.stop_requested);
	tid = k_thread_create(&g_ctx.thread, g_rdd2_log_stack, K_THREAD_STACK_SIZEOF(g_rdd2_log_stack),
			      rdd2_log_thread, &g_ctx, NULL, NULL,
			      CONFIG_RDD2_LOG_SDCARD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(tid, "rdd2_log_sdcard");
	return 0;
}

static int rdd2_log_stop(void)
{
	if (atomic_get(&g_ctx.running) == 0) {
		return -EALREADY;
	}

	atomic_set(&g_ctx.stop_requested, 1);
	return 0;
}

static bool rdd2_log_is_running(void)
{
	return atomic_get(&g_ctx.running) != 0;
}

static int cmd_log_start(const struct shell *sh, size_t argc, char **argv)
{
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rc = rdd2_log_start();
	if (rc == -EALREADY) {
		shell_print(sh, "logger already running");
		return 0;
	}
	if (rc != 0) {
		shell_error(sh, "failed to start logger: %d", rc);
		return rc;
	}

	shell_print(sh, "logger started");
	return 0;
}

static int cmd_log_stop(const struct shell *sh, size_t argc, char **argv)
{
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rc = rdd2_log_stop();
	if (rc == -EALREADY) {
		shell_print(sh, "logger not running");
		return 0;
	}
	if (rc != 0) {
		shell_error(sh, "failed to stop logger: %d", rc);
		return rc;
	}

	shell_print(sh, "logger stopping");
	return 0;
}

static int cmd_log_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "running=%d path=%s frames=%zu dropped=%zu bytes=%zu",
		    rdd2_log_is_running() ? 1 : 0, g_ctx.path[0] == '\0' ? "-" : g_ctx.path,
		    g_ctx.frames_written, g_ctx.dropped_frames, g_ctx.bytes_written);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_rdd2_log, SHELL_CMD(start, NULL, "start logger", cmd_log_start),
			      SHELL_CMD(stop, NULL, "stop logger", cmd_log_stop),
			      SHELL_CMD(status, NULL, "show logger status", cmd_log_status),
			      SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(log_sdcard, &sub_rdd2_log, "RDD2 self-describing FlatBuffer logger", NULL);

static int rdd2_log_autostart(void)
{
	if (IS_ENABLED(CONFIG_RDD2_LOG_SDCARD_AUTOSTART)) {
		return rdd2_log_start();
	}

	return 0;
}

SYS_INIT(rdd2_log_autostart, APPLICATION, 90);
