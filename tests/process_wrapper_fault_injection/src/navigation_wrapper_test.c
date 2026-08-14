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

enum navigation_cycle {
  NAV_CYCLE_NONFINITE_IMU_RESET = 0,
  NAV_CYCLE_NONFINITE_IMU_RECOVERY,
  NAV_CYCLE_GENERATED_ERROR,
  NAV_CYCLE_ERROR_RECOVERY,
  NAV_CYCLE_GENERATED_LATE_NAN,
  NAV_CYCLE_NAN_RECOVERY,
  NAV_CYCLE_NOT_INITIALIZED,
  NAV_CYCLE_INITIALIZED_RECOVERY,
  NAV_CYCLE_ESTIMATE_INVALID,
  NAV_CYCLE_ESTIMATE_RECOVERY,
  NAV_CYCLE_NEGATIVE_TIMESTAMP,
  NAV_CYCLE_TIMESTAMP_RECOVERY,
  NAV_CYCLE_INVALID_IMU,
  NAV_CYCLE_IMU_RECOVERY_NO_MOCAP,
  NAV_CYCLE_NONFINITE_MOCAP,
  NAV_CYCLE_LARGE_CORRECTION_REJECTED,
  NAV_CYCLE_RECOVERY_STAGE_1,
  NAV_CYCLE_RECOVERY_STAGE_2,
  NAV_CYCLE_RECOVERY_MISCONFIGURED,
  NAV_CYCLE_RECOVERY_NOMINAL,
  NAV_CYCLE_IMU_HOLD_START,
  NAV_CYCLE_IMU_HOLD_LAST =
      NAV_CYCLE_IMU_HOLD_START + RDD2_NAVIGATION_IMU_HOLD_MAX_RELEASES - 1U,
  NAV_CYCLE_IMU_HOLD_EXCEEDED,
  NAV_CYCLE_IMU_HOLD_RECOVERY,
  NAV_CYCLE_COUNT,
};

struct navigation_generated_observation {
  uint8_t imu_flags;
  bool reset;
  bool imu_valid;
  bool imu_fresh;
  bool mocap_valid;
  bool mocap_fresh;
  bool gps_valid;
  bool gps_fresh;
  bool optical_flow_valid;
  bool optical_flow_fresh;
  float imu_timestamp_s;
  float gyro[3];
  float accel[3];
  float mocap_position[3];
  float mocap_quaternion[4];
  bool output_estimate_valid;
  bool output_status_initialized;
  int32_t output_correction_outcome;
  int32_t output_correction_source;
  int32_t output_recovery_stage;
  bool output_imu_payload_held;
  uint32_t error_signal_status;
};

struct navigation_publication_observation {
  size_t odometry_publish_count;
  size_t attitude_publish_count;
  synapse_topic_OdometryEstimateData_t odometry;
  synapse_topic_AttitudeEstimateData_t attitude;
  uint16_t imu_payload_hold_count;
  bool imu_payload_usable_observed;
};

static jmp_buf navigation_loop_escape;
static size_t navigation_active_cycle;
static size_t navigation_next_cycle;
static size_t navigation_step_count;
static struct navigation_generated_observation
    navigation_generated[NAV_CYCLE_COUNT];
static struct navigation_publication_observation
    navigation_publications[NAV_CYCLE_COUNT];

struct zros_topic navigation_fake_topic_attitude_estimate;
struct zros_topic navigation_fake_topic_control_imu;
struct zros_topic navigation_fake_topic_external_odometry;
struct zros_topic navigation_fake_topic_gnss_fix;
struct zros_topic navigation_fake_topic_navigation_odometry;
struct zros_topic navigation_fake_topic_vehicle_health;

