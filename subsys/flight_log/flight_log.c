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
 *
 * Each session file is preallocated to a contiguous extent at open, so the
 * steady-state writes fill reserved clusters in place and update no FAT
 * allocation table. That removes the periodic cluster-growth writes that a card
 * pulled mid-flight could otherwise corrupt, leaving only the short unsynced
 * data tail as the residual removal risk. Close truncates the unused tail back
 * to the streamed byte count so the reservation frees cleanly.
 *
 * To keep the open-time reservation out of a mid-flight rotation, the extents
 * are built ahead of time and kept standing on the card as a pool of
 * reservations, reserve00.pre and up, together about RESERVE_BYTES of pre-built
 * and pre-erased space. While a session streams, one flush cycle at a time, the
 * writer grows the reservation of the first free slot to the rotation size in
 * small steps, so the pool refills in the background one reservation at a time.
 * A rotation and a boot both take the lowest ready reservation and rename it
 * into the next session index, a directory-entry-only operation that inherits
 * the pre-built cluster chain, so the swap writes no allocation table and does
 * not stall the writer. The pool survives stop and power cycles, so only a card
 * that has never recorded reserves inline, and that burst lands before arming
 * and is harmless. If no reservation is ready, the inline path runs as a
 * fallback.
 *
 * Everything the open path needs about the card comes from one directory pass
 * (rdd2_flight_log_fs_scan): the next session index, the ready reservations and
 * the slot to build in. A boot open is therefore one directory read, one rename,
 * and one file open.
 */

#include "flight_log.h"

#include "flight_log_fs.h"
#include "mcap_stream.h"

#include "interfaces/synapse_time_status.h"
#include "interfaces/zros_topics.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/fs/fs.h>
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

/*
 * Session-extent preallocation guard rails. The session file is expanded to a
 * contiguous extent at open so the streaming writes touch no FAT allocation
 * metadata, which collapses the surprise-removal corruption window. Keep
 * FLIGHT_LOG_PREALLOC_MARGIN_BYTES of free space in reserve so the expand never
 * consumes the last clusters the directory entry, FSINFO, and the close-time
 * truncate still need. Below FLIGHT_LOG_PREALLOC_FLOOR_BYTES a reservation is
 * not worth attempting, so the session grows on write instead.
 */
#define FLIGHT_LOG_PREALLOC_MARGIN_BYTES (1U << 20) /* 1 MiB */
#ifndef FLIGHT_LOG_PREALLOC_FLOOR_BYTES
#define FLIGHT_LOG_PREALLOC_FLOOR_BYTES (4U << 20)  /* 4 MiB */
#endif

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
	LOG_CH_OPTICAL_RAW,
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
	uint32_t last_sync_ms;
	uint32_t max_sync_ms;
};

/* Logger-owned direct-wire receiver snapshot, packed little-endian. Carries the
 * full per-stream snapshot the accessor copies out, including the source session
 * id and the last observed header flags and receiver time status. */
struct __packed flight_log_wire_stream {
	uint32_t received;
	uint32_t accepted;
	uint32_t publish_failed;
	uint32_t socket_errors;
	uint32_t sequence_gaps;
	uint32_t session_changes;
	uint32_t last_sequence;
	uint64_t session_id;
	uint16_t last_header_flags;
	uint8_t last_receiver_time_status;
	uint8_t reserved;
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
	/* Raw integrated flow product for the tightly coupled path. Capped at 50 Hz,
	 * comfortably above the flow frame rate, so every window is logged without
	 * letting a runaway producer flood the ring. */
	{&topic_optical_flow, LOG_CH_OPTICAL_RAW,
	 sizeof(synapse_topic_OpticalFlowData_t), 50.0},
	{&topic_gnss_fix, LOG_CH_GNSS, sizeof(synapse_topic_GnssFixData_t), 0.0},
};

#define LOG_SOURCE_COUNT ARRAY_SIZE(g_sources)

/* Every logged payload is copied into g_sub_data[][FLIGHT_LOG_MAX_PAYLOAD] and
 * into the ring frame, and the writer takes each frame's publish time from the
 * first 8 bytes of its payload, so every logged type has to fit that bound and
 * lead with timestamp_ns. Checked here for each type the table and the
 * logger-owned records carry. */
#define FLIGHT_LOG_ASSERT_PAYLOAD(type)                                                            \
	BUILD_ASSERT(sizeof(type) <= FLIGHT_LOG_MAX_PAYLOAD);                                      \
	BUILD_ASSERT(offsetof(type, timestamp_ns) == 0U)

