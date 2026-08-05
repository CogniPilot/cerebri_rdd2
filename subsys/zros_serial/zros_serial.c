/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zros_serial.h"

#include "topic_bus.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>

/*
 * The wire ids are synapse catalog TopicIds, taken straight from the generated
 * catalog. That is schema, not transport: the ground-side peer in
 * tools/synapse_serial hardcodes the same ids, so the link stays byte-identical
 * whichever local store the firmware happens to keep behind them.
 */
#include <synapse/topic_catalog.h>

#include <zros/private/zros_topic_struct.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>
#include <zros/zros_topic.h>

LOG_MODULE_REGISTER(zros_serial, LOG_LEVEL_INF);

#define ZROS_SERIAL_NODE DT_ALIAS(zros_serial)

/* Caught in the preprocessor so a mistyped or missing alias reports itself
 * instead of surfacing later as an unresolved DEVICE_DT_GET symbol. */
#if !DT_NODE_EXISTS(ZROS_SERIAL_NODE)
#error "CONFIG_RDD2_ZROS_SERIAL needs a \"zros-serial\" devicetree alias naming the UART"
#endif

BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(ZROS_SERIAL_NODE),
	     "the \"zros-serial\" devicetree alias names a disabled node");

#define MAX_PAYLOAD CONFIG_RDD2_ZROS_SERIAL_MAX_PAYLOAD
#define FRAME_MAX                                                                                  \
	(RDD2_ZROS_SERIAL_HEADER_SIZE + MAX_PAYLOAD + RDD2_ZROS_SERIAL_TRAILER_SIZE)

/* The CRC covers everything between the sync word and the trailer. */
#define CRC_START   2U
#define HEADER_TAIL (RDD2_ZROS_SERIAL_HEADER_SIZE - CRC_START)

/*
 * rx_byte() holds a frame-sized replay buffer and is re-entered once, so the
 * parser alone needs two of them plus the drain buffer. The Kconfig ranges
 * allow a large payload against a small stack, which would overflow rather
 * than fail to build.
 */
BUILD_ASSERT(CONFIG_RDD2_ZROS_SERIAL_THREAD_STACK_SIZE >
		     2 * (HEADER_TAIL + MAX_PAYLOAD + RDD2_ZROS_SERIAL_TRAILER_SIZE) + 512,
	     "RDD2_ZROS_SERIAL_THREAD_STACK_SIZE too small for this MAX_PAYLOAD");

/*
 * One poll period of line rate must fit the receive ring, or bytes are lost
 * before the thread ever looks at them.
 */
BUILD_ASSERT(CONFIG_RDD2_ZROS_SERIAL_RX_RING_SIZE >=
		     (DT_PROP(ZROS_SERIAL_NODE, current_speed) / 10) *
			     CONFIG_RDD2_ZROS_SERIAL_POLL_MS / 1000,
	     "RDD2_ZROS_SERIAL_RX_RING_SIZE too small for this baud and poll period");

enum rx_state {
	RX_SYNC0 = 0,
	RX_SYNC1,
	RX_HEADER,
	RX_PAYLOAD,
	RX_CRC,
};

struct tx_slot {
	uint32_t last_generation;
	int64_t last_sent_ms;
	bool primed;
};

/*
 * What this link carries, named explicitly rather than inferred from whatever
 * else the firmware happens to declare. A SiK radio is the scarcest link on the
 * vehicle, so the set of topics on it is a deliberate choice; `key` resolves
 * through the synapse catalog at init to the id and payload size both ends
 * agree on.
 *
 * `tx` and `rx` are independent, not two ends of one direction. A topic the
 * ground station injects is still a topic the ground station wants back: the
 * fix a mocap bridge supplies is the only position the vehicle holds, so it
 * must be telemetered like any other, and the returned frame doubles as proof
 * the injected one was accepted rather than counted as an error.
 *
 * Only inbound entries need a publisher, and every entry is a bare
 * fixed-layout struct, so moving a payload is an identity copy in both
 * directions.
 */
