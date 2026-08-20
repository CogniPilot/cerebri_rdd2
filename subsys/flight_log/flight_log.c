/* SPDX-License-Identifier: Apache-2.0 */

/*
 * SD flight logger pipeline.
 *
 * Two threads and one bounded single-producer single-consumer ring separate
 * the flight bus from the card. The capture thread subscribes to the logged
 * ZROS topics with per-topic rate limits, copies each accepted sample into the
 * ring, and never blocks: when the ring is full it drops the frame and counts
 * it. The writer thread drains the ring into the constant-memory synapse/1
 * MCAP writer, emits the periodic TimeReference and logger-status records,
 * flushes and syncs on a fixed cadence, and owns all card access including the
 * mount, session-file rotation, and start/stop requests.
 */

#include "flight_log.h"

#include "flight_log_fs.h"
#include "mcap_stream.h"

#include "interfaces/synapse_time_status.h"
#include "interfaces/zros_topics.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>

#include <zros/zros_node.h>
#include <zros/zros_sub.h>
#include <zros/zros_topic.h>

#include <synapse/mcap.h>
#include <synapse/mcap_topics.h>
#include <synapse/optical_flow_reader.h>

#if defined(CONFIG_RDD2_SYNAPSE_WIRE)
#include "synapse_wire.h"
#endif

LOG_MODULE_REGISTER(rdd2_flight_log, CONFIG_RDD2_FLIGHT_LOG_LOG_LEVEL);

#define FLIGHT_LOG_MCAP_LIBRARY "rdd2-flight-log/1"
#define FLIGHT_LOG_MCAP_SOURCE "cerebri-rdd2"

/* Largest logged fixed payload: OdometryEstimate at 232 bytes. */
#define FLIGHT_LOG_MAX_PAYLOAD 232U

/* Custom topic ids for the logger-owned records, chosen above the generated
 * catalog range so a decoder never confuses them with a catalog topic. */
#define FLIGHT_LOG_TOPIC_ID_LOGGER_STATUS 200U
#define FLIGHT_LOG_TOPIC_ID_WIRE_STATS 201U

enum log_channel {
	LOG_CH_CONTROL_IMU = 0,
	LOG_CH_PWM,
	LOG_CH_ODOMETRY,
	LOG_CH_ATT_EST,
	LOG_CH_VEHICLE_HEALTH,
	LOG_CH_LOOP_METRICS,
	LOG_CH_ATT_CMD,
	LOG_CH_RATE_CMD,
	LOG_CH_MANUAL,
	LOG_CH_OPTICAL,
	LOG_CH_GNSS,
	LOG_CH_TIMEREF,
	LOG_CH_SELF_STATUS,
	LOG_CH_WIRE_STATS,
	LOG_CH_COUNT,
};

/* Ring frame: fixed header then the raw fixed-layout topic payload. */
struct __packed ring_header {
	uint16_t channel_idx;
	uint16_t reserved;
	uint32_t len;
	uint64_t log_time_ns;
};

/* Logger-owned self-status record, packed little-endian. timestamp_ns leads so
 * the writer reads publish_time from offset 0 uniformly across all channels. */
struct __packed flight_log_self_record {
	uint64_t timestamp_ns;
	uint64_t bytes_written;
	uint32_t dropped_frames;
	uint32_t ring_high_water;
	uint32_t session_index;
	uint32_t flush_errors;
	uint8_t state;
	uint8_t reserved[3];
};

/* Logger-owned direct-wire receiver snapshot, packed little-endian. */
struct __packed flight_log_wire_stream {
	uint32_t received;
	uint32_t accepted;
	uint32_t publish_failed;
	uint32_t socket_errors;
	uint32_t sequence_gaps;
	uint32_t session_changes;
	uint32_t last_sequence;
};

struct __packed flight_log_wire_record {
	uint64_t timestamp_ns;
	struct flight_log_wire_stream optical;
	struct flight_log_wire_stream gnss;
};

struct log_source {
	struct zros_topic *topic;
	uint16_t channel_idx;
	uint16_t payload_size;
	double rate_hz;
};