FLIGHT_LOG_ASSERT_PAYLOAD(synapse_topic_InertialSampleData_t);
FLIGHT_LOG_ASSERT_PAYLOAD(synapse_topic_PwmSignalOutputsData_t);
FLIGHT_LOG_ASSERT_PAYLOAD(synapse_topic_OdometryEstimateData_t);
FLIGHT_LOG_ASSERT_PAYLOAD(synapse_topic_AttitudeEstimateData_t);
FLIGHT_LOG_ASSERT_PAYLOAD(synapse_topic_VehicleHealthData_t);
FLIGHT_LOG_ASSERT_PAYLOAD(synapse_topic_ControlLoopMetricsData_t);
FLIGHT_LOG_ASSERT_PAYLOAD(synapse_topic_AttitudeCommandData_t);
FLIGHT_LOG_ASSERT_PAYLOAD(synapse_topic_RateCommandData_t);
FLIGHT_LOG_ASSERT_PAYLOAD(synapse_topic_ManualControlData_t);
FLIGHT_LOG_ASSERT_PAYLOAD(synapse_topic_OpticalFlowVelocityData_t);
FLIGHT_LOG_ASSERT_PAYLOAD(synapse_topic_OpticalFlowData_t);
FLIGHT_LOG_ASSERT_PAYLOAD(synapse_topic_GnssFixData_t);
FLIGHT_LOG_ASSERT_PAYLOAD(struct flight_log_self_record);
FLIGHT_LOG_ASSERT_PAYLOAD(struct flight_log_wire_record);

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
 * schema bytes are required by the writer. The encoding label is fixed to
 * "flatbuffer" by the writer, while these bytes describe the real field
 * layout a decoder reads by offset. */
static const uint8_t g_self_status_schema[] =
	"rdd2.flight_log.LoggerStatus packed-le {"
	"u64 timestamp_ns; u64 bytes_written; u32 dropped_frames; "
	"u32 ring_high_water; u32 session_index; u32 flush_errors; "
	"u8 state; u8 reserved[3]; u32 last_sync_ms; u32 max_sync_ms;}";

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
	"u32 socket_errors; u32 sequence_gaps; u32 session_changes; u32 last_sequence; "
	"u64 session_id; u16 last_header_flags; u8 last_receiver_time_status; u8 reserved;} "
	"struct gnss{u32 received; u32 accepted; u32 publish_failed; "
	"u32 socket_errors; u32 sequence_gaps; u32 session_changes; u32 last_sequence; "
	"u64 session_id; u16 last_header_flags; u8 last_receiver_time_status; u8 reserved;}}";

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
	rc |= register_channel(SYNAPSE_MCAP_TOPIC_OpticalFlow, "optical_flow",
			       LOG_CH_OPTICAL_RAW);
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
	record.last_sync_ms = g_stream.last_sync_ms;
	record.max_sync_ms = g_stream.max_sync_ms;

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
	record.optical.session_id = snap.optical.session_id;
	record.optical.last_header_flags = snap.optical.last_header_flags;
	record.optical.last_receiver_time_status = snap.optical.last_receiver_time_status;
	record.gnss.received = snap.gnss.received;
	record.gnss.accepted = snap.gnss.accepted;
	record.gnss.publish_failed = snap.gnss.publish_failed;
	record.gnss.socket_errors = snap.gnss.socket_errors;
	record.gnss.sequence_gaps = snap.gnss.sequence_gaps;
	record.gnss.session_changes = snap.gnss.session_changes;
	record.gnss.last_sequence = snap.gnss.last_sequence;
	record.gnss.session_id = snap.gnss.session_id;
	record.gnss.last_header_flags = snap.gnss.last_header_flags;
	record.gnss.last_receiver_time_status = snap.gnss.last_receiver_time_status;

	(void)synapse_mcap_write_fixed(&g_writer, &g_channels[LOG_CH_WIRE_STATS], now_ns,
				       record.timestamp_ns, &record, sizeof(record));
}
#endif

/* Latched so the exhausted-index refusal is logged once, not on every retry.
 * Cleared once a session opens (the operator archived or pruned old files). */
static bool g_index_exhausted;

/*
 * The one reservation build in progress. Whether a reservation is ready is not a
 * state: it is a full-size file standing in a pool slot, which the directory
 * scan reports. Only the build has state.
 *
 *   IDLE     -> nothing being built; the next flush cycle starts one whenever
 *               the pool is short of RDD2_FLIGHT_LOG_RESERVE_SLOTS.
 *   BUILDING -> growing one step per flush cycle, the slot file open.
 *   GIVEUP   -> the card cannot hold another full reservation, so rotation
 *               takes the inline expand path. Re-evaluated at the next session
 *               open, which resets this to IDLE.
 */
enum reservation_state {
	RESERVATION_IDLE = 0,
	RESERVATION_BUILDING,
	RESERVATION_GIVEUP,
};

static enum reservation_state g_reservation_state;
static struct prealloc_reservation g_reservation;
static uint32_t g_reservation_slot; /* pool slot the build in progress owns */

