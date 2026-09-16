/* SPDX-License-Identifier: Apache-2.0 */

#include "processes.h"

#include "control_safety.h"
#include "imu_preintegration.h"
#include "interfaces/zros_topics.h"
#include "navigation_gps.h"
#include "navigation_optical_flow.h"
#include "navigation_optical_flow_raw.h"
#include "scheduling.h"

#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zros/private/zros_node_struct.h>
#include <zros/private/zros_pub_struct.h>
#include <zros/private/zros_sub_struct.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>
#include <zros/zros_sub.h>

#include "Vehicles_Rdd2_NavigationEstimator.h"

LOG_MODULE_DECLARE(rdd2, LOG_LEVEL_INF);

/* The hold deadline is wall clock; the count above is what that deadline
 * means at the rate this process is released at. Restating it as an
 * assertion here, where both headers are in scope, keeps processes.h free of
 * a scheduling dependency while still failing the build if the estimator
 * rate moves without the count moving with it. */
_Static_assert(RDD2_NAVIGATION_IMU_HOLD_MAX_RELEASES ==
                   (uint16_t)((RDD2_NAVIGATION_IMU_HOLD_DEADLINE_MS *
                               RDD2_NAVIGATION_ESTIMATOR_RATE_HZ) /
                              1000U),
               "the IMU hold release count must equal its wall-clock deadline "
               "at the estimator release rate");
_Static_assert(RDD2_NAVIGATION_IMU_HOLD_MAX_RELEASES >= 2U,
               "the IMU hold deadline must span at least two releases");

#define NAVIGATION_STACK_SIZE 32768
#define GPS_ORIGIN_INITIALIZATION_TIMEOUT_NS UINT64_C(100000000)

#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE)
/* Rate-noise density of the residual left after the producer removes camera
 * rotation, in rad/s/sqrt(Hz). The integrated-angle variance grows with the
 * exposure window as density^2 * integration_time_s (angle random walk). */
#define RDD2_OPTICAL_FLOW_COMPENSATION_RATE_NOISE_RAD_S_RTHZ                   \
  ((float)CONFIG_RDD2_OPTICAL_FLOW_COMPENSATION_RATE_URAD_S_RTHZ * 1.0e-6f)
/* The velocity standard deviation is interpolated between the best-case value
 * at quality 255 and the worst-case value at minimum quality, so the worst
 * case must not be tighter than the best case or the interpolation inverts. */
BUILD_ASSERT(CONFIG_RDD2_OPTICAL_FLOW_WORST_STDDEV_MM_S >=
                 CONFIG_RDD2_OPTICAL_FLOW_BEST_STDDEV_MM_S,
             "optical-flow worst-case velocity stddev must be at least the "
             "best-case velocity stddev");
#endif

struct navigation_estimator_process {
  NavigationEstimatorState efmu;
  synapse_topic_InertialSampleData_t imu;
  synapse_topic_ExternalOdometryData_t external_odometry;
  synapse_topic_GnssFixData_t gnss;
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE)
  synapse_topic_OpticalFlowVelocityData_t optical_flow;
#endif
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE_RAW)
  synapse_topic_OpticalFlowData_t optical_flow_raw;
#endif
  synapse_topic_VehicleHealthData_t health;
  synapse_topic_OdometryEstimateData_t odometry;
  synapse_topic_AttitudeEstimateData_t attitude;
  struct rdd2_imu_preintegrator preintegrator;
  struct rdd2_imu_packet imu_packet;
  struct rdd2_navigation_gps_adapter gps_adapter;
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE)
  struct rdd2_navigation_optical_flow_adapter optical_flow_adapter;
  uint64_t optical_flow_last_fusion_control_timestamp_ns;
  uint32_t optical_flow_fusion_accepted_count;
#endif
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE_RAW)
  struct rdd2_navigation_optical_flow_raw_adapter optical_flow_raw_adapter;
  uint64_t optical_flow_last_fusion_control_timestamp_ns;
  uint32_t optical_flow_fusion_accepted_count;
#endif
  struct zros_node node;
  struct zros_sub imu_sub;
  struct zros_sub external_odometry_sub;
  struct zros_sub gnss_sub;
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE)
  struct zros_sub optical_flow_sub;
#endif
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE_RAW)
  struct zros_sub optical_flow_raw_sub;
#endif
  struct zros_sub health_sub;
  struct zros_pub odometry_pub;
  struct zros_pub attitude_pub;
  struct rdd2_release_scheduler release_scheduler;
  bool initialized;
  bool gps_origin_initialization_pending;
  uint64_t gps_origin_initialization_started_ns;
  uint32_t imu_sample_rejected_count;
  uint16_t imu_payload_hold_count;
  uint8_t reset_counter;
  bool imu_payload_usable_observed;
  uint64_t filter_epoch_ns;
  bool filter_epoch_valid;
};

static struct navigation_estimator_process g_process;

static atomic_t g_origin_valid;
/* Gyroscope bias handed to the rate loop, which subtracts it from the raw
 * IMU sample every control tick instead of consuming the estimator's own
 * angular-velocity publication at the estimator rate. */
static struct k_spinlock g_gyroscope_bias_lock;
static float g_gyroscope_bias_rad_s[3];
static atomic_t g_gyroscope_bias_valid;
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE)
static struct k_spinlock g_optical_flow_diagnostics_lock;
static struct rdd2_navigation_optical_flow_diagnostics
    g_optical_flow_diagnostics;
#endif
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE_RAW)
static struct k_spinlock g_optical_flow_raw_diagnostics_lock;
static struct rdd2_navigation_optical_flow_raw_diagnostics
    g_optical_flow_raw_diagnostics;
#endif
static struct k_thread g_thread;
K_THREAD_STACK_DEFINE(g_navigation_stack, NAVIGATION_STACK_SIZE);

