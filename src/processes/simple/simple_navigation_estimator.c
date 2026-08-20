/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Stand-in navigation estimator.
 *
 * Replaces the generated NavigationEstimator eFMU with a hand-written
 * complementary attitude filter plus GNSS and optical-flow passthrough. It
 * publishes the same topics (attitude_estimate, navigation_odometry) with the
 * same field semantics, flags, quality gating, and cadence as the eFMI
 * estimator so guidance, the allocator, the planner, and mission_shell consume
 * it unchanged.
 *
 * Frames (see simple_control.h for the full convention block):
 *   - IMU accel/gyro arrive in body FLU (+x forward, +y left, +z up).
 *   - Published attitude is the world-from-body quaternion (w, x, y, z).
 *   - Published position/velocity are world ENU (+x east, +y north, +z up).
 *   - Published body rates are body FLU.
 *
 * Attitude: gyro is integrated for roll/pitch/yaw, and the accelerometer
 * gravity reference trims roll/pitch when the specific-force magnitude is near
 * 1 g. Yaw has no absolute reference here, so it integrates gyro only and
 * drifts over time without a magnetometer. This is stated plainly because a
 * bench operator must not expect a stable heading from this backend.
 *
 * Position/velocity: GNSS fixes are projected to a local tangent (ENU) frame
 * anchored at the first usable fix by the shared navigation_gps adapter.
 * Optical-flow velocity, when fresh, is rotated from body FLU into world ENU
 * using the current yaw estimate and published in the odometry twist in place
 * of the GNSS horizontal velocity, so optical-flow sign conventions are
 * observable end to end on the bench.
 */

#include "processes.h"

#include "control_safety.h"
#include "interfaces/zros_topics.h"
#include "navigation_gps.h"
#include "scheduling.h"
#include "simple/simple_control.h"

#include <math.h>
#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zros/private/zros_node_struct.h>
#include <zros/private/zros_pub_struct.h>
#include <zros/private/zros_sub_struct.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>
#include <zros/zros_sub.h>

LOG_MODULE_DECLARE(rdd2, LOG_LEVEL_INF);

#define NAVIGATION_STACK_SIZE 32768

struct simple_navigation_estimator_process {
  synapse_topic_InertialSampleData_t imu;
  synapse_topic_GnssFixData_t gnss;
  synapse_topic_OpticalFlowVelocityData_t optical_flow;
  synapse_topic_VehicleHealthData_t health;
  synapse_topic_OdometryEstimateData_t odometry;
  synapse_topic_AttitudeEstimateData_t attitude;
  struct rdd2_navigation_gps_adapter gps_adapter;
  struct zros_node node;
  struct zros_sub imu_sub;
  struct zros_sub gnss_sub;
  struct zros_sub optical_flow_sub;
  struct zros_sub health_sub;
  struct zros_pub odometry_pub;
  struct zros_pub attitude_pub;
  struct rdd2_release_scheduler release_scheduler;
  /* Complementary filter body attitude, radians, ENU/FLU convention. */
  float roll_rad;
  float pitch_rad;
  float yaw_rad;
  /* Latest world ENU position/velocity held between GNSS updates. */
  float position_enu_m[3];
  float velocity_enu_m_s[3];
  bool attitude_initialized;
  bool optical_flow_observed;
  uint64_t last_imu_timestamp_ns;
  bool have_last_imu_timestamp;
};

static struct simple_navigation_estimator_process g_process;
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

static float integration_dt_s(struct simple_navigation_estimator_process *process,
                              uint64_t timestamp_ns) {
  float dt;

  if (!process->have_last_imu_timestamp ||
      timestamp_ns <= process->last_imu_timestamp_ns) {
    process->last_imu_timestamp_ns = timestamp_ns;
    process->have_last_imu_timestamp = true;
    return 0.0f;
  }
  dt = (float)(timestamp_ns - process->last_imu_timestamp_ns) * 1.0e-9f;
  process->last_imu_timestamp_ns = timestamp_ns;
  if (dt > SIMPLE_MAX_DT_S) {
    dt = SIMPLE_MAX_DT_S;
  }
  return dt;
}

/*
 * Advance the complementary attitude filter. Gyro rates integrate roll about
 * body +x, pitch about body +y, and yaw about body +z. The accelerometer, when
 * its magnitude is close to gravity, supplies an absolute roll/pitch reference
 * that is blended in with a small gain.
 */
