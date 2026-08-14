/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mesh GNSS backend: the system fix arrives over the network instead of a
 * locally wired receiver. The rtk_gnss grandmaster publishes GnssFixData on
 * the catalog "gnss" key, CSyn delivers it as a fixed-layout struct into its
 * lock-free latest-sample store, and this backend identity-copies each newly
 * received sample onto the internal gnss_fix topic as its single publisher.
 *
 * Two robustness behaviors protect the control bus. A generation-aware cadence
 * gate republishes only a newly observed source generation, so a stale sample
 * is never re-emitted. A producer-restart re-seed treats a backward step of
 * the source generation as a fresh producer session and resets local freshness
 * state, so the backend recovers rather than stalling.
 *
 * Like the onboard reader this runs on a dedicated preemptible thread below
 * the control loop. SPEC_0005 forbids GNSS work in the rate loop, and the
 * system workqueue is cooperative and cannot be preempted by the 1600 Hz loop.
 */

#include "gnss_mesh.h"
#include "gnss_m10_protocol.h"

#include "interfaces/zros_topics.h"

#include <errno.h>
#include <string.h>

#include <csyn/csyn.h>

#include <synapse/sensors_reader.h>
#include <synapse/types_reader.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#endif

#include <zros/zros_node.h>
#include <zros/zros_pub.h>

LOG_MODULE_REGISTER(gnss_mesh, LOG_LEVEL_INF);

/* Wire key of the inbound fix. Its final segment resolves through the pinned
 * synapse_fbs catalog to GnssFixData, which is what CSyn validates against the
 * producer's value contract at the wire boundary. */
#define GNSS_MESH_KEY "gnss"

/* This build is the sole producer of gnss_fix: the RDD2_GNSS_SOURCE choice
 * compiles in exactly one backend, so the single-publisher topic has exactly
 * one registered publisher. */
static struct zros_node g_node;
static struct zros_pub g_pub;
static synapse_topic_GnssFixData_t g_fix;

/* Resolved once the CSyn registry is populated. */
static struct csyn_topic *g_csyn;

/* Generation gate state. Written only by the poll thread. */
static uint32_t g_last_generation;
static bool g_observed;

static struct k_spinlock g_lock;
static struct rdd2_gnss_mesh_stats g_stats;

static K_THREAD_STACK_DEFINE(g_stack, CONFIG_RDD2_GNSS_MESH_THREAD_STACK_SIZE);
static struct k_thread g_thread;

static bool fix_type_usable(uint8_t fix_type) {
  return fix_type == synapse_types_GnssFixType_Fix3d ||
         fix_type == synapse_types_GnssFixType_Dgnss ||
         fix_type == synapse_types_GnssFixType_RtkFloat ||
         fix_type == synapse_types_GnssFixType_RtkFixed;
}

/* Position-flight usability of one received fix. Freshness is judged locally
 * from arrival time, not from the fix timestamp, because the mesh producer
 * stamps its own clock domain (gPTP-disciplined when locked). */
static bool fix_usable(const synapse_topic_GnssFixData_t *fix) {
  return fix->timestamp_ns != 0U && fix_type_usable(fix->fix_type) &&
         fix->latitude_deg_e7 >= -INT32_C(900000000) &&
         fix->latitude_deg_e7 <= INT32_C(900000000) &&
         fix->longitude_deg_e7 >= -INT32_C(1800000000) &&
         fix->longitude_deg_e7 <= INT32_C(1800000000) &&
         fix->altitude_msl_mm >= -INT32_C(1000000) &&
         fix->altitude_msl_mm <= INT32_C(20000000) &&
         fix->horizontal_accuracy_mm <= RDD2_GNSS_M10_MAX_HACC_MM &&
         fix->vertical_accuracy_mm <= RDD2_GNSS_M10_MAX_VACC_MM &&
         fix->velocity_accuracy_mm_s <= RDD2_GNSS_M10_MAX_SACC_MM_S;
}

static bool ready_from(const struct rdd2_gnss_mesh_stats *s, int64_t now_ms) {
  return s->usable && s->stable_samples >= RDD2_GNSS_M10_STABLE_SAMPLES &&
         s->last_sample_ms >= 0 && now_ms >= s->last_sample_ms &&
         now_ms - s->last_sample_ms <= RDD2_GNSS_M10_RECENT_MS;
}