#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE)
static const struct rdd2_navigation_optical_flow_config g_optical_flow_config =
    {
        .max_age_ns =
            (uint64_t)CONFIG_RDD2_OPTICAL_FLOW_MAX_AGE_MS * UINT64_C(1000000),
        .min_distance_m =
            (float)CONFIG_RDD2_OPTICAL_FLOW_MIN_DISTANCE_MM * 1.0e-3f,
        .max_distance_m =
            (float)CONFIG_RDD2_OPTICAL_FLOW_MAX_DISTANCE_MM * 1.0e-3f,
        .max_tilt_rad = (float)CONFIG_RDD2_OPTICAL_FLOW_MAX_TILT_MRAD * 1.0e-3f,
        .max_speed_m_s =
            (float)CONFIG_RDD2_OPTICAL_FLOW_MAX_SPEED_MM_S * 1.0e-3f,
        .best_stddev_m_s =
            (float)CONFIG_RDD2_OPTICAL_FLOW_BEST_STDDEV_MM_S * 1.0e-3f,
        .worst_stddev_m_s =
            (float)CONFIG_RDD2_OPTICAL_FLOW_WORST_STDDEV_MM_S * 1.0e-3f,
        .range_variance_m2 =
            ((float)CONFIG_RDD2_OPTICAL_FLOW_RANGE_STDDEV_MM * 1.0e-3f) *
            ((float)CONFIG_RDD2_OPTICAL_FLOW_RANGE_STDDEV_MM * 1.0e-3f),
        .nominal_integration_time_s =
            (float)CONFIG_RDD2_OPTICAL_FLOW_NOMINAL_PERIOD_MS * 1.0e-3f,
        .min_integration_time_s =
            (float)CONFIG_RDD2_OPTICAL_FLOW_MIN_INTEGRATION_MS * 1.0e-3f,
        .max_integration_time_s =
            (float)CONFIG_RDD2_OPTICAL_FLOW_MAX_INTEGRATION_MS * 1.0e-3f,
        .sensor_id = CONFIG_RDD2_OPTICAL_FLOW_SENSOR_ID,
        .min_quality = CONFIG_RDD2_OPTICAL_FLOW_MIN_QUALITY,
        .require_gptp = true,
};
#endif

#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE_RAW)
static const struct rdd2_navigation_optical_flow_raw_config
    g_optical_flow_raw_config = {
        .max_age_ns = (uint64_t)CONFIG_RDD2_OPTICAL_FLOW_RAW_MAX_AGE_MS *
                      UINT64_C(1000000),
        .min_distance_m =
            (float)CONFIG_RDD2_OPTICAL_FLOW_RAW_MIN_DISTANCE_MM * 1.0e-3f,
        .max_distance_m =
            (float)CONFIG_RDD2_OPTICAL_FLOW_RAW_MAX_DISTANCE_MM * 1.0e-3f,
        .min_integration_time_s =
            (float)CONFIG_RDD2_OPTICAL_FLOW_RAW_MIN_INTEGRATION_MS * 1.0e-3f,
        .max_integration_time_s =
            (float)CONFIG_RDD2_OPTICAL_FLOW_RAW_MAX_INTEGRATION_MS * 1.0e-3f,
        .los_floor_rad =
            (float)CONFIG_RDD2_OPTICAL_FLOW_RAW_LOS_FLOOR_URAD * 1.0e-6f,
        .los_sens_best_frac =
            (float)CONFIG_RDD2_OPTICAL_FLOW_RAW_LOS_SENS_BEST_PERMILLE * 1.0e-3f,
        .los_sens_worst_frac =
            (float)CONFIG_RDD2_OPTICAL_FLOW_RAW_LOS_SENS_WORST_PERMILLE *
            1.0e-3f,
        .gyro_rate_noise_rad_s_rthz =
            (float)CONFIG_RDD2_OPTICAL_FLOW_RAW_GYRO_RATE_NOISE_URAD_S_RTHZ *
            1.0e-6f,
        .range_best_stddev_m =
            (float)CONFIG_RDD2_OPTICAL_FLOW_RAW_RANGE_BEST_STDDEV_MM * 1.0e-3f,
        .range_worst_stddev_m =
            (float)CONFIG_RDD2_OPTICAL_FLOW_RAW_RANGE_WORST_STDDEV_MM * 1.0e-3f,
        .mount_yaw_deg = CONFIG_RDD2_OPTICAL_FLOW_RAW_MOUNT_YAW_DEG,
        .sensor_id = CONFIG_RDD2_OPTICAL_FLOW_RAW_SENSOR_ID,
        .min_quality = CONFIG_RDD2_OPTICAL_FLOW_RAW_MIN_QUALITY,
        .require_gptp = true,
};
#endif

static bool imu_valid(const synapse_topic_InertialSampleData_t *imu) {
  const uint8_t required = synapse_topic_InertialFieldFlags_Accel |
                           synapse_topic_InertialFieldFlags_Gyro;
  const float values[] = {
      imu->accel_flu_m_s2.x, imu->accel_flu_m_s2.y, imu->accel_flu_m_s2.z,
      imu->gyro_flu_rad_s.x, imu->gyro_flu_rad_s.y, imu->gyro_flu_rad_s.z,
  };

  return (imu->flags & required) == required &&
         rdd2_control_values_are_finite(values, ARRAY_SIZE(values));
}

static bool
external_odometry_valid(const synapse_topic_ExternalOdometryData_t *odometry) {
  const uint8_t required = synapse_topic_ExternalOdometryFlags_PositionValid |
                           synapse_topic_ExternalOdometryFlags_AttitudeValid;
  const float values[] = {
      odometry->position_enu_m.x, odometry->position_enu_m.y,
      odometry->position_enu_m.z, odometry->attitude.w,
      odometry->attitude.x,       odometry->attitude.y,
      odometry->attitude.z,
  };

  return (odometry->flags & required) == required &&
         (odometry->flags & synapse_topic_ExternalOdometryFlags_Lost) == 0U &&
         rdd2_control_values_are_finite(values, ARRAY_SIZE(values));
}

static bool external_odometry_source_allowed(bool gps_origin_source) {
  return !gps_origin_source;
}

static bool gps_origin_initialization_timed_out(bool gps_origin_source,
                                                uint64_t started_ns,
                                                uint64_t now_ns) {
  return !gps_origin_source && now_ns >= started_ns &&
         now_ns - started_ns > GPS_ORIGIN_INITIALIZATION_TIMEOUT_NS;
}

/*
 * Compose the sample into the running preintegral. This runs on EVERY IMU
 * publication, not only on the releases: the samples between releases used to
 * be discarded, which is what made a lower estimator rate lossy. With the
 * window accumulated, closing it at 100 Hz throws nothing away.
 *
 * A sample that fails the payload contract advances nothing, deliberately:
 * it must not become the interval start of the next good sample, and the
 * measured interval that then spans the dropout is what the estimator sees.
 */
static void accumulate_imu_sample(struct navigation_estimator_process *process) {
  const synapse_topic_InertialSampleData_t *imu = &process->imu;
  float gyroscope_rad_s[3];
  float accelerometer_m_s2[3];

  if (!imu_valid(imu) || imu->timestamp_ns == 0U) {
    if (process->imu_sample_rejected_count < UINT32_MAX) {
      process->imu_sample_rejected_count++;
    }
    return;
  }
  gyroscope_rad_s[0] = imu->gyro_flu_rad_s.x;
  gyroscope_rad_s[1] = imu->gyro_flu_rad_s.y;
  gyroscope_rad_s[2] = imu->gyro_flu_rad_s.z;
  accelerometer_m_s2[0] = imu->accel_flu_m_s2.x;
  accelerometer_m_s2[1] = imu->accel_flu_m_s2.y;
  accelerometer_m_s2[2] = imu->accel_flu_m_s2.z;
  (void)rdd2_imu_preintegrator_accumulate(&process->preintegrator,
                                          gyroscope_rad_s, accelerometer_m_s2,
                                          imu->timestamp_ns);
}

