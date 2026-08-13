/* SPDX-License-Identifier: Apache-2.0 */

#include "processes/navigation_gps.h"

#include <math.h>
#include <string.h>
#include <zephyr/ztest.h>

#define TEST_NS_FROM_US(value) ((uint64_t)(value) * UINT64_C(1000))

static synapse_topic_GnssFixData_t valid_fix(uint64_t timestamp_ns) {
  return (synapse_topic_GnssFixData_t){
      .timestamp_ns = timestamp_ns,
      .latitude_deg_e7 = INT32_C(473970000),
      .longitude_deg_e7 = INT32_C(85450000),
      .altitude_msl_mm = INT32_C(488000),
      .horizontal_accuracy_mm = UINT16_C(400),
      .vertical_accuracy_mm = UINT16_C(700),
      .velocity_accuracy_mm_s = UINT16_C(50),
      .ground_speed_cm_s = UINT16_C(100),
      .course_over_ground_cdeg = UINT16_C(9000),
      .velocity_up_cm_s = INT16_C(25),
      .flags = synapse_topic_GnssFixFlags_CourseValid |
               synapse_topic_GnssFixFlags_VelocityUpValid,
      .fix_type = synapse_types_GnssFixType_Fix3d,
  };
}

static synapse_topic_VehicleHealthData_t
disarmed_health(uint64_t timestamp_ns) {
  return (synapse_topic_VehicleHealthData_t){.timestamp_ns = timestamp_ns};
}

static bool step(struct rdd2_navigation_gps_adapter *adapter,
                 struct rdd2_navigation_gps_measurement *measurement,
                 const synapse_topic_GnssFixData_t *fix, bool fix_fresh,
                 const synapse_topic_VehicleHealthData_t *health,
                 bool health_fresh, uint64_t now_ns) {
  return rdd2_navigation_gps_step(adapter, measurement, fix, fix_fresh, health,
                                  health_fresh, now_ns);
}

ZTEST(process_wrapper_fault_injection,
      test_navigation_gps_geodesy_and_velocity) {
  struct rdd2_navigation_gps_adapter adapter;
  struct rdd2_navigation_gps_measurement measurement;
  synapse_topic_GnssFixData_t fix =
      valid_fix(TEST_NS_FROM_US(UINT64_C(1000000)));
  synapse_topic_VehicleHealthData_t health = disarmed_health(fix.timestamp_ns);

  rdd2_navigation_gps_init(&adapter);
  zexpect_true(step(&adapter, &measurement, &fix, true, &health, true,
                    fix.timestamp_ns));
  zexpect_true(measurement.valid);
  zexpect_true(measurement.fresh);
  zexpect_true(measurement.position_valid);
  zexpect_true(measurement.velocity_valid);
  zexpect_within(measurement.position_enu_m[0], 0.0f, 1.0e-5f);
  zexpect_within(measurement.position_enu_m[1], 0.0f, 1.0e-5f);
  zexpect_within(measurement.position_enu_m[2], 0.0f, 1.0e-5f);
  zexpect_within(measurement.velocity_enu_m_s[0], 1.0f, 1.0e-5f);
  zexpect_within(measurement.velocity_enu_m_s[1], 0.0f, 1.0e-5f);
  zexpect_within(measurement.velocity_enu_m_s[2], 0.25f, 1.0e-5f);
  zexpect_within(measurement.position_covariance_enu_m2[0][0], 0.25f, 1.0e-6f);
  zexpect_within(measurement.position_covariance_enu_m2[1][1], 0.25f, 1.0e-6f);
  zexpect_within(measurement.position_covariance_enu_m2[2][2], 0.49f, 1.0e-6f);
  zexpect_within(measurement.velocity_covariance_enu_m2_s2[0][0], 0.01f,
                 1.0e-6f);

  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(2000000)));
  fix.latitude_deg_e7 += INT32_C(1);
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, false,
                     fix.timestamp_ns));
  zexpect_within(measurement.position_enu_m[0], 0.0f, 1.0e-5f);
  zexpect_within(measurement.position_enu_m[1], 0.011132f, 5.0e-5f);

  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(3000000)));
  fix.latitude_deg_e7 -= INT32_C(1);
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, false,
                     fix.timestamp_ns));
  zexpect_within(measurement.position_enu_m[1], -0.011132f, 5.0e-5f);

  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(4000000)));
  fix.longitude_deg_e7 += INT32_C(1);
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, false,
                     fix.timestamp_ns));
  zexpect_within(measurement.position_enu_m[0], 0.007527f, 5.0e-5f);
  zexpect_within(measurement.position_enu_m[1], 0.0f, 1.0e-5f);

  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(5000000)));
  fix.longitude_deg_e7 -= INT32_C(1);
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, false,
                     fix.timestamp_ns));
  zexpect_within(measurement.position_enu_m[0], -0.007527f, 5.0e-5f);

  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(6000000)));
  fix.latitude_deg_e7 -= INT32_C(10000);
  fix.longitude_deg_e7 -= INT32_C(10000);
  fix.altitude_msl_mm -= INT32_C(10000);
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, false,
                     fix.timestamp_ns));
  zexpect_within(measurement.position_enu_m[0], -75.35521f, 0.002f);
  zexpect_within(measurement.position_enu_m[1], -111.31901f, 0.002f);
  zexpect_within(measurement.position_enu_m[2], -10.0f, 1.0e-5f);

  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(7000000)));
  fix.flags = synapse_topic_GnssFixFlags_CourseValid;
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, false,
                     fix.timestamp_ns));
  zexpect_true(measurement.valid);
  zexpect_true(measurement.position_valid);
  zexpect_false(measurement.velocity_valid);
  zexpect_equal(measurement.velocity_enu_m_s[0], 0.0f);
  zexpect_equal(measurement.velocity_enu_m_s[1], 0.0f);
  zexpect_equal(measurement.velocity_enu_m_s[2], 0.0f);

  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(8000000)));
  fix.flags = synapse_topic_GnssFixFlags_VelocityUpValid;
  fix.velocity_up_cm_s = INT16_C(150);
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, false,
                     fix.timestamp_ns));
  zexpect_true(measurement.position_valid);
  zexpect_false(measurement.velocity_valid);
  zexpect_equal(measurement.velocity_enu_m_s[0], 0.0f);
  zexpect_equal(measurement.velocity_enu_m_s[1], 0.0f);
  zexpect_equal(measurement.velocity_enu_m_s[2], 0.0f);
}