/*
 * Mount generation whose contiguous reservation came back -ENOSPC, or 0 when
 * nothing is latched. The f_expand behind it reads the whole FAT looking for a
 * free run that large, which on a fragmented card is tens of seconds under the
 * card lock, and the answer cannot change until the card does: nothing is freed
 * while the logger runs. So it is asked once per mounted volume and every later
 * session on that same volume goes straight to grow-on-write. Keying it on the
 * generation rather than a bool means any remount is a new card as far as the
 * reservation is concerned, including an sd unmount/mount, a reinsert, and the
 * remount a bench `sd format` does inside the fs layer.
 */
static uint32_t g_prealloc_unavailable_gen;

/*
 * Free bytes on the mounted volume, false when the volume cannot answer. Both
 * reservation paths size themselves against this. Runs with the card lock held.
 */
static bool free_bytes(uint64_t *out)
{
	struct fs_statvfs vfs;

	if (fs_statvfs(RDD2_FLIGHT_LOG_MOUNT_POINT, &vfs) != 0) {
		return false;
	}
	*out = (uint64_t)vfs.f_bfree * (uint64_t)vfs.f_frsize;
	return true;
}

/*
 * Reserve a contiguous extent for the freshly opened session file so the
 * streaming writes fill it in place and never grow the FAT. Runs with the card
 * lock held, immediately after the file is opened and before any bytes are
 * written. Best-effort: any failure leaves the session on grow-on-write, which
 * is exactly today's behavior, so preallocation is never a gate on logging.
 *
 * The reservation size is the rotation size when the card has room for it. When
 * free space is tighter than that, shrink the reservation to what remains above
 * the safety margin, then clamp it to the largest contiguous free run the card
 * actually has, so a nearly full or fragmented card still gets a contiguous run
 * rather than falling all the way back to grow-on-write. Below the floor, skip
 * it.
 *
 * A reservation that succeeds is erased before the first write; one that finds
 * no contiguous run latches this mount's generation so the scan is not repeated
 * until the volume is remounted. path names the session file for those log lines.
 */
static void preallocate_session(const char *path)
{
	uint64_t target = (uint64_t)CONFIG_RDD2_FLIGHT_LOG_ROTATE_BYTES;
	uint64_t free_now;
	uint64_t largest_run;
	int rc;

	if (g_prealloc_unavailable_gen == rdd2_flight_log_fs_mount_generation()) {
		return;
	}

	if (free_bytes(&free_now)) {
		target = MIN(target, free_now > FLIGHT_LOG_PREALLOC_MARGIN_BYTES
					     ? free_now - FLIGHT_LOG_PREALLOC_MARGIN_BYTES
					     : 0U);
	}

	/* Free space is not the same as a free run. A grow-on-write session
	 * interleaves its clusters with the pool's growth steps, so a card that
	 * has run one has a free total far larger than anything contiguous left in
	 * it. Sizing the reservation to the longest run the card really has keeps
	 * the session on a contiguous extent instead of letting f_expand scan for a
	 * size it can never satisfy and fall back to grow-on-write, which would
	 * fragment the card further on every session. */
	if (rdd2_flight_log_fs_largest_free_run(&largest_run) == 0 && largest_run < target) {
		LOG_INF("largest contiguous free run is %llu MiB, reserving that",
			(unsigned long long)(largest_run >> 20));
		target = largest_run;
	}

	if (target < FLIGHT_LOG_PREALLOC_FLOOR_BYTES) {
		LOG_WRN("card too full to preallocate, growing session on write");
		return;
	}

	rc = mcap_stream_preallocate(&g_stream, target);
	if (rc == 0) {
		LOG_INF("preallocated %llu bytes for the session file",
			(unsigned long long)target);
		/* Erase the reserved extent before the first byte is written, so the
		 * session streams into known-erased blocks instead of making the card
		 * controller erase and collect garbage under the writer. */
		(void)rdd2_flight_log_fs_trim_file(&g_stream.file, path);
	} else {
		LOG_WRN("preallocation unavailable (%d), growing session on write", rc);
		/* No contiguous run that large exists on this card. Do not pay for
		 * that whole-FAT scan again on every retry and every later session:
		 * the card would have to change for the answer to. */
		if (rc == -ENOSPC) {
			g_prealloc_unavailable_gen = rdd2_flight_log_fs_mount_generation();
		}
	}
}

/*
 * Compose the path of the next session file from an already taken directory
 * scan. Returns 0 with path filled, -ERANGE when the index space is exhausted,
 * or -ENAMETOOLONG.
 */
