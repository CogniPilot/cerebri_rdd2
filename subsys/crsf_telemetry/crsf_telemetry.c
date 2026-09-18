/* SPDX-License-Identifier: Apache-2.0 */

/*
 * CRSF telemetry towards the ELRS receiver.
 *
 * One thread subscribes to the flight bus and feeds the receiver's downlink
 * with plain CRSF frames (GPS, battery, flight mode) plus the Yaapu
 * passthrough frames the Yaapu telemetry script on the transmitter decodes.
 *
 * The downlink is narrow. The link this is sized for is ELRS 2.4 GHz at a
 * 500 Hz packet rate with a 1:4 telemetry ratio: 125 telemetry packets/s,
 * about 4687 bit/s or 585 bytes/s of CRSF frames including the 4 framing
 * bytes, shared with the receiver's own link statistics. The receiver keeps
 * telemetry in a 512 byte FIFO: broadcast types (GPS, battery, flight mode)
 * overwrite the older frame of their type, but every custom telemetry frame
 * (type 0x80, the passthrough groups) is appended, so type 0x80 must be
 * offered well below the link's drain rate or a status frame carrying an
 * armed or failsafe edge waits seconds behind attitude frames. The default
 * periods below add up to roughly 240 bytes/s, of which about 205 bytes/s is
 * type 0x80:
 *
 *   attitude group  10 Hz x 18 B = 180 B/s
 *   GPS              1 Hz x 19 B =  19 B/s
 *   status group     1 Hz x 24 B =  24 B/s
 *   flight mode    0.5 Hz x 13 B =   7 B/s (plus one frame per mode change)
 *   battery x2    0.33 Hz x 12 B =   8 B/s
 *   params        0.25 Hz x 12 B =   3 B/s
 *
 * Every frame goes through one scheduler that sends at most one frame per
 * 10 ms tick. Among the entries whose period has elapsed it picks the one that
 * is the most periods overdue, so a starved entry catches up before an entry
 * that is merely due. A flight-mode change and the status texts are events,
 * not schedule entries: the thread wakes on every vehicle_health update, sends
 * them ahead of anything else, and holds the scheduled frame for that tick so
 * the event frame is alone on the link. The periodic flight-mode entry only
 * repairs a dropped event frame.
 *
 * The driver send is a synchronous poll-out (about 24 us per byte) that
 * returns success even when it drops the frame because another send is in
 * flight, so the counters here record what was offered to the driver.
 */

#include "crsf_telemetry.h"

#include "crsf_telemetry_encode.h"

#include "interfaces/zros_topics.h"

#include <math.h>
#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/input/input_crsf.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zros/zros_node.h>
#include <zros/zros_sub.h>
#include <zros/zros_topic.h>

LOG_MODULE_REGISTER(rdd2_crsf_telemetry, LOG_LEVEL_INF);

#define CRSF_TELEM_TICK_MS           10
#define CRSF_TELEM_BOOT_TEXT_MS      2000
#define CRSF_TELEM_GPS_PERIOD_MS     1000
#define CRSF_TELEM_MODE_PERIOD_MS    2000
#define CRSF_TELEM_BATTERY_PERIOD_MS 3000
#define CRSF_TELEM_PARAMS_PERIOD_MS  4000

enum crsf_telem_sub {
	SUB_HEALTH = 0,
	SUB_ATTITUDE,
	SUB_GNSS,
	SUB_COUNT,
};

static const struct device *const g_dev = DEVICE_DT_GET(DT_ALIAS(rc));

static struct zros_node g_node;
static struct zros_sub g_subs[SUB_COUNT];
static synapse_topic_VehicleHealthData_t g_health;
static synapse_topic_AttitudeEstimateData_t g_attitude;
static synapse_topic_GnssFixData_t g_gnss;

static const struct {
	struct zros_topic *topic;
	void *data;
	double rate_hz;
} g_sub_defs[SUB_COUNT] = {
	[SUB_HEALTH] = {&topic_vehicle_health, &g_health, 100.0},
	[SUB_ATTITUDE] = {&topic_attitude_estimate, &g_attitude, 10.0},
	[SUB_GNSS] = {&topic_gnss_fix, &g_gnss, 2.0},
};

static const uint16_t g_period_ms[CRSF_TELEM_ENTRY_COUNT] = {
	[CRSF_TELEM_ENTRY_ATTITUDE] = CONFIG_RDD2_CRSF_TELEMETRY_ATTITUDE_PERIOD_MS,
	[CRSF_TELEM_ENTRY_STATUS] = CONFIG_RDD2_CRSF_TELEMETRY_STATUS_PERIOD_MS,
	[CRSF_TELEM_ENTRY_GPS] = CRSF_TELEM_GPS_PERIOD_MS,
	[CRSF_TELEM_ENTRY_FLIGHT_MODE] = CRSF_TELEM_MODE_PERIOD_MS,
	[CRSF_TELEM_ENTRY_BATTERY] = CRSF_TELEM_BATTERY_PERIOD_MS,
	[CRSF_TELEM_ENTRY_BATTERY_PASSTHROUGH] = CRSF_TELEM_BATTERY_PERIOD_MS,
	[CRSF_TELEM_ENTRY_PARAMS] = CRSF_TELEM_PARAMS_PERIOD_MS,
};