/*
 * Close the window and re-anchor the next one on the bias the estimator
 * published on its PREVIOUS step. Reading the eFMU state before dostep is
 * exactly the pre() the model states at this boundary in
 * Vehicles.Rdd2.WaypointVehicleSystem, so the linearization anchor of the
 * packet being handed over is the anchor it was actually formed with.
 */
static void close_imu_packet(struct navigation_estimator_process *process) {
  float gyroscope_bias_rad_s[3];
  float accelerometer_bias_m_s2[3];
  bool bias_usable = process->efmu.status_initialized;

  for (size_t axis = 0U; axis < 3U; ++axis) {
    gyroscope_bias_rad_s[axis] =
        process->efmu.gyroscopeBiasBodyFlu_rad_s[axis];
    accelerometer_bias_m_s2[axis] =
        process->efmu.accelerometerBiasBodyFlu_m_s2[axis];
  }
  bias_usable =
      bias_usable &&
      rdd2_control_values_are_finite(gyroscope_bias_rad_s, 3U) &&
      rdd2_control_values_are_finite(accelerometer_bias_m_s2, 3U);
  if (!bias_usable) {
    for (size_t axis = 0U; axis < 3U; ++axis) {
      gyroscope_bias_rad_s[axis] =
          process->efmu.initialGyroscopeBiasBodyFlu_rad_s[axis];
      accelerometer_bias_m_s2[axis] =
          process->efmu.initialAccelerometerBiasBodyFlu_m_s2[axis];
    }
  }
  rdd2_imu_preintegrator_close(&process->preintegrator, &process->imu_packet,
                               process->imu.timestamp_ns, gyroscope_bias_rad_s,
                               accelerometer_bias_m_s2);
}

/*
 * Filter-relative sample time in seconds. Once this node is gPTP-disciplined a
 * wire timestamp_ns is a full TAI epoch (about 1.76e18 ns in 2026), a value a
 * single-precision float cannot resolve to better than roughly two minutes. The
 * estimator needs only the spacing between samples, so every stream is expressed
 * against one reference epoch captured from the first sample seen. Subtracting
 * it in 64-bit before the float cast keeps nanosecond spacing intact, and a
 * shared reference keeps the streams mutually consistent. In freerun the
 * subtraction only removes a constant boot offset the filter differences away,
 * so behavior there is unchanged. The preintegrator forms integration_time_s
 * from raw wire spacing, so subtracting a constant epoch here does not touch it.
 */
static float filter_relative_time_s(struct navigation_estimator_process *process,
                                    uint64_t timestamp_ns) {
  if (!process->filter_epoch_valid) {
    process->filter_epoch_ns = timestamp_ns;
    process->filter_epoch_valid = true;
  }
  return (float)((int64_t)(timestamp_ns - process->filter_epoch_ns)) * 1.0e-9f;
}

/*
 * Hand the generated block the accumulated packet. The rate and specific
 * force fields carry the packet mean, formed as the increment divided by the
 * integration time, which is how Vehicles.Rdd2.WaypointVehicleSystem feeds
 * the same block in simulation.
 */
static void copy_imu_input_to_efmu(struct navigation_estimator_process *process,
                                   const struct rdd2_imu_packet *packet) {
  NavigationEstimatorState *efmu = &process->efmu;
  efmu->imu_valid = packet->valid;
  efmu->imu_fresh = packet->valid;
  efmu->imu_timestamp_s = filter_relative_time_s(process, packet->timestamp_ns);
  efmu->imu_integrationTime_s = packet->integration_time_s;
  for (size_t axis = 0U; axis < 3U; ++axis) {
    efmu->imu_angularVelocityBodyFlu_rad_s[axis] =
        packet->angular_velocity_rad_s[axis];
    efmu->specificForceBodyFlu_m_s2[axis] = packet->specific_force_m_s2[axis];
    efmu->deltaAngleBodyFlu_rad[axis] = packet->delta_angle_rad[axis];
    efmu->deltaVelocityBodyFlu_m_s[axis] = packet->delta_velocity_m_s[axis];
    efmu->deltaPositionBodyFlu_m[axis] = packet->delta_position_m[axis];
    efmu->gyroscopeBiasLinearizationBodyFlu_rad_s[axis] =
        packet->gyroscope_bias_linearization_rad_s[axis];
    efmu->accelerometerBiasLinearizationBodyFlu_m_s2[axis] =
        packet->accelerometer_bias_linearization_m_s2[axis];
    for (size_t column = 0U; column < 3U; ++column) {
      efmu->deltaRotationGyroscopeBiasJacobian_s[axis][column] =
          packet->rotation_gyroscope_bias_jacobian_s[axis][column];
      efmu->deltaVelocityGyroscopeBiasJacobian_m[axis][column] =
          packet->velocity_gyroscope_bias_jacobian_m[axis][column];
      efmu->deltaVelocityAccelerometerBiasJacobian_s[axis][column] =
          packet->velocity_accelerometer_bias_jacobian_s[axis][column];
      efmu->deltaPositionGyroscopeBiasJacobian_m_s[axis][column] =
          packet->position_gyroscope_bias_jacobian_m_s[axis][column];
      efmu->deltaPositionAccelerometerBiasJacobian_s2[axis][column] =
          packet->position_accelerometer_bias_jacobian_s2[axis][column];
    }
  }
  for (size_t element = 0U; element < 4U; ++element) {
    efmu->deltaQuaternionBodyFlu[element] = packet->delta_quaternion[element];
  }
}

static void publish_gyroscope_bias(const NavigationEstimatorState *efmu,
                                   bool estimate_valid) {
  float bias_rad_s[3] = {
      efmu->gyroscopeBiasBodyFlu_rad_s[0],
      efmu->gyroscopeBiasBodyFlu_rad_s[1],
      efmu->gyroscopeBiasBodyFlu_rad_s[2],
  };
  bool usable = estimate_valid && efmu->status_initialized &&
                rdd2_control_values_are_finite(bias_rad_s, 3U);
  k_spinlock_key_t key = k_spin_lock(&g_gyroscope_bias_lock);

  if (usable) {
    g_gyroscope_bias_rad_s[0] = bias_rad_s[0];
    g_gyroscope_bias_rad_s[1] = bias_rad_s[1];
    g_gyroscope_bias_rad_s[2] = bias_rad_s[2];
  }
  k_spin_unlock(&g_gyroscope_bias_lock, key);
  atomic_set(&g_gyroscope_bias_valid, usable ? 1 : 0);
}

