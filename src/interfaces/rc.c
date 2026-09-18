/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "drivers.h"

#include "synapse_time_status.h"
#include "zros_topics.h"

#if defined(CONFIG_RDD2_CRSF_TELEMETRY)
#include "crsf_telemetry.h"
#include "crsf_telemetry_encode.h"
#endif

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/input/input_crsf.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <zros/private/zros_node_struct.h>
#include <zros/private/zros_pub_struct.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>

#define RC_NODE                DT_ALIAS(rc)
#define RC_CHANNEL_COUNT       16
#define THROTTLE_CHANNEL_INDEX 2
#define RC_US_CENTER           1500
#define RC_US_MIN              1000

static rdd2_rc_channels_t g_rc_staging;
static rdd2_rc_channels_t g_rc_latest;
static bool g_rc_staging_valid;
static bool g_rc_latest_valid;
static int64_t g_rc_latest_stamp_ms;
static atomic_t g_rc_seq;
static atomic_t g_rc_link_quality;
static struct zros_node g_rdd2_rc_node;
static struct zros_pub g_rdd2_rc_pub;
static synapse_topic_ManualControlData_t g_rdd2_manual_msg;
static bool g_rdd2_rc_pub_ready;

static void rc_channels_set_defaults(rdd2_rc_channels_t *rc, bool *valid, int64_t *stamp_ms)
{
	int32_t *channels = rdd2_topic_rc_channels_data(rc);

	for (size_t i = 0; i < RC_CHANNEL_COUNT; i++) {
		channels[i] = RC_US_CENTER;
	}

	channels[THROTTLE_CHANNEL_INDEX] = RC_US_MIN;
	if (valid != NULL) {
		*valid = false;
	}
	if (stamp_ms != NULL) {
		*stamp_ms = 0;
	}
}

int rdd2_rc_input_init(void)
{
	const struct device *dev = DEVICE_DT_GET(RC_NODE);
	int rc;

	if (!device_is_ready(dev)) {
		return -ENODEV;
	}

	rc_channels_set_defaults(&g_rc_staging, &g_rc_staging_valid, NULL);
	rc_channels_set_defaults(&g_rc_latest, &g_rc_latest_valid, &g_rc_latest_stamp_ms);
	atomic_set(&g_rc_seq, 0);
	atomic_set(&g_rc_link_quality, 0);
	zros_node_init(&g_rdd2_rc_node, "rdd2_rc_input");
	rc = zros_pub_init(&g_rdd2_rc_pub, &g_rdd2_rc_node, &topic_manual_input,
			   &g_rdd2_manual_msg);
	g_rdd2_rc_pub_ready = (rc == 0);
	return rc;
}

static int16_t centered_milli(int32_t channel_us)
{
	return (int16_t)CLAMP((channel_us - 1500) * 2, -1000, 1000);
}

uint8_t rdd2_rc_flight_mode(const rdd2_rc_channels_t *rc)
{
	const int32_t *channels;

	if (rc == NULL) {
		return 0U;
	}

	channels = rc->ch;
	if (channels[RDD2_FLIGHT_MODE_CHANNEL_INDEX] < RDD2_FLIGHT_MODE_ACRO_MAX_US) {
		return 0U;
	}
	if (channels[RDD2_FLIGHT_MODE_CHANNEL_INDEX] < RDD2_FLIGHT_MODE_POSITION_MIN_US) {
		return 1U;
	}
	return 2U;
}

