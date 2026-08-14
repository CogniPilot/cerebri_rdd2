/* SPDX-License-Identifier: Apache-2.0 */

#include <math.h>
#include <setjmp.h>
#include <string.h>
#include <zephyr/ztest.h>

#define TEST_NS_FROM_US(value) ((uint64_t)(value) * UINT64_C(1000))
#define RDD2_TEST_EVERY_SAMPLE_IS_RELEASE 1

#define zros_node_init navigation_fake_zros_node_init
#define zros_pub_init navigation_fake_zros_pub_init
#define zros_pub_update navigation_fake_zros_pub_update
#define zros_sub_init navigation_fake_zros_sub_init
#define zros_sub_update navigation_fake_zros_sub_update
#define zros_sub_wait navigation_fake_zros_sub_wait
#define topic_attitude_estimate navigation_fake_topic_attitude_estimate
#define topic_control_imu navigation_fake_topic_control_imu
#define topic_external_odometry navigation_fake_topic_external_odometry
#define topic_gnss_fix navigation_fake_topic_gnss_fix
#define topic_navigation_odometry navigation_fake_topic_navigation_odometry
#define topic_vehicle_health navigation_fake_topic_vehicle_health

#include "../../../src/processes/navigation_estimator.c"

#undef topic_navigation_odometry
#undef topic_vehicle_health
#undef topic_gnss_fix
#undef topic_external_odometry
#undef topic_control_imu
#undef topic_attitude_estimate
#undef zros_sub_wait
#undef zros_sub_update
#undef zros_sub_init
#undef zros_pub_update
#undef zros_pub_init
#undef zros_node_init

enum navigation_scenario {
  NAV_SCENARIO_BASELINE,
  NAV_SCENARIO_STARTUP_NAN,
  NAV_SCENARIO_POST_INITIALIZATION_NAN,
  NAV_SCENARIO_ZERO_MOCAP_QUATERNION,
  NAV_SCENARIO_GPS_POSITION_ONLY,
  NAV_SCENARIO_GPS_FIX2D_REJECTED,
  NAV_SCENARIO_GPS_VELOCITY,
  NAV_SCENARIO_GPS_WITH_RETAINED_MOCAP,
  NAV_SCENARIO_LOCKSTEP_WALL_HEALTH,
  NAV_SCENARIO_GPS_ORIGIN_RETRY,
  NAV_SCENARIO_GPS_ORIGIN_RADIO_TIMEOUT,
  NAV_SCENARIO_NONFINITE_MOCAP,
  NAV_SCENARIO_LARGE_GPS_CORRECTION_RECOVERY,
};

struct generated_observation {
  bool imu_valid;
  bool estimate_valid;
  bool status_initialized;
  bool status_prediction_accepted;
  bool reset;
  bool gps_valid;
  bool gps_fresh;
  bool gps_position_valid;
  bool gps_velocity_valid;
  bool mocap_valid;
  bool mocap_fresh;
  bool gps_origin_initialization_pending;
  bool status_gps_position_correction_accepted;
  bool status_gps_velocity_correction_accepted;
  int32_t status_consecutive_rejected_corrections;
  int32_t status_gps_consecutive_rejections;
  int32_t status_correction_outcome;
  int32_t status_correction_source;
  int32_t status_recovery_stage;
  int32_t status_anchor_source;
  float status_rejection_elapsed_s;
  uint32_t error_signal_status;
  float covariance_x;
  float position[3];
  float quaternion[4];
};

struct publication_observation {
  size_t odometry_publish_count;
  size_t attitude_publish_count;
  synapse_topic_OdometryEstimateData_t odometry;
  synapse_topic_AttitudeEstimateData_t attitude;
};

#define MAX_SCRIPT_CYCLES 9U

static jmp_buf loop_escape;
static enum navigation_scenario active_scenario;
static size_t active_cycle;
static size_t next_cycle;
static size_t cycle_count;
static struct generated_observation generated[MAX_SCRIPT_CYCLES];
static struct publication_observation publications[MAX_SCRIPT_CYCLES];

struct zros_topic navigation_fake_topic_attitude_estimate;
struct zros_topic navigation_fake_topic_control_imu;
struct zros_topic navigation_fake_topic_external_odometry;
struct zros_topic navigation_fake_topic_gnss_fix;
struct zros_topic navigation_fake_topic_navigation_odometry;
struct zros_topic navigation_fake_topic_vehicle_health;