static struct log_source g_sources[] = {
#if !defined(CONFIG_RDD2_COMMS_STUB)
	{&topic_control_imu, LOG_CH_CONTROL_IMU,
	 sizeof(synapse_topic_InertialSampleData_t), 0.0},
	{&topic_pwm_signal_outputs, LOG_CH_PWM,
	 sizeof(synapse_topic_PwmSignalOutputsData_t), 400.0},
	{&topic_navigation_odometry, LOG_CH_ODOMETRY,
	 sizeof(synapse_topic_OdometryEstimateData_t), 100.0},
	{&topic_attitude_estimate, LOG_CH_ATT_EST,
	 sizeof(synapse_topic_AttitudeEstimateData_t), 200.0},
	{&topic_vehicle_health, LOG_CH_VEHICLE_HEALTH,
	 sizeof(synapse_topic_VehicleHealthData_t), 0.0},
	{&topic_control_loop_metrics, LOG_CH_LOOP_METRICS,
	 sizeof(synapse_topic_ControlLoopMetricsData_t), 0.0},
	{&topic_attitude_command, LOG_CH_ATT_CMD,
	 sizeof(synapse_topic_AttitudeCommandData_t), 0.0},
	{&topic_rate_command, LOG_CH_RATE_CMD,
	 sizeof(synapse_topic_RateCommandData_t), 0.0},
	{&topic_manual_input, LOG_CH_MANUAL,
	 sizeof(synapse_topic_ManualControlData_t), 0.0},
#endif
	{&topic_optical_flow_vel, LOG_CH_OPTICAL,
	 sizeof(synapse_topic_OpticalFlowVelocityData_t), 0.0},
	{&topic_gnss_fix, LOG_CH_GNSS, sizeof(synapse_topic_GnssFixData_t), 0.0},
};

#define LOG_SOURCE_COUNT ARRAY_SIZE(g_sources)

BUILD_ASSERT(sizeof(synapse_topic_OdometryEstimateData_t) <= FLIGHT_LOG_MAX_PAYLOAD);
BUILD_ASSERT(sizeof(struct flight_log_self_record) <= FLIGHT_LOG_MAX_PAYLOAD);
BUILD_ASSERT(sizeof(struct flight_log_wire_record) <= FLIGHT_LOG_MAX_PAYLOAD);

RING_BUF_DECLARE(g_ring, CONFIG_RDD2_FLIGHT_LOG_RING_BYTES);

static struct zros_node g_node;
static struct zros_sub g_subs[LOG_SOURCE_COUNT];
static uint8_t g_sub_data[LOG_SOURCE_COUNT][FLIGHT_LOG_MAX_PAYLOAD];
static bool g_subs_inited;

static struct zros_sub *const *g_subs_ptrs(void);

static struct mcap_stream g_stream;
static synapse_mcap_writer_t g_writer;
static synapse_mcap_channel_t g_channels[LOG_CH_COUNT];
static bool g_session_active;
static uint32_t g_session_index;
static uint32_t g_flush_errors;
static bool g_time_ever_synced;

static atomic_t g_active;      /* session open, capture may enqueue */
static atomic_t g_want_logging = ATOMIC_INIT(1);
static atomic_t g_rotate_request;
static atomic_t g_dropped;
static atomic_t g_ring_high_water;
static atomic_t g_armed;

/* Documented packed-LE layout for the logger-status custom channel. Non-empty
 * schema bytes are required by the writer; the encoding label is fixed to
 * "flatbuffer" by the writer, while these bytes describe the real field
 * layout a decoder reads by offset. */
static const uint8_t g_self_status_schema[] =
	"rdd2.flight_log.LoggerStatus packed-le {"
	"u64 timestamp_ns; u64 bytes_written; u32 dropped_frames; "
	"u32 ring_high_water; u32 session_index; u32 flush_errors; "
	"u8 state; u8 reserved[3];}";

static synapse_mcap_topic_t logger_status_topic(void)
{
	return (synapse_mcap_topic_t){
		.topic_id = FLIGHT_LOG_TOPIC_ID_LOGGER_STATUS,
		.schema_name = "rdd2.flight_log.LoggerStatus",
		.schema_data = g_self_status_schema,
		.schema_size = sizeof(g_self_status_schema),
		.payload_size = sizeof(struct flight_log_self_record),
		.fixed_layout = 1U,
	};
}

