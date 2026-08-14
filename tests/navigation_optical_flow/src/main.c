/* SPDX-License-Identifier: Apache-2.0 */

#include "navigation_optical_flow.h"

#include <math.h>

#include <zephyr/ztest.h>

#define GOOD_FLAGS                                                            \
  (RDD2_OPTICAL_FLOW_VELOCITY_VALID |                                         \
   RDD2_OPTICAL_FLOW_TILT_COMPENSATED | RDD2_OPTICAL_FLOW_RANGE_TRUSTED)

static const struct rdd2_navigation_optical_flow_config g_config = {
    .max_age_ns = UINT64_C(100000000),
    .min_distance_m = 0.05f,
    .max_distance_m = 5.0f,
    .max_tilt_rad = 0.7f,
    .max_speed_m_s = 10.0f,
    .best_stddev_m_s = 0.1f,
    .worst_stddev_m_s = 1.0f,
    .sensor_id = 0U,
    .min_quality = 100U,
    .require_gptp = true,
};

static synapse_topic_OpticalFlowVelocityData_t good_sample(uint64_t timestamp,
                                                            uint8_t quality) {
  return (synapse_topic_OpticalFlowVelocityData_t){
      .timestamp_ns = timestamp,
      .velocity_flu_m_s = {.x = 1.25f, .y = -0.75f},
      .distance_m = 1.5f,
      .roll_rad = 0.1f,
      .pitch_rad = -0.2f,
      .quality = quality,
      .flags = GOOD_FLAGS,
      .time_status = synapse_types_TimeStatus_GptpSynced,
      .id = 0U,
  };
}

ZTEST(navigation_optical_flow, test_maps_good_sample_and_quality_covariance) {
  struct rdd2_navigation_optical_flow_adapter adapter;
  struct rdd2_navigation_optical_flow_measurement measurement;
  synapse_topic_OpticalFlowVelocityData_t sample = good_sample(1000U, 255U);

  rdd2_navigation_optical_flow_init(&adapter);
  rdd2_navigation_optical_flow_step(&adapter, &measurement, &sample, true,
                                    UINT64_C(200000000), &g_config);

  zassert_true(measurement.valid);
  zassert_true(measurement.fresh);
  zassert_within(measurement.timestamp_s, 0.2f, 1.0e-6f);
  zassert_within(measurement.velocity_body_flu_m_s[0], 1.25f, 1.0e-6f);
  zassert_within(measurement.velocity_body_flu_m_s[1], -0.75f, 1.0e-6f);
  zassert_within(measurement.velocity_covariance_body_m2_s2[0][0], 0.01f,
                 1.0e-6f);
  zassert_within(measurement.velocity_covariance_body_m2_s2[1][1], 0.01f,
                 1.0e-6f);
  zassert_equal(measurement.velocity_covariance_body_m2_s2[0][1], 0.0f);
  zassert_equal(measurement.velocity_covariance_body_m2_s2[1][0], 0.0f);
  zassert_within(measurement.ground_distance_m, 1.5f, 1.0e-6f);
  zassert_within(measurement.quality, 1.0f, 1.0e-6f);

  sample = good_sample(2000U, g_config.min_quality);
  rdd2_navigation_optical_flow_step(&adapter, &measurement, &sample, true,
                                    UINT64_C(201000000), &g_config);
  zassert_within(measurement.velocity_covariance_body_m2_s2[0][0], 1.0f,
                 1.0e-6f);
}

ZTEST(navigation_optical_flow, test_holds_then_expires_without_refresh) {
  struct rdd2_navigation_optical_flow_adapter adapter;
  struct rdd2_navigation_optical_flow_measurement measurement;
  synapse_topic_OpticalFlowVelocityData_t sample = good_sample(1000U, 200U);

  rdd2_navigation_optical_flow_init(&adapter);
  rdd2_navigation_optical_flow_step(&adapter, &measurement, &sample, true,
                                    UINT64_C(1000000000), &g_config);
  zassert_true(measurement.valid);

  rdd2_navigation_optical_flow_step(
      &adapter, &measurement, &sample, false, UINT64_C(1100000000), &g_config);
  zassert_true(measurement.valid);
  zassert_false(measurement.fresh);

  rdd2_navigation_optical_flow_step(
      &adapter, &measurement, &sample, false, UINT64_C(1100000001), &g_config);
  zassert_false(measurement.valid);
  zassert_false(measurement.fresh);
}

