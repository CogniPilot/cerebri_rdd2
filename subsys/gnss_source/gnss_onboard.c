/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Onboard u-blox receiver -> synapse gnss topic, read as UBX.
 *
 * Boot sends one bounded UBX-CFG-VALSET transaction that puts the receiver into
 * a known RAM configuration, then the same low-priority thread decodes
 * UBX-NAV-PVT. NAV-PVT alone carries every field the GnssFix contract wants,
 * including the accuracy estimates NMEA has no way to express.
 *
 * A dedicated thread rather than a workqueue is deliberate. SPEC_0005 forbids
 * GNSS threads justified only by convenience, but the alternative here is the
 * system workqueue, whose negative priority is cooperative and therefore
 * cannot be preempted by the 1600 Hz control loop. This thread is preemptible
 * and sits below it.
 */

#include "gnss_onboard.h"
#include "gnss_m10_protocol.h"
#include "gnss_m10_topic.h"

#include "interfaces/zros_topics.h"

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

BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(GNSS_NODE),
             "the \"gnss\" alias names a disabled node");
BUILD_ASSERT(DT_PROP(GNSS_UART, current_speed) == 115200U,
             "M10 UART and UBX boot configuration must both use 115200 baud");
BUILD_ASSERT(sizeof(struct ubx_nav_pvt) == 92U,
             "unexpected UBX NAV-PVT payload size");

static const struct device *const g_uart = DEVICE_DT_GET(GNSS_UART);

static uint8_t g_ring_buf[CONFIG_RDD2_GNSS_UBX_RX_RING_SIZE];
static struct ring_buf g_ring;
static K_THREAD_STACK_DEFINE(g_stack, CONFIG_RDD2_GNSS_UBX_THREAD_STACK_SIZE);
static struct k_thread g_thread;

static struct rdd2_gnss_onboard_stats g_stats;
static struct rdd2_gnss_m10_parser g_parser;
static struct rdd2_gnss_m10_state g_m10;
static uint8_t g_tx_frame[RDD2_GNSS_M10_FRAME_MAX];
static struct k_spinlock g_state_lock;
static struct rdd2_gnss_m10_publication_state g_publication;

/* This build is the sole producer of the fix: the Kconfig choice compiles in
 * either this reader or the transport's inbound path, never both, so the
 * single-publisher topic has exactly one registered publisher. */
static struct zros_node g_node;
static struct zros_pub g_pub;
static synapse_topic_GnssFixData_t g_fix;

/* Materialize the shell/status view whenever the protocol state is committed.
 * This keeps the concurrent getter to one fixed-size copy under the lock. */
static void sync_protocol_stats_locked(void) {
  g_stats.config_acks = g_m10.config_acks;
  g_stats.config_naks = g_m10.config_naks;
  g_stats.config_timeouts = g_m10.config_timeouts;
  g_stats.unexpected_acks = g_m10.unexpected_acks;
  g_stats.bad_ack_length = g_m10.bad_ack_length;
  g_stats.rate_errors = g_m10.rate_errors;
  g_stats.last_gap_ms = g_m10.last_gap_ms;
  g_stats.max_gap_ms = g_m10.max_gap_ms;
  g_stats.measured_rate_millihz = g_m10.measured_rate_millihz;
  g_stats.last_hacc_mm = g_m10.horizontal_accuracy_mm;
  g_stats.last_vacc_mm = g_m10.vertical_accuracy_mm;
  g_stats.last_sacc_mm_s = g_m10.velocity_accuracy_mm_s;
  g_stats.config_status = g_m10.config_status;
  g_stats.config_step = g_m10.config_step;
  g_stats.config_attempt = g_m10.attempts_this_step;
  g_stats.stable_samples = g_m10.stable_samples;
  g_stats.fix_accepted = g_m10.fix_accepted;
  g_stats.accuracy_accepted = g_m10.accuracy_accepted;
  g_stats.configured = g_m10.config_status == RDD2_GNSS_M10_CONFIG_CONFIGURED;
  g_stats.ready = g_m10.ready;
}

