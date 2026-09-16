/* SPDX-License-Identifier: Apache-2.0 */

#include "processes/navigation_optical_flow.h"

#include <math.h>
#include <string.h>
#include <zephyr/ztest.h>

#define TEST_MS_TO_NS(value) ((uint64_t)(value) * UINT64_C(1000000))

static const struct rdd2_navigation_optical_flow_config g_config = {
    .max_age_ns = TEST_MS_TO_NS(150),
    .min_distance_m = 0.05f,
    .max_distance_m = 5.0f,
    .max_tilt_rad = 0.7f,
    .max_speed_m_s = 10.0f,
    .best_stddev_m_s = 0.1f,
    .worst_stddev_m_s = 1.0f,
    .range_variance_m2 = 0.05f * 0.05f,
    .nominal_integration_time_s = 0.025f,
    .min_integration_time_s = 0.005f,
    .max_integration_time_s = 0.200f,
    .sensor_id = 0,
    .min_quality = 100,
    .require_gptp = true,
};

static synapse_topic_OpticalFlowVelocityData_t
valid_sample(uint64_t timestamp_ns, float vx, float vy, float distance_m) {
  synapse_topic_OpticalFlowVelocityData_t sample;
  memset(&sample, 0, sizeof(sample));
  sample.timestamp_ns = timestamp_ns;
  sample.velocity_flu_m_s.x = vx;
  sample.velocity_flu_m_s.y = vy;
  sample.distance_m = distance_m;
  sample.roll_rad = 0.0f;
  sample.pitch_rad = 0.0f;
  sample.quality = 200;
  sample.flags = RDD2_OPTICAL_FLOW_VELOCITY_VALID |
                 RDD2_OPTICAL_FLOW_TILT_COMPENSATED |
                 RDD2_OPTICAL_FLOW_RANGE_TRUSTED;
  sample.time_status = synapse_types_TimeStatus_GptpSynced;
  sample.id = 0;
  return sample;
}

/* The adapter derives the exposure window from the interval between accepted
 * source timestamps: the first accepted sample uses the nominal period and
 * subsequent samples use the measured gap. The line-of-sight the estimator
 * forms from this measurement then has magnitude v * dt / range. */
ZTEST(process_wrapper_fault_injection,
      test_optical_flow_integration_time_from_interval) {
  struct rdd2_navigation_optical_flow_adapter adapter;
  struct rdd2_navigation_optical_flow_measurement measurement;
  const float vx = 1.0f;
  const float vy = 0.5f;
  const float range_m = 2.0f;
  uint64_t t0 = TEST_MS_TO_NS(1000);

  rdd2_navigation_optical_flow_init(&adapter);

  /* First accepted sample after init falls back to the nominal period. */
  synapse_topic_OpticalFlowVelocityData_t s0 =
      valid_sample(t0, vx, vy, range_m);
  rdd2_navigation_optical_flow_step(&adapter, &measurement, &s0, true, t0,
                                    &g_config);
  zexpect_equal(adapter.status, RDD2_OPTICAL_FLOW_ACCEPTED);
  zexpect_true(measurement.valid);
  zexpect_within(measurement.integration_time_s, 0.025f, 1.0e-6f);

  /* Second accepted sample 25 ms later yields integration_time_s = 0.025. */
  uint64_t t1 = t0 + TEST_MS_TO_NS(25);
  synapse_topic_OpticalFlowVelocityData_t s1 =
      valid_sample(t1, vx, vy, range_m);
  rdd2_navigation_optical_flow_step(&adapter, &measurement, &s1, true, t1,
                                    &g_config);
  zexpect_equal(adapter.status, RDD2_OPTICAL_FLOW_ACCEPTED);
  zexpect_true(measurement.fresh);
  zexpect_within(measurement.integration_time_s, 0.025f, 1.0e-6f);

  /* The estimator scales velocity into a line-of-sight angle by
   * integration_time / range, so |line-of-sight| = |v| * dt / range. */
  float k = measurement.integration_time_s / measurement.ground_distance_m;
  float los0 = -measurement.velocity_body_flu_m_s[1] * k;
  float los1 = measurement.velocity_body_flu_m_s[0] * k;
  float los_mag = sqrtf(los0 * los0 + los1 * los1);
  float expected_mag = sqrtf(vx * vx + vy * vy) * 0.025f / range_m;
  zexpect_within(los_mag, expected_mag, 1.0e-6f);

  /* A different measured interval flows through unchanged, proving the value
   * tracks the source cadence rather than the nominal period. */
  uint64_t t2 = t1 + TEST_MS_TO_NS(40);
  synapse_topic_OpticalFlowVelocityData_t s2 =
      valid_sample(t2, vx, vy, range_m);
  rdd2_navigation_optical_flow_step(&adapter, &measurement, &s2, true, t2,
                                    &g_config);
  zexpect_equal(adapter.status, RDD2_OPTICAL_FLOW_ACCEPTED);
  zexpect_within(measurement.integration_time_s, 0.040f, 1.0e-6f);
}

/* An interval far above the plausible exposure window is a dropout, not an
 * exposure, so the sample is rejected with a named status and counted. */
ZTEST(process_wrapper_fault_injection,
      test_optical_flow_rejects_long_integration_interval) {
  struct rdd2_navigation_optical_flow_adapter adapter;
  struct rdd2_navigation_optical_flow_measurement measurement;
  const float range_m = 2.0f;
  uint64_t t0 = TEST_MS_TO_NS(1000);

  rdd2_navigation_optical_flow_init(&adapter);

  synapse_topic_OpticalFlowVelocityData_t s0 =
      valid_sample(t0, 1.0f, 0.5f, range_m);
  rdd2_navigation_optical_flow_step(&adapter, &measurement, &s0, true, t0,
                                    &g_config);
  zexpect_equal(adapter.status, RDD2_OPTICAL_FLOW_ACCEPTED);
  uint32_t rejected_before = adapter.rejected_count;

  /* 500 ms after the previous accepted sample exceeds the 200 ms bound. */
  uint64_t t1 = t0 + TEST_MS_TO_NS(500);
  synapse_topic_OpticalFlowVelocityData_t s1 =
      valid_sample(t1, 1.0f, 0.5f, range_m);
  rdd2_navigation_optical_flow_step(&adapter, &measurement, &s1, true, t1,
                                    &g_config);
  zexpect_equal(adapter.status, RDD2_OPTICAL_FLOW_REJECTED_INTEGRATION_TIME);
  zexpect_false(measurement.valid);
  zexpect_equal(adapter.rejected_count, rejected_before + 1U);
  zassert_equal(strcmp(rdd2_navigation_optical_flow_status_name(
                           RDD2_OPTICAL_FLOW_REJECTED_INTEGRATION_TIME),
                       "integration-time"),
                0);
}