static uint64_t scripted_imu_timestamp_ns(size_t cycle) {
  if (active_scenario == NAV_SCENARIO_GPS_ORIGIN_RADIO_TIMEOUT) {
    const uint64_t timestamps[] = {
        TEST_NS_FROM_US(UINT64_C(200000)),
        TEST_NS_FROM_US(UINT64_C(300000)),
        TEST_NS_FROM_US(UINT64_C(300001)),
    };

    return timestamps[cycle];
  }
  return TEST_NS_FROM_US(UINT64_C(200000)) +
         cycle * TEST_NS_FROM_US(UINT64_C(1000));
}

void navigation_fake_zros_node_init(struct zros_node *node, const char *name) {
  ARG_UNUSED(node);
  ARG_UNUSED(name);
}

int navigation_fake_zros_sub_init(struct zros_sub *sub, struct zros_node *node,
                                  struct zros_topic *topic, void *data,
                                  double rate_limit_hz) {
  ARG_UNUSED(node);
  ARG_UNUSED(rate_limit_hz);
  sub->_topic = topic;
  sub->_data = data;
  return 0;
}

int navigation_fake_zros_pub_init(struct zros_pub *pub, struct zros_node *node,
                                  struct zros_topic *topic, void *data) {
  ARG_UNUSED(node);
  pub->_topic = topic;
  pub->_data = data;
  return 0;
}

int navigation_fake_zros_sub_wait(struct zros_sub *sub, k_timeout_t timeout) {
  ARG_UNUSED(sub);
  ARG_UNUSED(timeout);
  if (next_cycle == cycle_count) {
    longjmp(loop_escape, 1);
  }
  active_cycle = next_cycle++;
  return 0;
}

static void fill_imu_sample(void) {
  g_process.imu = (synapse_topic_InertialSampleData_t){
      .timestamp_ns = scripted_imu_timestamp_ns(active_cycle),
      .accel_flu_m_s2 = {.x = 0.0f, .y = 0.0f, .z = 9.81f},
      .gyro_flu_rad_s = {.x = 0.01f, .y = -0.02f, .z = 0.03f},
      .flags = synapse_topic_InertialFieldFlags_Accel |
               synapse_topic_InertialFieldFlags_Gyro,
  };
  if (active_scenario == NAV_SCENARIO_STARTUP_NAN ||
      active_scenario == NAV_SCENARIO_GPS_ORIGIN_RADIO_TIMEOUT ||
      (active_scenario == NAV_SCENARIO_GPS_ORIGIN_RETRY &&
       active_cycle == 0U) ||
      (active_scenario == NAV_SCENARIO_POST_INITIALIZATION_NAN &&
       active_cycle == 1U)) {
    g_process.imu.accel_flu_m_s2.x = NAN;
  }
}

static void fill_mocap_sample(void) {
  if (active_scenario == NAV_SCENARIO_GPS_POSITION_ONLY ||
      active_scenario == NAV_SCENARIO_GPS_FIX2D_REJECTED ||
      active_scenario == NAV_SCENARIO_GPS_VELOCITY ||
      active_scenario == NAV_SCENARIO_LARGE_GPS_CORRECTION_RECOVERY) {
    g_process.external_odometry = (synapse_topic_ExternalOdometryData_t){0};
    return;
  }
  g_process.external_odometry = (synapse_topic_ExternalOdometryData_t){
      .timestamp_ns = TEST_NS_FROM_US(UINT64_C(199000)) +
                      active_cycle * TEST_NS_FROM_US(UINT64_C(1000)),
      .position_enu_m = {.x = 10.0f, .y = 20.0f, .z = 30.0f},
      .attitude = {.w = 1.0f, .x = 0.0f, .y = 0.0f, .z = 0.0f},
      .flags = synapse_topic_ExternalOdometryFlags_PositionValid |
               synapse_topic_ExternalOdometryFlags_AttitudeValid,
  };
  if (active_scenario == NAV_SCENARIO_ZERO_MOCAP_QUATERNION) {
    g_process.external_odometry.attitude.w = 0.0f;
    g_process.external_odometry.attitude.x = 0.0f;
    g_process.external_odometry.attitude.y = 0.0f;
    g_process.external_odometry.attitude.z = 0.0f;
  }
  if (active_scenario == NAV_SCENARIO_NONFINITE_MOCAP && active_cycle == 1U) {
    g_process.external_odometry.position_enu_m.x = NAN;
  }
}

