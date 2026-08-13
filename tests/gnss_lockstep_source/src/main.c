/* SPDX-License-Identifier: Apache-2.0 */

#include "gnss_lockstep.h"
#include "gnss_m10_protocol.h"
#include "interfaces/zros_topics.h"

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <zros/private/zros_topic_struct.h>

ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(gnss_fix, synapse_topic_GnssFixData_t);

static synapse_topic_GnssFixData_t usable_fix(uint64_t timestamp_ns) {
  return (synapse_topic_GnssFixData_t){
      .timestamp_ns = timestamp_ns,
      .latitude_deg_e7 = 404237000,
      .longitude_deg_e7 = -869212000,
      .altitude_ellipsoid_mm = 200000,
      .altitude_msl_mm = 200000,
      .horizontal_accuracy_mm = 400U,
      .vertical_accuracy_mm = 700U,
      .velocity_accuracy_mm_s = 150U,
      .fix_type = synapse_types_GnssFixType_Fix3d,
      .satellites_used = 16U,
      .time_status = synapse_types_TimeStatus_LocalFreerun,
  };
}

static uint32_t gnss_generation(void) {
  return (uint32_t)atomic_get(&topic_gnss_fix._lockless_generation);
}

ZTEST(gnss_lockstep_source, test_readiness_and_invalid_input_fail_closed) {
  synapse_topic_GnssFixData_t fix;
  uint32_t generation;
  uint64_t control_now_ns = UINT64_C(100000000);

  zassert_ok(rdd2_gnss_lockstep_init());
  zassert_false(rdd2_gnss_lockstep_ready_get());
  generation = gnss_generation();
  for (size_t sample = 0U; sample < RDD2_GNSS_M10_STABLE_SAMPLES; ++sample) {
    fix = usable_fix(control_now_ns);
    zassert_true(rdd2_gnss_lockstep_submit(&fix, control_now_ns));
    control_now_ns += UINT64_C(100000000);
    if (sample + 1U < RDD2_GNSS_M10_STABLE_SAMPLES) {
      zassert_false(rdd2_gnss_lockstep_ready_get());
    }
  }
  zassert_true(rdd2_gnss_lockstep_ready_get());
  zassert_equal(gnss_generation(), generation + RDD2_GNSS_M10_STABLE_SAMPLES);

  generation = gnss_generation();
  zassert_true(rdd2_gnss_lockstep_submit(&fix, control_now_ns));
  zassert_equal(gnss_generation(), generation,
                "a retained 200 Hz lockstep fix must not republish");
  fix = usable_fix(fix.timestamp_ns - UINT64_C(1000000));
  zassert_false(rdd2_gnss_lockstep_submit(&fix, control_now_ns));

  fix = usable_fix(control_now_ns + 1U);
  zassert_false(rdd2_gnss_lockstep_submit(&fix, control_now_ns),
                "a future fix must fail the exchange");

  fix = usable_fix(control_now_ns);
  fix.horizontal_accuracy_mm = RDD2_GNSS_M10_MAX_HACC_MM + 1U;
  zassert_true(rdd2_gnss_lockstep_submit(&fix, control_now_ns));
  zassert_false(rdd2_gnss_lockstep_ready_get());
  control_now_ns += UINT64_C(100000000);

  fix = usable_fix(control_now_ns);
  fix.latitude_deg_e7 = INT32_C(900000001);
  zassert_true(rdd2_gnss_lockstep_submit(&fix, control_now_ns));
  zassert_false(rdd2_gnss_lockstep_ready_get());
  control_now_ns += UINT64_C(100000000);

  for (size_t sample = 0U; sample < RDD2_GNSS_M10_STABLE_SAMPLES; ++sample) {
    fix = usable_fix(control_now_ns);
    zassert_true(rdd2_gnss_lockstep_submit(&fix, control_now_ns));
    control_now_ns += UINT64_C(100000000);
  }
  zassert_true(rdd2_gnss_lockstep_ready_get());
  control_now_ns = fix.timestamp_ns +
                   (uint64_t)(RDD2_GNSS_M10_RECENT_MS + 1U) * UINT64_C(1000000);
  zassert_true(rdd2_gnss_lockstep_submit(&fix, control_now_ns));
  zassert_false(rdd2_gnss_lockstep_ready_get());

  fix = usable_fix(fix.timestamp_ns + UINT64_C(200000000));
  fix.horizontal_accuracy_mm = RDD2_GNSS_M10_MAX_HACC_MM + 1U;
  zassert_true(rdd2_gnss_lockstep_submit(&fix, control_now_ns));
  zassert_false(rdd2_gnss_lockstep_ready_get());
  fix = usable_fix(fix.timestamp_ns + UINT64_C(200000000));
  control_now_ns = fix.timestamp_ns;
  zassert_true(rdd2_gnss_lockstep_submit(&fix, control_now_ns));
  zassert_false(rdd2_gnss_lockstep_ready_get(),
                "a 5 Hz successor must reset the stability window");
}

ZTEST_SUITE(gnss_lockstep_source, NULL, NULL, NULL, NULL, NULL);