bool rdd2_navigation_gyroscope_bias_get(float bias_body_flu_rad_s[3]) {
  k_spinlock_key_t key;

  if (atomic_get(&g_gyroscope_bias_valid) == 0) {
    return false;
  }
  key = k_spin_lock(&g_gyroscope_bias_lock);
  bias_body_flu_rad_s[0] = g_gyroscope_bias_rad_s[0];
  bias_body_flu_rad_s[1] = g_gyroscope_bias_rad_s[1];
  bias_body_flu_rad_s[2] = g_gyroscope_bias_rad_s[2];
  k_spin_unlock(&g_gyroscope_bias_lock, key);
  return true;
}

static void copy_external_odometry_input_to_efmu(
    struct navigation_estimator_process *process,
    const synapse_topic_ExternalOdometryData_t *odometry, bool fresh) {
  NavigationEstimatorState *efmu = &process->efmu;
  efmu->mocap_valid = external_odometry_valid(odometry);
  efmu->mocap_fresh = fresh;
  efmu->mocap_timestamp_s =
      filter_relative_time_s(process, odometry->timestamp_ns);
  efmu->mocap_positionWorldEnu_m[0] = odometry->position_enu_m.x;
  efmu->mocap_positionWorldEnu_m[1] = odometry->position_enu_m.y;
  efmu->mocap_positionWorldEnu_m[2] = odometry->position_enu_m.z;
  efmu->mocap_quaternionWorldBody[0] = odometry->attitude.w;
  efmu->mocap_quaternionWorldBody[1] = odometry->attitude.x;
  efmu->mocap_quaternionWorldBody[2] = odometry->attitude.y;
  efmu->mocap_quaternionWorldBody[3] = odometry->attitude.z;
}

static void copy_gps_input_to_efmu(
    struct navigation_estimator_process *process,
    const struct rdd2_navigation_gps_measurement *measurement) {
  NavigationEstimatorState *efmu = &process->efmu;
  efmu->gps_valid = measurement->valid;
  efmu->gps_fresh = measurement->fresh;
  efmu->positionValid = measurement->position_valid;
  efmu->velocityValid = measurement->velocity_valid;
  efmu->gps_timestamp_s =
      filter_relative_time_s(process, measurement->timestamp_ns);
  for (size_t axis = 0U; axis < 3U; ++axis) {
    efmu->geodetic_deg_m[axis] = measurement->geodetic_deg_m[axis];
    efmu->gps_positionWorldEnu_m[axis] = measurement->position_enu_m[axis];
    efmu->gps_velocityWorldEnu_m_s[axis] = measurement->velocity_enu_m_s[axis];
    for (size_t column = 0U; column < 3U; ++column) {
      efmu->gps_positionCovarianceWorld_m2[axis][column] =
          measurement->position_covariance_enu_m2[axis][column];
      efmu->velocityCovarianceWorld_m2_s2[axis][column] =
          measurement->velocity_covariance_enu_m2_s2[axis][column];
    }
  }
}

#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE)
/*
 * The ESKF takes the raw integrated flow observation, not a manufactured
 * body velocity: it forms
 *   v_forward = range * flow_y / dt,  v_left = -range * flow_x / dt
 * itself, from the gyro-compensated line-of-sight integral. The CSyn driver
 * reports an already rotation-compensated body velocity, so the adapter
 * inverts that relation exactly rather than approximating it, and reports a
 * zero rotation integral because the compensation has already been applied.
 *
 * The estimator scales the line-of-sight covariance by (range/dt)^2 to obtain
 * the velocity covariance, so the driver's velocity covariance is mapped back
 * through the same factor and through the same axis swap.
 */
static void copy_optical_flow_input_to_efmu(
    struct navigation_estimator_process *process,
    const struct rdd2_navigation_optical_flow_measurement *measurement) {
  NavigationEstimatorState *efmu = &process->efmu;
  float integration_time_s = measurement->integration_time_s > 0.0f
                                 ? measurement->integration_time_s
                                 : 1.0e-9f;
  float ground_distance_m = measurement->ground_distance_m > 0.0f
                                ? measurement->ground_distance_m
                                : 1.0e-9f;
  float radians_per_velocity = integration_time_s / ground_distance_m;

  efmu->opticalFlow_valid = measurement->valid;
  efmu->opticalFlow_fresh = measurement->fresh;
  efmu->opticalFlow_timestamp_s =
      filter_relative_time_s(process, measurement->timestamp_ns);
  /* flow_y carries forward motion, flow_x carries the negated left motion */
  efmu->integratedLineOfSight_rad[0] =
      -measurement->velocity_body_flu_m_s[1] * radians_per_velocity;
  efmu->integratedLineOfSight_rad[1] =
      measurement->velocity_body_flu_m_s[0] * radians_per_velocity;
  efmu->integratedLineOfSightCovariance_rad2[0][0] =
      measurement->velocity_covariance_body_m2_s2[1][1] *
      radians_per_velocity * radians_per_velocity;
  efmu->integratedLineOfSightCovariance_rad2[0][1] =
      -measurement->velocity_covariance_body_m2_s2[1][0] *
      radians_per_velocity * radians_per_velocity;
  efmu->integratedLineOfSightCovariance_rad2[1][0] =
      -measurement->velocity_covariance_body_m2_s2[0][1] *
      radians_per_velocity * radians_per_velocity;
  efmu->integratedLineOfSightCovariance_rad2[1][1] =
      measurement->velocity_covariance_body_m2_s2[0][0] *
      radians_per_velocity * radians_per_velocity;
  /* The rotation integral has already been removed by the driver, but the
   * residual compensation error accumulates over the exposure window as an
   * angle random walk, so its variance scales with the integration time. This
   * keeps the gyro term commensurate with the line-of-sight covariance, which
   * the estimator scales by (integration_time / range)^2, instead of letting a
   * fixed constant dominate the innovation covariance. */
  float compensation_variance_rad2 =
      RDD2_OPTICAL_FLOW_COMPENSATION_RATE_NOISE_RAD_S_RTHZ *
      RDD2_OPTICAL_FLOW_COMPENSATION_RATE_NOISE_RAD_S_RTHZ * integration_time_s;
  for (size_t axis = 0U; axis < 3U; ++axis) {
    efmu->integratedGyroscopeBodyFlu_rad[axis] = 0.0f;
    for (size_t column = 0U; column < 3U; ++column) {
      /* The block still requires a strictly positive diagonal, so declare the
       * residual compensation uncertainty rather than an impossible zero. */
      efmu->integratedGyroscopeCovariance_rad2[axis][column] =
          axis == column ? compensation_variance_rad2 : 0.0f;
    }
  }
  efmu->opticalFlow_integrationTime_s = integration_time_s;
  efmu->groundDistance_m = measurement->ground_distance_m;
  efmu->groundDistanceVariance_m2 =
      measurement->ground_distance_variance_m2;
  efmu->quality = measurement->quality;
}
#endif