static void fill_gps_sample(void) {
  g_process.gnss = (synapse_topic_GnssFixData_t){
      .timestamp_ns = g_process.imu.timestamp_ns,
      .latitude_deg_e7 =
          INT32_C(473970000) + (int32_t)active_cycle * INT32_C(100),
      .longitude_deg_e7 = INT32_C(85450000),
      .altitude_msl_mm = INT32_C(488000),
      .horizontal_accuracy_mm = UINT16_C(500),
      .vertical_accuracy_mm = UINT16_C(700),
      .velocity_accuracy_mm_s = UINT16_C(100),
      .fix_type = synapse_types_GnssFixType_Fix3d,
  };
  if (active_scenario == NAV_SCENARIO_GPS_VELOCITY) {
    g_process.gnss.ground_speed_cm_s = UINT16_C(100);
    g_process.gnss.course_over_ground_cdeg = UINT16_C(9000);
    g_process.gnss.velocity_up_cm_s = INT16_C(25);
    g_process.gnss.flags = synapse_topic_GnssFixFlags_CourseValid |
                           synapse_topic_GnssFixFlags_VelocityUpValid;
  }
  if (active_scenario == NAV_SCENARIO_GPS_FIX2D_REJECTED &&
      active_cycle == 1U) {
    g_process.gnss.fix_type = synapse_types_GnssFixType_Fix2d;
  }
  if (active_scenario == NAV_SCENARIO_LARGE_GPS_CORRECTION_RECOVERY) {
    g_process.gnss.latitude_deg_e7 = INT32_C(473970000);
    if (active_cycle >= 1U && active_cycle <= 6U) {
      /* About 1.1 km: finite and inside the firmware's 10 km ENU guard, but
       * far outside the estimator's nominal innovation gate. */
      g_process.gnss.latitude_deg_e7 += INT32_C(100000);
    }
  }
}

static void fill_health_sample(void) {
  g_process.health = (synapse_topic_VehicleHealthData_t){
      .timestamp_ns = g_process.imu.timestamp_ns,
  };
}

int navigation_fake_zros_sub_update(struct zros_sub *sub) {
  if (sub == &g_process.imu_sub) {
    fill_imu_sample();
    return 0;
  }
  if (sub == &g_process.external_odometry_sub) {
    fill_mocap_sample();
    if (active_scenario == NAV_SCENARIO_GPS_POSITION_ONLY ||
        active_scenario == NAV_SCENARIO_GPS_FIX2D_REJECTED ||
        active_scenario == NAV_SCENARIO_GPS_VELOCITY ||
        active_scenario == NAV_SCENARIO_LARGE_GPS_CORRECTION_RECOVERY) {
      return -1;
    }
    if (active_scenario == NAV_SCENARIO_POST_INITIALIZATION_NAN &&
        active_cycle == 1U) {
      return -1;
    }
    return 0;
  }
  if (sub == &g_process.gnss_sub) {
    if ((active_scenario == NAV_SCENARIO_GPS_ORIGIN_RETRY ||
         active_scenario == NAV_SCENARIO_GPS_ORIGIN_RADIO_TIMEOUT) &&
        active_cycle > 0U) {
      return -1;
    }
    if (active_scenario == NAV_SCENARIO_GPS_POSITION_ONLY ||
        active_scenario == NAV_SCENARIO_GPS_FIX2D_REJECTED ||
        active_scenario == NAV_SCENARIO_GPS_VELOCITY ||
        active_scenario == NAV_SCENARIO_GPS_WITH_RETAINED_MOCAP ||
        active_scenario == NAV_SCENARIO_LOCKSTEP_WALL_HEALTH ||
        active_scenario == NAV_SCENARIO_GPS_ORIGIN_RETRY ||
        active_scenario == NAV_SCENARIO_GPS_ORIGIN_RADIO_TIMEOUT ||
        active_scenario == NAV_SCENARIO_LARGE_GPS_CORRECTION_RECOVERY) {
      fill_gps_sample();
    }
    return 0;
  }
  if (sub == &g_process.health_sub) {
    if ((active_scenario == NAV_SCENARIO_GPS_ORIGIN_RETRY ||
         active_scenario == NAV_SCENARIO_GPS_ORIGIN_RADIO_TIMEOUT) &&
        active_cycle > 0U) {
      return -1;
    }
    if (active_scenario == NAV_SCENARIO_GPS_POSITION_ONLY ||
        active_scenario == NAV_SCENARIO_GPS_FIX2D_REJECTED ||
        active_scenario == NAV_SCENARIO_GPS_VELOCITY ||
        active_scenario == NAV_SCENARIO_GPS_WITH_RETAINED_MOCAP ||
        active_scenario == NAV_SCENARIO_LOCKSTEP_WALL_HEALTH ||
        active_scenario == NAV_SCENARIO_GPS_ORIGIN_RETRY ||
        active_scenario == NAV_SCENARIO_GPS_ORIGIN_RADIO_TIMEOUT ||
        active_scenario == NAV_SCENARIO_LARGE_GPS_CORRECTION_RECOVERY) {
      fill_health_sample();
      if (active_scenario == NAV_SCENARIO_LOCKSTEP_WALL_HEALTH) {
        g_process.health.timestamp_ns = TEST_NS_FROM_US(UINT64_C(900000000));
      }
      return 0;
    }
    return -1;
  }
  return -1;
}