static int64_t g_last_ms[CRSF_TELEM_ENTRY_COUNT];

/*
 * Home for the passthrough home frame, from GNSS: latched at the first 3D fix
 * and again on every arming. The home frame and the speeds are derived from
 * the receiver rather than from the navigation estimator so a diverged
 * estimate cannot put a bogus altitude or distance on the transmitter.
 */
static bool g_home_set;
static int32_t g_home_lat_e7;
static int32_t g_home_lon_e7;
static int32_t g_home_alt_mm;

#define METERS_PER_DEG 111320.0f

static void home_update(bool arming)
{
	if (g_gnss.fix_type < synapse_types_GnssFixType_Fix3d) {
		return;
	}
	if (!g_home_set || arming) {
		g_home_set = true;
		g_home_lat_e7 = g_gnss.latitude_deg_e7;
		g_home_lon_e7 = g_gnss.longitude_deg_e7;
		g_home_alt_mm = g_gnss.altitude_msl_mm;
	}
}

/* Offset of the current fix from home in metres, ENU. Zero without a home. */
static void home_offset(float *east_m, float *north_m, float *up_m)
{
	if (!g_home_set) {
		*east_m = 0.0f;
		*north_m = 0.0f;
		*up_m = 0.0f;
		return;
	}
	*north_m = (float)(g_gnss.latitude_deg_e7 - g_home_lat_e7) * 1e-7f * METERS_PER_DEG;
	*east_m = (float)(g_gnss.longitude_deg_e7 - g_home_lon_e7) * 1e-7f * METERS_PER_DEG *
		  cosf((float)g_gnss.latitude_deg_e7 * 1e-7f * 0.017453292f);
	*up_m = (float)(g_gnss.altitude_msl_mm - g_home_alt_mm) * 1e-3f;
}
static struct rdd2_crsf_telemetry_counters g_counters;
static uint8_t g_prev_flags;
static uint8_t g_prev_mode = UINT8_MAX;
static bool g_boot_text_sent;

static const char *mode_name(uint8_t flight_mode)
{
	static const char *const names[] = {"Acro", "Attitude", "Position"};

	return flight_mode < ARRAY_SIZE(names) ? names[flight_mode] : "Unknown";
}

static void telemetry_send(uint8_t type, uint8_t *buf, size_t len, uint32_t *counter)
{
	if (len == 0U || input_crsf_send_telemetry(g_dev, type, buf, len) != 0) {
		return;
	}

	*counter += 1U;
	g_counters.bytes += (uint32_t)len;
}

static void send_status_text(uint8_t severity, const char *text)
{
	uint8_t buf[CRSF_ENCODE_BUF_LEN];

	telemetry_send(CRSF_TYPE_AP_CUSTOM_TELEM, buf,
		       crsf_encode_status_text(buf, sizeof(buf), severity, text),
		       &g_counters.text_frames);
}

/* Fills buf with the entry's payload and its CRSF frame type. Returns 0 when
 * the entry has nothing to report yet, which skips the send but still spends
 * the entry's turn. */