#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE_RAW)
/*
 * The tightly coupled path hands the ESKF the raw integrated flow, the raw
 * integrated rotation and the range with their own covariances; the estimator
 * forms the body velocity itself. Every field is copied through and the gyro
 * integral is NOT zeroed: the estimator subtracts the rotation with its own
 * covariance rather than trusting a pre-compensated velocity.
 */
static void copy_optical_flow_raw_input_to_efmu(
    struct navigation_estimator_process *process,
    const struct rdd2_navigation_optical_flow_raw_measurement *measurement) {
  NavigationEstimatorState *efmu = &process->efmu;

  efmu->opticalFlow_valid = measurement->valid;
  efmu->opticalFlow_fresh = measurement->fresh;
  efmu->opticalFlow_timestamp_s =
      filter_relative_time_s(process, measurement->timestamp_ns);
  for (size_t row = 0U; row < 2U; ++row) {
    efmu->integratedLineOfSight_rad[row] =
        measurement->integrated_line_of_sight_rad[row];
    for (size_t column = 0U; column < 2U; ++column) {
      efmu->integratedLineOfSightCovariance_rad2[row][column] =
          measurement->integrated_line_of_sight_cov_rad2[row][column];
    }
  }
  for (size_t row = 0U; row < 3U; ++row) {
    efmu->integratedGyroscopeBodyFlu_rad[row] =
        measurement->integrated_gyro_body_flu_rad[row];
    for (size_t column = 0U; column < 3U; ++column) {
      efmu->integratedGyroscopeCovariance_rad2[row][column] =
          measurement->integrated_gyro_cov_rad2[row][column];
    }
  }
  efmu->opticalFlow_integrationTime_s = measurement->integration_time_s;
  efmu->groundDistance_m = measurement->ground_distance_m;
  efmu->groundDistanceVariance_m2 = measurement->ground_distance_variance_m2;
  efmu->quality = measurement->quality;
}
#endif

static void
health_use_control_time(synapse_topic_VehicleHealthData_t *health,
                        const synapse_topic_InertialSampleData_t *imu,
                        bool fresh) {
#if defined(CONFIG_RDD2_LOCKSTEP)
  if (fresh) {
    /* The rate thread publishes health after processing this lockstep IMU
     * tick. Its payload clock is wall time, so associate a newly observed
     * publication with the causal control tick before comparing domains. */
    health->timestamp_ns = imu->timestamp_ns;
  }
#else
  ARG_UNUSED(health);
  ARG_UNUSED(imu);
  ARG_UNUSED(fresh);
#endif
}

/*
 * Position quality is keyed only on the recovery stage the block reports; this
 * is the model's design, and the block owns the recovery ladder. Stage 1
 * (covariance inflated) is the first degradation signal the block raises, so it
 * is the point at which quality first drops below full. Innovation-gate
 * rejections that occur before the block opens its inflate window do not enter
 * this mapping and so do not lower quality on their own: only a stage the block
 * has actually entered changes the reported quality. Stages 2 and 3 hold
 * attitude and rates usable while demoting position below the usable floor.
 */
static int8_t estimate_quality_pct(const NavigationEstimatorState *efmu) {
  if (!efmu->estimate_valid || !efmu->status_initialized) {
    return 0;
  }

  switch (efmu->status_recoveryStage) {
  case 0: /* RecoveryNominal */
    return 100;
  case 1: /* RecoveryCovarianceInflated */
    return 50;
  case 2: /* RecoveryAidingDivergent */
  case 3: /* RecoveryMisconfigured */
  default:
    /* Preserve finite attitude and rates, but mark position as demoted. */
    return RDD2_NAVIGATION_POSITION_QUALITY_MIN_PCT - 1;
  }
}

static bool efmu_estimate_is_finite(const NavigationEstimatorState *efmu) {
  const float values[] = {
      efmu->estimate_quaternionWorldBody[0],
      efmu->estimate_quaternionWorldBody[1],
      efmu->estimate_quaternionWorldBody[2],
      efmu->estimate_quaternionWorldBody[3],
      efmu->estimate_positionWorldEnu_m[0],
      efmu->estimate_positionWorldEnu_m[1],
      efmu->estimate_positionWorldEnu_m[2],
      efmu->estimate_velocityWorldEnu_m_s[0],
      efmu->estimate_velocityWorldEnu_m_s[1],
      efmu->estimate_velocityWorldEnu_m_s[2],
      efmu->estimate_angularVelocityBodyFlu_rad_s[0],
      efmu->estimate_angularVelocityBodyFlu_rad_s[1],
      efmu->estimate_angularVelocityBodyFlu_rad_s[2],
      efmu->estimate_timestamp_s,
      /* The rate loop subtracts the published gyroscope bias from the raw IMU
       * sample every control tick, so a non-finite bias must withdraw
       * RatesValid here rather than reach the loop uncorrected. Including it
       * keeps this predicate and publish_gyroscope_bias() consistent: whenever
       * the estimate is published usable the bias is finite, so
       * body_rate_for_rate_loop never runs uncorrected while
       * control_inputs_are_usable passes. */
      efmu->gyroscopeBiasBodyFlu_rad_s[0],
      efmu->gyroscopeBiasBodyFlu_rad_s[1],
      efmu->gyroscopeBiasBodyFlu_rad_s[2],
  };
  const float max_timestamp_s = (float)(UINT64_MAX / UINT64_C(1000000000));

  return efmu->estimate_timestamp_s >= 0.0f &&
         efmu->estimate_timestamp_s <= max_timestamp_s &&
         rdd2_control_values_are_finite(values, ARRAY_SIZE(values));
}

/*
 * The published estimate carries a monotonic reset counter so a consumer can
 * distinguish an in-place update from a state discontinuity. The automatic
 * recovery ladder replaces the filter state in place on the tick it reports
 * status.reseeded, which is the only state jump the block signals: recovery
 * stages 1 and 2 leave the state in place, so they are not counted here.
 * reseeded is a per-tick level, so counting each tick it is set reproduces the
 * block's own reseedCount edge.
 */
static void
capture_estimator_health(struct navigation_estimator_process *process) {
  if (process->efmu.status_reseeded) {
    process->reset_counter = (uint8_t)(process->reset_counter + 1U);
  }
}