ZTEST(process_wrapper_fault_injection, test_navigation_gps_flight_site_vector) {
  struct rdd2_navigation_gps_adapter adapter;
  struct rdd2_navigation_gps_measurement measurement;
  synapse_topic_GnssFixData_t fix =
      valid_fix(TEST_NS_FROM_US(UINT64_C(1000000)));
  synapse_topic_VehicleHealthData_t health = disarmed_health(fix.timestamp_ns);

  fix.latitude_deg_e7 = INT32_C(404237000);
  fix.longitude_deg_e7 = -INT32_C(869212000);
  rdd2_navigation_gps_init(&adapter);
  zexpect_true(step(&adapter, &measurement, &fix, true, &health, true,
                    fix.timestamp_ns));

  fix.timestamp_ns += TEST_NS_FROM_US(UINT64_C(1000000));
  fix.latitude_deg_e7 += INT32_C(10000);
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, false,
                     fix.timestamp_ns));
  zexpect_within(measurement.position_enu_m[0], 0.0f, 0.002f);
  zexpect_within(measurement.position_enu_m[1], 111.3195f, 0.002f);

  fix.timestamp_ns += TEST_NS_FROM_US(UINT64_C(1000000));
  fix.latitude_deg_e7 -= INT32_C(10000);
  fix.longitude_deg_e7 += INT32_C(10000);
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, false,
                     fix.timestamp_ns));
  zexpect_within(measurement.position_enu_m[0], 84.7442f, 0.002f);
  zexpect_within(measurement.position_enu_m[1], 0.0f, 0.002f);
}