static int resolve_next_index(const struct rdd2_flight_log_scan *scan, char *path, size_t cap)
{
	uint32_t index = scan->next_index;
	int rc;

	if (index > RDD2_FLIGHT_LOG_MAX_SESSION_INDEX) {
		if (!g_index_exhausted) {
			LOG_ERR("session index space exhausted at flight%04u, "
				"archive existing files to resume logging",
				RDD2_FLIGHT_LOG_MAX_SESSION_INDEX);
			g_index_exhausted = true;
		}
		return -ERANGE;
	}
	g_index_exhausted = false;

	rc = rdd2_flight_log_fs_session_path(index, path, cap);
	return rc < 0 ? rc : 0;
}

/*
 * Attach the MCAP writer to the already-open g_stream and register channels.
 * Shared tail of the inline-open and rename paths. Runs with the card
 * lock held. Returns 0 on success or a negative errno, closing the stream on
 * failure so the caller does not have to.
 */
static int finish_open(uint32_t index, const char *path)
{
	char session_id[RDD2_FLIGHT_LOG_SESSION_ID_LEN];
	int rc;

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
	LOG_INF("logging session %s", path);
	return 0;
}

/*
 * Open the next session file with an inline reservation. Runs with the card lock
 * held. Used for the first session on a card that carries no reservation and as
 * the rotation fallback when none is ready. Does not touch g_active or the
 * session-active flag. Returns 0 on success or a negative errno.
 */
static int open_session_file(const struct rdd2_flight_log_scan *scan)
{
	char path[64];
	int rc;

	rc = resolve_next_index(scan, path, sizeof(path));
	if (rc != 0) {
		return rc;
	}

	memset(&g_stream, 0, sizeof(g_stream));
	rc = mcap_stream_open_file(&g_stream, path);
	if (rc != 0) {
		LOG_WRN("open %s failed: %d", path, rc);
		(void)rdd2_flight_log_fs_unmount();
		return rc;
	}

	/* Reserve the session extent while the file is still empty, before the MCAP
	 * header write. This is the only point at which the underlying f_expand is
	 * accepted, so it must precede synapse_mcap_open. */
	preallocate_session(path);

	rc = finish_open(scan->next_index, path);
	if (rc != 0) {
		/* finish_open closed the stream, so remove the zero-byte file it
		 * leaves behind: otherwise a retry burns the next index every time,
		 * which on a full card fills the directory with empty sessions. */
		(void)fs_unlink(path);
	}
	return rc;
}

/* One standing reservation is always the full rotation size. Unlike the inline
 * path it is not shrunk to fit a tight card: a card that cannot hold a live
 * session plus another reservation simply stops building and rotation falls back
 * to the inline expand. */
static uint64_t reservation_target(void)
{
	return (uint64_t)CONFIG_RDD2_FLIGHT_LOG_ROTATE_BYTES;
}

static int reservation_path(uint32_t slot, char *out, size_t cap)
{
	int written = snprintk(out, cap, "%s/" RDD2_FLIGHT_LOG_RESERVE_PATTERN,
			       RDD2_FLIGHT_LOG_MOUNT_POINT, (unsigned int)slot);

	if (written < 0 || (size_t)written >= cap) {
		return -ENAMETOOLONG;
	}
	return 0;
}

/*
 * Open the next session by renaming the lowest ready reservation of the scan
 * into the next index. Runs with the card lock held. The rename is a
 * directory-entry-only FatFs operation, so the pre-built cluster chain carries
 * into the new session untouched and no f_expand runs. The file is reopened
 * without truncation and streamed from offset 0, overwriting the reserved
 * clusters in place. Returns 0 on success or a negative errno; on failure any
 * renamed target is removed so the inline fallback reuses the index.
 */
static int open_from_reservation(const struct rdd2_flight_log_scan *scan)
{
	char path[64];
	char reserve[64];
	int rc;

	rc = resolve_next_index(scan, path, sizeof(path));
	if (rc != 0) {
		return rc;
	}

	rc = reservation_path((uint32_t)scan->ready_slot, reserve, sizeof(reserve));
	if (rc != 0) {
		return rc;
	}

	rc = fs_rename(reserve, path);
	if (rc != 0) {
		return rc;
	}

	memset(&g_stream, 0, sizeof(g_stream));
	rc = mcap_stream_open_existing(&g_stream, path, reservation_target());
	if (rc != 0) {
		/* The rename already consumed the reservation into this name; if it
		 * cannot be reopened, remove it so no orphaned full-size file is left
		 * and the inline fallback reuses this same index. */
		(void)fs_unlink(path);
		return rc;
	}

	rc = finish_open(scan->next_index, path);
	if (rc != 0) {
		(void)fs_unlink(path);
	}
	return rc;
}

/* Close the build in progress and remove its partial file, so no dangling
 * half-reservation is left. A completed reservation is a file in another slot
 * and is never touched. No-op when nothing is being built; does not change
 * g_reservation_state. */
