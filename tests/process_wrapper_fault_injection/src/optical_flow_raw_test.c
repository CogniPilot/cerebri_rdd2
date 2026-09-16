/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Tightly coupled optical-flow adapter tests.
 *
 * These pin the sign convention that bridges the flow node's OpticalFlowData to
 * the ESKF OpticalFlowSample, the rejection statuses, the covariance scaling
 * and the mount-yaw rotation. The convention (see navigation_optical_flow_raw.h)
 * is line-of-sight = mount_yaw(flow_rad) and integrated gyro =
 * -mount_yaw(delta_angle_flu), so the estimator's compensatedFlow =
 * line_of_sight + gyro reproduces the node's flow_rad - delta_angle_flu.
 */

#include "processes/navigation_optical_flow_raw.h"

#include <math.h>
#include <string.h>
#include <zephyr/ztest.h>

#define TEST_MS_TO_NS(value) ((uint64_t)(value) * UINT64_C(1000000))
#define GYRO_ND 3800.0e-6f
#define LOS_FLOOR 300.0e-6f
#define LOS_BEST 0.05f
#define LOS_WORST 0.25f

static const struct rdd2_navigation_optical_flow_raw_config g_config = {
    .max_age_ns = TEST_MS_TO_NS(150),
    .min_distance_m = 0.05f,
    .max_distance_m = 5.0f,
    .min_integration_time_s = 0.005f,
    .max_integration_time_s = 0.200f,
    .los_floor_rad = LOS_FLOOR,
    .los_sens_best_frac = LOS_BEST,
    .los_sens_worst_frac = LOS_WORST,
    .gyro_rate_noise_rad_s_rthz = GYRO_ND,
    .range_best_stddev_m = 0.03f,
    .range_worst_stddev_m = 0.15f,
    .mount_yaw_deg = 0,
    .sensor_id = 0,
    .min_quality = 100,
    .require_gptp = true,
};

static synapse_topic_OpticalFlowData_t
raw_sample(uint64_t timestamp_ns, float fx, float fy, float dx, float dy,
           float dz, float distance_m, uint32_t integration_ns) {
  synapse_topic_OpticalFlowData_t sample;

  memset(&sample, 0, sizeof(sample));
  sample.timestamp_ns = timestamp_ns;
  sample.flow_rad.x = fx;
  sample.flow_rad.y = fy;
  sample.delta_angle_flu_rad.x = dx;
  sample.delta_angle_flu_rad.y = dy;
  sample.delta_angle_flu_rad.z = dz;
  sample.distance_m = distance_m;
  sample.integration_timespan_ns = integration_ns;
  sample.quality = 200;
  sample.distance_quality = 200;
  sample.flags = RDD2_OPTICAL_FLOW_RAW_FLOW_VALID |
                 RDD2_OPTICAL_FLOW_RAW_DELTA_ANGLE_VALID |
                 RDD2_OPTICAL_FLOW_RAW_DISTANCE_VALID;
  sample.time_status = synapse_types_TimeStatus_GptpSynced;
  sample.id = 0;
  return sample;
}

static void step_once(struct rdd2_navigation_optical_flow_raw_adapter *adapter,
                      struct rdd2_navigation_optical_flow_raw_measurement *out,
                      const synapse_topic_OpticalFlowData_t *sample) {
  rdd2_navigation_optical_flow_raw_step(adapter, out, sample, true,
                                        sample->timestamp_ns, &g_config);
}

/* A pure-translation window (zero delta angle) leaves the line-of-sight equal
 * to the flow and the gyro term zero; forward motion (flow y > 0) gives a
 * positive line-of-sight y, which the estimator scales into forward velocity. */
ZTEST(process_wrapper_fault_injection, test_optical_flow_raw_pure_translation) {
  struct rdd2_navigation_optical_flow_raw_adapter adapter;
  struct rdd2_navigation_optical_flow_raw_measurement m;
  synapse_topic_OpticalFlowData_t sample = raw_sample(
      TEST_MS_TO_NS(1000), 0.01f, 0.02f, 0.0f, 0.0f, 0.0f, 2.0f, 25000000U);

  rdd2_navigation_optical_flow_raw_init(&adapter);
  step_once(&adapter, &m, &sample);

  zassert_true(m.valid, "translation window should be accepted");
  zassert_true(m.fresh, "fresh sample");
  zassert_equal(adapter.status, RDD2_OPTICAL_FLOW_RAW_ACCEPTED, "status %d",
                adapter.status);
  zassert_within(m.integrated_line_of_sight_rad[0], 0.01f, 1.0e-6f, "los x");
  zassert_within(m.integrated_line_of_sight_rad[1], 0.02f, 1.0e-6f, "los y");
  zassert_within(m.integrated_gyro_body_flu_rad[0], 0.0f, 1.0e-9f, "gyro x");
  zassert_within(m.integrated_gyro_body_flu_rad[1], 0.0f, 1.0e-9f, "gyro y");
  zassert_true(m.integrated_line_of_sight_rad[1] > 0.0f,
               "forward motion gives positive line-of-sight y");
  zassert_within(m.integration_time_s, 0.025f, 1.0e-6f, "dt");
}