struct serial_topic {
	const char *key;
	struct zros_topic *topic;
	/* Streamed out as telemetry. */
	bool tx;
	/* Accepted inbound; the transport is then the topic's publisher. */
	bool rx;
	struct zros_pub *pub;
	void *msg;
	/* Resolved from the catalog at init. */
	uint16_t id;
	uint16_t payload_size;
};

#if defined(CONFIG_RDD2_GNSS_SOURCE_RADIO)
#define GNSS_RX true
static struct zros_pub g_gnss_pub;
static synapse_topic_GnssFixData_t g_gnss_msg;
#define GNSS_PUB &g_gnss_pub
#define GNSS_MSG &g_gnss_msg
#else
#define GNSS_RX false
#define GNSS_PUB NULL
#define GNSS_MSG NULL
#endif

static struct serial_topic g_topics[] = {
	{.key = "health", .topic = &topic_vehicle_health, .tx = true},
	{.key = "att", .topic = &topic_attitude_estimate, .tx = true},
	{.key = "att_sp", .topic = &topic_attitude_command, .tx = true},
	{.key = "pwm", .topic = &topic_pwm_signal_outputs, .tx = true},
	{.key = "loop", .topic = &topic_control_loop_metrics, .tx = true},
	/* Outbound in both builds: telemetered when the onboard receiver
	 * produces it, echoed back when a ground station injects it. */
	{.key = "gnss",
	 .topic = &topic_gnss_fix,
	 .tx = true,
	 .rx = GNSS_RX,
	 .pub = GNSS_PUB,
	 .msg = GNSS_MSG},
};

#define TOPIC_COUNT ARRAY_SIZE(g_topics)

static struct zros_node g_node;

static const struct device *const g_uart = DEVICE_DT_GET(ZROS_SERIAL_NODE);

static uint8_t g_rx_ring_buf[CONFIG_RDD2_ZROS_SERIAL_RX_RING_SIZE];
static uint8_t g_tx_ring_buf[CONFIG_RDD2_ZROS_SERIAL_TX_RING_SIZE];
static struct ring_buf g_rx_ring;
static struct ring_buf g_tx_ring;

static K_THREAD_STACK_DEFINE(g_stack, CONFIG_RDD2_ZROS_SERIAL_THREAD_STACK_SIZE);
static struct k_thread g_thread;

/* Only the transport thread writes these, except rx_ring_overrun which only
 * the ISR writes. No counter has two writers, so no locking is needed. */
static struct rdd2_zros_serial_stats g_stats;
static bool g_ready;
static int g_init_rc;
static atomic_t g_paused;

/* Receive frame assembly. Touched only by the transport thread. */
static enum rx_state g_rx_state;
static uint8_t g_rx_header[HEADER_TAIL];
static uint8_t g_rx_payload[MAX_PAYLOAD];
static uint16_t g_rx_pos;
static uint16_t g_rx_len;
static uint16_t g_rx_topic_id;
static uint8_t g_rx_crc[RDD2_ZROS_SERIAL_TRAILER_SIZE];

static struct tx_slot g_tx_slots[TOPIC_COUNT];
static uint8_t g_tx_seq;