static void reservation_discard(void)
{
	char path[64];

	if (!g_reservation.open) {
		return;
	}
	mcap_stream_reservation_close(&g_reservation);
	if (reservation_path(g_reservation_slot, path, sizeof(path)) == 0) {
		(void)fs_unlink(path);
	}
}

/*
 * Start building the next standing reservation when the pool is short of one.
 * The directory scan counts what stands on the card, so the pool refills after
 * every consumption rather than once per session: after four sessions have
 * consumed four reservations, twelve remain and the writer builds back toward
 * sixteen, one reservation at a time in the background, whenever free space
 * allows, so the next boot always finds a ready reservation and opens by rename.
 * Runs with the card lock held.
 */
static void reservation_begin(void)
{
	struct rdd2_flight_log_scan scan;
	char path[64];
	uint64_t target = reservation_target();
	uint64_t free_now;
	int rc;

	if (rdd2_flight_log_fs_scan(&scan) != 0 || scan.free_slot < 0) {
		/* Card unreadable, or every slot already holds a reservation: the pool
		 * is full and the builder waits for one to be consumed. */
		return;
	}

	if (reservation_path((uint32_t)scan.free_slot, path, sizeof(path)) != 0) {
		g_reservation_state = RESERVATION_GIVEUP;
		return;
	}

	/* A wrong-sized file in that slot is a build a reset or a yank interrupted.
	 * It is unusable, so remove it before the free-space gate below measures:
	 * the clusters it holds are exactly the ones this build wants back, and
	 * leaving it in place makes a card that has room look full. */
	if (scan.free_slot_used) {
		(void)fs_unlink(path);
	}

	/* Do not start a build the card cannot finish. The session reservation
	 * shrinks to fit a tight card, but a standing one is always full size, so on
	 * a card with no room for another the build would walk a step at a time into
	 * the wall and the step that finally fails makes FatFs search the whole FAT
	 * for a free cluster it will not find, a stall long enough to pin the ring.
	 * Advisory only: free space can still shrink while the build runs, so the
	 * -ENOSPC return from reservation_step stays the backstop. */
	if (free_bytes(&free_now) &&
	    free_now < target + FLIGHT_LOG_PREALLOC_MARGIN_BYTES) {
		LOG_INF("%llu bytes free cannot hold another reservation, pool stands "
			"at %u", (unsigned long long)free_now, scan.ready_count);
		g_reservation_state = RESERVATION_GIVEUP;
		return;
	}

	rc = mcap_stream_reservation_open(&g_reservation, path, target);
	if (rc != 0) {
		LOG_WRN("reservation open failed (%d), rotation will expand inline", rc);
		g_reservation_state = RESERVATION_GIVEUP;
		return;
	}
	g_reservation_slot = (uint32_t)scan.free_slot;
	g_reservation_state = RESERVATION_BUILDING;
}

/* Advance the build in progress by one growth step. Runs with the card lock
 * held. A finished reservation simply stands in its slot, so the builder returns
 * to idle and the next cycle starts the following slot. On a full card the grow
 * returns -ENOSPC, so the partial file is discarded and rotation takes the
 * inline fallback. */
static void reservation_step(void)
{
	int rc = mcap_stream_reservation_grow(
		&g_reservation, (uint64_t)CONFIG_RDD2_FLIGHT_LOG_PREALLOC_STEP_BYTES);

	if (rc == 1) {
		LOG_INF("reservation ready (%llu bytes) in slot %u",
			(unsigned long long)g_reservation.reserved, g_reservation_slot);
		g_reservation_state = RESERVATION_IDLE;
	} else if (rc < 0) {
		LOG_WRN("reservation growth stopped (%d), rotation will expand inline", rc);
		reservation_discard();
		g_reservation_state = RESERVATION_GIVEUP;
	}
	/* rc == 0: more steps remain, stay BUILDING. */
}

/*
 * One reservation action per flush cycle, called from the writer batch under the
 * card lock and between drain batches. The active session file's whole extent is
 * already built, so this is the only FAT-allocation work the writer does during
 * a session and it lands entirely on the disposable file being built. A card
 * yank during a growth step can leave that file's chain inconsistent, but never
 * the streaming session, whose clusters are all pre-allocated and thus
 * metadata-quiet.
 */
static void reservation_maintain(void)
{
	if (!g_session_active) {
		return;
	}

	/* Build only in genuinely idle cycles: if the ring is holding more than a
	 * small fraction of its capacity, draining the session comes first and the
	 * pool waits for a quieter cycle. One reservation is seconds of cycles while
	 * a size-triggered rotation is tens of minutes of streaming away, so even
	 * sparse idle cycles keep the pool ahead with wide margin. */
	if (ring_buf_size_get(&g_ring) >
	    (CONFIG_RDD2_FLIGHT_LOG_RING_BYTES / 8)) {
		return;
	}

	switch (g_reservation_state) {
	case RESERVATION_IDLE:
		reservation_begin();
		break;
	case RESERVATION_BUILDING:
		reservation_step();
		break;
	case RESERVATION_GIVEUP:
	default:
		/* GIVEUP waits for the next session open to reset the state and
		 * retry, since only a card that changed can change the answer. */
		break;
	}
}