void rdd2_gnss_onboard_stats_get(struct rdd2_gnss_onboard_stats *stats) {
  if (stats != NULL) {
    int64_t now_ms = k_uptime_get();
    k_spinlock_key_t key = k_spin_lock(&g_state_lock);

    *stats = g_stats;
    k_spin_unlock(&g_state_lock, key);
    /* Keep shell readiness truthful even if this lower-priority thread
     * has not had a release to commit the age transition yet. */
    if (stats->ready &&
        (stats->last_sample_ms < 0 || now_ms < stats->last_sample_ms ||
         now_ms - stats->last_sample_ms > RDD2_GNSS_M10_RECENT_MS)) {
      stats->ready = false;
    }
  }
}

bool rdd2_gnss_onboard_ready_get(void) {
  int64_t now_ms = k_uptime_get();
  k_spinlock_key_t key = k_spin_lock(&g_state_lock);
  bool ready = rdd2_gnss_m10_ready_at(&g_m10, now_ms);

  k_spin_unlock(&g_state_lock, key);
  return ready;
}

static uint16_t saturate_u16(uint32_t value) {
  return (uint16_t)MIN(value, 65535U);
}

static void uart_isr(const struct device *dev, void *user_data) {
  ARG_UNUSED(user_data);

  /* uart_irq_update() returns void as of Zephyr main, so the status
   * refresh and the pending check are separate statements; it still has
   * to run once per iteration to re-cache the interrupt status. */
  while (true) {
    uint8_t buf[32];
    int read;

    uart_irq_update(dev);

    if (uart_irq_is_pending(dev) <= 0) {
      break;
    }

    if (!uart_irq_rx_ready(dev)) {
      continue;
    }

    read = uart_fifo_read(dev, buf, sizeof(buf));
    if (read > 0) {
      uint32_t stored = ring_buf_put(&g_ring, buf, (uint32_t)read);

      if (stored < (uint32_t)read) {
        k_spinlock_key_t key = k_spin_lock(&g_state_lock);

        g_stats.ring_overrun += (uint32_t)read - stored;
        k_spin_unlock(&g_state_lock, key);
      }
    }
  }
}

static synapse_types_GnssFixType_enum_t
fix_type_from(enum rdd2_gnss_m10_fix fix) {
  switch (fix) {
  case RDD2_GNSS_M10_FIX_DEAD_RECKONING:
    return synapse_types_GnssFixType_DeadReckoning;
  case RDD2_GNSS_M10_FIX_2D:
    return synapse_types_GnssFixType_Fix2d;
  case RDD2_GNSS_M10_FIX_3D:
    return synapse_types_GnssFixType_Fix3d;
  case RDD2_GNSS_M10_FIX_DGNSS:
    return synapse_types_GnssFixType_Dgnss;
  case RDD2_GNSS_M10_FIX_RTK_FLOAT:
    return synapse_types_GnssFixType_RtkFloat;
  case RDD2_GNSS_M10_FIX_RTK_FIXED:
    return synapse_types_GnssFixType_RtkFixed;
  case RDD2_GNSS_M10_FIX_TIME_ONLY:
    return synapse_types_GnssFixType_TimeOnly;
  case RDD2_GNSS_M10_FIX_NONE:
  default:
    return synapse_types_GnssFixType_NoFix;
  }
}

static bool publish_result(const synapse_topic_GnssFixData_t *fix) {
  g_fix = *fix;
  if (zros_pub_update(&g_pub) != 0) {
    k_spinlock_key_t key = k_spin_lock(&g_state_lock);

    g_stats.publish_failed++;
    k_spin_unlock(&g_state_lock, key);
    return false;
  }
  {
    k_spinlock_key_t key = k_spin_lock(&g_state_lock);

    g_stats.published++;
    k_spin_unlock(&g_state_lock, key);
  }
  return true;
}

static bool publish_fail_closed(int64_t now_ms) {
  synapse_topic_GnssFixData_t fix;

  rdd2_gnss_m10_topic_invalidate(now_ms, &fix);
  return publish_result(&fix);
}