#if defined(CONFIG_RDD2_SYNAPSE_WIRE)
static const uint8_t g_wire_stats_schema[] =
	"rdd2.flight_log.WireStats packed-le {"
	"u64 timestamp_ns; "
	"struct optical{u32 received; u32 accepted; u32 publish_failed; "
	"u32 socket_errors; u32 sequence_gaps; u32 session_changes; u32 last_sequence;} "
	"struct gnss{u32 received; u32 accepted; u32 publish_failed; "
	"u32 socket_errors; u32 sequence_gaps; u32 session_changes; u32 last_sequence;}}";

static synapse_mcap_topic_t wire_stats_topic(void)
{
	return (synapse_mcap_topic_t){
		.topic_id = FLIGHT_LOG_TOPIC_ID_WIRE_STATS,
		.schema_name = "rdd2.flight_log.WireStats",
		.schema_data = g_wire_stats_schema,
		.schema_size = sizeof(g_wire_stats_schema),
		.payload_size = sizeof(struct flight_log_wire_record),
		.fixed_layout = 1U,
	};
}
#endif

static uint64_t read_le64(const uint8_t *p)
{
	uint64_t v = 0U;

	for (unsigned int i = 0U; i < 8U; ++i) {
		v |= (uint64_t)p[i] << (8U * i);
	}
	return v;
}

static int register_channel(synapse_mcap_topic_t topic, const char *key, int idx)
{
	return synapse_mcap_add_topic(&g_writer, &topic, key, &g_channels[idx]);
}

static int register_channels(void)
{
	int rc = 0;

#if !defined(CONFIG_RDD2_COMMS_STUB)
	rc |= register_channel(SYNAPSE_MCAP_TOPIC_InertialSample, "control_imu",
			       LOG_CH_CONTROL_IMU);
	rc |= register_channel(SYNAPSE_MCAP_TOPIC_PwmSignalOutputs, "pwm_signal_outputs",
			       LOG_CH_PWM);
	rc |= register_channel(SYNAPSE_MCAP_TOPIC_OdometryEstimate, "navigation_odometry",
			       LOG_CH_ODOMETRY);
	rc |= register_channel(SYNAPSE_MCAP_TOPIC_AttitudeEstimate, "attitude_estimate",
			       LOG_CH_ATT_EST);
	rc |= register_channel(SYNAPSE_MCAP_TOPIC_VehicleHealth, "vehicle_health",
			       LOG_CH_VEHICLE_HEALTH);
	rc |= register_channel(SYNAPSE_MCAP_TOPIC_ControlLoopMetrics, "control_loop_metrics",
			       LOG_CH_LOOP_METRICS);
	rc |= register_channel(SYNAPSE_MCAP_TOPIC_AttitudeCommand, "attitude_command",
			       LOG_CH_ATT_CMD);
	rc |= register_channel(SYNAPSE_MCAP_TOPIC_RateCommand, "rate_command",
			       LOG_CH_RATE_CMD);
	rc |= register_channel(SYNAPSE_MCAP_TOPIC_ManualControlCommand, "manual_input",
			       LOG_CH_MANUAL);
#endif
	rc |= register_channel(SYNAPSE_MCAP_TOPIC_OpticalFlowVelocity, "optical_flow_vel",
			       LOG_CH_OPTICAL);
	rc |= register_channel(SYNAPSE_MCAP_TOPIC_GnssFix, "gnss_fix", LOG_CH_GNSS);
	rc |= register_channel(SYNAPSE_MCAP_TOPIC_TimeReference, "time_reference",
			       LOG_CH_TIMEREF);
	rc |= register_channel(logger_status_topic(), "logger_status", LOG_CH_SELF_STATUS);
#if defined(CONFIG_RDD2_SYNAPSE_WIRE)
	rc |= register_channel(wire_stats_topic(), "wire_stats", LOG_CH_WIRE_STATS);
#endif

	return rc == SYNAPSE_MCAP_OK ? 0 : -EIO;
}

/* ---- capture thread ---- */

static void ensure_subs_inited(void)
{
	if (g_subs_inited) {
		return;
	}

	zros_node_init(&g_node, "flight_log");
	for (size_t i = 0U; i < LOG_SOURCE_COUNT; ++i) {
		(void)zros_sub_init(&g_subs[i], &g_node, g_sources[i].topic,
				    g_sub_data[i], g_sources[i].rate_hz);
	}
	g_subs_inited = true;
}