static int start_session(void)
{
	struct rdd2_flight_log_scan scan;
	int rc;

	rdd2_flight_log_fs_lock();

	if (!rdd2_flight_log_fs_mounted()) {
		rc = rdd2_flight_log_fs_mount();
		if (rc != 0) {
			rdd2_flight_log_fs_unlock();
			return rc;
		}
	}

	rc = rdd2_flight_log_fs_scan(&scan);
	if (rc != 0) {
		/* A directory-read failure most likely means the card was pulled
		 * between the mount and the read. Unmount so the next attempt remounts
		 * cleanly and the low-rate retry can pick up a reinserted card. */
		(void)rdd2_flight_log_fs_unmount();
		rdd2_flight_log_fs_unlock();
		return rc;
	}

	/*
	 * The pool that was standing before the last stop is still on the card, so
	 * this session opens the way a rotation does: a directory-entry rename of an
	 * extent that was erased as it was reserved. After the first flight no boot
	 * scans the FAT for a free run, the reservations simply standing on the card
	 * across power cycles. Only a card that has never recorded falls through to
	 * the inline reservation below.
	 */
	rc = -ENOENT;
	if (scan.ready_slot >= 0) {
		rc = open_from_reservation(&scan);
		if (rc != 0) {
			LOG_WRN("standing reservation unusable (%d), reserving inline", rc);
		}
	}
	if (rc != 0) {
		rc = open_session_file(&scan);
	}

	/* One reservation just became this session's file (or it was reserved
	 * inline), so the pool is one short and the builder refills it from the next
	 * flush cycle. This also retries a card that once had no room. */
	g_reservation_state = RESERVATION_IDLE;
	if (rc != 0) {
		rdd2_flight_log_fs_unlock();
		return rc;
	}
	g_session_active = true;
	atomic_set(&g_active, 1);
	rdd2_flight_log_fs_unlock();
	return 0;
}

/* Discard and count any frames the capture thread enqueued during the stop
 * window. Consumer-side ring reads are safe against the still-live producer, and
 * counting them keeps the dropped total honest instead of silently losing them
 * or bleeding them into the next session. */
static void drop_ring_remainder(void)
{
	uint8_t scratch[FLIGHT_LOG_MAX_PAYLOAD];

	while (ring_buf_size_get(&g_ring) >= sizeof(struct ring_header)) {
		struct ring_header header;

		if (ring_buf_get(&g_ring, (uint8_t *)&header, sizeof(header)) != sizeof(header)) {
			break;
		}
		if (header.len > sizeof(scratch)) {
			/* Framing invariant broken. Stop rather than reset, since the
			 * capture producer may still be finishing a batch. */
			break;
		}
		if (ring_buf_get(&g_ring, scratch, header.len) != header.len) {
			break;
		}
		atomic_inc(&g_dropped);
	}
}

static void stop_session(void)
{
	rdd2_flight_log_fs_lock();

	if (!g_session_active) {
		rdd2_flight_log_fs_unlock();
		return;
	}

	atomic_set(&g_active, 0);
	drain_ring();
	(void)synapse_mcap_close(&g_writer);
	(void)mcap_stream_close_file(&g_stream);
	g_session_active = false;
	/*
	 * The standing reservations are the pool the next boot opens from, so they
	 * stay on the card and the next session renames one in. A build still in
	 * progress is closed and removed instead: a partial chain left by a yank
	 * cannot be trusted, and rebuilding one is cheap.
	 */
	reservation_discard();
	g_reservation_state = RESERVATION_IDLE;
	/* Capture parks on its next loop check, but may have enqueued one more
	 * batch after g_active cleared. Those frames missed the closed file, so
	 * count them as dropped rather than lose them uncounted. */
	drop_ring_remainder();
	LOG_INF("logging session closed (%llu bytes)",
		(unsigned long long)g_stream.bytes_written);
	rdd2_flight_log_fs_unlock();
}

/*
 * Roll to the next session file without stopping capture. Runs on the writer
 * thread with the card lock taken here. g_active stays set the whole time, so
 * the capture thread keeps enqueuing into the ring across the close and reopen.
 * Capture never touches the card, so the 128 KiB ring rides the gap and samples
 * that land during the swap window are drained into the next file instead of
 * vanishing uncounted.
 *
 * When a reservation stands on the card the swap is a rename of the pre-built
 * extent: no f_expand, no free-extent scan, so the writer stalls only for the
 * close and the rename, both directory-entry work. When none is ready (an early
 * manual rotate on a fresh card, or a build that gave up on a full card) it
 * falls back to the inline expand path, which carries the known open-time burst.
 * Returns 0 on success or a negative errno.
 */