static void manual_message_from_rc(synapse_topic_ManualControlData_t *manual,
				   const rdd2_rc_channels_t *rc, bool valid)
{
	const int32_t *channels = rdd2_topic_rc_channels_data_const(rc);
	uint8_t flags = valid ? synapse_topic_ManualControlFlags_Valid |
					synapse_topic_ManualControlFlags_Active
			      : 0U;

	if (channels[4] >= 1500) {
		flags |= synapse_topic_ManualControlFlags_ArmSwitch;
	}
	*manual = (synapse_topic_ManualControlData_t){
		.timestamp_ns = synapse_time_boot_ns(),
		.active_axes = synapse_topic_ManualControlAxes_Roll |
			       synapse_topic_ManualControlAxes_Pitch |
			       synapse_topic_ManualControlAxes_Throttle |
			       synapse_topic_ManualControlAxes_Yaw,
		.roll_milli = centered_milli(channels[0]),
		.pitch_milli = centered_milli(channels[1]),
		.throttle_milli = (int16_t)CLAMP(channels[2] - 1000, 0, 1000),
		.yaw_milli = (int16_t)-centered_milli(channels[3]),
		.flight_mode = rdd2_rc_flight_mode(rc),
		.flags = flags,
	};
}

void rdd2_rc_input_latest_get(rdd2_rc_channels_t *rc, int64_t *stamp_ms, bool *valid)
{
	atomic_val_t seq_start;
	atomic_val_t seq_end = 0;

	if (rc == NULL || stamp_ms == NULL || valid == NULL) {
		return;
	}

	do {
		seq_start = atomic_get(&g_rc_seq);
		if ((seq_start & 1) != 0) {
			continue;
		}

		*rc = g_rc_latest;
		*stamp_ms = g_rc_latest_stamp_ms;
		*valid = g_rc_latest_valid;
		seq_end = atomic_get(&g_rc_seq);
	} while (seq_start != seq_end);
}

uint8_t rdd2_rc_input_link_quality_get(void)
{
#if DT_NODE_HAS_COMPAT(RC_NODE, tbs_crsf)
	return input_crsf_get_link_stats(DEVICE_DT_GET(RC_NODE)).uplink_link_quality;
#else
	return (uint8_t)atomic_get(&g_rc_link_quality);
#endif
}

static void rc_input_cb(struct input_event *evt, void *user_data)
{
	int32_t *staging_channels = rdd2_topic_rc_channels_data(&g_rc_staging);

	ARG_UNUSED(user_data);

	if (evt->type == INPUT_EV_ABS && evt->code >= 1 && evt->code <= RC_CHANNEL_COUNT) {
		staging_channels[evt->code - 1] = evt->value;
		g_rc_staging_valid = true;
	} else if (evt->type == INPUT_EV_MSC && evt->code == RDD2_RC_INPUT_EVENT_LINK_QUALITY) {
		atomic_set(&g_rc_link_quality, evt->value);
	} else if (evt->type == INPUT_EV_MSC && evt->code == RDD2_RC_INPUT_EVENT_VALID) {
		g_rc_staging_valid = evt->value != 0;
	}

	if (evt->sync) {
		atomic_inc(&g_rc_seq);
		g_rc_latest = g_rc_staging;
		g_rc_latest_valid = g_rc_staging_valid;
		g_rc_latest_stamp_ms = k_uptime_get();
		atomic_inc(&g_rc_seq);
		if (g_rdd2_rc_pub_ready) {
			manual_message_from_rc(&g_rdd2_manual_msg, &g_rc_latest, g_rc_latest_valid);
			(void)zros_pub_update(&g_rdd2_rc_pub);
		}
	}
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(RC_NODE), rc_input_cb, NULL);