static uint16_t get_le16(const uint8_t *buf)
{
	return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static void put_le16(uint16_t value, uint8_t *buf)
{
	buf[0] = (uint8_t)(value & 0xffU);
	buf[1] = (uint8_t)((value >> 8) & 0xffU);
}

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	/* uart_irq_update() returns void as of Zephyr main, so the status
	 * refresh and the pending check are separate statements; it still has
	 * to run once per iteration to re-cache the interrupt status. */
	while (true) {
		uart_irq_update(dev);

		if (uart_irq_is_pending(dev) <= 0) {
			break;
		}

		if (uart_irq_rx_ready(dev)) {
			uint8_t buf[32];
			int read = uart_fifo_read(dev, buf, sizeof(buf));

			if (read > 0) {
				uint32_t stored = ring_buf_put(&g_rx_ring, buf, (uint32_t)read);

				if (stored < (uint32_t)read) {
					g_stats.rx_ring_overrun += (uint32_t)read - stored;
				}
			}
		}

		if (uart_irq_tx_ready(dev)) {
			uint8_t *data;
			uint32_t claimed = ring_buf_get_claim(&g_tx_ring, &data, 32U);
			int written = 0;

			if (claimed > 0U) {
				written = uart_fifo_fill(dev, data, (int)claimed);
			}

			(void)ring_buf_get_finish(&g_tx_ring, written > 0 ? (uint32_t)written : 0U);

			if (claimed == 0U) {
				uart_irq_tx_disable(dev);
			}
		}
	}
}

static void rx_byte(uint8_t byte);

/*
 * A rejected candidate can still contain the real start of a frame: one
 * corrupted length byte must not swallow the frames behind it. Its bytes are
 * rescanned exactly as the reference decoder does. One level deep only, so a
 * rejection during a replay resets instead of nesting and the work any single
 * corrupted byte can cause stays bounded.
 */
static void rx_replay(const uint8_t *bytes, size_t len)
{
	static bool replaying;

	g_rx_state = RX_SYNC0;
	g_rx_pos = 0U;

	if (replaying) {
		return;
	}

	replaying = true;
	for (size_t i = 0U; i < len; i++) {
		rx_byte(bytes[i]);
	}
	replaying = false;
}

static struct serial_topic *topic_by_id(uint16_t id)
{
	for (size_t i = 0U; i < TOPIC_COUNT; i++) {
		if (g_topics[i].id == id) {
			return &g_topics[i];
		}
	}

	return NULL;
}

/*
 * A frame carries no schema of its own: the catalog entry behind topic_id
 * decides what the payload must be. Reject anything this link does not carry
 * inbound rather than letting a short frame into a fixed-layout topic, where
 * the store would keep it and later readers would misread it.
 */
static void rx_deliver(void)
{
	struct serial_topic *entry = topic_by_id(g_rx_topic_id);

	if (entry == NULL) {
		g_stats.rx_unknown_topic++;
		return;
	}

	if (!entry->rx || entry->pub == NULL) {
		g_stats.rx_wrong_direction++;
		return;
	}

	if ((size_t)g_rx_len != entry->payload_size) {
		g_stats.rx_bad_length++;
		return;
	}

	memcpy(entry->msg, g_rx_payload, g_rx_len);
	if (zros_pub_update(entry->pub) != 0) {
		g_stats.rx_bad_length++;
		return;
	}

	g_stats.rx_frames++;
}

/*
 * The sender stamps one wrapping counter across all topics, so a gap means
 * the link dropped a frame rather than that a topic went quiet. Counted only
 * for frames that already passed CRC.
 */
static void rx_track_sequence(uint8_t seq)
{
	static bool have_last;
	static uint8_t last_seq;

	if (have_last && seq != (uint8_t)(last_seq + 1U)) {
		g_stats.rx_seq_gaps++;
	}

	last_seq = seq;
	have_last = true;
}