/* A pure-rotation window has flow equal to the co-timed body rotation, so the
 * estimator's compensated flow (line_of_sight + gyro) cancels to zero. */
ZTEST(process_wrapper_fault_injection, test_optical_flow_raw_pure_rotation) {
  struct rdd2_navigation_optical_flow_raw_adapter adapter;
  struct rdd2_navigation_optical_flow_raw_measurement m;
  synapse_topic_OpticalFlowData_t sample = raw_sample(
      TEST_MS_TO_NS(1000), 0.03f, -0.02f, 0.03f, -0.02f, 0.01f, 2.0f,
      25000000U);

  rdd2_navigation_optical_flow_raw_init(&adapter);
  step_once(&adapter, &m, &sample);

  zassert_true(m.valid, "rotation window should be accepted");
  zassert_within(m.integrated_line_of_sight_rad[0] +
                     m.integrated_gyro_body_flu_rad[0],
                 0.0f, 1.0e-6f, "compensated x cancels");
  zassert_within(m.integrated_line_of_sight_rad[1] +
                     m.integrated_gyro_body_flu_rad[1],
                 0.0f, 1.0e-6f, "compensated y cancels");
  /* the reported gyro is the negated body-angle integral */
  zassert_within(m.integrated_gyro_body_flu_rad[2], -0.01f, 1.0e-6f, "gyro z");
}

/* Mount yaw rotates both the flow and the delta before the gyro is negated. */
ZTEST(process_wrapper_fault_injection, test_optical_flow_raw_mount_yaw_90) {
  struct rdd2_navigation_optical_flow_raw_adapter adapter;
  struct rdd2_navigation_optical_flow_raw_measurement m;
  struct rdd2_navigation_optical_flow_raw_config cfg = g_config;
  synapse_topic_OpticalFlowData_t sample = raw_sample(
      TEST_MS_TO_NS(1000), 0.01f, 0.02f, 0.005f, 0.0f, 0.0f, 2.0f, 25000000U);

  cfg.mount_yaw_deg = 90;
  rdd2_navigation_optical_flow_raw_init(&adapter);
  rdd2_navigation_optical_flow_raw_step(&adapter, &m, &sample, true,
                                        sample.timestamp_ns, &cfg);

  zassert_true(m.valid, "accepted");
  /* Rz(90): x' = -y, y' = x */
  zassert_within(m.integrated_line_of_sight_rad[0], -0.02f, 1.0e-6f, "los x");
  zassert_within(m.integrated_line_of_sight_rad[1], 0.01f, 1.0e-6f, "los y");
  /* delta (0.005, 0) rotates to (0, 0.005), then negated */
  zassert_within(m.integrated_gyro_body_flu_rad[0], 0.0f, 1.0e-6f, "gyro x");
  zassert_within(m.integrated_gyro_body_flu_rad[1], -0.005f, 1.0e-6f, "gyro y");
}

/* Line-of-sight variance is (floor + sensitivity * |angle|)^2 with the
 * sensitivity interpolated by quality; gyro variance is density^2 * window;
 * range variance is the quality-interpolated range stddev squared. */
ZTEST(process_wrapper_fault_injection, test_optical_flow_raw_covariance) {
  struct rdd2_navigation_optical_flow_raw_adapter adapter;
  struct rdd2_navigation_optical_flow_raw_measurement m;
  synapse_topic_OpticalFlowData_t sample = raw_sample(
      TEST_MS_TO_NS(1000), 0.01f, 0.02f, 0.0f, 0.0f, 0.0f, 2.0f, 25000000U);
  float norm_q = (200.0f - 100.0f) / (255.0f - 100.0f);
  float sens = LOS_WORST - norm_q * (LOS_WORST - LOS_BEST);
  float sigma0 = LOS_FLOOR + sens * 0.01f;
  float sigma1 = LOS_FLOOR + sens * 0.02f;
  float gyro_var = GYRO_ND * GYRO_ND * 0.025f;
  float norm_dq = 200.0f / 255.0f;
  float range_stddev = 0.15f - norm_dq * (0.15f - 0.03f);

  rdd2_navigation_optical_flow_raw_init(&adapter);
  step_once(&adapter, &m, &sample);

  zassert_within(m.integrated_line_of_sight_cov_rad2[0][0], sigma0 * sigma0,
                 1.0e-9f, "los var x");
  zassert_within(m.integrated_line_of_sight_cov_rad2[1][1], sigma1 * sigma1,
                 1.0e-9f, "los var y");
  zassert_within(m.integrated_line_of_sight_cov_rad2[0][1], 0.0f, 1.0e-12f,
                 "los cov off-diagonal");
  zassert_within(m.integrated_gyro_cov_rad2[0][0], gyro_var, 1.0e-12f,
                 "gyro var x");
  zassert_within(m.integrated_gyro_cov_rad2[2][2], gyro_var, 1.0e-12f,
                 "gyro var z");
  zassert_within(m.ground_distance_variance_m2, range_stddev * range_stddev,
                 1.0e-9f, "range var");
}