ZTEST(process_wrapper_fault_injection, test_navigation_gps_time_and_arm_gates) {
  struct rdd2_navigation_gps_adapter adapter;
  struct rdd2_navigation_gps_measurement measurement;
  synapse_topic_GnssFixData_t fix =
      valid_fix(TEST_NS_FROM_US(UINT64_C(1050000)));
  synapse_topic_VehicleHealthData_t health =
      disarmed_health(TEST_NS_FROM_US(UINT64_C(1000000)));

  rdd2_navigation_gps_init(&adapter);
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, true,
                     TEST_NS_FROM_US(UINT64_C(1000000))));
  zexpect_true(adapter.pending);
  zexpect_true(adapter.pending_origin_eligible);
  zexpect_false(measurement.valid);
  health.timestamp_ns = fix.timestamp_ns;
  zexpect_true(step(&adapter, &measurement, &fix, false, &health, true,
                    fix.timestamp_ns));
  zexpect_false(adapter.pending);

  rdd2_navigation_gps_init(&adapter);
  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(1100000)));
  health = disarmed_health(TEST_NS_FROM_US(UINT64_C(1000000)));
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, true,
                     TEST_NS_FROM_US(UINT64_C(1000000))));
  zexpect_true(adapter.pending);
  health.timestamp_ns = fix.timestamp_ns;
  zexpect_true(step(&adapter, &measurement, &fix, false, &health, true,
                    fix.timestamp_ns));
  zexpect_false(adapter.pending);

  rdd2_navigation_gps_init(&adapter);
  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(1100001)));
  health = disarmed_health(TEST_NS_FROM_US(UINT64_C(1000000)));
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, true,
                     TEST_NS_FROM_US(UINT64_C(1000000))));
  zexpect_false(adapter.pending);

  rdd2_navigation_gps_init(&adapter);
  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(500000)));
  health = disarmed_health(TEST_NS_FROM_US(UINT64_C(1000000)));
  zexpect_true(step(&adapter, &measurement, &fix, true, &health, true,
                    TEST_NS_FROM_US(UINT64_C(1000000))));
  zexpect_true(measurement.valid);

  rdd2_navigation_gps_init(&adapter);
  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(499999)));
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, true,
                     TEST_NS_FROM_US(UINT64_C(1000000))));
  zexpect_false(adapter.pending);

  rdd2_navigation_gps_init(&adapter);
  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(1000000)));
  health = disarmed_health(fix.timestamp_ns);
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, false,
                     fix.timestamp_ns));
  zexpect_false(adapter.pending);
  zexpect_false(adapter.origin_valid);

  rdd2_navigation_gps_init(&adapter);
  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(1000000)));
  health = disarmed_health(fix.timestamp_ns + TEST_NS_FROM_US(UINT64_C(20000)));
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, true,
                     fix.timestamp_ns));
  zexpect_false(adapter.origin_valid);

  rdd2_navigation_gps_init(&adapter);
  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(1050000)));
  health = disarmed_health(TEST_NS_FROM_US(UINT64_C(1000000)));
  health.flags = synapse_topic_VehicleHealthFlags_Armed;
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, true,
                     TEST_NS_FROM_US(UINT64_C(1000000))));
  zexpect_true(adapter.pending);
  zexpect_false(adapter.pending_origin_eligible);
  zexpect_false(adapter.origin_valid);
  health.flags = 0U;
  health.timestamp_ns = fix.timestamp_ns;
  zexpect_false(step(&adapter, &measurement, &fix, false, &health, true,
                     fix.timestamp_ns));
  zexpect_false(adapter.pending);
  zexpect_false(adapter.origin_valid);

  rdd2_navigation_gps_init(&adapter);
  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(1050000)));
  health = disarmed_health(TEST_NS_FROM_US(UINT64_C(1000000)));
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, true,
                     TEST_NS_FROM_US(UINT64_C(1000000))));
  health.timestamp_ns = fix.timestamp_ns;
  health.flags = synapse_topic_VehicleHealthFlags_Armed;
  zexpect_false(step(&adapter, &measurement, &fix, false, &health, true,
                     fix.timestamp_ns));
  zexpect_false(adapter.pending);
  zexpect_false(adapter.origin_valid);
}