static void update_attitude(struct simple_navigation_estimator_process *process,
                            const synapse_topic_InertialSampleData_t *imu,
                            float dt) {
  float ax = imu->accel_flu_m_s2.x;
  float ay = imu->accel_flu_m_s2.y;
  float az = imu->accel_flu_m_s2.z;
  float accel_norm = sqrtf(ax * ax + ay * ay + az * az);

  process->roll_rad += imu->gyro_flu_rad_s.x * dt;
  process->pitch_rad += imu->gyro_flu_rad_s.y * dt;
  process->yaw_rad += imu->gyro_flu_rad_s.z * dt;

  if (accel_norm > SIMPLE_ACCEL_TRUST_LOW * SIMPLE_GRAVITY_M_S2 &&
      accel_norm < SIMPLE_ACCEL_TRUST_HIGH * SIMPLE_GRAVITY_M_S2) {
    /*
     * At rest a FLU accelerometer reads the specific force reacting gravity, so
     * the world up vector expressed in body is (ax, ay, az) normalized. With the
     * Rz(yaw)Ry(pitch)Rx(roll) body-to-world convention this gives
     *   ax = -sin(pitch) g, ay = cos(pitch) sin(roll) g, az = cos(pitch) cos(roll) g,
     * hence the roll/pitch references below.
     */
    float roll_acc = atan2f(ay, az);
    float pitch_acc = atan2f(-ax, sqrtf(ay * ay + az * az));

    process->roll_rad = (1.0f - SIMPLE_ACCEL_BLEND) * process->roll_rad +
                        SIMPLE_ACCEL_BLEND * roll_acc;
    process->pitch_rad = (1.0f - SIMPLE_ACCEL_BLEND) * process->pitch_rad +
                         SIMPLE_ACCEL_BLEND * pitch_acc;
  }

  /* Keep yaw wrapped to avoid unbounded growth in the float. */
  if (process->yaw_rad > (float)M_PI) {
    process->yaw_rad -= 2.0f * (float)M_PI;
  } else if (process->yaw_rad < -(float)M_PI) {
    process->yaw_rad += 2.0f * (float)M_PI;
  }
}

static void euler_to_quaternion(float roll, float pitch, float yaw,
                                float quaternion_wxyz[4]) {
  float cr = cosf(roll * 0.5f);
  float sr = sinf(roll * 0.5f);
  float cp = cosf(pitch * 0.5f);
  float sp = sinf(pitch * 0.5f);
  float cy = cosf(yaw * 0.5f);
  float sy = sinf(yaw * 0.5f);

  /* world-from-body quaternion for Rz(yaw) Ry(pitch) Rx(roll). */
  quaternion_wxyz[0] = cr * cp * cy + sr * sp * sy;
  quaternion_wxyz[1] = sr * cp * cy - cr * sp * sy;
  quaternion_wxyz[2] = cr * sp * cy + sr * cp * sy;
  quaternion_wxyz[3] = cr * cp * sy - sr * sp * cy;
}

static bool optical_flow_is_fresh(
    const synapse_topic_OpticalFlowVelocityData_t *flow, bool observed,
    uint64_t control_now_ns) {
  const float values[] = {
      flow->velocity_flu_m_s.x,
      flow->velocity_flu_m_s.y,
  };

  return rdd2_control_timestamp_is_fresh(observed, flow->timestamp_ns,
                                         control_now_ns,
                                         SIMPLE_OPTICAL_FLOW_TIMEOUT_NS) &&
         rdd2_control_values_are_finite(values, ARRAY_SIZE(values));
}

/*
 * Fold GNSS and optical-flow measurements into the held world ENU
 * position/velocity. GNSS supplies position and full velocity when usable.
 * When optical flow is fresh, its body FLU velocity is rotated into world ENU
 * about the current yaw and overwrites the horizontal velocity so its sign is
 * observable at the odometry twist.
 */
static void update_translation(
    struct simple_navigation_estimator_process *process,
    const struct rdd2_navigation_gps_measurement *gps, bool optical_fresh) {
  if (gps->valid && gps->position_valid) {
    process->position_enu_m[0] = gps->position_enu_m[0];
    process->position_enu_m[1] = gps->position_enu_m[1];
    process->position_enu_m[2] = gps->position_enu_m[2];
  }
  if (gps->valid && gps->velocity_valid) {
    process->velocity_enu_m_s[0] = gps->velocity_enu_m_s[0];
    process->velocity_enu_m_s[1] = gps->velocity_enu_m_s[1];
    process->velocity_enu_m_s[2] = gps->velocity_enu_m_s[2];
  }
  if (optical_fresh) {
    float vx = process->optical_flow.velocity_flu_m_s.x; /* body forward */
    float vy = process->optical_flow.velocity_flu_m_s.y; /* body left */
    float cy = cosf(process->yaw_rad);
    float sy = sinf(process->yaw_rad);

    /* Rz(yaw) maps body (forward, left) into world (east, north). */
    process->velocity_enu_m_s[0] = vx * cy - vy * sy; /* east */
    process->velocity_enu_m_s[1] = vx * sy + vy * cy; /* north */
  }
}