static size_t build_entry(size_t idx, uint8_t *buf, uint8_t *type)
{
	struct crsf_yaapu_packet pk[CRSF_MULTI_PACKET_MAX];

	*type = CRSF_TYPE_AP_CUSTOM_TELEM;

	switch (idx) {
	case CRSF_TELEM_ENTRY_ATTITUDE: {
		float roll_rad;
		float pitch_rad;
		float yaw_rad;

		crsf_quat_to_euler321(g_attitude.attitude.w, g_attitude.attitude.x,
				      g_attitude.attitude.y, g_attitude.attitude.z, &roll_rad,
				      &pitch_rad, &yaw_rad);
		/* The estimator's Euler angles follow the ENU/FLU convention
		 * (positive pitch is nose down, yaw counts from East), while the
		 * transmitter expects a nose-up positive pitch and a compass
		 * heading from North. Roll has the same sign in both. */
		pk[0].appid = CRSF_YAAPU_ROLLPITCH_APPID;
		pk[0].data = crsf_yaapu_rollpitch(roll_rad, -pitch_rad);
		pk[1].appid = CRSF_YAAPU_VELANDYAW_APPID;
		pk[1].data = crsf_yaapu_velandyaw(
			(g_gnss.flags & synapse_topic_GnssFixFlags_VelocityUpValid) != 0U
				? (float)g_gnss.velocity_up_cm_s * 0.01f
				: 0.0f,
			(float)g_gnss.ground_speed_cm_s * 0.01f, 1.57079633f - yaw_rad);
		return crsf_encode_multi_packet(buf, CRSF_ENCODE_BUF_LEN, pk, 2U);
	}
	case CRSF_TELEM_ENTRY_STATUS:
		/* Throttle is reported as zero: manual_input is a semaphore-backed
		 * topic whose publisher (the CRSF input callback) must collect
		 * every read token within 1 ms, and a reader at this thread's
		 * priority can be preempted for longer than that by the flight
		 * log writer. Every topic read here is single-publisher and
		 * lock-free for that reason. */
		pk[0].appid = CRSF_YAAPU_AP_STATUS_APPID;
		pk[0].data = crsf_yaapu_ap_status(
			g_health.flight_mode,
			(g_health.flags & synapse_topic_VehicleHealthFlags_Armed) != 0U,
			(g_health.flags & synapse_topic_VehicleHealthFlags_Failsafe) != 0U, 0);
		pk[1].appid = CRSF_YAAPU_GPS_STATUS_APPID;
		pk[1].data = crsf_yaapu_gps_status(g_gnss.satellites_used, (uint8_t)g_gnss.fix_type,
						   g_gnss.hdop_centi, g_gnss.altitude_msl_mm);
		{
			float east_m;
			float north_m;
			float up_m;

			home_offset(&east_m, &north_m, &up_m);
			pk[2].appid = CRSF_YAAPU_HOME_APPID;
			pk[2].data = crsf_yaapu_home(east_m, north_m, up_m);
		}
		return crsf_encode_multi_packet(buf, CRSF_ENCODE_BUF_LEN, pk, 3U);
	case CRSF_TELEM_ENTRY_GPS:
		*type = CRSF_TYPE_GPS;
		return crsf_encode_gps(buf, CRSF_ENCODE_BUF_LEN, g_gnss.latitude_deg_e7,
				       g_gnss.longitude_deg_e7, g_gnss.ground_speed_cm_s,
				       g_gnss.course_over_ground_cdeg, g_gnss.altitude_msl_mm,
				       g_gnss.satellites_used);
	case CRSF_TELEM_ENTRY_FLIGHT_MODE: {
		/* The mode string carries the arm state as a trailing '*' while
		 * disarmed. The receiver overwrites a queued flight mode frame
		 * with the newest one instead of appending it, so unlike the
		 * passthrough status word this can never be stale. */
		char name[16];

		snprintf(name, sizeof(name), "%s%s", mode_name(g_health.flight_mode),
			 (g_health.flags & synapse_topic_VehicleHealthFlags_Armed) != 0U ? "" : "*");
		*type = CRSF_TYPE_FLIGHT_MODE;
		return crsf_encode_flight_mode(buf, CRSF_ENCODE_BUF_LEN, name);
	}
	case CRSF_TELEM_ENTRY_BATTERY:
		/* A zero pack voltage means no monitor reading yet, so report
		 * nothing rather than a flat 0 V the transmitter would announce
		 * as a dead pack. Consumed mAh is not tracked either: the
		 * monitor publishes instantaneous voltage and current only, so
		 * both battery frames carry a consumption of zero and the
		 * transmitter shows the remaining percent instead. */
		if (g_health.voltage_battery_cv == 0U) {
			return 0U;
		}
		*type = CRSF_TYPE_BATTERY;
		return crsf_encode_battery(buf, CRSF_ENCODE_BUF_LEN, g_health.voltage_battery_cv,
					   g_health.current_battery_da, 0U,
					   (uint8_t)MAX(g_health.battery_remaining_pct, 0));
	case CRSF_TELEM_ENTRY_BATTERY_PASSTHROUGH:
		if (g_health.voltage_battery_cv == 0U) {
			return 0U;
		}
		pk[0].appid = CRSF_YAAPU_BATTERY_APPID;
		pk[0].data = crsf_yaapu_battery(g_health.voltage_battery_cv,
						g_health.current_battery_da, 0U);
		return crsf_encode_multi_packet(buf, CRSF_ENCODE_BUF_LEN, pk, 1U);
	case CRSF_TELEM_ENTRY_PARAMS:
		pk[0].appid = CRSF_YAAPU_PARAMS_APPID;
		pk[0].data = crsf_yaapu_param(CRSF_YAAPU_PARAM_FRAME_TYPE, CRSF_MAV_TYPE_QUADROTOR);
		return crsf_encode_multi_packet(buf, CRSF_ENCODE_BUF_LEN, pk, 1U);
	default:
		return 0U;
	}
}