ZTEST(navigation_optical_flow, test_replay_does_not_refresh_age) {
  struct rdd2_navigation_optical_flow_adapter adapter;
  struct rdd2_navigation_optical_flow_measurement measurement;
  synapse_topic_OpticalFlowVelocityData_t sample = good_sample(1000U, 200U);

  rdd2_navigation_optical_flow_init(&adapter);
  rdd2_navigation_optical_flow_step(&adapter, &measurement, &sample, true,
                                    UINT64_C(1000000000), &g_config);
  rdd2_navigation_optical_flow_step(
      &adapter, &measurement, &sample, true, UINT64_C(1050000000), &g_config);
  zassert_true(measurement.valid);
  zassert_false(measurement.fresh);
  rdd2_navigation_optical_flow_step(
      &adapter, &measurement, &sample, true, UINT64_C(1100000001), &g_config);
  zassert_false(measurement.valid);
}

static void expect_rejected(synapse_topic_OpticalFlowVelocityData_t sample) {
  struct rdd2_navigation_optical_flow_adapter adapter;
  struct rdd2_navigation_optical_flow_measurement measurement;

  rdd2_navigation_optical_flow_init(&adapter);
  rdd2_navigation_optical_flow_step(&adapter, &measurement, &sample, true,
                                    UINT64_C(1000000000), &g_config);
  zassert_false(measurement.valid);
  zassert_false(measurement.fresh);
}

ZTEST(navigation_optical_flow, test_rejects_untrusted_payloads) {
  synapse_topic_OpticalFlowVelocityData_t sample = good_sample(1000U, 200U);

  sample.flags &= (uint8_t)~RDD2_OPTICAL_FLOW_RANGE_TRUSTED;
  expect_rejected(sample);
  sample = good_sample(1000U, g_config.min_quality - 1U);
  expect_rejected(sample);
  sample = good_sample(1000U, 200U);
  sample.time_status = synapse_types_TimeStatus_LocalFreerun;
  expect_rejected(sample);
  sample = good_sample(1000U, 200U);
  sample.velocity_flu_m_s.x = NAN;
  expect_rejected(sample);
  sample = good_sample(1000U, 200U);
  sample.distance_m = g_config.max_distance_m + 0.01f;
  expect_rejected(sample);
  sample = good_sample(1000U, 200U);
  sample.roll_rad = g_config.max_tilt_rad + 0.01f;
  expect_rejected(sample);
  sample = good_sample(1000U, 200U);
  sample.velocity_flu_m_s.y = g_config.max_speed_m_s + 0.01f;
  expect_rejected(sample);
  sample = good_sample(1000U, 200U);
  sample.id = 1U;
  expect_rejected(sample);
}

ZTEST(navigation_optical_flow, test_new_invalid_sample_revokes_then_recovers) {
  struct rdd2_navigation_optical_flow_adapter adapter;
  struct rdd2_navigation_optical_flow_measurement measurement;
  synapse_topic_OpticalFlowVelocityData_t sample = good_sample(1000U, 200U);

  rdd2_navigation_optical_flow_init(&adapter);
  rdd2_navigation_optical_flow_step(&adapter, &measurement, &sample, true,
                                    UINT64_C(1000000000), &g_config);
  zassert_true(measurement.valid);

  sample = good_sample(2000U, 200U);
  sample.flags = 0U;
  rdd2_navigation_optical_flow_step(&adapter, &measurement, &sample, true,
                                    UINT64_C(1010000000), &g_config);
  zassert_false(measurement.valid);

  sample = good_sample(3000U, 200U);
  sample.time_status = synapse_types_TimeStatus_GptpHoldover;
  rdd2_navigation_optical_flow_step(&adapter, &measurement, &sample, true,
                                    UINT64_C(1020000000), &g_config);
  zassert_true(measurement.valid);
  zassert_true(measurement.fresh);
}

ZTEST_SUITE(navigation_optical_flow, NULL, NULL, NULL, NULL, NULL);
