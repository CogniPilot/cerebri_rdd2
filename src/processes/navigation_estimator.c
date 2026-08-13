/* SPDX-License-Identifier: Apache-2.0 */

#include "processes.h"

#include "control_safety.h"
#include "interfaces/zros_topics.h"
#include "navigation_gps.h"
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

#define NAVIGATION_STACK_SIZE 32768
#define GPS_ORIGIN_INITIALIZATION_TIMEOUT_NS UINT64_C(100000000)

struct navigation_estimator_process {
  NavigationEstimatorState efmu;
  synapse_topic_InertialSampleData_t imu;
  synapse_topic_ExternalOdometryData_t external_odometry;
  synapse_topic_GnssFixData_t gnss;
  synapse_topic_VehicleHealthData_t health;
  synapse_topic_OdometryEstimateData_t odometry;
  synapse_topic_AttitudeEstimateData_t attitude;
  struct rdd2_navigation_gps_adapter gps_adapter;
  struct zros_node node;
  struct zros_sub imu_sub;
  struct zros_sub external_odometry_sub;
  struct zros_sub gnss_sub;
  struct zros_sub health_sub;
  struct zros_pub odometry_pub;
  struct zros_pub attitude_pub;
  struct rdd2_release_scheduler release_scheduler;
  bool initialized;
  bool gps_origin_initialization_pending;
  uint64_t gps_origin_initialization_started_ns;
  uint8_t reset_counter;
};

static struct navigation_estimator_process g_process;
static atomic_t g_origin_valid;
static struct k_thread g_thread;
K_THREAD_STACK_DEFINE(g_navigation_stack, NAVIGATION_STACK_SIZE);

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

  return (odometry->flags & required) == required &&
         (odometry->flags & synapse_topic_ExternalOdometryFlags_Lost) == 0U;
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

static void
copy_imu_input_to_efmu(NavigationEstimatorState *efmu,
                       const synapse_topic_InertialSampleData_t *imu) {
  efmu->imu_valid = imu_valid(imu);
  efmu->imu_fresh = true;
  efmu->imu_timestamp_s = (float)imu->timestamp_ns * 1.0e-9f;
  efmu->imu_angularVelocityBodyFlu_rad_s[0] = imu->gyro_flu_rad_s.x;
  efmu->imu_angularVelocityBodyFlu_rad_s[1] = imu->gyro_flu_rad_s.y;
  efmu->imu_angularVelocityBodyFlu_rad_s[2] = imu->gyro_flu_rad_s.z;
  efmu->specificForceBodyFlu_m_s2[0] = imu->accel_flu_m_s2.x;
  efmu->specificForceBodyFlu_m_s2[1] = imu->accel_flu_m_s2.y;
  efmu->specificForceBodyFlu_m_s2[2] = imu->accel_flu_m_s2.z;
}

static void copy_external_odometry_input_to_efmu(
    NavigationEstimatorState *efmu,
    const synapse_topic_ExternalOdometryData_t *odometry, bool fresh) {
  efmu->mocap_valid = external_odometry_valid(odometry);
  efmu->mocap_fresh = fresh;
  efmu->mocap_timestamp_s = (float)odometry->timestamp_ns * 1.0e-9f;
  efmu->mocap_positionWorldEnu_m[0] = odometry->position_enu_m.x;
  efmu->mocap_positionWorldEnu_m[1] = odometry->position_enu_m.y;
  efmu->mocap_positionWorldEnu_m[2] = odometry->position_enu_m.z;
  efmu->mocap_quaternionWorldBody[0] = odometry->attitude.w;
  efmu->mocap_quaternionWorldBody[1] = odometry->attitude.x;
  efmu->mocap_quaternionWorldBody[2] = odometry->attitude.y;
  efmu->mocap_quaternionWorldBody[3] = odometry->attitude.z;
}