static bool
navigation_imu_payload_is_usable(struct navigation_estimator_process *process,
                                 bool state_usable) {
  if (!state_usable) {
    process->imu_payload_hold_count = 0U;
    process->imu_payload_usable_observed = false;
    return false;
  }
  if (!process->efmu.status_imuPayloadHeld) {
    process->imu_payload_hold_count = 0U;
    if (!process->efmu.imu_valid) {
      return false;
    }
    process->imu_payload_usable_observed = true;
    return true;
  }
  if (process->imu_payload_hold_count < UINT16_MAX) {
    process->imu_payload_hold_count++;
  }
  return rdd2_navigation_imu_hold_is_usable(
      process->imu_payload_usable_observed, process->imu_payload_hold_count);
}

static void publish_efmu_estimate(struct navigation_estimator_process *process,
                                  bool estimate_valid) {
  NavigationEstimatorState *efmu = &process->efmu;
  uint64_t timestamp_ns = process->imu.timestamp_ns;
  uint8_t flags = estimate_valid
                      ? synapse_topic_AttitudeEstimateFlags_AttitudeValid |
                            synapse_topic_AttitudeEstimateFlags_RatesValid
                      : 0U;

  process->attitude = (synapse_topic_AttitudeEstimateData_t){
      .timestamp_ns = timestamp_ns,
      .attitude =
          {
              .w =
                  estimate_valid ? efmu->estimate_quaternionWorldBody[0] : 1.0f,
              .x =
                  estimate_valid ? efmu->estimate_quaternionWorldBody[1] : 0.0f,
              .y =
                  estimate_valid ? efmu->estimate_quaternionWorldBody[2] : 0.0f,
              .z =
                  estimate_valid ? efmu->estimate_quaternionWorldBody[3] : 0.0f,
          },
      .angular_velocity_flu_rad_s =
          {
              .roll = estimate_valid
                          ? efmu->estimate_angularVelocityBodyFlu_rad_s[0]
                          : 0.0f,
              .pitch = estimate_valid
                           ? efmu->estimate_angularVelocityBodyFlu_rad_s[1]
                           : 0.0f,
              .yaw = estimate_valid
                         ? efmu->estimate_angularVelocityBodyFlu_rad_s[2]
                         : 0.0f,
          },
      .flags = flags,
  };
  process->odometry = (synapse_topic_OdometryEstimateData_t){
      .timestamp_ns = process->attitude.timestamp_ns,
      .position_enu_m =
          {
              .x = estimate_valid ? efmu->estimate_positionWorldEnu_m[0] : 0.0f,
              .y = estimate_valid ? efmu->estimate_positionWorldEnu_m[1] : 0.0f,
              .z = estimate_valid ? efmu->estimate_positionWorldEnu_m[2] : 0.0f,
          },
      .attitude = process->attitude.attitude,
      .velocity_enu_m_s =
          {
              .x = estimate_valid ? efmu->estimate_velocityWorldEnu_m_s[0]
                                  : 0.0f,
              .y = estimate_valid ? efmu->estimate_velocityWorldEnu_m_s[1]
                                  : 0.0f,
              .z = estimate_valid ? efmu->estimate_velocityWorldEnu_m_s[2]
                                  : 0.0f,
          },
      .angular_velocity_flu_rad_s =
          process->attitude.angular_velocity_flu_rad_s,
      .reset_counter = process->reset_counter,
      .estimator_type = 1U,
      .quality_pct = estimate_valid ? estimate_quality_pct(efmu) : 0,
  };
  (void)zros_pub_update(&process->odometry_pub);
  (void)zros_pub_update(&process->attitude_pub);
}

#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE)
static void update_optical_flow_diagnostics(
    const struct navigation_estimator_process *process,
    const struct rdd2_navigation_optical_flow_measurement *measurement) {
  const struct rdd2_navigation_optical_flow_adapter *adapter =
      &process->optical_flow_adapter;
  struct rdd2_navigation_optical_flow_diagnostics diagnostics = {
      .control_timestamp_ns = process->imu.timestamp_ns,
      .source_timestamp_ns = process->optical_flow.timestamp_ns,
      .source_generation = rdd2_topic_generation(&topic_optical_flow_vel),
      .accepted_count = adapter->accepted_count,
      .rejected_count = adapter->rejected_count,
      .fusion_accepted_count = process->optical_flow_fusion_accepted_count,
      .last_fusion_control_timestamp_ns =
          process->optical_flow_last_fusion_control_timestamp_ns,
      .consecutive_estimator_rejections =
          process->efmu.opticalFlowConsecutiveRejections,
      .velocity_body_flu_m_s =
          {
              process->optical_flow.velocity_flu_m_s.x,
              process->optical_flow.velocity_flu_m_s.y,
          },
      .ground_distance_m = process->optical_flow.distance_m,
      .quality = process->optical_flow.quality,
      .flags = process->optical_flow.flags,
      .time_status = process->optical_flow.time_status,
      .correction_outcome = process->efmu.status_correctionOutcome,
      .correction_source = process->efmu.status_correctionSource,
      .recovery_stage = process->efmu.status_recoveryStage,
      .adapter_status = adapter->status,
      .measurement_valid = measurement->valid,
      .measurement_fresh = measurement->fresh,
      .correction_accepted = process->efmu.status_opticalFlowCorrectionAccepted,
  };
  k_spinlock_key_t key;

  if (adapter->sample_valid &&
      process->imu.timestamp_ns >= adapter->last_valid_control_timestamp_ns) {
    diagnostics.accepted_age_ns =
        process->imu.timestamp_ns - adapter->last_valid_control_timestamp_ns;
  }
  key = k_spin_lock(&g_optical_flow_diagnostics_lock);
  g_optical_flow_diagnostics = diagnostics;
  k_spin_unlock(&g_optical_flow_diagnostics_lock, key);
}
#endif