static void navigation_fill_valid_estimate(NavigationEstimatorState *self) {
  self->rumoca_galec_error_signal_status = 0U;
  self->estimate_valid = true;
  self->status_initialized = true;
  self->estimate_timestamp_s = self->imu_timestamp_s;
  self->estimate_positionWorldEnu_m[0] = 1.25f;
  self->estimate_positionWorldEnu_m[1] = -2.5f;
  self->estimate_positionWorldEnu_m[2] = 3.75f;
  self->estimate_velocityWorldEnu_m_s[0] = 0.1f;
  self->estimate_velocityWorldEnu_m_s[1] = -0.2f;
  self->estimate_velocityWorldEnu_m_s[2] = 0.3f;
  self->estimate_quaternionWorldBody[0] = 1.0f;
  self->estimate_quaternionWorldBody[1] = 0.0f;
  self->estimate_quaternionWorldBody[2] = 0.0f;
  self->estimate_quaternionWorldBody[3] = 0.0f;
  self->estimate_angularVelocityBodyFlu_rad_s[0] = 0.01f;
  self->estimate_angularVelocityBodyFlu_rad_s[1] = -0.02f;
  self->estimate_angularVelocityBodyFlu_rad_s[2] = 0.03f;
  self->status_consecutiveRejectedCorrections = 0;
  self->status_rejectionElapsed_s = 0.0f;
  self->mocapConsecutiveRejections = 0;
  self->gpsConsecutiveRejections = 0;
  self->opticalFlowConsecutiveRejections = 0;
  self->status_correctionOutcome = 0;
  self->status_correctionSource = 0;
  self->status_recoveryStage = 0;
  self->status_imuPayloadHeld = false;
  self->status_anchorSource = 0;
}

void NavigationEstimator_startup(NavigationEstimatorState *self) {
  memset(self, 0, sizeof(*self));
}

void NavigationEstimator_recalibrate(NavigationEstimatorState *self) {
  ARG_UNUSED(self);
}

void NavigationEstimator_dostep(NavigationEstimatorState *self) {
  struct navigation_generated_observation *observation =
      &navigation_generated[navigation_active_cycle];

  observation->reset = self->reset;
  observation->imu_valid = self->imu_valid;
  observation->imu_fresh = self->imu_fresh;
  observation->mocap_valid = self->mocap_valid;
  observation->mocap_fresh = self->mocap_fresh;
  observation->gps_valid = self->gps_valid;
  observation->gps_fresh = self->gps_fresh;
  observation->optical_flow_valid = self->opticalFlow_valid;
  observation->optical_flow_fresh = self->opticalFlow_fresh;
  observation->imu_timestamp_s = self->imu_timestamp_s;
  memcpy(observation->gyro, self->imu_angularVelocityBodyFlu_rad_s,
         sizeof(observation->gyro));
  memcpy(observation->accel, self->specificForceBodyFlu_m_s2,
         sizeof(observation->accel));
  memcpy(observation->mocap_position, self->mocap_positionWorldEnu_m,
         sizeof(observation->mocap_position));
  memcpy(observation->mocap_quaternion, self->mocap_quaternionWorldBody,
         sizeof(observation->mocap_quaternion));

  navigation_fill_valid_estimate(self);
  if (navigation_active_cycle == NAV_CYCLE_NONFINITE_IMU_RESET ||
      navigation_active_cycle == NAV_CYCLE_INVALID_IMU ||
      (navigation_active_cycle >= NAV_CYCLE_IMU_HOLD_START &&
       navigation_active_cycle <= NAV_CYCLE_IMU_HOLD_EXCEEDED)) {
    self->status_imuPayloadHeld = true;
  }
  switch (navigation_active_cycle) {
  case NAV_CYCLE_GENERATED_ERROR:
    self->rumoca_galec_error_signal_status = UINT32_C(0x80);
    break;
  case NAV_CYCLE_GENERATED_LATE_NAN:
    self->estimate_velocityWorldEnu_m_s[2] = NAN;
    break;
  case NAV_CYCLE_NOT_INITIALIZED:
    self->status_initialized = false;
    break;
  case NAV_CYCLE_ESTIMATE_INVALID:
    self->estimate_valid = false;
    break;
  case NAV_CYCLE_NEGATIVE_TIMESTAMP:
    self->estimate_timestamp_s = -0.001f;
    break;
  case NAV_CYCLE_LARGE_CORRECTION_REJECTED:
    self->status_consecutiveRejectedCorrections = 1;
    self->status_rejectionElapsed_s = 0.001f;
    self->gpsConsecutiveRejections = 1;
    self->status_correctionOutcome = 3; /* CorrectionRejectedGate */
    self->status_correctionSource = 2;  /* SourceGps */
    self->status_anchorSource = 2;
    break;
  case NAV_CYCLE_RECOVERY_STAGE_1:
    self->status_recoveryStage = 1; /* RecoveryCovarianceInflated */
    self->status_anchorSource = 2;
    break;
  case NAV_CYCLE_RECOVERY_STAGE_2:
    self->status_recoveryStage = 2; /* RecoveryAidingDivergent */
    self->status_anchorSource = 2;
    break;
  case NAV_CYCLE_RECOVERY_MISCONFIGURED:
    self->status_recoveryStage = 3; /* RecoveryMisconfigured */
    break;
  default:
    break;
  }
  observation->output_estimate_valid = self->estimate_valid;
  observation->output_status_initialized = self->status_initialized;
  observation->output_correction_outcome = self->status_correctionOutcome;
  observation->output_correction_source = self->status_correctionSource;
  observation->output_recovery_stage = self->status_recoveryStage;
  observation->output_imu_payload_held = self->status_imuPayloadHeld;
  observation->error_signal_status = self->rumoca_galec_error_signal_status;
  navigation_step_count++;
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
  if (navigation_next_cycle == NAV_CYCLE_COUNT) {
    longjmp(navigation_loop_escape, 1);
  }
  navigation_active_cycle = navigation_next_cycle++;
  return 0;
}