static void rx_byte(uint8_t byte)
{
	switch (g_rx_state) {
	case RX_SYNC0:
		if (byte == RDD2_ZROS_SERIAL_SYNC0) {
			g_rx_state = RX_SYNC1;
		}
		break;

	case RX_SYNC1:
		if (byte == RDD2_ZROS_SERIAL_SYNC1) {
			g_rx_state = RX_HEADER;
			g_rx_pos = 0U;
		} else if (byte != RDD2_ZROS_SERIAL_SYNC0) {
			g_rx_state = RX_SYNC0;
		}
		break;

	case RX_HEADER:
		g_rx_header[g_rx_pos++] = byte;
		if (g_rx_pos < HEADER_TAIL) {
			break;
		}

		g_rx_len = get_le16(&g_rx_header[0]);
		g_rx_topic_id = get_le16(&g_rx_header[2]);
		if (g_rx_len == 0U || g_rx_len > MAX_PAYLOAD) {
			uint8_t header[HEADER_TAIL];

			g_stats.rx_bad_length++;
			memcpy(header, g_rx_header, sizeof(header));
			rx_replay(header, sizeof(header));
			break;
		}

		g_rx_pos = 0U;
		g_rx_state = RX_PAYLOAD;
		break;

	case RX_PAYLOAD:
		g_rx_payload[g_rx_pos++] = byte;
		if (g_rx_pos >= g_rx_len) {
			g_rx_pos = 0U;
			g_rx_state = RX_CRC;
		}
		break;

	case RX_CRC:
	default: {
		uint16_t crc;

		g_rx_crc[g_rx_pos++] = byte;
		if (g_rx_pos < RDD2_ZROS_SERIAL_TRAILER_SIZE) {
			break;
		}

		crc = crc16_itu_t(RDD2_ZROS_SERIAL_CRC_SEED, g_rx_header, HEADER_TAIL);
		crc = crc16_itu_t(crc, g_rx_payload, g_rx_len);

		if (crc == get_le16(g_rx_crc)) {
			rx_track_sequence(g_rx_header[4]);
			rx_deliver();
			g_rx_state = RX_SYNC0;
			g_rx_pos = 0U;
		} else {
			uint8_t scan[HEADER_TAIL + MAX_PAYLOAD + RDD2_ZROS_SERIAL_TRAILER_SIZE];
			size_t n = HEADER_TAIL;

			g_stats.rx_crc_errors++;
			memcpy(scan, g_rx_header, HEADER_TAIL);
			memcpy(&scan[n], g_rx_payload, g_rx_len);
			n += g_rx_len;
			memcpy(&scan[n], g_rx_crc, RDD2_ZROS_SERIAL_TRAILER_SIZE);
			n += RDD2_ZROS_SERIAL_TRAILER_SIZE;
			rx_replay(scan, n);
		}
		break;
	}
	}
}

static void rx_drain(void)
{
	uint8_t buf[64];
	uint32_t read;

	while ((read = ring_buf_get(&g_rx_ring, buf, sizeof(buf))) > 0U) {
		for (uint32_t i = 0U; i < read; i++) {
			rx_byte(buf[i]);
		}
	}
}

static void tx_topic_if_due(struct serial_topic *entry, struct tx_slot *slot, int64_t now_ms)
{
	uint8_t frame[FRAME_MAX];
	uint32_t generation = rdd2_topic_generation(entry->topic);
	size_t len = entry->payload_size;
	size_t frame_len;
	uint16_t crc;

	if (generation == 0U || generation == slot->last_generation) {
		return;
	}

	if (slot->primed &&
	    (now_ms - slot->last_sent_ms) < CONFIG_RDD2_ZROS_SERIAL_TX_MIN_INTERVAL_MS) {
		return;
	}

	/* Read straight into the frame at the payload offset, so no staging
	 * copy stands between the topic and the wire. The double-buffer read
	 * retries internally until the copy is consistent, so the payload is
	 * always one whole sample. */
	if (zros_topic_read(entry->topic, &frame[RDD2_ZROS_SERIAL_HEADER_SIZE]) != 0) {
		return;
	}

	frame[0] = RDD2_ZROS_SERIAL_SYNC0;
	frame[1] = RDD2_ZROS_SERIAL_SYNC1;
	put_le16((uint16_t)len, &frame[2]);
	put_le16(entry->id, &frame[4]);
	frame[6] = g_tx_seq++;
	frame[7] = 0U;

	crc = crc16_itu_t(RDD2_ZROS_SERIAL_CRC_SEED, &frame[CRC_START], HEADER_TAIL + len);
	put_le16(crc, &frame[RDD2_ZROS_SERIAL_HEADER_SIZE + len]);

	frame_len = RDD2_ZROS_SERIAL_HEADER_SIZE + len + RDD2_ZROS_SERIAL_TRAILER_SIZE;

	/* Never block the transport thread on a slow or unplugged radio: a
	 * frame that does not fit is dropped. The sequence number is still
	 * consumed so the far end sees the loss as a gap. */
	if (ring_buf_space_get(&g_tx_ring) < frame_len) {
		g_stats.tx_dropped++;
	} else {
		(void)ring_buf_put(&g_tx_ring, frame, (uint32_t)frame_len);
		g_stats.tx_frames++;
		uart_irq_tx_enable(g_uart);
	}

	/*
	 * The generation sampled before the read, not after. A publish landing
	 * during the read makes the read retry and return the newer sample
	 * against this older generation, so the next poll resends it: one
	 * duplicate frame, a fifth of a second apart on this link. Sampling
	 * after would instead record a generation the frame does not hold and
	 * leave the ground station on stale state until the topic next moves.
	 */
	slot->last_generation = generation;
	slot->last_sent_ms = now_ms;
	slot->primed = true;
}