#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE_RAW)
static void update_optical_flow_raw_diagnostics(
    const struct navigation_estimator_process *process,
    const struct rdd2_navigation_optical_flow_raw_measurement *measurement) {
  const struct rdd2_navigation_optical_flow_raw_adapter *adapter =
      &process->optical_flow_raw_adapter;
  struct rdd2_navigation_optical_flow_raw_diagnostics diagnostics = {
      .control_timestamp_ns = process->imu.timestamp_ns,
      .source_timestamp_ns = process->optical_flow_raw.timestamp_ns,
      .source_generation = rdd2_topic_generation(&topic_optical_flow),
      .accepted_count = adapter->accepted_count,
      .rejected_count = adapter->rejected_count,
      .fusion_accepted_count = process->optical_flow_fusion_accepted_count,
      .last_fusion_control_timestamp_ns =
          process->optical_flow_last_fusion_control_timestamp_ns,
      .consecutive_estimator_rejections =
          process->efmu.opticalFlowConsecutiveRejections,
      .line_of_sight_rad =
          {
              measurement->integrated_line_of_sight_rad[0],
              measurement->integrated_line_of_sight_rad[1],
          },
      .gyro_body_flu_rad =
          {
              measurement->integrated_gyro_body_flu_rad[0],
              measurement->integrated_gyro_body_flu_rad[1],
          },
      .integration_time_s = measurement->integration_time_s,
      .ground_distance_m = measurement->ground_distance_m,
      .quality = measurement->quality,
      .flags = process->optical_flow_raw.flags,
      .time_status = process->optical_flow_raw.time_status,
      .correction_outcome = process->efmu.status_correctionOutcome,
      .correction_source = process->efmu.status_correctionSource,
      .recovery_stage = process->efmu.status_recoveryStage,
      .adapter_status = adapter->status,
      .measurement_valid = measurement->valid,
      .measurement_fresh = measurement->fresh,
      .correction_accepted = process->efmu.status_opticalFlowCorrectionAccepted,
  };
  k_spinlock_key_t key;

  if (adapter->sample_valid &&
      process->imu.timestamp_ns >= adapter->last_valid_control_timestamp_ns) {
    diagnostics.accepted_age_ns =
        process->imu.timestamp_ns - adapter->last_valid_control_timestamp_ns;
  }
  key = k_spin_lock(&g_optical_flow_raw_diagnostics_lock);
  g_optical_flow_raw_diagnostics = diagnostics;
  k_spin_unlock(&g_optical_flow_raw_diagnostics_lock, key);
}
#endif

static void navigation_estimator_thread(void *arg1, void *arg2, void *arg3) {
  struct navigation_estimator_process *process = arg1;

  ARG_UNUSED(arg2);
  ARG_UNUSED(arg3);

  while (true) {
    struct rdd2_navigation_gps_measurement gps_measurement;
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE)
    struct rdd2_navigation_optical_flow_measurement optical_flow_measurement;
#endif
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE_RAW)
    struct rdd2_navigation_optical_flow_raw_measurement
        optical_flow_raw_measurement;
#endif
    bool external_fresh;
    bool gnss_fresh;
    bool health_fresh;
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE)
    bool optical_flow_fresh;
#endif
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE_RAW)
    bool optical_flow_raw_fresh;
#endif
    bool origin_captured;
    bool estimate_valid;
    bool outputs_finite;
    bool state_usable;
    bool step_ok;

    if (zros_sub_wait(&process->imu_sub, K_FOREVER) != 0 ||
        zros_sub_update(&process->imu_sub) != 0) {
      continue;
    }
    accumulate_imu_sample(process);
    if (!rdd2_release_due(&process->release_scheduler, RDD2_CONTROL_RATE_HZ,
                          RDD2_NAVIGATION_ESTIMATOR_RATE_HZ)) {
      continue;
    }
    close_imu_packet(process);

    external_fresh = zros_sub_update(&process->external_odometry_sub) == 0;
    gnss_fresh = zros_sub_update(&process->gnss_sub) == 0;
    health_fresh = zros_sub_update(&process->health_sub) == 0;
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE)
    optical_flow_fresh = zros_sub_update(&process->optical_flow_sub) == 0;
#endif
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE_RAW)
    optical_flow_raw_fresh =
        zros_sub_update(&process->optical_flow_raw_sub) == 0;
#endif
    health_use_control_time(&process->health, &process->imu, health_fresh);
    copy_imu_input_to_efmu(process, &process->imu_packet);
    if (external_odometry_source_allowed(
            IS_ENABLED(CONFIG_RDD2_GNSS_SOURCE_ONBOARD) ||
            IS_ENABLED(CONFIG_RDD2_GNSS_SOURCE_LOCKSTEP) ||
            IS_ENABLED(CONFIG_RDD2_GNSS_SOURCE_WIRE))) {
      copy_external_odometry_input_to_efmu(
          process, &process->external_odometry, external_fresh);
    } else {
      process->efmu.mocap_valid = false;
      process->efmu.mocap_fresh = false;
    }
    origin_captured = rdd2_navigation_gps_step(
        &process->gps_adapter, &gps_measurement, &process->gnss, gnss_fresh,
        &process->health, health_fresh, process->imu.timestamp_ns);
    atomic_set(&g_origin_valid, process->gps_adapter.origin_valid ? 1 : 0);
    copy_gps_input_to_efmu(process, &gps_measurement);
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE)
    rdd2_navigation_optical_flow_step(
        &process->optical_flow_adapter, &optical_flow_measurement,
        &process->optical_flow, optical_flow_fresh, process->imu.timestamp_ns,
        &g_optical_flow_config);
    copy_optical_flow_input_to_efmu(process, &optical_flow_measurement);
#elif defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE_RAW)
    rdd2_navigation_optical_flow_raw_step(
        &process->optical_flow_raw_adapter, &optical_flow_raw_measurement,
        &process->optical_flow_raw, optical_flow_raw_fresh,
        process->imu.timestamp_ns, &g_optical_flow_raw_config);
    copy_optical_flow_raw_input_to_efmu(process, &optical_flow_raw_measurement);
#else
    process->efmu.opticalFlow_valid = false;
    process->efmu.opticalFlow_fresh = false;
#endif
    if (origin_captured) {
      process->gps_origin_initialization_pending = true;
      process->gps_origin_initialization_started_ns = process->imu.timestamp_ns;
    }
    if (process->gps_origin_initialization_pending &&
        gps_origin_initialization_timed_out(
            IS_ENABLED(CONFIG_RDD2_GNSS_SOURCE_ONBOARD) ||
                IS_ENABLED(CONFIG_RDD2_GNSS_SOURCE_LOCKSTEP) ||
                IS_ENABLED(CONFIG_RDD2_GNSS_SOURCE_WIRE),
            process->gps_origin_initialization_started_ns,
            process->imu.timestamp_ns)) {
      process->gps_origin_initialization_pending = false;
    }
    if (process->gps_origin_initialization_pending) {
      process->efmu.mocap_valid = false;
      process->efmu.mocap_fresh = false;
    }
    process->efmu.reset =
        !process->initialized || process->gps_origin_initialization_pending;
    NavigationEstimator_dostep(&process->efmu);
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE)
    if (process->efmu.status_opticalFlowCorrectionAccepted) {
      process->optical_flow_fusion_accepted_count++;
      process->optical_flow_last_fusion_control_timestamp_ns =
          process->imu.timestamp_ns;
    }
    update_optical_flow_diagnostics(process, &optical_flow_measurement);
