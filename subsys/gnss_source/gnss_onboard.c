/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Onboard u-blox receiver -> synapse gnss topic, read as UBX.
 *
 * The module streams UBX-NAV-PVT unprompted, so this is receive-only: it sends
 * the receiver nothing and configures nothing, which keeps it working whatever
 * output rate the module happens to be set to. NAV-PVT alone carries every
 * field the GnssFix contract wants, including the accuracy estimates NMEA has
 * no way to express.
 *
 * A dedicated thread rather than a workqueue is deliberate. SPEC_0005 forbids
 * GNSS threads justified only by convenience, but the alternative here is the
 * system workqueue, whose negative priority is cooperative and therefore
 * cannot be preempted by the 1600 Hz control loop. This thread is preemptible
 * and sits below it.
 */

#include "gnss_onboard.h"

#include "topic_bus.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/modem/ubx/protocol.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>

#include <zros/zros_node.h>
#include <zros/zros_pub.h>

LOG_MODULE_REGISTER(gnss_onboard, LOG_LEVEL_INF);

#define GNSS_NODE DT_ALIAS(gnss)
#define GNSS_UART DT_PARENT(GNSS_NODE)

#if !DT_NODE_EXISTS(GNSS_NODE)
#error "RDD2_GNSS_SOURCE_ONBOARD needs a \"gnss\" devicetree alias"
#endif

BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(GNSS_NODE), "the \"gnss\" alias names a disabled node");
BUILD_ASSERT(sizeof(struct ubx_nav_pvt) == 92U, "unexpected UBX NAV-PVT payload size");

#define UBX_HEADER_SIZE  6U  /* sync(2) class id len(2) */
#define UBX_MAX_PAYLOAD  CONFIG_RDD2_GNSS_UBX_MAX_PAYLOAD
#define UBX_NAV_PVT_ID   0x07U

/* The schema wants "unusable" rather than a zero that reads as perfect. */
#define ACCURACY_UNKNOWN 65535U

/* Below this the receiver's heading of motion is noise rather than a course. */
#define COURSE_VALID_MIN_MM_S 150

enum rx_state {
	RX_SYNC1 = 0,
	RX_SYNC2,
	RX_HEADER,
	RX_PAYLOAD,
	RX_CHECKSUM,
};

static const struct device *const g_uart = DEVICE_DT_GET(GNSS_UART);

static uint8_t g_ring_buf[CONFIG_RDD2_GNSS_UBX_RX_RING_SIZE];
static struct ring_buf g_ring;
static K_THREAD_STACK_DEFINE(g_stack, CONFIG_RDD2_GNSS_UBX_THREAD_STACK_SIZE);
static struct k_thread g_thread;

static struct rdd2_gnss_onboard_stats g_stats;

/* This build is the sole producer of the fix: the Kconfig choice compiles in
 * either this reader or the transport's inbound path, never both, so the
 * single-publisher topic has exactly one registered publisher. */
static struct zros_node g_node;
static struct zros_pub g_pub;
static synapse_topic_GnssFixData_t g_fix;

static enum rx_state g_state;
static uint8_t g_header[4]; /* class, id, len lo, len hi */
static uint8_t g_payload[UBX_MAX_PAYLOAD];
static uint8_t g_checksum[2];
static uint16_t g_pos;
static uint16_t g_len;

void rdd2_gnss_onboard_stats_get(struct rdd2_gnss_onboard_stats *stats)
{
	if (stats != NULL) {
		*stats = g_stats;
	}
}

static uint16_t saturate_u16(uint32_t value)
{
	return (uint16_t)MIN(value, 65535U);
}

static int16_t clamp_i16(int32_t value)
{
	return (int16_t)CLAMP(value, INT16_MIN, INT16_MAX);
}

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		uint8_t buf[32];
		int read;

		if (!uart_irq_rx_ready(dev)) {
			continue;
		}

		read = uart_fifo_read(dev, buf, sizeof(buf));
		if (read > 0) {
			uint32_t stored = ring_buf_put(&g_ring, buf, (uint32_t)read);

			if (stored < (uint32_t)read) {
				g_stats.ring_overrun += (uint32_t)read - stored;
			}
		}
	}
}

/* UBX Fletcher-8 over class, id, length and payload. */
static void checksum(const uint8_t *data, size_t len, uint8_t *ck_a, uint8_t *ck_b)
{
	for (size_t i = 0U; i < len; i++) {
		*ck_a = (uint8_t)(*ck_a + data[i]);
		*ck_b = (uint8_t)(*ck_b + *ck_a);
	}
}

