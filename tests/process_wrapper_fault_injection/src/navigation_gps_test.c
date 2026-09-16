/* SPDX-License-Identifier: Apache-2.0 */

#include "processes/navigation_gps.h"
#include "synapse_wire_restamp.h"

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

  /*
   * A CourseValid-only fix (the vehicle's GNSS receiver never sets
   * VelocityUpValid) yields a usable horizontal velocity: vz is forced to zero
   * and its variance is the large unobserved-vertical-velocity value, while the
   * horizontal components come from course and ground speed.
   */
  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(7000000)));
  fix.flags = synapse_topic_GnssFixFlags_CourseValid;
  fix.velocity_up_cm_s = INT16_C(150);
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, false,
                     fix.timestamp_ns));
  zexpect_true(measurement.valid);
  zexpect_true(measurement.position_valid);
  zexpect_true(measurement.velocity_valid);
  zexpect_within(measurement.velocity_enu_m_s[0], 1.0f, 1.0e-5f);
  zexpect_within(measurement.velocity_enu_m_s[1], 0.0f, 1.0e-5f);
  zexpect_equal(measurement.velocity_enu_m_s[2], 0.0f);
  zexpect_within(measurement.velocity_covariance_enu_m2_s2[0][0], 0.01f,
                 1.0e-6f);
  zexpect_within(measurement.velocity_covariance_enu_m2_s2[1][1], 0.01f,
                 1.0e-6f);
  zexpect_within(measurement.velocity_covariance_enu_m2_s2[2][2], 25.0f,
                 1.0e-4f);

  /*
   * VelocityUpValid without CourseValid carries no horizontal velocity, so the
   * measurement remains velocity-invalid.
   */
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

  /*
   * A fix with neither velocity flag is velocity-invalid.
   */
  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(9000000)));
  fix.flags = 0U;
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, false,
                     fix.timestamp_ns));
  zexpect_true(measurement.position_valid);
  zexpect_false(measurement.velocity_valid);
  zexpect_equal(measurement.velocity_enu_m_s[0], 0.0f);
  zexpect_equal(measurement.velocity_enu_m_s[1], 0.0f);
  zexpect_equal(measurement.velocity_enu_m_s[2], 0.0f);
}

/*
 * The vehicle's GNSS receiver reports course over ground and ground speed but
 * no vertical velocity, so every wire fix carries CourseValid without
 * VelocityUpValid. Such a fix must fuse as a horizontal velocity measurement:
 * horizontal components from course and speed, vertical velocity forced to zero
 * with the large unobserved-vertical-velocity variance, and the horizontal
 * variances still following the accuracy floor rule. A fix that additionally
 * reports VelocityUpValid keeps the full 3D velocity with the accuracy-derived
 * vertical variance.
 */