static void enqueue_sample(size_t source_index, uint64_t boot_ns)
{
	const struct log_source *src = &g_sources[source_index];
	uint16_t idx = src->channel_idx;
	uint16_t len = src->payload_size;
	uint32_t total = (uint32_t)sizeof(struct ring_header) + len;
	uint8_t frame[sizeof(struct ring_header) + FLIGHT_LOG_MAX_PAYLOAD];
	struct ring_header header = {
		.channel_idx = idx,
		.reserved = 0U,
		.len = len,
		.log_time_ns = boot_ns,
	};
	uint32_t used;

	if (idx == LOG_CH_VEHICLE_HEALTH) {
		const synapse_topic_VehicleHealthData_t *vh =
			(const synapse_topic_VehicleHealthData_t *)g_sub_data[source_index];

		atomic_set(&g_armed,
			   (vh->flags & synapse_topic_VehicleHealthFlags_Armed) ? 1 : 0);
	}

#if defined(CONFIG_RDD2_FLIGHT_LOG_ARM_GATED_HIGH_RATE)
	if ((idx == LOG_CH_CONTROL_IMU || idx == LOG_CH_PWM) &&
	    atomic_get(&g_armed) == 0) {
		return;
	}
#endif

	if (ring_buf_space_get(&g_ring) < total) {
		atomic_inc(&g_dropped);
		return;
	}

	memcpy(frame, &header, sizeof(header));
	memcpy(frame + sizeof(header), g_sub_data[source_index], len);
	(void)ring_buf_put(&g_ring, frame, total);

	used = ring_buf_size_get(&g_ring);
	if ((atomic_val_t)used > atomic_get(&g_ring_high_water)) {
		atomic_set(&g_ring_high_water, (atomic_val_t)used);
	}
}

static void capture_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (true) {
		if (atomic_get(&g_active) == 0) {
			k_sleep(K_MSEC(100));
			continue;
		}

		ensure_subs_inited();

		if (zros_sub_wait_many(g_subs_ptrs(), LOG_SOURCE_COUNT, K_MSEC(100)) != 0) {
			continue;
		}

		for (size_t i = 0U; i < LOG_SOURCE_COUNT; ++i) {
			if (!zros_sub_update_available(&g_subs[i])) {
				continue;
			}
			(void)zros_sub_update(&g_subs[i]);
			enqueue_sample(i, synapse_time_boot_ns());
		}
	}
}

/* ---- writer thread ---- */

static void drain_ring(void)
{
	uint8_t scratch[FLIGHT_LOG_MAX_PAYLOAD];

	while (ring_buf_size_get(&g_ring) >= sizeof(struct ring_header)) {
		struct ring_header header;
		uint32_t got;
		uint64_t publish_time_ns;

		got = ring_buf_get(&g_ring, (uint8_t *)&header, sizeof(header));
		if (got != sizeof(header)) {
			break;
		}
		if (header.len > sizeof(scratch) || header.channel_idx >= LOG_CH_COUNT) {
			/* Framing invariant broken: stop feeding the writer. */
			g_stream.write_failed = true;
			return;
		}
		got = ring_buf_get(&g_ring, scratch, header.len);
		if (got != header.len) {
			break;
		}
		if (g_channels[header.channel_idx].fixed_layout == 0U) {
			continue;
		}

		publish_time_ns = read_le64(scratch);
		(void)synapse_mcap_write_fixed(&g_writer, &g_channels[header.channel_idx],
					       header.log_time_ns, publish_time_ns, scratch,
					       header.len);
	}
}

static void emit_time_reference(uint64_t now_ns)
{
	int64_t offset_ns = 0;
	synapse_types_TimeStatus_enum_t status =
		synapse_time_status_resolve(&g_time_ever_synced, &offset_ns);
	bool on_domain = (status != synapse_types_TimeStatus_LocalFreerun);
	synapse_topic_TimeReferenceData_t tr = {0};

	tr.timestamp_ns = now_ns;
	tr.time_unix_ns = on_domain ? synapse_time_apply_offset(now_ns, offset_ns) : 0U;
	tr.time_status = status;

	(void)synapse_mcap_write_fixed(&g_writer, &g_channels[LOG_CH_TIMEREF], now_ns,
				       tr.timestamp_ns, &tr, sizeof(tr));
}

