/* SPDX-License-Identifier: Apache-2.0 */

#include "gnss_lockstep.h"
#include "gnss_m10_protocol.h"

#include "interfaces/zros_topics.h"

#include <string.h>

#include <zephyr/kernel.h>

#include <zros/private/zros_node_struct.h>
#include <zros/private/zros_pub_struct.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>

struct gnss_lockstep_state {
  uint64_t control_now_ns;
  uint64_t last_timestamp_ns;
  uint8_t stable_samples;
  bool observed;
  bool usable;
  bool published;
};

static struct gnss_lockstep_state g_state;
static struct k_spinlock g_lock;
static struct zros_node g_node;
static struct zros_pub g_pub;
static synapse_topic_GnssFixData_t g_fix;
static bool g_initialized;

static bool fix_type_usable(uint8_t fix_type) {
  return fix_type == synapse_types_GnssFixType_Fix3d ||
         fix_type == synapse_types_GnssFixType_Dgnss ||
         fix_type == synapse_types_GnssFixType_RtkFloat ||
         fix_type == synapse_types_GnssFixType_RtkFixed;
}

/*
 * Which clock domain an injected fix must carry to be trusted. This tracks the
 * same CONFIG_NET_GPTP switch the local producers resolve against, so the gate
 * and the upstream stamps always name one domain. Without the gPTP stack a fix
 * is taken in the boot-time domain and is stamped LocalFreerun, which is the
 * native_sim SITL case. With the gPTP stack up a fix is only accepted once it
 * is carried on the shared grandmaster timescale: GptpSynced while a
 * grandmaster is present, or GptpHoldover while a previously locked PHC coasts.
 */
static bool fix_time_status_usable(uint8_t time_status) {
#if defined(CONFIG_NET_GPTP)
  return time_status == synapse_types_TimeStatus_GptpSynced ||
         time_status == synapse_types_TimeStatus_GptpHoldover;
#else
  return time_status == synapse_types_TimeStatus_LocalFreerun;
#endif
}

static bool fix_usable(const synapse_topic_GnssFixData_t *fix) {
  return fix != NULL && fix->timestamp_ns != 0U &&
         fix_time_status_usable(fix->time_status) &&
         fix_type_usable(fix->fix_type) &&
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

int rdd2_gnss_lockstep_init(void) {
  int rc;

  memset(&g_state, 0, sizeof(g_state));
  memset(&g_fix, 0, sizeof(g_fix));
  g_fix.horizontal_accuracy_mm = UINT16_MAX;
  g_fix.vertical_accuracy_mm = UINT16_MAX;
  g_fix.velocity_accuracy_mm_s = UINT16_MAX;
  g_fix.yaw_accuracy_cdeg = UINT16_MAX;
  g_fix.fix_type = synapse_types_GnssFixType_NoFix;
  g_fix.time_status = synapse_types_TimeStatus_LocalFreerun;
  zros_node_init(&g_node, "rdd2_gnss_lockstep");
  rc = zros_pub_init(&g_pub, &g_node, &topic_gnss_fix, &g_fix);
  if (rc == 0) {
    rc = zros_pub_update(&g_pub);
  }
  g_initialized = rc == 0;
  return rc;
}

bool rdd2_gnss_lockstep_submit(const synapse_topic_GnssFixData_t *fix,
                               uint64_t control_now_ns) {
  struct gnss_lockstep_state next;
  uint64_t gap_ns = 0U;
  bool usable;
  bool published;
  k_spinlock_key_t key;

  if (!g_initialized || fix == NULL || control_now_ns == 0U) {
    return false;
  }
  key = k_spin_lock(&g_lock);
  next = g_state;
  k_spin_unlock(&g_lock, key);
  if (control_now_ns < next.control_now_ns ||
      fix->timestamp_ns > control_now_ns ||
      (next.observed && fix->timestamp_ns < next.last_timestamp_ns)) {
    return false;
  }
  if (next.observed && fix->timestamp_ns == next.last_timestamp_ns) {
    if (memcmp(fix, &g_fix, sizeof(*fix)) != 0) {
      return false;
    }
    key = k_spin_lock(&g_lock);
    g_state.control_now_ns = control_now_ns;
    k_spin_unlock(&g_lock, key);
    return true;
  }
  usable = fix_usable(fix);
  if (next.observed) {
    gap_ns = fix->timestamp_ns - next.last_timestamp_ns;
  }
  if (!usable ||
      (next.observed &&
       (gap_ns < (uint64_t)RDD2_GNSS_M10_MIN_GAP_MS * UINT64_C(1000000) ||
        gap_ns > (uint64_t)RDD2_GNSS_M10_MAX_GAP_MS * UINT64_C(1000000)))) {
    next.stable_samples = 0U;
  } else if (next.stable_samples < RDD2_GNSS_M10_STABLE_SAMPLES) {
    next.stable_samples++;
  }
  next.observed = true;
  next.control_now_ns = control_now_ns;
  next.last_timestamp_ns = fix->timestamp_ns;
  next.usable = usable;
  next.published = false;
  key = k_spin_lock(&g_lock);
  g_state = next;
  k_spin_unlock(&g_lock, key);

  g_fix = *fix;
  published = zros_pub_update(&g_pub) == 0;
  key = k_spin_lock(&g_lock);
  g_state.published = published;
  if (!published) {
    g_state.usable = false;
  }
  k_spin_unlock(&g_lock, key);
  return published;
}

bool rdd2_gnss_lockstep_ready_get(void) {
  struct gnss_lockstep_state state;
  k_spinlock_key_t key = k_spin_lock(&g_lock);

  state = g_state;
  k_spin_unlock(&g_lock, key);
  return state.observed && state.usable && state.published &&
         state.stable_samples >= RDD2_GNSS_M10_STABLE_SAMPLES &&
         state.control_now_ns >= state.last_timestamp_ns &&
         state.control_now_ns - state.last_timestamp_ns <=
             (uint64_t)RDD2_GNSS_M10_RECENT_MS * UINT64_C(1000000);
}