static void send_entry(size_t idx, int64_t now_ms)
{
	uint8_t buf[CRSF_ENCODE_BUF_LEN];
	uint8_t type = 0U;
	size_t len = build_entry(idx, buf, &type);

	g_last_ms[idx] = now_ms;
	telemetry_send(type, buf, len, &g_counters.frames[idx]);
}

static void send_due(int64_t now_ms)
{
	size_t best = CRSF_TELEM_ENTRY_COUNT;
	int64_t best_overdue = 0;

	for (size_t i = 0U; i < CRSF_TELEM_ENTRY_COUNT; ++i) {
		int64_t age_ms = now_ms - g_last_ms[i];
		int64_t overdue;

		if (age_ms < g_period_ms[i]) {
			continue;
		}

		/* Periods elapsed since the last send. The >= lets a later entry
		 * take a tie, so the fixed rotation order does not starve the
		 * tail of the table. */
		overdue = age_ms / g_period_ms[i];
		if (overdue >= best_overdue) {
			best_overdue = overdue;
			best = i;
		}
	}

	if (best < CRSF_TELEM_ENTRY_COUNT) {
		send_entry(best, now_ms);
	}
}

/* Returns true when an event frame went out this tick. */
static bool send_events(int64_t now_ms)
{
	uint8_t flags = g_health.flags;
	uint8_t rising = flags & ~g_prev_flags;
	bool sent = false;

	home_update((rising & synapse_topic_VehicleHealthFlags_Armed) != 0U);

	if (g_health.flight_mode != g_prev_mode) {
		g_prev_mode = g_health.flight_mode;
		send_entry(CRSF_TELEM_ENTRY_FLIGHT_MODE, now_ms);
		sent = true;
	}

	/* The armed and failsafe bits live in the status group; an edge on
	 * either goes out at once instead of waiting for its slow period. */
	if ((flags ^ g_prev_flags) &
	    (synapse_topic_VehicleHealthFlags_Armed | synapse_topic_VehicleHealthFlags_Failsafe)) {
		send_entry(CRSF_TELEM_ENTRY_STATUS, now_ms);
		sent = true;
	}
	if ((flags ^ g_prev_flags) & synapse_topic_VehicleHealthFlags_Armed) {
		send_entry(CRSF_TELEM_ENTRY_FLIGHT_MODE, now_ms);
	}

	if (!g_boot_text_sent && now_ms >= CRSF_TELEM_BOOT_TEXT_MS) {
		g_boot_text_sent = true;
		send_status_text(CRSF_AP_SEVERITY_INFO, "Cerebri RDD2 ready");
		sent = true;
	}

	if ((flags ^ g_prev_flags) & synapse_topic_VehicleHealthFlags_Armed) {
		/* The wording the Yaapu voice files are keyed on. */
		send_status_text(CRSF_AP_SEVERITY_INFO,
				 (flags & synapse_topic_VehicleHealthFlags_Armed)
					 ? "Arming motors"
					 : "Disarming motors");
		sent = true;
	}

	if (rising & synapse_topic_VehicleHealthFlags_Failsafe) {
		send_status_text(CRSF_AP_SEVERITY_CRITICAL, "Failsafe");
		sent = true;
	}

	g_prev_flags = flags;
	return sent;
}

void rdd2_crsf_telemetry_counters_get(struct rdd2_crsf_telemetry_counters *out)
{
	if (out != NULL) {
		*out = g_counters;
	}
}

static void crsf_telemetry_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	if (!device_is_ready(g_dev)) {
		LOG_ERR("%s not ready, no telemetry", g_dev->name);
		return;
	}

	zros_node_init(&g_node, "crsf_telemetry");
	for (size_t i = 0U; i < SUB_COUNT; ++i) {
		(void)zros_sub_init(&g_subs[i], &g_node, g_sub_defs[i].topic, g_sub_defs[i].data,
				    g_sub_defs[i].rate_hz);
	}

	while (true) {
		int64_t now_ms = k_uptime_get();

		for (size_t i = 0U; i < SUB_COUNT; ++i) {
			if (zros_sub_update_available(&g_subs[i])) {
				(void)zros_sub_update(&g_subs[i]);
			}
		}

		if (!send_events(now_ms)) {
			send_due(now_ms);
		}
		/* Wake early on a vehicle_health update (rate limited to the
		 * tick) so a mode change is on the link within one tick. */
		(void)zros_sub_wait(&g_subs[SUB_HEALTH], K_MSEC(CRSF_TELEM_TICK_MS));
	}
}

K_THREAD_DEFINE(crsf_telemetry, CONFIG_RDD2_CRSF_TELEMETRY_THREAD_STACK_SIZE,
		crsf_telemetry_thread, NULL, NULL, NULL,
		CONFIG_RDD2_CRSF_TELEMETRY_THREAD_PRIORITY, 0, 0);