static void emit_self_status(uint64_t now_ns)
{
	struct flight_log_self_record record = {0};

	record.timestamp_ns = now_ns;
	record.bytes_written = g_stream.bytes_written;
	record.dropped_frames = (uint32_t)atomic_get(&g_dropped);
	record.ring_high_water = (uint32_t)atomic_get(&g_ring_high_water);
	record.session_index = g_session_index;
	record.flush_errors = g_flush_errors;
	record.state = (uint8_t)(g_session_active ? 1U : 0U);

	(void)synapse_mcap_write_fixed(&g_writer, &g_channels[LOG_CH_SELF_STATUS], now_ns,
				       record.timestamp_ns, &record, sizeof(record));
}

#if defined(CONFIG_RDD2_SYNAPSE_WIRE)
static void emit_wire_stats(uint64_t now_ns)
{
	struct rdd2_synapse_wire_snapshot snap;
	struct flight_log_wire_record record = {0};

	rdd2_synapse_wire_stats_snapshot(&snap);

	record.timestamp_ns = now_ns;
	record.optical.received = snap.optical.received;
	record.optical.accepted = snap.optical.accepted;
	record.optical.publish_failed = snap.optical.publish_failed;
	record.optical.socket_errors = snap.optical.socket_errors;
	record.optical.sequence_gaps = snap.optical.sequence_gaps;
	record.optical.session_changes = snap.optical.session_changes;
	record.optical.last_sequence = snap.optical.last_sequence;
	record.gnss.received = snap.gnss.received;
	record.gnss.accepted = snap.gnss.accepted;
	record.gnss.publish_failed = snap.gnss.publish_failed;
	record.gnss.socket_errors = snap.gnss.socket_errors;
	record.gnss.sequence_gaps = snap.gnss.sequence_gaps;
	record.gnss.session_changes = snap.gnss.session_changes;
	record.gnss.last_sequence = snap.gnss.last_sequence;

	(void)synapse_mcap_write_fixed(&g_writer, &g_channels[LOG_CH_WIRE_STATS], now_ns,
				       record.timestamp_ns, &record, sizeof(record));
}
#endif

static int start_session(void)
{
	uint32_t index = 0U;
	char path[64];
	char session_id[RDD2_FLIGHT_LOG_SESSION_ID_LEN];
	int rc;

	if (!rdd2_flight_log_fs_mounted()) {
		rc = rdd2_flight_log_fs_mount();
		if (rc != 0) {
			return rc;
		}
	}

	rc = rdd2_flight_log_fs_next_index(&index);
	if (rc != 0) {
		return rc;
	}
	rc = rdd2_flight_log_fs_session_path(index, path, sizeof(path));
	if (rc < 0) {
		return rc;
	}

	memset(&g_stream, 0, sizeof(g_stream));
	rc = mcap_stream_open_file(&g_stream, path);
	if (rc != 0) {
		LOG_WRN("open %s failed: %d", path, rc);
		return rc;
	}

	mcap_stream_session_id(session_id);
	memset(g_channels, 0, sizeof(g_channels));
	rc = synapse_mcap_open(&g_writer, mcap_stream_sink(&g_stream), NULL, 0U,
			       FLIGHT_LOG_MCAP_LIBRARY, session_id, FLIGHT_LOG_MCAP_SOURCE,
			       SYNAPSE_MCAP_TIME_CORRELATED);
	if (rc != SYNAPSE_MCAP_OK) {
		(void)mcap_stream_close_file(&g_stream);
		return -EIO;
	}

	rc = register_channels();
	if (rc != 0) {
		(void)synapse_mcap_close(&g_writer);
		(void)mcap_stream_close_file(&g_stream);
		return rc;
	}

	g_session_index = index;
	g_session_active = true;
	atomic_set(&g_active, 1);
	LOG_INF("logging session %s", path);
	return 0;
}

static void stop_session(void)
{
	if (!g_session_active) {
		return;
	}

	atomic_set(&g_active, 0);
	drain_ring();
	(void)synapse_mcap_close(&g_writer);
	(void)mcap_stream_close_file(&g_stream);
	g_session_active = false;
	LOG_INF("logging session closed (%llu bytes)",
		(unsigned long long)g_stream.bytes_written);
}