ZTEST(process_wrapper_fault_injection,
      test_navigation_gps_preserves_due_pending_fix) {
  struct rdd2_navigation_gps_adapter adapter;
  struct rdd2_navigation_gps_measurement measurement;
  synapse_topic_GnssFixData_t pending_fix =
      valid_fix(TEST_NS_FROM_US(UINT64_C(1100000)));
  synapse_topic_GnssFixData_t newer_invalid_fix =
      valid_fix(TEST_NS_FROM_US(UINT64_C(1200000)));
  synapse_topic_VehicleHealthData_t health =
      disarmed_health(TEST_NS_FROM_US(UINT64_C(1000000)));

  newer_invalid_fix.fix_type = synapse_types_GnssFixType_Fix2d;
  rdd2_navigation_gps_init(&adapter);
  zexpect_false(step(&adapter, &measurement, &pending_fix, true, &health, true,
                     TEST_NS_FROM_US(UINT64_C(1000000))));
  zexpect_true(adapter.pending);

  health.timestamp_ns = pending_fix.timestamp_ns;
  zexpect_true(step(&adapter, &measurement, &newer_invalid_fix, true, &health,
                    true, pending_fix.timestamp_ns));
  zexpect_true(measurement.valid);
  zexpect_equal(adapter.last_consumed_timestamp_ns, pending_fix.timestamp_ns);
  zexpect_equal(adapter.origin_latitude_deg_e7, pending_fix.latitude_deg_e7);
  zexpect_within(measurement.position_enu_m[0], 0.0f, 1.0e-5f);
  zexpect_within(measurement.position_enu_m[1], 0.0f, 1.0e-5f);

  health.timestamp_ns = newer_invalid_fix.timestamp_ns;
  zexpect_false(step(&adapter, &measurement, &newer_invalid_fix, false, &health,
                     true, newer_invalid_fix.timestamp_ns));
  zexpect_false(adapter.pending);
  zexpect_false(measurement.valid);
}

ZTEST(process_wrapper_fault_injection,
      test_navigation_gps_retains_newer_while_oldest_is_future) {
  struct rdd2_navigation_gps_adapter adapter;
  struct rdd2_navigation_gps_measurement measurement;
  synapse_topic_GnssFixData_t oldest =
      valid_fix(TEST_NS_FROM_US(UINT64_C(1100000)));
  synapse_topic_GnssFixData_t successor =
      valid_fix(TEST_NS_FROM_US(UINT64_C(1150000)));
  synapse_topic_VehicleHealthData_t health =
      disarmed_health(TEST_NS_FROM_US(UINT64_C(1000000)));

  successor.latitude_deg_e7 += INT32_C(100);
  rdd2_navigation_gps_init(&adapter);
  zexpect_false(step(&adapter, &measurement, &oldest, true, &health, true,
                     TEST_NS_FROM_US(UINT64_C(1000000))));
  zexpect_true(adapter.pending);
  zexpect_false(adapter.successor_pending);

  health.timestamp_ns = TEST_NS_FROM_US(UINT64_C(1025000));
  zexpect_false(step(&adapter, &measurement, &successor, true, &health, true,
                     TEST_NS_FROM_US(UINT64_C(1025000))));
  zexpect_true(adapter.pending);
  zexpect_true(adapter.successor_pending);
  zexpect_equal(adapter.pending_fix.timestamp_ns, oldest.timestamp_ns);
  zexpect_equal(adapter.successor_fix.timestamp_ns, successor.timestamp_ns);

  health.timestamp_ns = oldest.timestamp_ns;
  zexpect_true(step(&adapter, &measurement, &successor, false, &health, true,
                    oldest.timestamp_ns));
  zexpect_true(measurement.valid);
  zexpect_equal(adapter.last_consumed_timestamp_ns, oldest.timestamp_ns);
  zexpect_true(adapter.pending);
  zexpect_equal(adapter.pending_fix.timestamp_ns, successor.timestamp_ns);

  health.timestamp_ns = successor.timestamp_ns;
  zexpect_false(step(&adapter, &measurement, &successor, false, &health, true,
                     successor.timestamp_ns));
  zexpect_true(measurement.valid);
  zexpect_equal(adapter.last_consumed_timestamp_ns, successor.timestamp_ns);
  zexpect_within(measurement.position_enu_m[1], 1.1132f, 0.002f);
}