int navigation_fake_zros_sub_update(struct zros_sub *sub) {
  if (sub == &g_process.imu_sub) {
    g_process.imu = (synapse_topic_InertialSampleData_t){
        .timestamp_ns =
            TEST_NS_FROM_US(UINT64_C(200000) + navigation_active_cycle * 5000U),
        .accel_flu_m_s2 = {.x = 0.25f, .y = -0.5f, .z = 9.75f},
        .gyro_flu_rad_s = {.x = 0.01f, .y = -0.02f, .z = 0.03f},
        .flags = synapse_topic_InertialFieldFlags_Accel |
                 synapse_topic_InertialFieldFlags_Gyro,
    };
    if (navigation_active_cycle == NAV_CYCLE_INVALID_IMU ||
        (navigation_active_cycle >= NAV_CYCLE_IMU_HOLD_START &&
         navigation_active_cycle <= NAV_CYCLE_IMU_HOLD_EXCEEDED)) {
      g_process.imu.flags = synapse_topic_InertialFieldFlags_Accel;
    } else if (navigation_active_cycle == NAV_CYCLE_NONFINITE_IMU_RESET) {
      g_process.imu.accel_flu_m_s2.z = NAN;
    }
    navigation_generated[navigation_active_cycle].imu_flags =
        g_process.imu.flags;
    return 0;
  }
  if (sub == &g_process.external_odometry_sub) {
    g_process.external_odometry = (synapse_topic_ExternalOdometryData_t){
        .timestamp_ns =
            TEST_NS_FROM_US(UINT64_C(199000) + navigation_active_cycle * 5000U),
        .position_enu_m = {.x = 10.0f, .y = 20.0f, .z = 30.0f},
        .attitude = {.w = 1.0f, .x = 0.1f, .y = 0.2f, .z = 0.3f},
        .flags = synapse_topic_ExternalOdometryFlags_PositionValid |
                 synapse_topic_ExternalOdometryFlags_AttitudeValid,
    };
    if (navigation_active_cycle == NAV_CYCLE_NONFINITE_MOCAP) {
      g_process.external_odometry.position_enu_m.x = NAN;
    }
    return navigation_active_cycle == NAV_CYCLE_IMU_RECOVERY_NO_MOCAP ? -1 : 0;
  }
  if (sub == &g_process.gnss_sub) {
    return 0;
  }
  return -1;
}