static void capture_generated_observation(void) {
  struct generated_observation *observation = &generated[active_cycle];

  observation->imu_valid = g_process.efmu.imu_valid;
  observation->estimate_valid = g_process.efmu.estimate_valid;
  observation->status_initialized = g_process.efmu.status_initialized;
  observation->status_prediction_accepted =
      g_process.efmu.status_predictionAccepted;
  observation->reset = g_process.efmu.reset;
  observation->gps_valid = g_process.efmu.gps_valid;
  observation->gps_fresh = g_process.efmu.gps_fresh;
  observation->gps_position_valid = g_process.efmu.positionValid;
  observation->gps_velocity_valid = g_process.efmu.velocityValid;
  observation->mocap_valid = g_process.efmu.mocap_valid;
  observation->mocap_fresh = g_process.efmu.mocap_fresh;
  observation->gps_origin_initialization_pending =
      g_process.gps_origin_initialization_pending;
  observation->status_gps_position_correction_accepted =
      g_process.efmu.status_gpsPositionCorrectionAccepted;
  observation->status_gps_velocity_correction_accepted =
      g_process.efmu.status_gpsVelocityCorrectionAccepted;
  observation->status_consecutive_rejected_corrections =
      g_process.efmu.status_consecutiveRejectedCorrections;
  observation->status_gps_consecutive_rejections =
      g_process.efmu.gpsConsecutiveRejections;
  observation->status_correction_outcome =
      g_process.efmu.status_correctionOutcome;
  observation->status_correction_source =
      g_process.efmu.status_correctionSource;
  observation->status_recovery_stage = g_process.efmu.status_recoveryStage;
  observation->status_anchor_source = g_process.efmu.status_anchorSource;
  observation->status_rejection_elapsed_s =
      g_process.efmu.status_rejectionElapsed_s;
  observation->error_signal_status =
      g_process.efmu.rumoca_galec_error_signal_status;
  observation->covariance_x = g_process.efmu.stateCovariance[0][0];
  memcpy(observation->position, g_process.efmu.estimate_positionWorldEnu_m,
         sizeof(observation->position));
  memcpy(observation->quaternion, g_process.efmu.estimate_quaternionWorldBody,
         sizeof(observation->quaternion));
}

int navigation_fake_zros_pub_update(struct zros_pub *pub) {
  struct publication_observation *observation = &publications[active_cycle];

  capture_generated_observation();
  if (pub == &g_process.odometry_pub) {
    observation->odometry_publish_count++;
    observation->odometry = g_process.odometry;
    return 0;
  }
  if (pub == &g_process.attitude_pub) {
    observation->attitude_publish_count++;
    observation->attitude = g_process.attitude;
    return 0;
  }
  return -1;
}

static void run_scenario(enum navigation_scenario scenario, size_t cycles) {
  zassert_true(cycles > 0U && cycles <= MAX_SCRIPT_CYCLES,
               "invalid scripted cycle count");
  memset(&g_process, 0, sizeof(g_process));
  memset(generated, 0, sizeof(generated));
  memset(publications, 0, sizeof(publications));
  active_scenario = scenario;
  active_cycle = 0U;
  next_cycle = 0U;
  cycle_count = cycles;
  NavigationEstimator_startup(&g_process.efmu);
  rdd2_navigation_gps_init(&g_process.gps_adapter);
  set_default_mocap_covariance(&g_process.efmu);
  NavigationEstimator_recalibrate(&g_process.efmu);
  if (scenario == NAV_SCENARIO_LARGE_GPS_CORRECTION_RECOVERY) {
    g_process.efmu.covarianceInflateWindow_s = 0.002f;
    g_process.efmu.covarianceInflateTimeConstant_s = 0.001f;
    g_process.efmu.aidingDivergentWindow_s = 0.005f;
    g_process.efmu.aidingStaleTimeout_s = 0.0001f;
  }

  if (setjmp(loop_escape) == 0) {
    navigation_estimator_thread(&g_process, NULL, NULL);
    zassert_unreachable("navigation wrapper returned before script completion");
  }
  zassert_equal(next_cycle, cycles, "not all scripted cycles ran");
}