static void copy_gps_input_to_efmu(
    NavigationEstimatorState *efmu,
    const struct rdd2_navigation_gps_measurement *measurement) {
  efmu->gps_valid = measurement->valid;
  efmu->gps_fresh = measurement->fresh;
  efmu->positionValid = measurement->position_valid;
  efmu->velocityValid = measurement->velocity_valid;
  efmu->gps_timestamp_s = measurement->timestamp_s;
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

static int8_t estimate_quality_pct(const NavigationEstimatorState *efmu) {
  int32_t rejection_count = efmu->status_consecutiveRejectedCorrections;
  int32_t rejection_limit = efmu->rejectedCorrectionLimit;
  int64_t rejection_penalty;

  if (!efmu->estimate_valid || !efmu->status_initialized) {
    return 0;
  }
  if (rejection_count < 0) {
    rejection_count = 0;
  }
  if (efmu->status_innovationGateRejected && rejection_count == 0) {
    rejection_count = 1;
  }
  if (rejection_limit <= 0 || rejection_count >= rejection_limit) {
    return 0;
  }

  rejection_penalty =
      ((int64_t)rejection_count * 100 + rejection_limit - 1) / rejection_limit;
  return (int8_t)(100 - rejection_penalty);
}

static void
capture_estimator_health(struct navigation_estimator_process *process) {
  if (process->efmu.status_covarianceReinitialized) {
    process->reset_counter = (uint8_t)(process->reset_counter + 1U);
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
  };
  const float max_timestamp_s = (float)(UINT64_MAX / UINT64_C(1000000000));

  return efmu->estimate_timestamp_s >= 0.0f &&
         efmu->estimate_timestamp_s <= max_timestamp_s &&
         rdd2_control_values_are_finite(values, ARRAY_SIZE(values));
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

static void navigation_estimator_thread(void *arg1, void *arg2, void *arg3) {
  struct navigation_estimator_process *process = arg1;

  ARG_UNUSED(arg2);
  ARG_UNUSED(arg3);

  while (true) {
    struct rdd2_navigation_gps_measurement gps_measurement;
    bool external_fresh;
    bool gnss_fresh;
    bool health_fresh;
    bool origin_captured;
    bool estimate_valid;
    bool outputs_finite;
    bool step_ok;

    if (zros_sub_wait(&process->imu_sub, K_FOREVER) != 0 ||
        zros_sub_update(&process->imu_sub) != 0) {
      continue;
    }
    if (!rdd2_release_due(&process->release_scheduler, RDD2_CONTROL_RATE_HZ,
                          RDD2_NAVIGATION_ESTIMATOR_RATE_HZ)) {
      continue;
    }

    external_fresh = zros_sub_update(&process->external_odometry_sub) == 0;
    gnss_fresh = zros_sub_update(&process->gnss_sub) == 0;
    health_fresh = zros_sub_update(&process->health_sub) == 0;
    health_use_control_time(&process->health, &process->imu, health_fresh);
    copy_imu_input_to_efmu(&process->efmu, &process->imu);
    if (external_odometry_source_allowed(
            IS_ENABLED(CONFIG_RDD2_GNSS_SOURCE_ONBOARD) ||
            IS_ENABLED(CONFIG_RDD2_GNSS_SOURCE_LOCKSTEP))) {
      copy_external_odometry_input_to_efmu(
          &process->efmu, &process->external_odometry, external_fresh);
    } else {
      process->efmu.mocap_valid = false;
      process->efmu.mocap_fresh = false;
    }
    origin_captured = rdd2_navigation_gps_step(
        &process->gps_adapter, &gps_measurement, &process->gnss, gnss_fresh,
        &process->health, health_fresh, process->imu.timestamp_ns);
    atomic_set(&g_origin_valid, process->gps_adapter.origin_valid ? 1 : 0);
    copy_gps_input_to_efmu(&process->efmu, &gps_measurement);
    if (origin_captured) {
      process->gps_origin_initialization_pending = true;
      process->gps_origin_initialization_started_ns = process->imu.timestamp_ns;
    }
    if (process->gps_origin_initialization_pending &&
        gps_origin_initialization_timed_out(
            IS_ENABLED(CONFIG_RDD2_GNSS_SOURCE_ONBOARD) ||
                IS_ENABLED(CONFIG_RDD2_GNSS_SOURCE_LOCKSTEP),
            process->gps_origin_initialization_started_ns,
            process->imu.timestamp_ns)) {
      process->gps_origin_initialization_pending = false;
    }
    if (process->gps_origin_initialization_pending) {
      process->efmu.mocap_valid = false;
      process->efmu.mocap_fresh = false;
    }
    process->efmu.opticalFlow_valid = false;
    process->efmu.opticalFlow_fresh = false;
    process->efmu.reset =
        !process->initialized || process->gps_origin_initialization_pending;
    NavigationEstimator_dostep(&process->efmu);
    step_ok =
        rdd2_generated_step_ok(process->efmu.rumoca_galec_error_signal_status);
    outputs_finite = efmu_estimate_is_finite(&process->efmu);
    estimate_valid = process->efmu.imu_valid && step_ok && outputs_finite &&
                     process->efmu.estimate_valid &&
                     process->efmu.status_initialized;
    process->initialized = estimate_valid;
    if (estimate_valid) {
      process->gps_origin_initialization_pending = false;
    }
    capture_estimator_health(process);
    publish_efmu_estimate(process, estimate_valid);
  }
}

bool rdd2_navigation_origin_valid_get(void) {
  return atomic_get(&g_origin_valid) != 0;
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
  NavigationEstimator_startup(&process->efmu);
  set_default_mocap_covariance(&process->efmu);
  NavigationEstimator_recalibrate(&process->efmu);
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