static void tx_scan(void)
{
	int64_t now_ms = k_uptime_get();

	for (size_t i = 0U; i < TOPIC_COUNT; i++) {
		/* An inbound topic's store is filled by rx_deliver and its
		 * generation moves on receipt, so change detection works the
		 * same for it: it goes out at the TX interval, not at the rate
		 * the ground station injects. */
		if (g_topics[i].tx) {
			tx_topic_if_due(&g_topics[i], &g_tx_slots[i], now_ms);
		}
	}
}

static void zros_serial_thread(void *arg0, void *arg1, void *arg2)
{
	ARG_UNUSED(arg0);
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);

	while (true) {
		if (atomic_get(&g_paused) == 0) {
			rx_drain();
			tx_scan();
		}
		k_sleep(K_MSEC(CONFIG_RDD2_ZROS_SERIAL_POLL_MS));
	}
}

/*
 * A port scan reconfigures the baud and polls for bytes, neither of which can
 * be done safely under a live transport on the same UART:
 *
 *  - uart_irq_tx_enable() from this thread and uart_irq_rx_disable() from the
 *    shell thread are both unlocked read-modify-writes of the same peripheral
 *    CTRL register, and this thread is the higher priority one.
 *  - On the NXP LPUART, irq_rx_ready() reports the data-register-full flag
 *    without consulting the interrupt enable, so any TX interrupt re-enters
 *    the ISR and its RX branch drains bytes the scan is trying to count.
 *
 * Parking is therefore not politeness but correctness. The TX ring is dropped
 * rather than held, because uart_configure() resets the peripheral and would
 * otherwise emit the stranded remainder as a corrupt prefix afterwards.
 */
void rdd2_zros_serial_pause(bool pause)
{
	if (!g_ready) {
		return;
	}

	if (!pause) {
		atomic_set(&g_paused, 0);
		return;
	}

	atomic_set(&g_paused, 1);
	/* Outlast any poll already in flight before touching the port. */
	k_sleep(K_MSEC(2 * CONFIG_RDD2_ZROS_SERIAL_POLL_MS + 1));
	uart_irq_tx_disable(g_uart);
	ring_buf_reset(&g_tx_ring);
}

bool rdd2_zros_serial_ready(void)
{
	return g_ready;
}

int rdd2_zros_serial_init_result(void)
{
	return g_init_rc;
}

bool rdd2_zros_serial_owns(const struct device *dev)
{
	return g_ready && dev == g_uart;
}

void rdd2_zros_serial_stats_get(struct rdd2_zros_serial_stats *stats)
{
	if (stats != NULL) {
		*stats = g_stats;
	}
}

const char *rdd2_zros_serial_port_name(void)
{
	return DT_NODE_FULL_NAME(ZROS_SERIAL_NODE);
}

uint32_t rdd2_zros_serial_baud(void)
{
	return DT_PROP(ZROS_SERIAL_NODE, current_speed);
}