static bool floats_are_finite(const float *values, size_t count) {
  for (size_t index = 0U; index < count; ++index) {
    if (!isfinite(values[index])) {
      return false;
    }
  }
  return true;
}

static void expect_one_publication(size_t cycle) {
  zexpect_equal(publications[cycle].odometry_publish_count, 1U);
  zexpect_equal(publications[cycle].attitude_publish_count, 1U);
  zexpect_equal(publications[cycle].odometry.timestamp_ns,
                scripted_imu_timestamp_ns(cycle));
  zexpect_equal(publications[cycle].attitude.timestamp_ns,
                scripted_imu_timestamp_ns(cycle));
}

static void expect_safe_publication(size_t cycle) {
  const struct publication_observation *observation = &publications[cycle];

  expect_one_publication(cycle);
  zexpect_equal(observation->attitude.flags, 0U);
  zexpect_equal(observation->attitude.attitude.w, 1.0f);
  zexpect_equal(observation->attitude.attitude.x, 0.0f);
  zexpect_equal(observation->attitude.attitude.y, 0.0f);
  zexpect_equal(observation->attitude.attitude.z, 0.0f);
  zexpect_equal(observation->attitude.angular_velocity_flu_rad_s.roll, 0.0f);
  zexpect_equal(observation->attitude.angular_velocity_flu_rad_s.pitch, 0.0f);
  zexpect_equal(observation->attitude.angular_velocity_flu_rad_s.yaw, 0.0f);
  zexpect_equal(observation->odometry.position_enu_m.x, 0.0f);
  zexpect_equal(observation->odometry.position_enu_m.y, 0.0f);
  zexpect_equal(observation->odometry.position_enu_m.z, 0.0f);
  zexpect_equal(observation->odometry.velocity_enu_m_s.x, 0.0f);
  zexpect_equal(observation->odometry.velocity_enu_m_s.y, 0.0f);
  zexpect_equal(observation->odometry.velocity_enu_m_s.z, 0.0f);
  zexpect_equal(observation->odometry.attitude.w, 1.0f);
  zexpect_equal(observation->odometry.attitude.x, 0.0f);
  zexpect_equal(observation->odometry.attitude.y, 0.0f);
  zexpect_equal(observation->odometry.attitude.z, 0.0f);
  zexpect_equal(observation->odometry.angular_velocity_flu_rad_s.roll, 0.0f);
  zexpect_equal(observation->odometry.angular_velocity_flu_rad_s.pitch, 0.0f);
  zexpect_equal(observation->odometry.angular_velocity_flu_rad_s.yaw, 0.0f);
  zexpect_equal(observation->odometry.quality_pct, 0);
  zexpect_true(isfinite(observation->attitude.attitude.w));
  zexpect_true(isfinite(observation->odometry.position_enu_m.x));
}

static void expect_valid_publication(size_t cycle) {
  const uint8_t required = synapse_topic_AttitudeEstimateFlags_AttitudeValid |
                           synapse_topic_AttitudeEstimateFlags_RatesValid;

  expect_one_publication(cycle);
  zexpect_equal(publications[cycle].attitude.flags, required);
  zexpect_equal(publications[cycle].attitude.attitude.w, 1.0f);
  zexpect_equal(publications[cycle].attitude.angular_velocity_flu_rad_s.roll,
                0.01f);
  zexpect_equal(publications[cycle].odometry.position_enu_m.x, 10.0f);
  zexpect_equal(publications[cycle].odometry.position_enu_m.y, 20.0f);
  zexpect_equal(publications[cycle].odometry.position_enu_m.z, 30.0f);
  zexpect_equal(publications[cycle].odometry.quality_pct, 100);
}

static void expect_powered_publication(size_t cycle, int8_t quality_pct) {
  const uint8_t required = synapse_topic_AttitudeEstimateFlags_AttitudeValid |
                           synapse_topic_AttitudeEstimateFlags_RatesValid;

  expect_one_publication(cycle);
  zexpect_equal(publications[cycle].attitude.flags, required);
  zexpect_equal(publications[cycle].odometry.quality_pct, quality_pct);
  zexpect_true(isfinite(publications[cycle].attitude.attitude.w));
  zexpect_true(
      isfinite(publications[cycle].attitude.angular_velocity_flu_rad_s.roll));
  zexpect_true(isfinite(publications[cycle].odometry.position_enu_m.x));
}