int navigation_fake_zros_pub_update(struct zros_pub *pub) {
  struct navigation_publication_observation *observation =
      &navigation_publications[navigation_active_cycle];

  if (pub == &g_process.odometry_pub) {
    observation->odometry_publish_count++;
    observation->odometry = g_process.odometry;
    observation->imu_payload_hold_count = g_process.imu_payload_hold_count;
    observation->imu_payload_usable_observed =
        g_process.imu_payload_usable_observed;
    return 0;
  }
  if (pub == &g_process.attitude_pub) {
    observation->attitude_publish_count++;
    observation->attitude = g_process.attitude;
    return 0;
  }
  return -1;
}

static void navigation_expect_safe_publication(size_t cycle) {
  const struct navigation_publication_observation *observation =
      &navigation_publications[cycle];

  const uint64_t timestamp_ns =
      TEST_NS_FROM_US(UINT64_C(200000) + cycle * 5000U);
  const synapse_types_CovarianceUpperTriangle21f_t zero_covariance = {0};

  zexpect_equal(observation->odometry_publish_count, 1U,
                "cycle %zu odometry publish count mismatch", cycle);
  zexpect_equal(observation->attitude_publish_count, 1U,
                "cycle %zu attitude publish count mismatch", cycle);
  zexpect_equal(observation->odometry.timestamp_ns, timestamp_ns);
  zexpect_equal(observation->attitude.timestamp_ns, timestamp_ns);
  zexpect_equal(observation->attitude.flags, 0U,
                "cycle %zu advertised invalid attitude", cycle);
  zexpect_equal(observation->attitude.attitude.w, 1.0f);
  zexpect_equal(observation->attitude.attitude.x, 0.0f);
  zexpect_equal(observation->attitude.attitude.y, 0.0f);
  zexpect_equal(observation->attitude.attitude.z, 0.0f);
  zexpect_equal(observation->attitude.angular_velocity_flu_rad_s.roll, 0.0f);
  zexpect_equal(observation->attitude.angular_velocity_flu_rad_s.pitch, 0.0f);
  zexpect_equal(observation->attitude.angular_velocity_flu_rad_s.yaw, 0.0f);
  zexpect_equal(observation->odometry.quality_pct, 0);
  zexpect_equal(observation->odometry.position_enu_m.x, 0.0f);
  zexpect_equal(observation->odometry.position_enu_m.y, 0.0f);
  zexpect_equal(observation->odometry.position_enu_m.z, 0.0f);
  zexpect_equal(observation->odometry.attitude.w, 1.0f);
  zexpect_equal(observation->odometry.attitude.x, 0.0f);
  zexpect_equal(observation->odometry.attitude.y, 0.0f);
  zexpect_equal(observation->odometry.attitude.z, 0.0f);
  zexpect_equal(observation->odometry.velocity_enu_m_s.x, 0.0f);
  zexpect_equal(observation->odometry.velocity_enu_m_s.y, 0.0f);
  zexpect_equal(observation->odometry.velocity_enu_m_s.z, 0.0f);
  zexpect_equal(observation->odometry.angular_velocity_flu_rad_s.roll, 0.0f);
  zexpect_equal(observation->odometry.angular_velocity_flu_rad_s.pitch, 0.0f);
  zexpect_equal(observation->odometry.angular_velocity_flu_rad_s.yaw, 0.0f);
  zexpect_mem_equal(&observation->odometry.pose_covariance, &zero_covariance,
                    sizeof(zero_covariance));
  zexpect_mem_equal(&observation->odometry.velocity_covariance,
                    &zero_covariance, sizeof(zero_covariance));
}