#elif defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE_RAW)
    if (process->efmu.status_opticalFlowCorrectionAccepted) {
      process->optical_flow_fusion_accepted_count++;
      process->optical_flow_last_fusion_control_timestamp_ns =
          process->imu.timestamp_ns;
    }
    update_optical_flow_raw_diagnostics(process, &optical_flow_raw_measurement);
#endif
    step_ok =
        rdd2_generated_step_ok(process->efmu.rumoca_galec_error_signal_status);
    outputs_finite = efmu_estimate_is_finite(&process->efmu);
    state_usable = step_ok && outputs_finite && process->efmu.estimate_valid &&
                   process->efmu.status_initialized;
    estimate_valid = navigation_imu_payload_is_usable(process, state_usable);
    /* Keep a bounded held-IMU publication from resetting the estimator. If
     * the hold exceeds its deadline, invalid publication makes Rate latch its
     * existing motors-zero fault until the pilot acknowledges it. */
    process->initialized = process->gps_origin_initialization_pending
                               ? estimate_valid
                               : state_usable;
    if (estimate_valid) {
      process->gps_origin_initialization_pending = false;
    }
    capture_estimator_health(process);
    publish_gyroscope_bias(&process->efmu, estimate_valid);
    publish_efmu_estimate(process, estimate_valid);
  }
}

bool rdd2_navigation_origin_valid_get(void) {
  return atomic_get(&g_origin_valid) != 0;
}

bool rdd2_navigation_optical_flow_diagnostics_get(
    struct rdd2_navigation_optical_flow_diagnostics *diagnostics) {
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE)
  k_spinlock_key_t key;

  if (diagnostics == NULL) {
    return false;
  }
  key = k_spin_lock(&g_optical_flow_diagnostics_lock);
  *diagnostics = g_optical_flow_diagnostics;
  k_spin_unlock(&g_optical_flow_diagnostics_lock, key);
  return true;
#else
  ARG_UNUSED(diagnostics);
  return false;
#endif
}

bool rdd2_navigation_optical_flow_raw_diagnostics_get(
    struct rdd2_navigation_optical_flow_raw_diagnostics *diagnostics) {
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE_RAW)
  k_spinlock_key_t key;

  if (diagnostics == NULL) {
    return false;
  }
  key = k_spin_lock(&g_optical_flow_raw_diagnostics_lock);
  *diagnostics = g_optical_flow_raw_diagnostics;
  k_spin_unlock(&g_optical_flow_raw_diagnostics_lock, key);
  return true;
#else
  ARG_UNUSED(diagnostics);
  return false;
#endif
}

static void set_default_mocap_covariance(NavigationEstimatorState *efmu) {
  for (size_t i = 0U; i < 3U; ++i) {
    efmu->mocap_positionCovarianceWorld_m2[i][i] = 0.01f;
    efmu->attitudeCovarianceBody_rad2[i][i] = 0.01f;
  }
}

int rdd2_navigation_estimator_process_start(void) {
  struct navigation_estimator_process *process = &g_process;
  int rc;

  *process = (struct navigation_estimator_process){0};
  rdd2_navigation_gps_init(&process->gps_adapter);
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE)
  rdd2_navigation_optical_flow_init(&process->optical_flow_adapter);
  if (!rdd2_navigation_optical_flow_config_valid(&g_optical_flow_config)) {
    LOG_ERR("optical-flow configuration is invalid; every flow sample will be "
            "rejected until the Kconfig limits are corrected");
  }
#endif
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE_RAW)
  rdd2_navigation_optical_flow_raw_init(&process->optical_flow_raw_adapter);
  if (!rdd2_navigation_optical_flow_raw_config_valid(
          &g_optical_flow_raw_config)) {
    LOG_ERR("optical-flow raw configuration is invalid; every flow sample will "
            "be rejected until the Kconfig limits are corrected");
  }
#endif
  NavigationEstimator_startup(&process->efmu);
  set_default_mocap_covariance(&process->efmu);
  /* The generated default is the model's 1 kHz service interval. The block
   * times its recovery ladder and its process noise on this value, so it has
   * to state the rate the block is actually released at. */
  process->efmu.samplePeriod = 1.0f / (float)RDD2_NAVIGATION_ESTIMATOR_RATE_HZ;
  NavigationEstimator_recalibrate(&process->efmu);
  rdd2_imu_preintegrator_reset(
      &process->preintegrator, process->efmu.initialGyroscopeBiasBodyFlu_rad_s,
      process->efmu.initialAccelerometerBiasBodyFlu_m_s2);
  /* The first sample after a reset has no earlier timestamp to difference
   * against, but it still covers one control period, which is what the model
   * integrates it over. */
  process->preintegrator.nominal_sample_period_s = RDD2_CONTROL_DT_S;
  zros_node_init(&process->node, "efmu_navigation");

  rc = zros_sub_init(&process->imu_sub, &process->node, &topic_control_imu,
                     &process->imu, 0.0);
  if (rc == 0) {
    rc = zros_sub_init(&process->external_odometry_sub, &process->node,
                       &topic_external_odometry, &process->external_odometry,
                       0.0);
  }
  if (rc == 0) {
    rc = zros_sub_init(&process->gnss_sub, &process->node, &topic_gnss_fix,
                       &process->gnss, 0.0);
  }
  if (rc == 0) {
    rc = zros_sub_init(&process->health_sub, &process->node,
                       &topic_vehicle_health, &process->health, 0.0);
  }
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE)
  if (rc == 0) {
    rc = zros_sub_init(&process->optical_flow_sub, &process->node,
                       &topic_optical_flow_vel, &process->optical_flow,
                       0.0);
  }
#endif
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE_RAW)
  if (rc == 0) {
    rc = zros_sub_init(&process->optical_flow_raw_sub, &process->node,
                       &topic_optical_flow, &process->optical_flow_raw, 0.0);
  }
#endif
  if (rc == 0) {
    rc = zros_pub_init(&process->odometry_pub, &process->node,
                       &topic_navigation_odometry, &process->odometry);
  }
  if (rc == 0) {
    rc = zros_pub_init(&process->attitude_pub, &process->node,
                       &topic_attitude_estimate, &process->attitude);
  }
  if (rc != 0) {
    return rc;
  }

  k_thread_create(&g_thread, g_navigation_stack,
                  K_THREAD_STACK_SIZEOF(g_navigation_stack),
                  navigation_estimator_thread, process, NULL, NULL,
                  RDD2_NAVIGATION_PRIORITY, 0, K_NO_WAIT);
  k_thread_name_set(&g_thread, "efmu_navigation");
  return 0;
}