ZTEST(generated_navigation_fault_injection, test_baseline_is_valid_and_finite) {
  run_scenario(NAV_SCENARIO_BASELINE, 1U);

  zexpect_true(generated[0].imu_valid);
  zexpect_equal(generated[0].error_signal_status, 0U);
  zexpect_true(generated[0].estimate_valid);
  zexpect_true(generated[0].status_initialized);
  zexpect_true(floats_are_finite(generated[0].position, 3U));
  zexpect_true(floats_are_finite(generated[0].quaternion, 4U));
  const float quaternion_norm_squared =
      generated[0].quaternion[0] * generated[0].quaternion[0] +
      generated[0].quaternion[1] * generated[0].quaternion[1] +
      generated[0].quaternion[2] * generated[0].quaternion[2] +
      generated[0].quaternion[3] * generated[0].quaternion[3];
  zexpect_within(quaternion_norm_squared, 1.0f, 1.0e-6f);
  expect_valid_publication(0U);
}

ZTEST(generated_navigation_fault_injection,
      test_onboard_gnss_configuration_disables_mocap) {
  zexpect_false(external_odometry_source_allowed(true));
  zexpect_true(external_odometry_source_allowed(false));
  zexpect_false(gps_origin_initialization_timed_out(
      true, TEST_NS_FROM_US(UINT64_C(200000)),
      TEST_NS_FROM_US(UINT64_C(300001))));
  zexpect_false(gps_origin_initialization_timed_out(
      false, TEST_NS_FROM_US(UINT64_C(200000)),
      TEST_NS_FROM_US(UINT64_C(300000))));
  zexpect_true(gps_origin_initialization_timed_out(
      false, TEST_NS_FROM_US(UINT64_C(200000)),
      TEST_NS_FROM_US(UINT64_C(300001))));
}

ZTEST(generated_navigation_fault_injection, test_startup_nan_imu_is_invalid) {
  run_scenario(NAV_SCENARIO_STARTUP_NAN, 1U);

  zexpect_false(generated[0].imu_valid);
  /* The reset path does not consume the invalid force in status-producing
   * estimator math. Firmware input validity is therefore the safety gate. */
  zexpect_equal(generated[0].error_signal_status, 0U);
  zexpect_true(generated[0].estimate_valid);
  zexpect_true(generated[0].status_initialized);
  expect_safe_publication(0U);
}

ZTEST(generated_navigation_fault_injection,
      test_post_initialization_nan_imu_is_invalid) {
  run_scenario(NAV_SCENARIO_POST_INITIALIZATION_NAN, 2U);

  expect_valid_publication(0U);
  zexpect_true(generated[0].imu_valid);
  zexpect_false(generated[1].imu_valid);
  zexpect_equal(generated[1].error_signal_status, 0U);
  zexpect_true(generated[1].estimate_valid);
  zexpect_true(generated[1].status_initialized);
  zexpect_false(generated[1].status_prediction_accepted);
  zexpect_true(floats_are_finite(generated[1].position, 3U));
  zexpect_true(floats_are_finite(generated[1].quaternion, 4U));
  expect_safe_publication(1U);
}

ZTEST(generated_navigation_fault_injection,
      test_zero_mocap_quaternion_is_invalid) {
  run_scenario(NAV_SCENARIO_ZERO_MOCAP_QUATERNION, 1U);

  zexpect_true(generated[0].imu_valid);
  zexpect_true((generated[0].error_signal_status & UINT32_C(1)) != 0U);
  zexpect_false(generated[0].estimate_valid);
  zexpect_true(generated[0].status_initialized);
  zexpect_false(floats_are_finite(generated[0].quaternion, 4U));
  expect_safe_publication(0U);
}