ZTEST(process_wrapper_fault_injection,
      test_navigation_gps_discards_invalid_successor_before_new) {
  struct rdd2_navigation_gps_adapter adapter;
  struct rdd2_navigation_gps_measurement measurement;
  synapse_topic_GnssFixData_t oldest =
      valid_fix(TEST_NS_FROM_US(UINT64_C(1100000)));
  synapse_topic_GnssFixData_t invalid =
      valid_fix(TEST_NS_FROM_US(UINT64_C(1150000)));
  synapse_topic_GnssFixData_t newest =
      valid_fix(TEST_NS_FROM_US(UINT64_C(1200000)));
  synapse_topic_VehicleHealthData_t health =
      disarmed_health(TEST_NS_FROM_US(UINT64_C(1000000)));

  invalid.fix_type = synapse_types_GnssFixType_Fix2d;
  newest.latitude_deg_e7 += INT32_C(100);
  rdd2_navigation_gps_init(&adapter);
  zexpect_false(step(&adapter, &measurement, &oldest, true, &health, true,
                     TEST_NS_FROM_US(UINT64_C(1000000))));
  health.timestamp_ns = TEST_NS_FROM_US(UINT64_C(1025000));
  zexpect_false(step(&adapter, &measurement, &invalid, true, &health, true,
                     TEST_NS_FROM_US(UINT64_C(1025000))));

  health.timestamp_ns = oldest.timestamp_ns;
  zexpect_true(step(&adapter, &measurement, &invalid, false, &health, true,
                    oldest.timestamp_ns));
  zexpect_equal(adapter.last_consumed_timestamp_ns, oldest.timestamp_ns);
  zexpect_true(adapter.pending);
  zexpect_equal(adapter.pending_fix.timestamp_ns, invalid.timestamp_ns);

  health.timestamp_ns = newest.timestamp_ns;
  zexpect_false(step(&adapter, &measurement, &newest, true, &health, true,
                     newest.timestamp_ns));
  zexpect_true(measurement.valid);
  zexpect_equal(adapter.last_consumed_timestamp_ns, newest.timestamp_ns);
  zexpect_false(adapter.pending);
  zexpect_false(adapter.successor_pending);
}

ZTEST(process_wrapper_fault_injection,
      test_navigation_gps_health_discriminators) {
  struct rdd2_navigation_gps_adapter adapter;
  struct rdd2_navigation_gps_measurement measurement;
  synapse_topic_GnssFixData_t fix =
      valid_fix(TEST_NS_FROM_US(UINT64_C(1000000)));
  synapse_topic_VehicleHealthData_t health = disarmed_health(fix.timestamp_ns);

  rdd2_navigation_gps_init(&adapter);
  health.flags = synapse_topic_VehicleHealthFlags_Failsafe;
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, true,
                     fix.timestamp_ns));
  zexpect_false(adapter.origin_valid);

  rdd2_navigation_gps_init(&adapter);
  health = disarmed_health(fix.timestamp_ns + TEST_NS_FROM_US(UINT64_C(1)));
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, true,
                     fix.timestamp_ns));
  zexpect_false(adapter.origin_valid);

  rdd2_navigation_gps_init(&adapter);
  health = disarmed_health(fix.timestamp_ns - TEST_NS_FROM_US(UINT64_C(25001)));
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, true,
                     fix.timestamp_ns));
  zexpect_false(adapter.origin_valid);

  rdd2_navigation_gps_init(&adapter);
  health = disarmed_health(fix.timestamp_ns - TEST_NS_FROM_US(UINT64_C(25000)));
  zexpect_true(step(&adapter, &measurement, &fix, true, &health, true,
                    fix.timestamp_ns));
}

static void expect_rejected_origin(synapse_topic_GnssFixData_t fix) {
  struct rdd2_navigation_gps_adapter adapter;
  struct rdd2_navigation_gps_measurement measurement;
  synapse_topic_VehicleHealthData_t health = disarmed_health(fix.timestamp_ns);

  rdd2_navigation_gps_init(&adapter);
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, true,
                     fix.timestamp_ns));
  zexpect_false(adapter.pending);
  zexpect_false(adapter.origin_valid);
  zexpect_false(measurement.valid);
}

ZTEST(process_wrapper_fault_injection,
      test_navigation_gps_fix_and_accuracy_gates) {
  synapse_topic_GnssFixData_t fix =
      valid_fix(TEST_NS_FROM_US(UINT64_C(1000000)));

  fix.fix_type = synapse_types_GnssFixType_Fix2d;
  expect_rejected_origin(fix);
  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(1000000)));
  fix.fix_type = synapse_types_GnssFixType_DeadReckoning;
  expect_rejected_origin(fix);
  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(1000000)));
  fix.latitude_deg_e7 = INT32_C(900000001);
  expect_rejected_origin(fix);
  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(1000000)));
  fix.longitude_deg_e7 = INT32_C(1800000001);
  expect_rejected_origin(fix);
  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(1000000)));
  fix.horizontal_accuracy_mm = UINT16_MAX;
  expect_rejected_origin(fix);
  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(1000000)));
  fix.vertical_accuracy_mm = UINT16_C(15001);
  expect_rejected_origin(fix);
  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(1000000)));
  fix.altitude_msl_mm = INT32_C(20000001);
  expect_rejected_origin(fix);
}