static void writer_thread(void *a, void *b, void *c)
{
	uint64_t last_timeref = 0U;
	uint64_t last_self = 0U;
	uint64_t last_flush = 0U;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (true) {
		uint64_t now;

		if (atomic_get(&g_want_logging) == 0) {
			if (g_session_active) {
				stop_session();
			}
			k_sleep(K_MSEC(CONFIG_RDD2_FLIGHT_LOG_RETRY_MS));
			continue;
		}

		if (!g_session_active) {
			if (start_session() != 0) {
				k_sleep(K_MSEC(CONFIG_RDD2_FLIGHT_LOG_RETRY_MS));
				continue;
			}
			now = synapse_time_boot_ns();
			last_timeref = now;
			last_self = now;
			last_flush = now;
		}

		if (atomic_cas(&g_rotate_request, 1, 0)) {
			stop_session();
			if (start_session() != 0) {
				k_sleep(K_MSEC(CONFIG_RDD2_FLIGHT_LOG_RETRY_MS));
				continue;
			}
		}

		now = synapse_time_boot_ns();
		drain_ring();

		if (now - last_timeref >=
		    (uint64_t)CONFIG_RDD2_FLIGHT_LOG_TIMEREF_PERIOD_MS * 1000000ULL) {
			emit_time_reference(now);
			last_timeref = now;
		}
		if (now - last_self >= 1000000000ULL) {
			emit_self_status(now);
#if defined(CONFIG_RDD2_SYNAPSE_WIRE)
			emit_wire_stats(now);
#endif
			last_self = now;
		}
		if (now - last_flush >=
		    (uint64_t)CONFIG_RDD2_FLIGHT_LOG_FLUSH_PERIOD_MS * 1000000ULL) {
			if (synapse_mcap_flush(&g_writer) != SYNAPSE_MCAP_OK) {
				g_flush_errors++;
			}
			last_flush = now;
		}

		if (g_stream.write_failed || synapse_mcap_error(&g_writer) != SYNAPSE_MCAP_OK) {
			LOG_WRN("writer error, closing session");
			stop_session();
			k_sleep(K_MSEC(CONFIG_RDD2_FLIGHT_LOG_RETRY_MS));
			continue;
		}

		if (g_stream.bytes_written >= (uint64_t)CONFIG_RDD2_FLIGHT_LOG_ROTATE_BYTES) {
			atomic_set(&g_rotate_request, 1);
		}

		k_sleep(K_MSEC(20));
	}
}

/* ---- public API ---- */

bool rdd2_flight_log_healthy(void)
{
	return atomic_get(&g_active) != 0 && g_stream.file_open && g_stream.last_sync_ok;
}

void rdd2_flight_log_status_get(struct rdd2_flight_log_status *out)
{
	if (out == NULL) {
		return;
	}

	out->mounted = rdd2_flight_log_fs_mounted();
	out->active = g_session_active;
	out->last_sync_ok = g_stream.last_sync_ok;
	out->want_logging = atomic_get(&g_want_logging) != 0;
	out->session_index = g_session_index;
	out->bytes_written = g_stream.bytes_written;
	out->dropped_frames = (uint32_t)atomic_get(&g_dropped);
	out->ring_high_water = (uint32_t)atomic_get(&g_ring_high_water);
	out->flush_errors = g_flush_errors;
}

void rdd2_flight_log_request_start(void)
{
	atomic_set(&g_want_logging, 1);
}

void rdd2_flight_log_request_stop(void)
{
	atomic_set(&g_want_logging, 0);
}

void rdd2_flight_log_request_rotate(void)
{
	atomic_set(&g_rotate_request, 1);
}

/* Pointer table for zros_sub_wait_many, filled once on first use. */
static struct zros_sub *g_sub_ptr_storage[LOG_SOURCE_COUNT];

static struct zros_sub *const *g_subs_ptrs(void)
{
	for (size_t i = 0U; i < LOG_SOURCE_COUNT; ++i) {
		g_sub_ptr_storage[i] = &g_subs[i];
	}
	return g_sub_ptr_storage;
}

K_THREAD_DEFINE(flight_log_writer, CONFIG_RDD2_FLIGHT_LOG_WRITER_STACK_SIZE, writer_thread,
		NULL, NULL, NULL, CONFIG_RDD2_FLIGHT_LOG_WRITER_PRIORITY, 0, 1000);
K_THREAD_DEFINE(flight_log_capture, CONFIG_RDD2_FLIGHT_LOG_FRONT_STACK_SIZE, capture_thread,
		NULL, NULL, NULL, CONFIG_RDD2_FLIGHT_LOG_FRONT_PRIORITY, 0, 1000);