static void navigation_expect_valid_publication_with_quality(size_t cycle,
                                                             int8_t quality) {
  const struct navigation_publication_observation *observation =
      &navigation_publications[cycle];
  const uint8_t required = synapse_topic_AttitudeEstimateFlags_AttitudeValid |
                           synapse_topic_AttitudeEstimateFlags_RatesValid;

  const uint64_t timestamp_ns =
      TEST_NS_FROM_US(UINT64_C(200000) + cycle * 5000U);

  zexpect_equal(observation->odometry_publish_count, 1U,
                "cycle %zu odometry publish count mismatch", cycle);
  zexpect_equal(observation->attitude_publish_count, 1U,
                "cycle %zu attitude publish count mismatch", cycle);
  zexpect_equal(observation->odometry.timestamp_ns, timestamp_ns);
  zexpect_equal(observation->attitude.timestamp_ns, timestamp_ns);
  zexpect_equal(observation->attitude.flags, required);
  zexpect_equal(observation->attitude.attitude.w, 1.0f);
  zexpect_equal(observation->attitude.angular_velocity_flu_rad_s.yaw, 0.03f);
  zexpect_equal(observation->odometry.position_enu_m.y, -2.5f);
  zexpect_equal(observation->odometry.velocity_enu_m_s.z, 0.3f);
  zexpect_equal(observation->odometry.quality_pct, quality);
}

static void navigation_expect_valid_publication(size_t cycle) {
  navigation_expect_valid_publication_with_quality(cycle, 100);
}