ZTEST(process_wrapper_fault_injection, test_navigation_gps_local_range_gate) {
  struct rdd2_navigation_gps_adapter adapter;
  struct rdd2_navigation_gps_measurement measurement;
  synapse_topic_GnssFixData_t fix =
      valid_fix(TEST_NS_FROM_US(UINT64_C(1000000)));
  synapse_topic_VehicleHealthData_t health = disarmed_health(fix.timestamp_ns);

  rdd2_navigation_gps_init(&adapter);
  zexpect_true(step(&adapter, &measurement, &fix, true, &health, true,
                    fix.timestamp_ns));

  fix.timestamp_ns += TEST_NS_FROM_US(UINT64_C(1000000));
  fix.latitude_deg_e7 += INT32_C(900000);
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, false,
                     fix.timestamp_ns));
  zexpect_false(measurement.valid);
  zexpect_equal(adapter.last_consumed_timestamp_ns,
                TEST_NS_FROM_US(UINT64_C(1000000)));

  fix.timestamp_ns += TEST_NS_FROM_US(UINT64_C(1000000));
  fix.latitude_deg_e7 = INT32_C(473970000);
  fix.longitude_deg_e7 += INT32_C(1400000);
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, false,
                     fix.timestamp_ns));
  zexpect_false(measurement.valid);

  fix.timestamp_ns += TEST_NS_FROM_US(UINT64_C(1000000));
  fix.longitude_deg_e7 = INT32_C(85450000);
  fix.altitude_msl_mm += INT32_C(10000001);
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, false,
                     fix.timestamp_ns));
  zexpect_false(measurement.valid);
}

ZTEST(process_wrapper_fault_injection,
      test_navigation_gps_exact_accepted_thresholds) {
  const synapse_types_GnssFixType_enum_t accepted[] = {
      synapse_types_GnssFixType_Fix3d,
      synapse_types_GnssFixType_Dgnss,
      synapse_types_GnssFixType_RtkFloat,
      synapse_types_GnssFixType_RtkFixed,
  };

  for (size_t index = 0U; index < ARRAY_SIZE(accepted); ++index) {
    struct rdd2_navigation_gps_adapter adapter;
    struct rdd2_navigation_gps_measurement measurement;
    synapse_topic_GnssFixData_t fix =
        valid_fix(TEST_NS_FROM_US(UINT64_C(1000000)));
    synapse_topic_VehicleHealthData_t health =
        disarmed_health(fix.timestamp_ns);

    fix.fix_type = accepted[index];
    fix.horizontal_accuracy_mm = UINT16_C(10000);
    fix.vertical_accuracy_mm = UINT16_C(15000);
    fix.velocity_accuracy_mm_s = UINT16_C(5000);
    fix.course_over_ground_cdeg = UINT16_C(35999);
    rdd2_navigation_gps_init(&adapter);
    zexpect_true(step(&adapter, &measurement, &fix, true, &health, true,
                      fix.timestamp_ns));
    zexpect_true(measurement.position_valid);
    zexpect_true(measurement.velocity_valid);
    zexpect_within(measurement.position_covariance_enu_m2[0][0], 100.0f,
                   1.0e-4f);
    zexpect_within(measurement.position_covariance_enu_m2[2][2], 225.0f,
                   1.0e-4f);
    zexpect_within(measurement.velocity_covariance_enu_m2_s2[0][0], 25.0f,
                   1.0e-4f);
  }

  struct rdd2_navigation_gps_adapter adapter;
  struct rdd2_navigation_gps_measurement measurement;
  synapse_topic_GnssFixData_t fix =
      valid_fix(TEST_NS_FROM_US(UINT64_C(1000000)));
  synapse_topic_VehicleHealthData_t health = disarmed_health(fix.timestamp_ns);

  fix.velocity_accuracy_mm_s = UINT16_C(5001);
  rdd2_navigation_gps_init(&adapter);
  zexpect_true(step(&adapter, &measurement, &fix, true, &health, true,
                    fix.timestamp_ns));
  zexpect_false(measurement.velocity_valid);

  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(1000000)));
  fix.course_over_ground_cdeg = UINT16_C(36000);
  rdd2_navigation_gps_init(&adapter);
  zexpect_true(step(&adapter, &measurement, &fix, true, &health, true,
                    fix.timestamp_ns));
  zexpect_false(measurement.velocity_valid);

  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(1000000)));
  fix.horizontal_accuracy_mm = 0U;
  fix.vertical_accuracy_mm = 0U;
  fix.velocity_accuracy_mm_s = 0U;
  rdd2_navigation_gps_init(&adapter);
  zexpect_true(step(&adapter, &measurement, &fix, true, &health, true,
                    fix.timestamp_ns));
  zexpect_within(measurement.position_covariance_enu_m2[0][0], 0.25f, 1.0e-6f);
  zexpect_within(measurement.position_covariance_enu_m2[2][2], 0.25f, 1.0e-6f);
  zexpect_within(measurement.velocity_covariance_enu_m2_s2[0][0], 0.01f,
                 1.0e-6f);
}