ZTEST(generated_navigation_fault_injection,
      test_gps_position_only_aids_actual_v3) {
  run_scenario(NAV_SCENARIO_GPS_POSITION_ONLY, 2U);

  zexpect_true(generated[0].reset);
  zexpect_true(generated[0].gps_valid);
  zexpect_true(generated[0].gps_fresh);
  zexpect_true(generated[0].gps_position_valid);
  zexpect_false(generated[0].gps_velocity_valid);
  zexpect_false(generated[1].reset);
  zexpect_true(generated[1].gps_valid);
  zexpect_true(generated[1].gps_fresh);
  zexpect_true(generated[1].gps_position_valid);
  zexpect_false(generated[1].gps_velocity_valid);
  zexpect_true(generated[1].status_gps_position_correction_accepted);
  zexpect_false(generated[1].status_gps_velocity_correction_accepted);
  zexpect_equal(generated[1].status_correction_outcome, 1);
  zexpect_equal(generated[1].status_correction_source, 2);
  zexpect_equal(generated[1].status_consecutive_rejected_corrections, 0);
  zexpect_true(isfinite(generated[0].covariance_x));
  zexpect_true(isfinite(generated[1].covariance_x));
  zexpect_true(generated[1].covariance_x < generated[0].covariance_x * 0.75f);
  zexpect_equal(generated[1].error_signal_status, 0U);
  zexpect_true(floats_are_finite(generated[1].position, 3U));
  expect_one_publication(0U);
  expect_one_publication(1U);
}

ZTEST(generated_navigation_fault_injection,
      test_fix2d_never_reaches_actual_v3_gps_gate) {
  run_scenario(NAV_SCENARIO_GPS_FIX2D_REJECTED, 2U);

  zexpect_true(generated[0].gps_valid);
  zexpect_true(generated[0].gps_position_valid);
  zexpect_false(generated[1].reset);
  zexpect_false(generated[1].gps_valid);
  zexpect_false(generated[1].gps_fresh);
  zexpect_false(generated[1].gps_position_valid);
  zexpect_false(generated[1].gps_velocity_valid);
  zexpect_false(generated[1].status_gps_position_correction_accepted);
  zexpect_false(generated[1].status_gps_velocity_correction_accepted);
  zexpect_equal(generated[1].error_signal_status, 0U);
  zexpect_true(floats_are_finite(generated[1].position, 3U));
  expect_one_publication(0U);
  expect_one_publication(1U);
}

ZTEST(generated_navigation_fault_injection,
      test_gps_velocity_aids_actual_generated_bundle) {
  run_scenario(NAV_SCENARIO_GPS_VELOCITY, 2U);

  zexpect_true(generated[0].gps_velocity_valid);
  zexpect_true(generated[1].gps_velocity_valid);
  zexpect_true(generated[1].status_gps_position_correction_accepted);
  zexpect_true(generated[1].status_gps_velocity_correction_accepted);
  zexpect_equal(generated[1].error_signal_status, 0U);
  zexpect_true(floats_are_finite(generated[1].position, 3U));
}

ZTEST(generated_navigation_fault_injection,
      test_origin_reset_suppresses_retained_mocap) {
  run_scenario(NAV_SCENARIO_GPS_WITH_RETAINED_MOCAP, 1U);

  zexpect_true(generated[0].reset);
  zexpect_true(generated[0].gps_valid);
  zexpect_true(generated[0].gps_position_valid);
  zexpect_false(generated[0].mocap_valid);
  zexpect_false(generated[0].mocap_fresh);
  zexpect_equal(generated[0].error_signal_status, 0U);
  zexpect_within(generated[0].position[0], 0.0f, 1.0e-5f);
  zexpect_within(generated[0].position[1], 0.0f, 1.0e-5f);
  zexpect_within(generated[0].position[2], 0.0f, 1.0e-5f);
}

ZTEST(generated_navigation_fault_injection,
      test_origin_reset_ownership_survives_failed_initialization_actual_v3) {
  run_scenario(NAV_SCENARIO_GPS_ORIGIN_RETRY, 3U);

  zexpect_true(generated[0].reset);
  zexpect_true(generated[0].gps_valid);
  zexpect_false(generated[0].mocap_valid);
  zexpect_false(generated[0].mocap_fresh);
  expect_safe_publication(0U);

  zexpect_true(generated[1].reset);
  zexpect_false(generated[1].gps_valid);
  zexpect_false(generated[1].mocap_valid);
  zexpect_false(generated[1].mocap_fresh);
  zexpect_within(generated[1].position[0], 0.0f, 1.0e-5f);
  zexpect_within(generated[1].position[1], 0.0f, 1.0e-5f);
  zexpect_within(generated[1].position[2], 0.0f, 1.0e-5f);
  expect_one_publication(1U);

  zexpect_false(generated[2].reset);
  zexpect_true(generated[2].mocap_valid);
  zexpect_true(generated[2].mocap_fresh);
  zexpect_true(floats_are_finite(generated[2].position, 3U));
  expect_one_publication(2U);
  zexpect_true(g_process.gps_adapter.origin_valid);
  zexpect_false(g_process.gps_origin_initialization_pending);
}