static void process_frame(const struct rdd2_gnss_m10_frame *frame,
                          int64_t now_ms) {
  enum rdd2_gnss_m10_frame_kind kind;
  enum rdd2_gnss_m10_fix receiver_fix = RDD2_GNSS_M10_FIX_NONE;
  synapse_topic_GnssFixData_t fix;
  struct rdd2_gnss_m10_state next_state;
  bool topic_usable = false;
  k_spinlock_key_t key = k_spin_lock(&g_state_lock);

  next_state = g_m10;
  k_spin_unlock(&g_state_lock, key);

  kind = rdd2_gnss_m10_handle_frame(&next_state, frame, now_ms);
  if (kind == RDD2_GNSS_M10_FRAME_PVT) {
    receiver_fix = next_state.fix;
    rdd2_gnss_m10_topic_build((const struct ubx_nav_pvt *)frame->payload,
                              &next_state, now_ms, &fix);
    /* Publication is part of readiness. Commit the gate closed before
     * touching ZROS so a higher-priority reader can never observe ready
     * after a rejected sample or before a newly usable sample lands. */
    topic_usable = rdd2_gnss_m10_publication_barrier(&next_state, &fix);
  }

  key = k_spin_lock(&g_state_lock);
  g_m10 = next_state;
  g_stats.frames++;
  if (kind == RDD2_GNSS_M10_FRAME_BAD_LENGTH) {
    g_stats.bad_length++;
  } else if (kind != RDD2_GNSS_M10_FRAME_PVT) {
    g_stats.other_frames++;
  } else {
    const struct ubx_nav_pvt *pvt = (const struct ubx_nav_pvt *)frame->payload;

    g_stats.samples++;
    g_stats.last_sample_ms = now_ms;
    g_stats.last_fix_type = fix_type_from(receiver_fix);
    g_stats.last_satellites = pvt->nav.num_sv;
    g_stats.last_hdop_centi = saturate_u16(pvt->nav.pdop);
  }
  sync_protocol_stats_locked();
  k_spin_unlock(&g_state_lock, key);

  if (kind == RDD2_GNSS_M10_FRAME_PVT) {
    bool published = publish_result(&fix);

    key = k_spin_lock(&g_state_lock);
    rdd2_gnss_m10_publication_complete(&g_publication, &g_m10, topic_usable,
                                       published);
    sync_protocol_stats_locked();
    k_spin_unlock(&g_state_lock, key);
  }
}

static void rx_byte(uint8_t byte) {
  struct rdd2_gnss_m10_frame frame;
  enum rdd2_gnss_m10_rx_event event =
      rdd2_gnss_m10_parser_feed(&g_parser, byte, &frame);

  if (event == RDD2_GNSS_M10_RX_FRAME) {
    process_frame(&frame, k_uptime_get());
  } else if (event == RDD2_GNSS_M10_RX_CHECKSUM_ERROR) {
    k_spinlock_key_t key = k_spin_lock(&g_state_lock);

    g_stats.checksum_errors++;
    k_spin_unlock(&g_state_lock, key);
  } else if (event == RDD2_GNSS_M10_RX_OVERSIZE) {
    k_spinlock_key_t key = k_spin_lock(&g_state_lock);

    g_stats.oversize++;
    k_spin_unlock(&g_state_lock, key);
  }
}

static void advance_configuration(int64_t now_ms) {
  int length;
  struct rdd2_gnss_m10_state next_state;
  k_spinlock_key_t key = k_spin_lock(&g_state_lock);

  next_state = g_m10;
  k_spin_unlock(&g_state_lock, key);

  /* Frame preparation includes payload encoding and checksum work. Keep it
   * outside the IRQ-disabling spinlock; only the resulting state commit is
   * serialized. The GNSS thread is the sole state writer. */
  length = rdd2_gnss_m10_prepare_config(&next_state, now_ms, g_tx_frame,
                                        sizeof(g_tx_frame));
  key = k_spin_lock(&g_state_lock);
  g_m10 = next_state;
  sync_protocol_stats_locked();
  k_spin_unlock(&g_state_lock, key);

  if (length <= 0) {
    return;
  }

  /* uart_poll_out can busy-wait for FIFO space, but this thread is
   * preemptible and below every flight-control process. Configuration is
   * boot-only and the one bounded frame is 152 bytes. Zephyr's poll-out API
   * has no error result; a missing receiver acceptance is detected by the
   * bounded ACK timeout and fails closed after three identical attempts. */
  for (int i = 0; i < length; i++) {
    uart_poll_out(g_uart, g_tx_frame[i]);
  }
}