static int rotate_session(void)
{
	struct rdd2_flight_log_scan scan;
	int rc;

	rdd2_flight_log_fs_lock();

	if (!g_session_active) {
		rdd2_flight_log_fs_unlock();
		return -EINVAL;
	}

	drain_ring();
	(void)synapse_mcap_close(&g_writer);

	rc = rdd2_flight_log_fs_scan(&scan);
	if (rc == 0) {
		if (scan.ready_slot >= 0) {
			/* Size-triggered rotation filled the whole reservation, so close
			 * session N without truncation (the tail is already spent) and
			 * rename a standing reservation into session N+1. */
			(void)mcap_stream_close_file_full(&g_stream);
			rc = open_from_reservation(&scan);
			if (rc != 0) {
				LOG_WRN("reservation rotation failed (%d), expanding "
					"inline", rc);
			}
		} else {
			/* None ready: close session N normally, freeing its unused
			 * tail, and reserve the next extent inline. */
			(void)mcap_stream_close_file(&g_stream);
			LOG_WRN("no reservation ready at rotation, expanding inline");
			rc = -ENOENT;
		}

		if (rc != 0) {
			/* Hand the build in progress its clusters back so the inline
			 * expand has room, then reserve at the same index. */
			reservation_discard();
			rc = open_session_file(&scan);
		}
	} else {
		(void)mcap_stream_close_file(&g_stream);
	}

	/* The pool is one short either way, so the builder refills from the next
	 * flush cycle. */
	g_reservation_state = RESERVATION_IDLE;
	if (rc != 0) {
		/* Could not open the next file, most likely a pulled card. Park
		 * capture and drop the mount so the retry path remounts cleanly. */
		atomic_set(&g_active, 0);
		g_session_active = false;
		(void)rdd2_flight_log_fs_unmount();
		rdd2_flight_log_fs_unlock();
		return rc;
	}

	rdd2_flight_log_fs_unlock();
	return 0;
}

/* True when a size-triggered rotation may swap now: a reservation stands on the
 * card, so the swap is a rename, or the builder gave up and the inline fallback
 * is all there will be. Reads the card, so the caller asks only once the cheap
 * gates have passed. Runs with the card lock held. */
static bool rotation_ready(void)
{
	struct rdd2_flight_log_scan scan;

	return g_reservation_state == RESERVATION_GIVEUP ||
	       (rdd2_flight_log_fs_scan(&scan) == 0 && scan.ready_slot >= 0);
}

static void writer_thread(void *a, void *b, void *c)
{
	uint64_t last_timeref = 0U;
	uint64_t last_self = 0U;
	uint64_t last_flush = 0U;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	/*
	 * Bring the card up before anything else at boot. Identifying the card is
	 * about 1.3 s on this hardware, and a blank card is also formatted here, so
	 * at the writer's normal priority that work would queue behind main and the
	 * flight threads and the session would open long after the vehicle could be
	 * armed. Priority 1 puts this one mount attempt above main (priority 2) and
	 * every flight thread; the priority is dropped again before the session
	 * opens, so the steady-state writer still runs below the control domain. The
	 * retry loop for an absent card runs at the normal priority, where a missing
	 * card costs nothing.
	 *
	 * The SD stack busy-polls the card while it identifies, so a slow card
	 * holds the CPU for those polls at priority 1; that is the price of
	 * having the card up before anything else runs.
	 */
	k_thread_priority_set(k_current_get(), 1);
	(void)rdd2_flight_log_fs_mount();
	k_thread_priority_set(k_current_get(), CONFIG_RDD2_FLIGHT_LOG_WRITER_PRIORITY);

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
			if (rotate_session() != 0) {
				k_sleep(K_MSEC(CONFIG_RDD2_FLIGHT_LOG_RETRY_MS));
				continue;
			}
			now = synapse_time_boot_ns();
			last_timeref = now;
			last_self = now;
			last_flush = now;
		}

		/* Serialize this drain/flush batch against every shell command that
		 * touches the card. Capture keeps enqueuing into the ring meanwhile
		 * because it never touches FatFs. Release before the sleep so a shell
		 * command is not blocked across the idle window. */
		rdd2_flight_log_fs_lock();
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
			/* Advance the reservation pool one step per flush cycle, still
			 * under the card lock and after the session is synced. This moves
			 * the next rotation's f_expand burst off the rotation path and
			 * onto a disposable file, amortized over idle cycles. */
			reservation_maintain();
			last_flush = now;
		}

		if (g_stream.write_failed || synapse_mcap_error(&g_writer) != SYNAPSE_MCAP_OK) {
			LOG_WRN("writer error, closing session");
			stop_session();
			/* A write failure usually means the card was removed. Drop
			 * the mount so a reinserted card is picked up fresh. */
			(void)rdd2_flight_log_fs_unmount();
			rdd2_flight_log_fs_unlock();
			k_sleep(K_MSEC(CONFIG_RDD2_FLIGHT_LOG_RETRY_MS));
			continue;
		}

		/* Rotate on the streamed byte count, not the on-disk file size. With
		 * preallocation the file size jumps to the reserved extent at open,
		 * but bytes_written still climbs from zero as records are written, so
		 * this triggers at the real data volume and the reservation cannot
		 * make it fire early.
		 *
		 * The byte count only arms the size-triggered rotation; the swap then
		 * waits for a cycle where it can run without dropping frames. Two gates
		 * define that cycle:
		 *
		 *   - the ring must be nearly empty (the same quiet threshold the pool
		 *     builds on), so its full ~510 ms of headroom is free to absorb the
		 *     close, f_sync and rename while capture keeps enqueuing;
		 *
		 *   - and a reservation must stand on the card, so the reopen renames a
		 *     pre-built extent with no inline f_expand burst. A builder that
		 *     gave up passes too: no better state is coming and the inline
		 *     fallback is the only option left. While a build is running the
		 *     next reservation is seconds away, so waiting avoids forcing the
		 *     fallback needlessly. That check reads the card, so it is asked
		 *     only after the two cheap gates above have passed.
		 *
		 * Deferring to a quiet cycle costs only a few kilobytes past the
		 * reservation size. A manual `flightlog rotate` bypasses these gates and
		 * rotates immediately through the same request path. */