/* Commit shell and readiness state for one accepted sample. Held under g_lock. */
static void record_sample_locked(const synapse_topic_GnssFixData_t *fix,
                                 int64_t now_ms, bool usable, bool reseed) {
  if (reseed) {
    g_stats.producer_restarts++;
    g_stats.stable_samples = 0U;
  }
  if (usable) {
    if (g_stats.stable_samples < RDD2_GNSS_M10_STABLE_SAMPLES) {
      g_stats.stable_samples++;
    }
  } else {
    g_stats.stable_samples = 0U;
  }
  g_stats.samples++;
  g_stats.last_sample_ms = now_ms;
  g_stats.last_latitude_deg_e7 = fix->latitude_deg_e7;
  g_stats.last_longitude_deg_e7 = fix->longitude_deg_e7;
  g_stats.last_altitude_msl_mm = fix->altitude_msl_mm;
  g_stats.last_hacc_mm = fix->horizontal_accuracy_mm;
  g_stats.last_fix_type = fix->fix_type;
  g_stats.last_satellites = fix->satellites_used;
  g_stats.usable = usable;
}

static void poll_once(void) {
  synapse_topic_GnssFixData_t fix;
  size_t len = 0U;
  uint32_t generation;
  int64_t now_ms;
  bool reseed;
  bool usable;
  bool published;
  k_spinlock_key_t key;

  if (g_csyn == NULL) {
    g_csyn = csyn_topic_find(GNSS_MESH_KEY);
    if (g_csyn == NULL) {
      return;
    }
    key = k_spin_lock(&g_lock);
    g_stats.topic_found = true;
    k_spin_unlock(&g_lock, key);
  }

  generation = csyn_topic_generation(g_csyn);
  if (generation == 0U) {
    /* No sample received from the producer yet. */
    return;
  }

  /* Cadence gate: act only on a newly observed source generation. */
  if (g_observed && generation == g_last_generation) {
    key = k_spin_lock(&g_lock);
    g_stats.stale_polls++;
    k_spin_unlock(&g_lock, key);
    return;
  }

  /* Producer-restart re-seed: a backward step of the source generation is a
   * fresh producer session, not a reason to stall. */
  reseed = g_observed && generation < g_last_generation;

  if (!csyn_topic_copy(g_csyn, &fix, sizeof(fix), &len, &generation)) {
    key = k_spin_lock(&g_lock);
    g_stats.copy_failed++;
    k_spin_unlock(&g_lock, key);
    return;
  }
  if (len != sizeof(fix)) {
    key = k_spin_lock(&g_lock);
    g_stats.size_mismatch++;
    k_spin_unlock(&g_lock, key);
    return;
  }

  now_ms = k_uptime_get();
  usable = fix_usable(&fix);

  /* Identity copy onto the internal bus. This backend is the topic's sole
   * publisher, so it is the only writer of gnss_fix. */
  g_fix = fix;
  published = zros_pub_update(&g_pub) == 0;

  key = k_spin_lock(&g_lock);
  g_observed = true;
  g_last_generation = generation;
  g_stats.last_generation = generation;
  if (published) {
    g_stats.published++;
  } else {
    g_stats.publish_failed++;
  }
  record_sample_locked(&fix, now_ms, usable && published, reseed);
  k_spin_unlock(&g_lock, key);
}

static void gnss_mesh_thread(void *arg0, void *arg1, void *arg2) {
  ARG_UNUSED(arg0);
  ARG_UNUSED(arg1);
  ARG_UNUSED(arg2);

  while (true) {
    poll_once();
    k_sleep(K_MSEC(CONFIG_RDD2_GNSS_MESH_POLL_MS));
  }
}

void rdd2_gnss_mesh_stats_get(struct rdd2_gnss_mesh_stats *stats) {
  int64_t now_ms;
  k_spinlock_key_t key;

  if (stats == NULL) {
    return;
  }
  now_ms = k_uptime_get();
  key = k_spin_lock(&g_lock);
  *stats = g_stats;
  k_spin_unlock(&g_lock, key);
  /* Recompute readiness at the caller's time so an idle poll thread cannot
   * leave a stale ready true after the fix has aged out. */
  stats->ready = ready_from(stats, now_ms);
}

bool rdd2_gnss_mesh_ready_get(void) {
  struct rdd2_gnss_mesh_stats snapshot;
  int64_t now_ms = k_uptime_get();
  k_spinlock_key_t key = k_spin_lock(&g_lock);

  snapshot = g_stats;
  k_spin_unlock(&g_lock, key);
  return ready_from(&snapshot, now_ms);
}