#if DT_NODE_HAS_COMPAT(RC_NODE, tbs_crsf)
static int cmd_crsf_status(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev = DEVICE_DT_GET(RC_NODE);
	struct crsf_diagnostics diag = input_crsf_get_diagnostics(dev);
	struct crsf_link_stats link = input_crsf_get_link_stats(dev);
	int64_t stamp_ms;
	bool valid;
	rdd2_rc_channels_t channels;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	rdd2_rc_input_latest_get(&channels, &stamp_ms, &valid);

	shell_print(sh, "device=%s ready=%s rc_valid=%s last_rc_age_ms=%lld", dev->name,
		    device_is_ready(dev) ? "yes" : "no", valid ? "yes" : "no",
		    stamp_ms > 0 ? (long long)(k_uptime_get() - stamp_ms) : -1LL);
	shell_print(sh, "bytes=%u valid_frames=%u channel_frames=%u link_frames=%u",
		    diag.uart_rx_bytes, diag.valid_frames, diag.channel_frames, diag.link_frames);
	shell_print(sh, "crc_errors=%u unsupported=%u queue_drops=%u", diag.crc_errors,
		    diag.unsupported_frames, diag.queue_drops);
	shell_print(sh, "uart_stopped=%u uart_restarts=%u uart_errors=%u", diag.uart_rx_stopped,
		    diag.uart_rx_restarts, diag.uart_errors);
	shell_print(sh, "rx_input_errors=%u parser_overflows=%u framing_errors=%u rx_buf_errors=%u",
		    diag.rx_input_errors, diag.parser_overflows, diag.framing_errors,
		    diag.rx_buf_errors);
	shell_print(sh, "uplink_lq=%u rssi1=-%u rssi2=-%u snr=%d rf_mode=%u",
		    link.uplink_link_quality, link.uplink_rssi_1, link.uplink_rssi_2,
		    link.uplink_snr, link.rf_mode);
	{
		uint16_t voltage_cv;
		int16_t current_da;
		int8_t remaining_pct;
		uint32_t consumed_mah;

		rdd2_power_get(&voltage_cv, &current_da, &remaining_pct, &consumed_mah);
		shell_print(sh, "batt_cv=%u batt_da=%d batt_pct=%d batt_mah=%u", voltage_cv,
			    current_da, remaining_pct, consumed_mah);
	}
#if defined(CONFIG_RDD2_CRSF_TELEMETRY)
	{
		struct rdd2_crsf_telemetry_counters telem;

		rdd2_crsf_telemetry_counters_get(&telem);
		shell_print(sh,
			    "telem_att=%u telem_status=%u telem_gps=%u telem_mode=%u "
			    "telem_batt=%u telem_batt_pt=%u telem_param=%u telem_rpm=%u "
			    "telem_text=%u telem_bytes=%u",
			    telem.frames[CRSF_TELEM_ENTRY_ATTITUDE],
			    telem.frames[CRSF_TELEM_ENTRY_STATUS],
			    telem.frames[CRSF_TELEM_ENTRY_GPS],
			    telem.frames[CRSF_TELEM_ENTRY_FLIGHT_MODE],
			    telem.frames[CRSF_TELEM_ENTRY_BATTERY],
			    telem.frames[CRSF_TELEM_ENTRY_BATTERY_PASSTHROUGH],
			    telem.frames[CRSF_TELEM_ENTRY_PARAMS],
			    telem.frames[CRSF_TELEM_ENTRY_ESC_RPM], telem.text_frames,
			    telem.bytes);
	}
#endif
	return 0;
}

#if defined(CONFIG_RDD2_CRSF_TELEMETRY)
/*
 * Frame type 0x32 command, receiver sub command 0x10, bind 0x01, sent from the
 * flight controller address to the receiver address. The receiver checks the
 * frame CRC and the destination address, enters bind mode and drops the link;
 * it sends no reply.
 */
static int cmd_crsf_bind(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev = DEVICE_DT_GET(RC_NODE);
	uint8_t payload[CRSF_BIND_COMMAND_LEN];
	size_t len = crsf_encode_bind_command(payload, sizeof(payload));
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ret = input_crsf_send_telemetry(dev, 0x32, payload, len);
	shell_print(sh, "sent type=0x32 payload=%02x %02x %02x %02x %02x ret=%d", payload[0],
		    payload[1], payload[2], payload[3], payload[4], ret);
	shell_print(sh, "receiver drops the link and signals bind mode on its LED until the "
			"transmitter binds (Bind in the receiver Lua script on the radio)");
	return ret;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(crsf_cmds,
			       SHELL_CMD(status, NULL, "Show CRSF UART and frame diagnostics.",
					 cmd_crsf_status),
#if defined(CONFIG_RDD2_CRSF_TELEMETRY)
			       SHELL_CMD(bind, NULL, "Put the receiver into bind mode.",
					 cmd_crsf_bind),
#endif
			       SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(crsf, &crsf_cmds, "CRSF receiver diagnostics.", NULL);
#endif
