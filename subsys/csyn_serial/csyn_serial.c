/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "csyn_serial.h"

#include <csyn/csyn.h>

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

LOG_MODULE_REGISTER(csyn_serial, LOG_LEVEL_INF);

#define CSYN_SERIAL_NODE DT_ALIAS(csyn_serial)

/* Caught in the preprocessor so a mistyped or missing alias reports itself
 * instead of surfacing later as an unresolved DEVICE_DT_GET symbol. */
#if !DT_NODE_EXISTS(CSYN_SERIAL_NODE)
#error "CONFIG_RDD2_CSYN_SERIAL needs a \"csyn-serial\" devicetree alias naming the UART"
#endif

BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(CSYN_SERIAL_NODE),
	     "the \"csyn-serial\" devicetree alias names a disabled node");

#define MAX_PAYLOAD CONFIG_RDD2_CSYN_SERIAL_MAX_PAYLOAD
#define MAX_TOPICS  CONFIG_RDD2_CSYN_SERIAL_MAX_TOPICS
#define FRAME_MAX                                                                                  \
	(RDD2_CSYN_SERIAL_HEADER_SIZE + MAX_PAYLOAD + RDD2_CSYN_SERIAL_TRAILER_SIZE)

/* The CRC covers everything between the sync word and the trailer. */
#define CRC_START   2U
#define HEADER_TAIL (RDD2_CSYN_SERIAL_HEADER_SIZE - CRC_START)

/*
 * rx_byte() holds a frame-sized replay buffer and is re-entered once, so the
 * parser alone needs two of them plus the drain buffer. The Kconfig ranges
 * allow a large payload against a small stack, which would overflow rather
 * than fail to build.
 */
BUILD_ASSERT(CONFIG_RDD2_CSYN_SERIAL_THREAD_STACK_SIZE >
		     2 * (HEADER_TAIL + MAX_PAYLOAD + RDD2_CSYN_SERIAL_TRAILER_SIZE) + 512,
	     "RDD2_CSYN_SERIAL_THREAD_STACK_SIZE too small for this MAX_PAYLOAD");

/*
 * One poll period of line rate must fit the receive ring, or bytes are lost
 * before the thread ever looks at them.
 */
BUILD_ASSERT(CONFIG_RDD2_CSYN_SERIAL_RX_RING_SIZE >=
		     (DT_PROP(CSYN_SERIAL_NODE, current_speed) / 10) *
			     CONFIG_RDD2_CSYN_SERIAL_POLL_MS / 1000,
	     "RDD2_CSYN_SERIAL_RX_RING_SIZE too small for this baud and poll period");

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

static const struct device *const g_uart = DEVICE_DT_GET(CSYN_SERIAL_NODE);

static uint8_t g_rx_ring_buf[CONFIG_RDD2_CSYN_SERIAL_RX_RING_SIZE];
static uint8_t g_tx_ring_buf[CONFIG_RDD2_CSYN_SERIAL_TX_RING_SIZE];
static struct ring_buf g_rx_ring;
static struct ring_buf g_tx_ring;

static K_THREAD_STACK_DEFINE(g_stack, CONFIG_RDD2_CSYN_SERIAL_THREAD_STACK_SIZE);
static struct k_thread g_thread;

/* Only the transport thread writes these, except rx_ring_overrun which only
 * the ISR writes. No counter has two writers, so no locking is needed. */
static struct rdd2_csyn_serial_stats g_stats;
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
static uint8_t g_rx_crc[RDD2_CSYN_SERIAL_TRAILER_SIZE];

static struct tx_slot g_tx_slots[MAX_TOPICS];
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

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
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

/*
 * A frame carries no schema of its own: the catalog entry behind topic_id
 * decides what the payload must be. Reject anything the local declaration
 * cannot accept rather than letting a short frame into a fixed-layout slot,
 * where csyn_topic_publish would store it and later readers would misread it.
 */