ZTEST(generated_navigation_fault_injection,
      test_radio_mocap_recovers_from_origin_initialization_timeout_actual_v3) {
  run_scenario(NAV_SCENARIO_GPS_ORIGIN_RADIO_TIMEOUT, 3U);

  zexpect_true(generated[0].reset);
  zexpect_true(generated[0].gps_valid);
  zexpect_false(generated[0].mocap_valid);
  zexpect_true(generated[0].gps_origin_initialization_pending);
  expect_safe_publication(0U);

  zexpect_true(generated[1].reset);
  zexpect_false(generated[1].mocap_valid);
  zexpect_true(generated[1].gps_origin_initialization_pending);
  expect_safe_publication(1U);

  zexpect_true(generated[2].reset);
  zexpect_true(generated[2].mocap_valid);
  zexpect_true(generated[2].mocap_fresh);
  zexpect_false(generated[2].gps_origin_initialization_pending);
  zexpect_within(generated[2].position[0], 10.0f, 1.0e-5f);
  zexpect_within(generated[2].position[1], 20.0f, 1.0e-5f);
  zexpect_within(generated[2].position[2], 30.0f, 1.0e-5f);
  expect_safe_publication(2U);
}

ZTEST(generated_navigation_fault_injection,
      test_lockstep_health_uses_control_time) {
  run_scenario(NAV_SCENARIO_LOCKSTEP_WALL_HEALTH, 1U);

  zexpect_true(generated[0].reset);
  zexpect_true(generated[0].gps_valid);
  zexpect_true(generated[0].gps_position_valid);
  zexpect_equal(generated[0].error_signal_status, 0U);
}

ZTEST(generated_navigation_fault_injection,
      test_nonfinite_mocap_is_rejected_before_generated_estimator) {
  run_scenario(NAV_SCENARIO_NONFINITE_MOCAP, 2U);

  expect_valid_publication(0U);
  zexpect_true(generated[0].mocap_valid);
  zexpect_false(generated[1].mocap_valid);
  zexpect_true(generated[1].mocap_fresh);
  zexpect_true(generated[1].estimate_valid);
  zexpect_true(generated[1].status_initialized);
  zexpect_true(floats_are_finite(generated[1].position, 3U));
  zexpect_true(floats_are_finite(generated[1].quaternion, 4U));
  zexpect_equal(generated[1].error_signal_status, 0U);
  expect_powered_publication(1U, 100);
}

ZTEST(generated_navigation_fault_injection,
      test_large_gps_correction_demotes_position_without_latching_rates) {
  run_scenario(NAV_SCENARIO_LARGE_GPS_CORRECTION_RECOVERY, 9U);

  zexpect_true(generated[0].reset);
  zexpect_equal(generated[1].status_correction_outcome, 3);
  zexpect_equal(generated[1].status_correction_source, 2);
  zexpect_equal(generated[1].status_anchor_source, 2);
  zexpect_equal(generated[1].status_gps_consecutive_rejections, 1);
  zexpect_equal(generated[1].status_recovery_stage, 0);
  zexpect_true(floats_are_finite(generated[1].position, 3U));
  zexpect_within(generated[1].position[1], 0.0f, 0.1f);
  expect_powered_publication(1U, 100);

  zexpect_equal(generated[3].status_recovery_stage, 1);
  zexpect_equal(publications[3].odometry.quality_pct, 50);
  zexpect_true(rdd2_navigation_position_quality_is_usable(
      publications[3].odometry.quality_pct));
  expect_powered_publication(3U, 50);

  zexpect_equal(generated[6].status_recovery_stage, 2);
  expect_powered_publication(6U, 1);
  zexpect_false(rdd2_navigation_position_quality_is_usable(
      publications[6].odometry.quality_pct));
  zexpect_true((publications[6].attitude.flags &
                synapse_topic_AttitudeEstimateFlags_RatesValid) != 0U);
  zexpect_true(generated[6].estimate_valid);
  zexpect_equal(generated[6].error_signal_status, 0U);

  zexpect_true(generated[7].status_gps_position_correction_accepted);
  zexpect_equal(generated[7].status_correction_outcome, 1);
  zexpect_equal(generated[7].status_recovery_stage, 2);
  zexpect_equal(generated[8].status_recovery_stage, 0);
  zexpect_equal(publications[8].odometry.quality_pct, 100);
  zexpect_true(rdd2_navigation_position_quality_is_usable(
      publications[8].odometry.quality_pct));
  expect_powered_publication(8U, 100);
}

ZTEST_SUITE(generated_navigation_fault_injection, NULL, NULL, NULL, NULL, NULL);