#if defined(CONFIG_RDD2_FLIGHT_LOG_EAGER_ROTATE)
		/* Rotate as soon as the byte threshold is crossed, without waiting
		 * for a burst-free cycle. This reproduces the pre-deferral behavior
		 * whose mid-flight reservation stall overflowed the ring. */
		bool ring_quiet = true;
#else
		bool ring_quiet = ring_buf_size_get(&g_ring) <=
				  (CONFIG_RDD2_FLIGHT_LOG_RING_BYTES / 8U);
#endif

		if (g_stream.bytes_written >= (uint64_t)CONFIG_RDD2_FLIGHT_LOG_ROTATE_BYTES &&
		    ring_quiet &&
		    (IS_ENABLED(CONFIG_RDD2_FLIGHT_LOG_EAGER_ROTATE) || rotation_ready())) {
			atomic_set(&g_rotate_request, 1);
		}

		rdd2_flight_log_fs_unlock();
		k_sleep(K_MSEC(20));
	}
}

/* ---- public API ---- */

bool rdd2_flight_log_healthy(void)
{
	return atomic_get(&g_active) != 0 && g_stream.file_open && g_stream.last_sync_ok;
}

bool rdd2_flight_log_session_active(void)
{
	return atomic_get(&g_active) != 0;
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
	out->last_sync_ms = g_stream.last_sync_ms;
	out->max_sync_ms = g_stream.max_sync_ms;
	out->reservation_state = (uint8_t)g_reservation_state;
	out->reservation_bytes =
		g_reservation_state == RESERVATION_BUILDING ? g_reservation.reserved : 0U;
	out->reservation_slots = RDD2_FLIGHT_LOG_RESERVE_SLOTS;
	out->ready_reservations = 0U;
	if (rdd2_flight_log_fs_mounted()) {
		struct rdd2_flight_log_scan scan;

		/* Counting what stands on the card is a directory read, so take the
		 * card lock the same way a shell command that touches the volume does.
		 * The lock is recursive and the writer holds it only in short batches. */
		rdd2_flight_log_fs_lock();
		if (rdd2_flight_log_fs_scan(&scan) == 0) {
			out->ready_reservations = scan.ready_count;
		}
		rdd2_flight_log_fs_unlock();
	}
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

/* No start delay: the card is identified and mounted as early as the kernel can
 * run a thread, so logging is live before the vehicle can be armed. */
K_THREAD_DEFINE(flight_log_writer, CONFIG_RDD2_FLIGHT_LOG_WRITER_STACK_SIZE, writer_thread,
		NULL, NULL, NULL, CONFIG_RDD2_FLIGHT_LOG_WRITER_PRIORITY, 0, 0);
K_THREAD_DEFINE(flight_log_capture, CONFIG_RDD2_FLIGHT_LOG_FRONT_STACK_SIZE, capture_thread,
		NULL, NULL, NULL, CONFIG_RDD2_FLIGHT_LOG_FRONT_PRIORITY, 0, 0);