static synapse_types_GnssFixType_enum_t fix_type_from(const struct ubx_nav_pvt *pvt)
{
	if ((pvt->flags & UBX_NAV_PVT_FLAGS_GNSS_FIX_OK) == 0U) {
		return synapse_types_GnssFixType_NoFix;
	}
	if ((pvt->flags & UBX_NAV_PVT_FLAGS_GNSS_CARR_SOLN_FIXED) != 0U) {
		return synapse_types_GnssFixType_RtkFixed;
	}
	if ((pvt->flags & UBX_NAV_PVT_FLAGS_GNSS_CARR_SOLN_FLOATING) != 0U) {
		return synapse_types_GnssFixType_RtkFloat;
	}

	switch (pvt->fix_type) {
	case UBX_NAV_FIX_TYPE_2D:
		return synapse_types_GnssFixType_Fix2d;
	case UBX_NAV_FIX_TYPE_3D:
		return synapse_types_GnssFixType_Fix3d;
	case UBX_NAV_FIX_TYPE_GNSS_DR_COMBINED:
	case UBX_NAV_FIX_TYPE_DR:
		return synapse_types_GnssFixType_DeadReckoning;
	case UBX_NAV_FIX_TYPE_TIME_ONLY:
		return synapse_types_GnssFixType_TimeOnly;
	case UBX_NAV_FIX_TYPE_NO_FIX:
	default:
		return synapse_types_GnssFixType_NoFix;
	}
}

/* Days from 1970-01-01, Howard Hinnant's days_from_civil. */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
	int64_t era;
	unsigned yoe;
	unsigned doy;
	unsigned doe;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (unsigned)(y - era * 400);
	doy = (153U * (m + (m > 2 ? -3U : 9U)) + 2U) / 5U + d - 1U;
	doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;

	return era * 146097 + (int64_t)doe - 719468;
}

static void publish_nav_pvt(const struct ubx_nav_pvt *pvt)
{
	synapse_topic_GnssFixData_t fix = {0};
	int32_t ground_speed_mm_s = pvt->nav.ground_speed;
	uint32_t course_cdeg;

	fix.timestamp_us = (uint64_t)k_uptime_get() * 1000ULL;
	fix.latitude_deg_e7 = pvt->nav.latitude;
	fix.longitude_deg_e7 = pvt->nav.longitude;
	fix.altitude_msl_mm = pvt->nav.hmsl;
	fix.altitude_ellipsoid_mm = pvt->nav.height;

	/* The estimates NMEA could not provide. */
	fix.horizontal_accuracy_mm = saturate_u16(pvt->nav.horiz_acc);
	fix.vertical_accuracy_mm = saturate_u16(pvt->nav.vert_acc);
	fix.velocity_accuracy_mm_s = saturate_u16(pvt->nav.speed_acc);
	fix.hdop_centi = saturate_u16(pvt->nav.pdop);
	fix.vdop_centi = saturate_u16(pvt->nav.pdop);

	fix.ground_speed_cm_s = saturate_u16((uint32_t)MAX(ground_speed_mm_s, 0) / 10U);
	/* NED down positive, the contract wants up positive. */
	fix.velocity_up_cm_s = clamp_i16(-pvt->nav.vel_down / 10);

	/* Heading of motion is 1e-5 deg; the contract wants centidegrees. */
	course_cdeg = (uint32_t)(((int64_t)pvt->nav.head_motion / 1000) % 36000 + 36000) % 36000;
	fix.course_over_ground_cdeg = (uint16_t)course_cdeg;

	fix.fix_type = fix_type_from(pvt);
	fix.satellites_used = pvt->nav.num_sv;
	fix.satellites_visible = pvt->nav.num_sv;

	fix.flags = synapse_topic_GnssFixFlags_VelocityUpValid;
	if (ground_speed_mm_s >= COURSE_VALID_MIN_MM_S) {
		fix.flags |= synapse_topic_GnssFixFlags_CourseValid;
	}

	/*
	 * validDate and validTime, and only with a fix: NAV-PVT keeps
	 * reporting a time field once acquired, and a timestamp that silently
	 * stops advancing is worse for a consumer than no timestamp at all.
	 */
	if ((pvt->time.valid & 0x03U) == 0x03U &&
	    fix.fix_type != synapse_types_GnssFixType_NoFix) {
		int64_t days = days_from_civil(pvt->time.year, pvt->time.month, pvt->time.day);

		if (days >= 0) {
			fix.time_unix_us = ((uint64_t)days * 86400ULL +
					    (uint64_t)pvt->time.hour * 3600ULL +
					    (uint64_t)pvt->time.minute * 60ULL +
					    (uint64_t)pvt->time.second) *
					   1000000ULL;
			fix.flags |= synapse_topic_GnssFixFlags_TimeValid;
		}
	}

	g_stats.samples++;
	g_stats.last_sample_ms = k_uptime_get();
	g_stats.last_fix_type = fix.fix_type;
	g_stats.last_satellites = fix.satellites_used;
	g_stats.last_hdop_centi = fix.hdop_centi;
	g_stats.last_hacc_mm = fix.horizontal_accuracy_mm;

	g_fix = fix;
	if (zros_pub_update(&g_pub) != 0) {
		g_stats.publish_failed++;
		return;
	}
	g_stats.published++;
}