/*
 * Zephyr does not abort the boot when a non-device SYS_INIT hook fails, so the
 * reason is kept for `zros_serial status` instead of only reaching the log.
 */
/*
 * Bind the table to the catalog. A key the pinned synapse_fbs release does not
 * define, a payload this link cannot carry, or a local struct that disagrees
 * with the catalog size are all contract errors: the far end decodes by id and
 * would misread the bytes, so the link refuses to come up rather than emit
 * frames no peer can trust.
 */
static int topics_resolve(void)
{
	for (size_t i = 0U; i < TOPIC_COUNT; i++) {
		struct serial_topic *entry = &g_topics[i];
		const synapse_topic_info_t *info = synapse_topic_by_key(entry->key);

		if (info == NULL || !info->fixed_layout) {
			LOG_ERR("\"%s\" is not a fixed-layout catalog topic", entry->key);
			return -ENOENT;
		}

		if (info->payload_size != (size_t)entry->topic->_size) {
			LOG_ERR("\"%s\" is %u bytes on the bus, %u in the catalog", entry->key,
				(unsigned int)entry->topic->_size,
				(unsigned int)info->payload_size);
			return -EINVAL;
		}

		if (info->payload_size > MAX_PAYLOAD) {
			LOG_ERR("\"%s\" is %u bytes, over the %u byte frame limit", entry->key,
				(unsigned int)info->payload_size, (unsigned int)MAX_PAYLOAD);
			return -EMSGSIZE;
		}

		entry->id = info->id;
		entry->payload_size = (uint16_t)info->payload_size;
	}

	return 0;
}

/* Inbound topics are published by this transport, so it registers as their
 * publisher before the thread that fills them exists. */
static int publishers_init(void)
{
	bool any = false;
	int rc;

	for (size_t i = 0U; i < TOPIC_COUNT; i++) {
		struct serial_topic *entry = &g_topics[i];

		if (!entry->rx) {
			continue;
		}

		if (!any) {
			zros_node_init(&g_node, "rdd2_zros_serial");
			any = true;
		}

		rc = zros_pub_init(entry->pub, &g_node, entry->topic, entry->msg);
		if (rc != 0) {
			LOG_ERR("publisher for \"%s\" failed: %d", entry->key, rc);
			return rc;
		}
	}

	return 0;
}

static int zros_serial_init(void)
{
	int rc;

	if (!device_is_ready(g_uart)) {
		LOG_ERR("%s not ready", rdd2_zros_serial_port_name());
		g_init_rc = -ENODEV;
		return g_init_rc;
	}

	rc = topics_resolve();
	if (rc != 0) {
		g_init_rc = rc;
		return rc;
	}

	rc = publishers_init();
	if (rc != 0) {
		g_init_rc = rc;
		return rc;
	}

	ring_buf_init(&g_rx_ring, sizeof(g_rx_ring_buf), g_rx_ring_buf);
	ring_buf_init(&g_tx_ring, sizeof(g_tx_ring_buf), g_tx_ring_buf);

	rc = uart_irq_callback_user_data_set(g_uart, uart_isr, NULL);
	if (rc != 0) {
		LOG_ERR("uart callback setup failed: %d", rc);
		g_init_rc = rc;
		return rc;
	}

	uart_irq_rx_enable(g_uart);

	k_thread_create(&g_thread, g_stack, K_THREAD_STACK_SIZEOF(g_stack), zros_serial_thread,
			NULL, NULL, NULL, CONFIG_RDD2_ZROS_SERIAL_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&g_thread, "zros_serial");
	g_ready = true;

	LOG_INF("zros serial on %s at %u baud, %u topics, tx interval %u ms",
		rdd2_zros_serial_port_name(), rdd2_zros_serial_baud(), (unsigned int)TOPIC_COUNT,
		CONFIG_RDD2_ZROS_SERIAL_TX_MIN_INTERVAL_MS);

	return 0;
}

SYS_INIT(zros_serial_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