ZTEST(process_wrapper_fault_injection,
      test_navigation_wrapper_sanitizes_generated_failures) {
  memset(&g_process, 0, sizeof(g_process));
  memset(navigation_generated, 0, sizeof(navigation_generated));
  memset(navigation_publications, 0, sizeof(navigation_publications));
  navigation_active_cycle = 0U;
  navigation_next_cycle = 0U;
  navigation_step_count = 0U;

  if (setjmp(navigation_loop_escape) == 0) {
    navigation_estimator_thread(&g_process, NULL, NULL);
    zassert_unreachable(
        "navigation wrapper returned before the script completed");
  }

  zexpect_equal(navigation_step_count, NAV_CYCLE_COUNT);
  navigation_expect_safe_publication(NAV_CYCLE_NONFINITE_IMU_RESET);
  navigation_expect_valid_publication(NAV_CYCLE_NONFINITE_IMU_RECOVERY);
  navigation_expect_safe_publication(NAV_CYCLE_GENERATED_ERROR);
  navigation_expect_valid_publication(NAV_CYCLE_ERROR_RECOVERY);
  navigation_expect_safe_publication(NAV_CYCLE_GENERATED_LATE_NAN);
  navigation_expect_valid_publication(NAV_CYCLE_NAN_RECOVERY);
  navigation_expect_safe_publication(NAV_CYCLE_NOT_INITIALIZED);
  navigation_expect_valid_publication(NAV_CYCLE_INITIALIZED_RECOVERY);
  navigation_expect_safe_publication(NAV_CYCLE_ESTIMATE_INVALID);
  navigation_expect_valid_publication(NAV_CYCLE_ESTIMATE_RECOVERY);
  navigation_expect_safe_publication(NAV_CYCLE_NEGATIVE_TIMESTAMP);
  navigation_expect_valid_publication(NAV_CYCLE_TIMESTAMP_RECOVERY);
  navigation_expect_valid_publication(NAV_CYCLE_INVALID_IMU);
  navigation_expect_valid_publication(NAV_CYCLE_IMU_RECOVERY_NO_MOCAP);
  navigation_expect_valid_publication(NAV_CYCLE_NONFINITE_MOCAP);
  navigation_expect_valid_publication(NAV_CYCLE_LARGE_CORRECTION_REJECTED);
  navigation_expect_valid_publication_with_quality(NAV_CYCLE_RECOVERY_STAGE_1,
                                                   50);
  navigation_expect_valid_publication_with_quality(NAV_CYCLE_RECOVERY_STAGE_2,
                                                   1);
  navigation_expect_valid_publication_with_quality(
      NAV_CYCLE_RECOVERY_MISCONFIGURED, 1);
  navigation_expect_valid_publication(NAV_CYCLE_RECOVERY_NOMINAL);
  for (size_t cycle = NAV_CYCLE_IMU_HOLD_START;
       cycle <= NAV_CYCLE_IMU_HOLD_LAST; ++cycle) {
    navigation_expect_valid_publication(cycle);
  }
  navigation_expect_safe_publication(NAV_CYCLE_IMU_HOLD_EXCEEDED);
  navigation_expect_valid_publication(NAV_CYCLE_IMU_HOLD_RECOVERY);

  zexpect_true(navigation_generated[NAV_CYCLE_NONFINITE_IMU_RESET].reset);
  zexpect_false(navigation_generated[NAV_CYCLE_NONFINITE_IMU_RESET].imu_valid);
  zexpect_true(navigation_generated[NAV_CYCLE_NONFINITE_IMU_RESET].imu_fresh);
  zexpect_equal(navigation_generated[NAV_CYCLE_NONFINITE_IMU_RESET].imu_flags,
                synapse_topic_InertialFieldFlags_Accel |
                    synapse_topic_InertialFieldFlags_Gyro);
  zexpect_true(
      isnan(navigation_generated[NAV_CYCLE_NONFINITE_IMU_RESET].accel[2]));
  zexpect_true(navigation_generated[NAV_CYCLE_NONFINITE_IMU_RESET]
                   .output_estimate_valid);
  zexpect_true(navigation_generated[NAV_CYCLE_NONFINITE_IMU_RESET]
                   .output_status_initialized);
  zexpect_false(navigation_generated[NAV_CYCLE_NONFINITE_IMU_RECOVERY].reset);
  zexpect_true(
      navigation_generated[NAV_CYCLE_NONFINITE_IMU_RECOVERY].imu_valid);
  zexpect_true(
      navigation_generated[NAV_CYCLE_NONFINITE_IMU_RECOVERY].imu_fresh);
  zexpect_equal(navigation_publications[NAV_CYCLE_NONFINITE_IMU_RESET]
                    .attitude.timestamp_ns,
                TEST_NS_FROM_US(UINT64_C(200000)));
  zexpect_equal(navigation_publications[NAV_CYCLE_NONFINITE_IMU_RESET]
                    .odometry.timestamp_ns,
                TEST_NS_FROM_US(UINT64_C(200000)));
  zexpect_equal(navigation_publications[NAV_CYCLE_NONFINITE_IMU_RECOVERY]
                    .attitude.timestamp_ns,
                TEST_NS_FROM_US(UINT64_C(205000)));
  zexpect_equal(navigation_publications[NAV_CYCLE_NONFINITE_IMU_RECOVERY]
                    .odometry.timestamp_ns,
                TEST_NS_FROM_US(UINT64_C(205000)));
  zexpect_false(navigation_generated[NAV_CYCLE_GENERATED_ERROR].reset);
  zexpect_true(navigation_generated[NAV_CYCLE_ERROR_RECOVERY].reset);
  zexpect_true(navigation_generated[NAV_CYCLE_NAN_RECOVERY].reset);
  zexpect_true(navigation_generated[NAV_CYCLE_INITIALIZED_RECOVERY].reset);
  zexpect_true(navigation_generated[NAV_CYCLE_ESTIMATE_RECOVERY].reset);
  zexpect_true(navigation_generated[NAV_CYCLE_TIMESTAMP_RECOVERY].reset);
  zexpect_false(navigation_generated[NAV_CYCLE_INVALID_IMU].imu_valid);
  zexpect_true(navigation_generated[NAV_CYCLE_INVALID_IMU].imu_fresh);
  zexpect_true(
      navigation_generated[NAV_CYCLE_INVALID_IMU].output_imu_payload_held);
  zexpect_equal(
      navigation_publications[NAV_CYCLE_INVALID_IMU].imu_payload_hold_count,
      1U);
  zexpect_true(navigation_generated[NAV_CYCLE_IMU_RECOVERY_NO_MOCAP].imu_valid);
  zexpect_false(navigation_generated[NAV_CYCLE_IMU_RECOVERY_NO_MOCAP].reset);
  zexpect_false(
      navigation_generated[NAV_CYCLE_IMU_RECOVERY_NO_MOCAP].mocap_fresh);
  zexpect_false(navigation_generated[NAV_CYCLE_NONFINITE_IMU_RESET].gps_valid);
  zexpect_false(navigation_generated[NAV_CYCLE_NONFINITE_IMU_RESET].gps_fresh);
  zexpect_false(
      navigation_generated[NAV_CYCLE_NONFINITE_IMU_RESET].optical_flow_valid);
  zexpect_false(
      navigation_generated[NAV_CYCLE_NONFINITE_IMU_RESET].optical_flow_fresh);
  zexpect_within(
      navigation_generated[NAV_CYCLE_NONFINITE_IMU_RESET].imu_timestamp_s, 0.2f,
      1.0e-6f);
  zexpect_equal(navigation_generated[NAV_CYCLE_NONFINITE_IMU_RESET].gyro[1],
                -0.02f);
  zexpect_equal(navigation_generated[NAV_CYCLE_NONFINITE_IMU_RECOVERY].accel[2],
                9.75f);
  zexpect_equal(
      navigation_generated[NAV_CYCLE_NONFINITE_IMU_RESET].mocap_position[2],
      30.0f);
  zexpect_equal(
      navigation_generated[NAV_CYCLE_NONFINITE_IMU_RESET].mocap_quaternion[3],
      0.3f);
  zexpect_equal(
      navigation_generated[NAV_CYCLE_GENERATED_ERROR].error_signal_status,
      UINT32_C(0x80));
  zexpect_true(
      isnan(navigation_generated[NAV_CYCLE_NONFINITE_MOCAP].mocap_position[0]));
  zexpect_false(navigation_generated[NAV_CYCLE_NONFINITE_MOCAP].mocap_valid);
  zexpect_equal(navigation_generated[NAV_CYCLE_LARGE_CORRECTION_REJECTED]
                    .output_correction_outcome,
                3);
  zexpect_equal(navigation_generated[NAV_CYCLE_LARGE_CORRECTION_REJECTED]
                    .output_correction_source,
                2);
  zexpect_equal(
      navigation_generated[NAV_CYCLE_RECOVERY_STAGE_1].output_recovery_stage,
      1);
  zexpect_equal(
      navigation_generated[NAV_CYCLE_RECOVERY_STAGE_2].output_recovery_stage,
      2);
  zexpect_false(rdd2_navigation_position_quality_is_usable(
      navigation_publications[NAV_CYCLE_RECOVERY_STAGE_2]
          .odometry.quality_pct));
  zexpect_false(rdd2_navigation_position_quality_is_usable(
      navigation_publications[NAV_CYCLE_RECOVERY_MISCONFIGURED]
          .odometry.quality_pct));
  zexpect_true(rdd2_navigation_position_quality_is_usable(
      navigation_publications[NAV_CYCLE_RECOVERY_STAGE_1]
          .odometry.quality_pct));
  zexpect_equal(
      navigation_publications[NAV_CYCLE_IMU_HOLD_LAST].imu_payload_hold_count,
      RDD2_NAVIGATION_IMU_HOLD_MAX_RELEASES);
  zexpect_true(navigation_publications[NAV_CYCLE_IMU_HOLD_LAST]
                   .imu_payload_usable_observed);
  zexpect_equal(navigation_publications[NAV_CYCLE_IMU_HOLD_EXCEEDED]
                    .imu_payload_hold_count,
                RDD2_NAVIGATION_IMU_HOLD_MAX_RELEASES + 1U);
  zexpect_true(navigation_generated[NAV_CYCLE_IMU_HOLD_EXCEEDED]
                   .output_imu_payload_held);
  zexpect_equal(navigation_publications[NAV_CYCLE_IMU_HOLD_RECOVERY]
                    .imu_payload_hold_count,
                0U);
  zexpect_true(navigation_publications[NAV_CYCLE_IMU_HOLD_RECOVERY]
                   .imu_payload_usable_observed);
  zexpect_equal(navigation_publications[NAV_CYCLE_RECOVERY_NOMINAL]
                    .odometry.reset_counter,
                0U);
}