static int gnss_mesh_init(void) {
  int rc;

  g_stats.last_sample_ms = -1;

  /* Fail-closed retained value before the first mesh sample: NoFix with
   * unusable accuracy sentinels, so a consumer cannot mistake the initial
   * topic state for a position solution. */
  memset(&g_fix, 0, sizeof(g_fix));
  g_fix.fix_type = synapse_types_GnssFixType_NoFix;
  g_fix.time_status = synapse_types_TimeStatus_LocalFreerun;
  g_fix.horizontal_accuracy_mm = RDD2_GNSS_M10_ACCURACY_UNUSABLE;
  g_fix.vertical_accuracy_mm = RDD2_GNSS_M10_ACCURACY_UNUSABLE;
  g_fix.velocity_accuracy_mm_s = RDD2_GNSS_M10_ACCURACY_UNUSABLE;
  g_fix.yaw_accuracy_cdeg = RDD2_GNSS_M10_ACCURACY_UNUSABLE;

  zros_node_init(&g_node, "rdd2_gnss_mesh");
  rc = zros_pub_init(&g_pub, &g_node, &topic_gnss_fix, &g_fix);
  if (rc != 0) {
    LOG_ERR("gnss mesh publisher init failed: %d", rc);
    return rc;
  }
  /* Establish an initial topic generation so consumers see a defined
   * fail-closed sample rather than zero-initialized storage. */
  if (zros_pub_update(&g_pub) != 0) {
    LOG_ERR("gnss mesh initial publish failed");
    return -EIO;
  }

  /* Resolve the inbound topic now. The poll thread re-resolves if the CSyn
   * registry is not yet populated at this init priority. */
  g_csyn = csyn_topic_find(GNSS_MESH_KEY);
  if (g_csyn != NULL) {
    g_stats.topic_found = true;
  } else {
    LOG_WRN("csyn \"%s\" RX topic not resolved at init", GNSS_MESH_KEY);
  }

  k_thread_create(&g_thread, g_stack, K_THREAD_STACK_SIZEOF(g_stack),
                  gnss_mesh_thread, NULL, NULL, NULL,
                  CONFIG_RDD2_GNSS_MESH_THREAD_PRIORITY, 0, K_NO_WAIT);
  k_thread_name_set(&g_thread, "gnss_mesh");

  LOG_INF("mesh GNSS backend: identity-copying \"%s\" (GnssFixData) into gnss_fix",
          GNSS_MESH_KEY);
  return 0;
}

SYS_INIT(gnss_mesh_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

#if defined(CONFIG_SHELL)
/* Reads stored counters only. The poll thread is never disturbed. */
static int cmd_gnss_status(const struct shell *sh, size_t argc, char **argv) {
  struct rdd2_gnss_mesh_stats stats;
  int64_t now_ms = k_uptime_get();

  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  rdd2_gnss_mesh_stats_get(&stats);

  shell_print(sh, "source=mesh key=%s topic=%s", GNSS_MESH_KEY,
              stats.topic_found ? "resolved" : "MISSING");
  shell_print(sh, "gen=%u samples=%u published=%u failed=%u",
              (unsigned int)stats.last_generation, (unsigned int)stats.samples,
              (unsigned int)stats.published,
              (unsigned int)stats.publish_failed);
  shell_print(sh, "stale_polls=%u copy_fail=%u size_mismatch=%u restarts=%u",
              (unsigned int)stats.stale_polls, (unsigned int)stats.copy_failed,
              (unsigned int)stats.size_mismatch,
              (unsigned int)stats.producer_restarts);

  if (!stats.topic_found) {
    shell_warn(sh,
               "inbound csyn gnss topic not registered: build has no mesh RX");
    return 0;
  }
  if (stats.last_sample_ms < 0) {
    shell_warn(sh, "no mesh fix received yet");
    return 0;
  }

  shell_print(sh, "last fix=%s sats=%u lat_e7=%d lon_e7=%d alt_mm=%d",
              synapse_types_GnssFixType_name(stats.last_fix_type),
              (unsigned int)stats.last_satellites,
              (int)stats.last_latitude_deg_e7,
              (int)stats.last_longitude_deg_e7,
              (int)stats.last_altitude_msl_mm);
  shell_print(sh, "hacc=%u/%u mm stable=%u/%u age=%lld ms usable=%s READY=%s",
              (unsigned int)stats.last_hacc_mm, RDD2_GNSS_M10_MAX_HACC_MM,
              (unsigned int)stats.stable_samples, RDD2_GNSS_M10_STABLE_SAMPLES,
              (long long)(now_ms - stats.last_sample_ms),
              stats.usable ? "yes" : "NO", stats.ready ? "YES" : "NO");
  return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_gnss,
                               SHELL_CMD(status, NULL,
                                         "mesh GNSS backend counters",
                                         cmd_gnss_status),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(gnss, &sub_gnss, "mesh GNSS receive diagnostics", NULL);
#endif