static void frame_complete(void)
{
	uint8_t ck_a = 0U;
	uint8_t ck_b = 0U;

	checksum(g_header, sizeof(g_header), &ck_a, &ck_b);
	checksum(g_payload, g_len, &ck_a, &ck_b);

	if (ck_a != g_checksum[0] || ck_b != g_checksum[1]) {
		g_stats.checksum_errors++;
		return;
	}

	g_stats.frames++;

	if (g_header[0] != UBX_CLASS_ID_NAV || g_header[1] != UBX_NAV_PVT_ID) {
		g_stats.other_frames++;
		return;
	}
	if (g_len != sizeof(struct ubx_nav_pvt)) {
		g_stats.bad_length++;
		return;
	}

	publish_nav_pvt((const struct ubx_nav_pvt *)g_payload);
}

static void rx_byte(uint8_t byte)
{
	switch (g_state) {
	case RX_SYNC1:
		if (byte == UBX_PREAMBLE_SYNC_CHAR_1) {
			g_state = RX_SYNC2;
		}
		break;

	case RX_SYNC2:
		if (byte == UBX_PREAMBLE_SYNC_CHAR_2) {
			g_state = RX_HEADER;
			g_pos = 0U;
		} else if (byte != UBX_PREAMBLE_SYNC_CHAR_1) {
			g_state = RX_SYNC1;
		}
		break;

	case RX_HEADER:
		g_header[g_pos++] = byte;
		if (g_pos < sizeof(g_header)) {
			break;
		}
		g_len = (uint16_t)g_header[2] | ((uint16_t)g_header[3] << 8);
		if (g_len > UBX_MAX_PAYLOAD) {
			/* Not ours, and too big to buffer: resynchronise
			 * rather than tie up the parser for a frame we would
			 * discard anyway. */
			g_stats.oversize++;
			g_state = RX_SYNC1;
			break;
		}
		g_pos = 0U;
		g_state = g_len == 0U ? RX_CHECKSUM : RX_PAYLOAD;
		break;

	case RX_PAYLOAD:
		g_payload[g_pos++] = byte;
		if (g_pos >= g_len) {
			g_pos = 0U;
			g_state = RX_CHECKSUM;
		}
		break;

	case RX_CHECKSUM:
	default:
		g_checksum[g_pos++] = byte;
		if (g_pos < sizeof(g_checksum)) {
			break;
		}
		frame_complete();
		g_state = RX_SYNC1;
		g_pos = 0U;
		break;
	}
}

static void gnss_thread(void *arg0, void *arg1, void *arg2)
{
	uint8_t buf[64];

	ARG_UNUSED(arg0);
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);

	while (true) {
		uint32_t read;

		while ((read = ring_buf_get(&g_ring, buf, sizeof(buf))) > 0U) {
			for (uint32_t i = 0U; i < read; i++) {
				rx_byte(buf[i]);
			}
		}
		k_sleep(K_MSEC(CONFIG_RDD2_GNSS_UBX_POLL_MS));
	}
}

static int gnss_onboard_init(void)
{
	int rc;

	if (!device_is_ready(g_uart)) {
		LOG_ERR("%s not ready", DT_NODE_FULL_NAME(GNSS_UART));
		return -ENODEV;
	}

	ring_buf_init(&g_ring, sizeof(g_ring_buf), g_ring_buf);

	/* Registered before the reader thread exists, so the first decoded
	 * frame cannot reach an unpublishable topic. */
	zros_node_init(&g_node, "rdd2_gnss");
	rc = zros_pub_init(&g_pub, &g_node, &topic_gnss_fix, &g_fix);
	if (rc != 0) {
		LOG_ERR("gnss publisher init failed: %d", rc);
		return rc;
	}

	rc = uart_irq_callback_user_data_set(g_uart, uart_isr, NULL);
	if (rc != 0) {
		LOG_ERR("uart callback setup failed: %d", rc);
		return rc;
	}

	uart_irq_rx_enable(g_uart);

	k_thread_create(&g_thread, g_stack, K_THREAD_STACK_SIZEOF(g_stack), gnss_thread, NULL,
			NULL, NULL, CONFIG_RDD2_GNSS_UBX_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&g_thread, "gnss_ubx");

	LOG_INF("ubx reader on %s at %u baud", DT_NODE_FULL_NAME(GNSS_UART),
		(unsigned int)DT_PROP(GNSS_UART, current_speed));

	return 0;
}

SYS_INIT(gnss_onboard_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