static void rx_deliver(void)
{
	struct csyn_topic *topic = csyn_topic_by_catalog_id(g_rx_topic_id);

	if (topic == NULL || topic->info == NULL) {
		g_stats.rx_unknown_topic++;
		return;
	}

	if (topic->dir != CSYN_DIR_RX) {
		g_stats.rx_wrong_direction++;
		return;
	}

	if (topic->info->fixed_layout && (size_t)g_rx_len != topic->info->payload_size) {
		g_stats.rx_bad_length++;
		return;
	}

	if (!csyn_topic_publish(topic, g_rx_payload, g_rx_len)) {
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
		if (byte == RDD2_CSYN_SERIAL_SYNC0) {
			g_rx_state = RX_SYNC1;
		}
		break;

	case RX_SYNC1:
		if (byte == RDD2_CSYN_SERIAL_SYNC1) {
			g_rx_state = RX_HEADER;
			g_rx_pos = 0U;
		} else if (byte != RDD2_CSYN_SERIAL_SYNC0) {
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
		if (g_rx_pos < RDD2_CSYN_SERIAL_TRAILER_SIZE) {
			break;
		}

		crc = crc16_itu_t(RDD2_CSYN_SERIAL_CRC_SEED, g_rx_header, HEADER_TAIL);
		crc = crc16_itu_t(crc, g_rx_payload, g_rx_len);

		if (crc == get_le16(g_rx_crc)) {
			rx_track_sequence(g_rx_header[4]);
			rx_deliver();
			g_rx_state = RX_SYNC0;
			g_rx_pos = 0U;
		} else {
			uint8_t scan[HEADER_TAIL + MAX_PAYLOAD + RDD2_CSYN_SERIAL_TRAILER_SIZE];
			size_t n = HEADER_TAIL;

			g_stats.rx_crc_errors++;
			memcpy(scan, g_rx_header, HEADER_TAIL);
			memcpy(&scan[n], g_rx_payload, g_rx_len);
			n += g_rx_len;
			memcpy(&scan[n], g_rx_crc, RDD2_CSYN_SERIAL_TRAILER_SIZE);
			n += RDD2_CSYN_SERIAL_TRAILER_SIZE;
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

static void tx_topic_if_due(struct csyn_topic *topic, struct tx_slot *slot, int64_t now_ms)
{
	uint8_t frame[FRAME_MAX];
	uint32_t generation = csyn_topic_generation(topic);
	size_t len = 0U;
	size_t frame_len;
	uint16_t crc;

	if (topic->info == NULL || generation == 0U || generation == slot->last_generation) {
		return;
	}

	if (slot->primed &&
	    (now_ms - slot->last_sent_ms) < CONFIG_RDD2_CSYN_SERIAL_TX_MIN_INTERVAL_MS) {
		return;
	}

	/* Copied straight into the frame at the payload offset, so a topic
	 * larger than this link carries fails here rather than after a
	 * pointless staging copy. */
	/* Take the generation the copy actually returned, not the one sampled
	 * before it: a publish landing in between would otherwise record the
	 * older value and make the next poll resend an identical frame. */
	if (!csyn_topic_copy(topic, &frame[RDD2_CSYN_SERIAL_HEADER_SIZE], MAX_PAYLOAD, &len,
			     &generation) ||
	    len == 0U) {
		/* Retrying the same oversize sample every poll would spin
		 * silently forever, so consume the generation and count it. */
		g_stats.tx_oversize++;
		slot->last_generation = generation;
		return;
	}

	frame[0] = RDD2_CSYN_SERIAL_SYNC0;
	frame[1] = RDD2_CSYN_SERIAL_SYNC1;
	put_le16((uint16_t)len, &frame[2]);
	put_le16(topic->info->id, &frame[4]);
	frame[6] = g_tx_seq++;
	frame[7] = 0U;

	crc = crc16_itu_t(RDD2_CSYN_SERIAL_CRC_SEED, &frame[CRC_START], HEADER_TAIL + len);
	put_le16(crc, &frame[RDD2_CSYN_SERIAL_HEADER_SIZE + len]);

	frame_len = RDD2_CSYN_SERIAL_HEADER_SIZE + len + RDD2_CSYN_SERIAL_TRAILER_SIZE;

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

	slot->last_generation = generation;
	slot->last_sent_ms = now_ms;
	slot->primed = true;
}

/* Resolved once at init: comparing pointers keeps tx_scan free of a string
 * compare per topic per poll, and keeps this transport free of any knowledge
 * of which topic the application wants echoed. */
static struct csyn_topic *g_echo_topic;

static bool tx_eligible(const struct csyn_topic *topic)
{
	/* An echoed topic is inbound, so its store is filled by rx_deliver and
	 * its generation moves on receipt. Change detection therefore works
	 * unchanged: the echo goes out at the TX interval, not at the rate the
	 * ground station injects. */
	return topic->dir == CSYN_DIR_TX || topic == g_echo_topic;
}

static void tx_scan(void)
{
	int64_t now_ms = k_uptime_get();
	size_t count = MIN(csyn_topic_count(), (size_t)MAX_TOPICS);

	for (size_t i = 0U; i < count; i++) {
		struct csyn_topic *topic = csyn_topic_at(i);

		if (topic != NULL && tx_eligible(topic)) {
			tx_topic_if_due(topic, &g_tx_slots[i], now_ms);
		}
	}
}

static void echo_topic_resolve(void)
{
	const char *key = CONFIG_RDD2_CSYN_SERIAL_ECHO_KEY;

	if (key[0] == '\0') {
		return;
	}

	g_echo_topic = csyn_topic_find(key);
	if (g_echo_topic == NULL) {
		LOG_WRN("echo key \"%s\" is not a declared topic; nothing echoed", key);
		return;
	}

	/* Echoing an outbound topic is already what happens, so a key naming
	 * one is a configuration mistake worth reporting rather than a no-op. */
	if (g_echo_topic->dir != CSYN_DIR_RX) {
		LOG_WRN("echo key \"%s\" is not inbound; it is already transmitted", key);
		g_echo_topic = NULL;
		return;
	}

	LOG_INF("echoing inbound topic \"%s\" as telemetry", key);
}

static void csyn_serial_thread(void *arg0, void *arg1, void *arg2)
{
	ARG_UNUSED(arg0);
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);

	while (true) {
		if (atomic_get(&g_paused) == 0) {
			rx_drain();
			tx_scan();
		}
		k_sleep(K_MSEC(CONFIG_RDD2_CSYN_SERIAL_POLL_MS));
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
void rdd2_csyn_serial_pause(bool pause)
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
	k_sleep(K_MSEC(2 * CONFIG_RDD2_CSYN_SERIAL_POLL_MS + 1));
	uart_irq_tx_disable(g_uart);
	ring_buf_reset(&g_tx_ring);
}

bool rdd2_csyn_serial_ready(void)
{
	return g_ready;
}

int rdd2_csyn_serial_init_result(void)
{
	return g_init_rc;
}

bool rdd2_csyn_serial_owns(const struct device *dev)
{
	return g_ready && dev == g_uart;
}

void rdd2_csyn_serial_stats_get(struct rdd2_csyn_serial_stats *stats)
{
	if (stats != NULL) {
		*stats = g_stats;
	}
}

const char *rdd2_csyn_serial_port_name(void)
{
	return DT_NODE_FULL_NAME(CSYN_SERIAL_NODE);
}

uint32_t rdd2_csyn_serial_baud(void)
{
	return DT_PROP(CSYN_SERIAL_NODE, current_speed);
}

/*
 * Zephyr does not abort the boot when a non-device SYS_INIT hook fails, so the
 * reason is kept for `csyn_serial status` instead of only reaching the log.
 */
static int csyn_serial_init(void)
{
	int rc;

	if (!device_is_ready(g_uart)) {
		LOG_ERR("%s not ready", rdd2_csyn_serial_port_name());
		g_init_rc = -ENODEV;
		return g_init_rc;
	}

	if (csyn_topic_count() > MAX_TOPICS) {
		LOG_ERR("too many csyn topics for the serial transport");
		g_init_rc = -ENOMEM;
		return g_init_rc;
	}

	ring_buf_init(&g_rx_ring, sizeof(g_rx_ring_buf), g_rx_ring_buf);
	ring_buf_init(&g_tx_ring, sizeof(g_tx_ring_buf), g_tx_ring_buf);

	rc = uart_irq_callback_user_data_set(g_uart, uart_isr, NULL);
	if (rc != 0) {
		LOG_ERR("uart callback setup failed: %d", rc);
		g_init_rc = rc;
		return rc;
	}

	echo_topic_resolve();

	uart_irq_rx_enable(g_uart);

	k_thread_create(&g_thread, g_stack, K_THREAD_STACK_SIZEOF(g_stack), csyn_serial_thread,
			NULL, NULL, NULL, CONFIG_RDD2_CSYN_SERIAL_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&g_thread, "csyn_serial");
	g_ready = true;

	LOG_INF("csyn serial on %s at %u baud, %u topics, tx interval %u ms",
		rdd2_csyn_serial_port_name(), rdd2_csyn_serial_baud(),
		(unsigned int)csyn_topic_count(), CONFIG_RDD2_CSYN_SERIAL_TX_MIN_INTERVAL_MS);

	return 0;
}

SYS_INIT(csyn_serial_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