static void expect_reject(synapse_topic_OpticalFlowData_t *sample,
                          enum rdd2_navigation_optical_flow_raw_status expected) {
  struct rdd2_navigation_optical_flow_raw_adapter adapter;
  struct rdd2_navigation_optical_flow_raw_measurement m;

  rdd2_navigation_optical_flow_raw_init(&adapter);
  step_once(&adapter, &m, sample);
  zassert_false(m.valid, "rejected sample must not be valid");
  zassert_equal(adapter.status, expected, "status %d expected %d",
                adapter.status, expected);
  zassert_equal(adapter.rejected_count, 1U, "one rejection counted");
}

ZTEST(process_wrapper_fault_injection, test_optical_flow_raw_rejections) {
  synapse_topic_OpticalFlowData_t s;

  s = raw_sample(TEST_MS_TO_NS(1000), 0.01f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f,
                 25000000U);
  s.timestamp_ns = 0U;
  expect_reject(&s, RDD2_OPTICAL_FLOW_RAW_REJECTED_ZERO_TIMESTAMP);

  s = raw_sample(TEST_MS_TO_NS(1000), 0.01f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f,
                 25000000U);
  s.id = 3U;
  expect_reject(&s, RDD2_OPTICAL_FLOW_RAW_REJECTED_SENSOR_ID);

  s = raw_sample(TEST_MS_TO_NS(1000), 0.01f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f,
                 25000000U);
  s.flags &= (uint8_t)~RDD2_OPTICAL_FLOW_RAW_FLOW_VALID;
  expect_reject(&s, RDD2_OPTICAL_FLOW_RAW_REJECTED_FLOW_INVALID);

  s = raw_sample(TEST_MS_TO_NS(1000), 0.01f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f,
                 25000000U);
  s.flags &= (uint8_t)~RDD2_OPTICAL_FLOW_RAW_DELTA_ANGLE_VALID;
  expect_reject(&s, RDD2_OPTICAL_FLOW_RAW_REJECTED_DELTA_ANGLE_INVALID);

  s = raw_sample(TEST_MS_TO_NS(1000), 0.01f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f,
                 25000000U);
  s.flags &= (uint8_t)~RDD2_OPTICAL_FLOW_RAW_DISTANCE_VALID;
  expect_reject(&s, RDD2_OPTICAL_FLOW_RAW_REJECTED_DISTANCE_INVALID);

  s = raw_sample(TEST_MS_TO_NS(1000), 0.01f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f,
                 25000000U);
  s.flags |= RDD2_OPTICAL_FLOW_RAW_DISTANCE_AMBIGUOUS;
  expect_reject(&s, RDD2_OPTICAL_FLOW_RAW_REJECTED_DISTANCE_AMBIGUOUS);

  s = raw_sample(TEST_MS_TO_NS(1000), 0.01f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f,
                 25000000U);
  s.quality = 50U;
  expect_reject(&s, RDD2_OPTICAL_FLOW_RAW_REJECTED_QUALITY);

  s = raw_sample(TEST_MS_TO_NS(1000), 0.01f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f,
                 25000000U);
  s.time_status = synapse_types_TimeStatus_LocalFreerun;
  expect_reject(&s, RDD2_OPTICAL_FLOW_RAW_REJECTED_TIME_STATUS);

  s = raw_sample(TEST_MS_TO_NS(1000), 0.01f, 0.0f, 0.0f, 0.0f, 0.0f, 10.0f,
                 25000000U);
  expect_reject(&s, RDD2_OPTICAL_FLOW_RAW_REJECTED_DISTANCE);

  s = raw_sample(TEST_MS_TO_NS(1000), 0.01f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f,
                 1000000U);
  expect_reject(&s, RDD2_OPTICAL_FLOW_RAW_REJECTED_INTEGRATION_TIME);

  s = raw_sample(TEST_MS_TO_NS(1000), NAN, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f,
                 25000000U);
  expect_reject(&s, RDD2_OPTICAL_FLOW_RAW_REJECTED_NONFINITE);
}

/* A non-increasing source timestamp is a replay and is rejected. */
ZTEST(process_wrapper_fault_injection, test_optical_flow_raw_replay) {
  struct rdd2_navigation_optical_flow_raw_adapter adapter;
  struct rdd2_navigation_optical_flow_raw_measurement m;
  synapse_topic_OpticalFlowData_t first = raw_sample(
      TEST_MS_TO_NS(1000), 0.01f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f, 25000000U);
  synapse_topic_OpticalFlowData_t replay = raw_sample(
      TEST_MS_TO_NS(1000), 0.02f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f, 25000000U);

  rdd2_navigation_optical_flow_raw_init(&adapter);
  step_once(&adapter, &m, &first);
  zassert_true(m.valid, "first accepted");
  rdd2_navigation_optical_flow_raw_step(&adapter, &m, &replay, true,
                                        TEST_MS_TO_NS(1000), &g_config);
  zassert_equal(adapter.status, RDD2_OPTICAL_FLOW_RAW_REJECTED_REPLAY,
                "status %d", adapter.status);
}