ZTEST(process_wrapper_fault_injection,
      test_navigation_gps_course_only_velocity) {
  struct rdd2_navigation_gps_adapter adapter;
  struct rdd2_navigation_gps_measurement measurement;
  synapse_topic_GnssFixData_t fix =
      valid_fix(TEST_NS_FROM_US(UINT64_C(1000000)));
  synapse_topic_VehicleHealthData_t health = disarmed_health(fix.timestamp_ns);

  /* Course 135 deg, ground speed 2 m/s: east +1.41421, north -1.41421. */
  fix.flags = synapse_topic_GnssFixFlags_CourseValid;
  fix.course_over_ground_cdeg = UINT16_C(13500);
  fix.ground_speed_cm_s = UINT16_C(200);
  fix.velocity_up_cm_s = INT16_C(300);
  fix.velocity_accuracy_mm_s = UINT16_C(300);
  rdd2_navigation_gps_init(&adapter);
  zexpect_true(step(&adapter, &measurement, &fix, true, &health, true,
                    fix.timestamp_ns));
  zexpect_true(measurement.velocity_valid);
  zexpect_within(measurement.velocity_enu_m_s[0], 1.41421f, 1.0e-4f);
  zexpect_within(measurement.velocity_enu_m_s[1], -1.41421f, 1.0e-4f);
  zexpect_equal(measurement.velocity_enu_m_s[2], 0.0f);
  zexpect_within(measurement.velocity_covariance_enu_m2_s2[0][0], 0.09f,
                 1.0e-5f);
  zexpect_within(measurement.velocity_covariance_enu_m2_s2[1][1], 0.09f,
                 1.0e-5f);
  zexpect_within(measurement.velocity_covariance_enu_m2_s2[2][2], 25.0f,
                 1.0e-4f);

  /* Same fix but with VelocityUpValid: vertical velocity and variance return. */
  fix = valid_fix(TEST_NS_FROM_US(UINT64_C(2000000)));
  fix.flags = synapse_topic_GnssFixFlags_CourseValid |
              synapse_topic_GnssFixFlags_VelocityUpValid;
  fix.course_over_ground_cdeg = UINT16_C(13500);
  fix.ground_speed_cm_s = UINT16_C(200);
  fix.velocity_up_cm_s = INT16_C(300);
  fix.velocity_accuracy_mm_s = UINT16_C(300);
  zexpect_false(step(&adapter, &measurement, &fix, true, &health, false,
                     fix.timestamp_ns));
  zexpect_true(measurement.velocity_valid);
  zexpect_within(measurement.velocity_enu_m_s[2], 3.0f, 1.0e-5f);
  zexpect_within(measurement.velocity_covariance_enu_m2_s2[2][2], 0.09f,
                 1.0e-5f);
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

/*
 * The direct-wire receiver re-stamps GNSS payload timestamps into the control
 * IMU boot domain before publishing. A producer freerun clock runs hundreds of
 * milliseconds ahead of the local IMU boot clock, and a gPTP producer runs on a
 * TAI epoch about 1.79e9 s ahead; without the re-stamp every such fix reads as
 * far in the future and the adapter never consumes one, so no origin is ever
 * captured. Feed the adapter the re-stamped timestamps for both domains and
 * assert it consumes each and captures the origin.
 */
ZTEST(process_wrapper_fault_injection,
      test_navigation_gps_consumes_restamped_wire_domains) {
  const uint64_t imu_now = TEST_NS_FROM_US(UINT64_C(1000000));
  const uint64_t receive_monotonic = imu_now;
  const uint64_t latency_ns = UINT64_C(2000000);

  {
    struct rdd2_navigation_gps_adapter adapter;
    struct rdd2_navigation_gps_measurement measurement;
    uint64_t producer_ts = imu_now + UINT64_C(355000000);
    uint64_t restamped = rdd2_synapse_wire_local_boot_timestamp_ns(
        producer_ts, synapse_types_TimeStatus_LocalFreerun, receive_monotonic,
        synapse_types_TimeStatus_LocalFreerun, 0, latency_ns);
    synapse_topic_GnssFixData_t fix = valid_fix(restamped);
    synapse_topic_VehicleHealthData_t health = disarmed_health(imu_now);

    zexpect_equal(restamped, receive_monotonic - latency_ns);
    rdd2_navigation_gps_init(&adapter);
    zexpect_true(
        step(&adapter, &measurement, &fix, true, &health, true, imu_now));
    zexpect_true(measurement.valid);
    zexpect_true(adapter.origin_valid);
  }

  {
    struct rdd2_navigation_gps_adapter adapter;
    struct rdd2_navigation_gps_measurement measurement;
    uint64_t producer_ts = UINT64_C(1790000000000000000);
    int64_t offset_ns =
        (int64_t)producer_ts - (int64_t)(imu_now - latency_ns);
    uint64_t restamped = rdd2_synapse_wire_local_boot_timestamp_ns(
        producer_ts, synapse_types_TimeStatus_GptpSynced, receive_monotonic,
        synapse_types_TimeStatus_GptpSynced, offset_ns, latency_ns);
    synapse_topic_GnssFixData_t fix = valid_fix(restamped);
    synapse_topic_VehicleHealthData_t health = disarmed_health(imu_now);

    zexpect_equal(restamped, imu_now - latency_ns);
    rdd2_navigation_gps_init(&adapter);
    zexpect_true(
        step(&adapter, &measurement, &fix, true, &health, true, imu_now));
    zexpect_true(measurement.valid);
    zexpect_true(adapter.origin_valid);
  }
}

/*
 * Health that trails the control tick by more than HEALTH_MAX_AGE_NS (25 ms)
 * must not seed a GNSS origin. The lockstep and same-image BIL paths otherwise
 * present health stamped at the current control tick, so this disarmed-origin
 * age gate is never exercised there. Drive it to its edge: a fix that arrives
 * with 30 ms-old health is deferred (no origin, no valid measurement, no
 * fault), and a later fix with in-window health captures the origin, proving
 * the rejection was a deferral rather than a permanent refusal.
 */
ZTEST(process_wrapper_fault_injection,
      test_navigation_gps_origin_defers_on_stale_health) {
  struct rdd2_navigation_gps_adapter adapter;
  struct rdd2_navigation_gps_measurement measurement;
  const uint64_t first_tick = TEST_NS_FROM_US(UINT64_C(1000000));
  const uint64_t second_tick = TEST_NS_FROM_US(UINT64_C(1100000));
  synapse_topic_GnssFixData_t fix = valid_fix(first_tick);
  synapse_topic_VehicleHealthData_t health =
      disarmed_health(first_tick - TEST_NS_FROM_US(UINT64_C(30000)));

  rdd2_navigation_gps_init(&adapter);
  zexpect_false(
      step(&adapter, &measurement, &fix, true, &health, true, first_tick));
  zexpect_false(measurement.valid);
  zexpect_false(adapter.origin_valid);
  zexpect_false(adapter.pending);

  fix = valid_fix(second_tick);
  health = disarmed_health(second_tick - TEST_NS_FROM_US(UINT64_C(5000)));
  zexpect_true(
      step(&adapter, &measurement, &fix, true, &health, true, second_tick));
  zexpect_true(measurement.valid);
  zexpect_true(adapter.origin_valid);
}