static void gnss_thread(void *arg0, void *arg1, void *arg2) {
  uint8_t buf[64];

  ARG_UNUSED(arg0);
  ARG_UNUSED(arg1);
  ARG_UNUSED(arg2);

  while (true) {
    uint32_t read;
    int64_t now_ms;
    bool was_ready;
    struct rdd2_gnss_m10_state next_state;

    while ((read = ring_buf_get(&g_ring, buf, sizeof(buf))) > 0U) {
      for (uint32_t i = 0U; i < read; i++) {
        rx_byte(buf[i]);
      }
    }
    now_ms = k_uptime_get();
    k_spinlock_key_t key = k_spin_lock(&g_state_lock);

    next_state = g_m10;
    k_spin_unlock(&g_state_lock, key);
    was_ready = next_state.ready;
    rdd2_gnss_m10_tick(&next_state, now_ms);
    key = k_spin_lock(&g_state_lock);
    g_m10 = next_state;
    sync_protocol_stats_locked();
    k_spin_unlock(&g_state_lock, key);
    if (rdd2_gnss_m10_invalidation_due(&g_publication, was_ready,
                                       &next_state)) {
      rdd2_gnss_m10_publication_result(&g_publication,
                                       publish_fail_closed(now_ms));
    }
    advance_configuration(now_ms);
    k_sleep(K_MSEC(CONFIG_RDD2_GNSS_UBX_POLL_MS));
  }
}

static int gnss_onboard_init(void) {
  int rc;

  if (!device_is_ready(g_uart)) {
    LOG_ERR("%s not ready", DT_NODE_FULL_NAME(GNSS_UART));
    return -ENODEV;
  }

  ring_buf_init(&g_ring, sizeof(g_ring_buf), g_ring_buf);
  rdd2_gnss_m10_parser_init(&g_parser);
  rdd2_gnss_m10_state_init(&g_m10);
  g_stats.last_sample_ms = -1;
  sync_protocol_stats_locked();
  /* The publisher's initial retained value is fail-closed even before the
   * first UART byte or scheduler release. */
  rdd2_gnss_m10_topic_invalidate(k_uptime_get(), &g_fix);

  /* Registered before the reader thread exists, so the first decoded
   * frame cannot reach an unpublishable topic. */
  zros_node_init(&g_node, "rdd2_gnss");
  rc = zros_pub_init(&g_pub, &g_node, &topic_gnss_fix, &g_fix);
  if (rc != 0) {
    LOG_ERR("gnss publisher init failed: %d", rc);
    return rc;
  }
  /* Create an actual initial topic generation, rather than relying only on
   * zero-initialized storage that consumers correctly treat as no sample. */
  rdd2_gnss_m10_publication_result(&g_publication, publish_result(&g_fix));

  rc = uart_irq_callback_user_data_set(g_uart, uart_isr, NULL);
  if (rc != 0) {
    LOG_ERR("uart callback setup failed: %d", rc);
    return rc;
  }

  uart_irq_rx_enable(g_uart);

  k_thread_create(&g_thread, g_stack, K_THREAD_STACK_SIZEOF(g_stack),
                  gnss_thread, NULL, NULL, NULL,
                  CONFIG_RDD2_GNSS_UBX_THREAD_PRIORITY, 0, K_NO_WAIT);
  k_thread_name_set(&g_thread, "gnss_ubx");

  LOG_INF("M10 UBX configure/read on %s at %u baud, NAV-PVT %u Hz",
          DT_NODE_FULL_NAME(GNSS_UART),
          (unsigned int)DT_PROP(GNSS_UART, current_speed),
          (unsigned int)RDD2_GNSS_M10_TARGET_RATE_HZ);

  return 0;
}

SYS_INIT(gnss_onboard_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