ZTEST(process_wrapper_fault_injection,
      test_navigation_gps_exactly_once_monotonic) {
  struct rdd2_navigation_gps_adapter adapter;
  struct rdd2_navigation_gps_measurement measurement;
  synapse_topic_GnssFixData_t fix = valid_fix(TEST_NS_FROM_US(UINT64_C(0)));
  synapse_topic_VehicleHealthData_t health =
      disarmed_health(TEST_NS_FROM_US(UINT64_C(0)));

  rdd2_navigation_gps_init(&adapter);
  zexpect_true(step(&adapter, &measurement, &fix, true, &health, true,
                    TEST_NS_FROM_US(UINT64_C(0))));
  zexpect_true(adapter.has_consumed_timestamp);
  zexpect_equal(adapter.last_consumed_timestamp_ns,
                TEST_NS_FROM_US(UINT64_C(0)));

  zexpect_false(step(&adapter, &measurement, &fix, true, &health, false,
                     TEST_NS_FROM_US(UINT64_C(1000))));
  zexpect_false(measurement.valid);
  zexpect_false(adapter.pending);

  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(2000)));
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, false,
                     fix.timestamp_ns));
  zexpect_true(measurement.valid);
  zexpect_equal(adapter.last_consumed_timestamp_ns,
                TEST_NS_FROM_US(UINT64_C(2000)));

  fix.timestamp_ns = TEST_NS_FROM_US(UINT64_C(1000));
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, false,
                     TEST_NS_FROM_US(UINT64_C(3000))));
  zexpect_false(measurement.valid);
  zexpect_equal(adapter.last_consumed_timestamp_ns,
                TEST_NS_FROM_US(UINT64_C(2000)));

  fix.timestamp_ns = TEST_NS_FROM_US(UINT64_C(3000));
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, false,
                     TEST_NS_FROM_US(UINT64_C(503001))));
  zexpect_false(measurement.valid);
  zexpect_equal(adapter.last_consumed_timestamp_ns,
                TEST_NS_FROM_US(UINT64_C(2000)));

  fix.timestamp_ns = TEST_NS_FROM_US(UINT64_C(504000));
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, false,
                     fix.timestamp_ns));
  zexpect_true(measurement.valid);
  zexpect_equal(adapter.last_consumed_timestamp_ns,
                TEST_NS_FROM_US(UINT64_C(504000)));
}

ZTEST(process_wrapper_fault_injection,
      test_navigation_gps_origin_is_immutable) {
  struct rdd2_navigation_gps_adapter adapter;
  struct rdd2_navigation_gps_measurement measurement;
  synapse_topic_GnssFixData_t fix =
      valid_fix(TEST_NS_FROM_US(UINT64_C(1000000)));
  synapse_topic_VehicleHealthData_t health = disarmed_health(fix.timestamp_ns);

  rdd2_navigation_gps_init(&adapter);
  zexpect_true(step(&adapter, &measurement, &fix, true, &health, true,
                    fix.timestamp_ns));
  fix.timestamp_ns += TEST_NS_FROM_US(UINT64_C(1000000));
  fix.latitude_deg_e7 += INT32_C(10000);
  health.flags = synapse_topic_VehicleHealthFlags_Armed;
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, true,
                     fix.timestamp_ns));
  zexpect_equal(adapter.origin_latitude_deg_e7, INT32_C(473970000));
  zexpect_within(measurement.position_enu_m[1], 111.31949f, 0.002f);
  zexpect_true(isfinite(measurement.position_enu_m[0]));
  zexpect_true(isfinite(measurement.position_enu_m[1]));
  zexpect_true(isfinite(measurement.position_enu_m[2]));
}