static void publish_estimate(struct simple_navigation_estimator_process *process,
                             bool estimate_valid) {
  uint64_t timestamp_ns = process->imu.timestamp_ns;
  uint8_t flags = estimate_valid
                      ? synapse_topic_AttitudeEstimateFlags_AttitudeValid |
                            synapse_topic_AttitudeEstimateFlags_RatesValid
                      : 0U;
  float quaternion[4] = {1.0f, 0.0f, 0.0f, 0.0f};

  if (estimate_valid) {
    euler_to_quaternion(process->roll_rad, process->pitch_rad,
                        process->yaw_rad, quaternion);
  }

  process->attitude = (synapse_topic_AttitudeEstimateData_t){
      .timestamp_ns = timestamp_ns,
      .attitude =
          {
              .w = quaternion[0],
              .x = quaternion[1],
              .y = quaternion[2],
              .z = quaternion[3],
          },
      .angular_velocity_flu_rad_s =
          {
              .roll = estimate_valid ? process->imu.gyro_flu_rad_s.x : 0.0f,
              .pitch = estimate_valid ? process->imu.gyro_flu_rad_s.y : 0.0f,
              .yaw = estimate_valid ? process->imu.gyro_flu_rad_s.z : 0.0f,
          },
      .flags = flags,
  };
  process->odometry = (synapse_topic_OdometryEstimateData_t){
      .timestamp_ns = timestamp_ns,
      .position_enu_m =
          {
              .x = estimate_valid ? process->position_enu_m[0] : 0.0f,
              .y = estimate_valid ? process->position_enu_m[1] : 0.0f,
              .z = estimate_valid ? process->position_enu_m[2] : 0.0f,
          },
      .attitude = process->attitude.attitude,
      .velocity_enu_m_s =
          {
              .x = estimate_valid ? process->velocity_enu_m_s[0] : 0.0f,
              .y = estimate_valid ? process->velocity_enu_m_s[1] : 0.0f,
              .z = estimate_valid ? process->velocity_enu_m_s[2] : 0.0f,
          },
      .angular_velocity_flu_rad_s = process->attitude.angular_velocity_flu_rad_s,
      .reset_counter = 0U,
      .estimator_type = 1U,
      .quality_pct = estimate_valid ? 100 : 0,
  };
  (void)zros_pub_update(&process->odometry_pub);
  (void)zros_pub_update(&process->attitude_pub);
}

static void simple_navigation_estimator_thread(void *arg1, void *arg2,
                                               void *arg3) {
  struct simple_navigation_estimator_process *process = arg1;

  ARG_UNUSED(arg2);
  ARG_UNUSED(arg3);

  while (true) {
    struct rdd2_navigation_gps_measurement gps_measurement;
    bool gnss_fresh;
    bool health_fresh;
    bool optical_fresh;
    bool sample_valid;
    float dt;

    if (zros_sub_wait(&process->imu_sub, K_FOREVER) != 0 ||
        zros_sub_update(&process->imu_sub) != 0) {
      continue;
    }
    if (!rdd2_release_due(&process->release_scheduler, RDD2_CONTROL_RATE_HZ,
                          RDD2_NAVIGATION_ESTIMATOR_RATE_HZ)) {
      continue;
    }

    gnss_fresh = zros_sub_update(&process->gnss_sub) == 0;
    health_fresh = zros_sub_update(&process->health_sub) == 0;
    if (zros_sub_update(&process->optical_flow_sub) == 0) {
      process->optical_flow_observed = true;
    }

    sample_valid = imu_valid(&process->imu);
    dt = integration_dt_s(process, process->imu.timestamp_ns);
    if (sample_valid) {
      update_attitude(process, &process->imu, dt);
      process->attitude_initialized = true;
    }

    (void)rdd2_navigation_gps_step(&process->gps_adapter, &gps_measurement,
                                   &process->gnss, gnss_fresh, &process->health,
                                   health_fresh, process->imu.timestamp_ns);
    atomic_set(&g_origin_valid, process->gps_adapter.origin_valid ? 1 : 0);

    optical_fresh = optical_flow_is_fresh(&process->optical_flow,
                                          process->optical_flow_observed,
                                          process->imu.timestamp_ns);
    update_translation(process, &gps_measurement, optical_fresh);

    publish_estimate(process, sample_valid && process->attitude_initialized);
  }
}

bool rdd2_navigation_origin_valid_get(void) {
  return atomic_get(&g_origin_valid) != 0;
}

int rdd2_navigation_estimator_process_start(void) {
  struct simple_navigation_estimator_process *process = &g_process;
  int rc;

  *process = (struct simple_navigation_estimator_process){0};
  rdd2_navigation_gps_init(&process->gps_adapter);
  zros_node_init(&process->node, "simple_navigation");

  rc = zros_sub_init(&process->imu_sub, &process->node, &topic_control_imu,
                     &process->imu, 0.0);
  if (rc == 0) {
    rc = zros_sub_init(&process->gnss_sub, &process->node, &topic_gnss_fix,
                       &process->gnss, 0.0);
  }
  if (rc == 0) {
    rc = zros_sub_init(&process->optical_flow_sub, &process->node,
                       &topic_optical_flow_vel, &process->optical_flow, 0.0);
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
                  simple_navigation_estimator_thread, process, NULL, NULL,
                  RDD2_NAVIGATION_PRIORITY, 0, K_NO_WAIT);
  k_thread_name_set(&g_thread, "simple_navigation");
  return 0;
}
