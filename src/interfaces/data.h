/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_INTERFACES_DATA_H_
#define RDD2_INTERFACES_DATA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <synapse/control_reader.h>
#include <synapse/sensors_reader.h>
#include <synapse/state_reader.h>
#include <synapse/types_reader.h>

typedef synapse_types_Vec3f_t rdd2_vec3f_t;

#define RDD2_MAX_WAYPOINTS 16U
#define RDD2_RC_CHANNEL_COUNT 16U

/* Fixed-capacity image of Planning.Interfaces.WaypointPlanInput. */
typedef struct {
  int32_t sequence;
  int32_t waypoint_count;
  float origin_geodetic[3];
  float waypoint[RDD2_MAX_WAYPOINTS][3];
  float velocity_enu[RDD2_MAX_WAYPOINTS][3];
  float yaw[RDD2_MAX_WAYPOINTS];
  float nominal_speed;
  float min_segment_duration;
  bool valid;
  bool global_frame;
} rdd2_waypoint_plan_t;

_Static_assert(sizeof(rdd2_waypoint_plan_t) == 480U,
               "waypoint plan lockstep ABI size mismatch");
_Static_assert(offsetof(rdd2_waypoint_plan_t, waypoint) == 20U,
               "waypoint plan waypoint offset mismatch");
_Static_assert(offsetof(rdd2_waypoint_plan_t, velocity_enu) == 212U,
               "waypoint plan velocity offset mismatch");
_Static_assert(offsetof(rdd2_waypoint_plan_t, yaw) == 404U,
               "waypoint plan yaw offset mismatch");
_Static_assert(offsetof(rdd2_waypoint_plan_t, nominal_speed) == 468U,
               "waypoint plan speed offset mismatch");
_Static_assert(offsetof(rdd2_waypoint_plan_t, valid) == 476U,
               "waypoint plan valid offset mismatch");

enum rdd2_waypoint_mission_state {
  RDD2_WAYPOINT_MISSION_EMPTY = 0,
  RDD2_WAYPOINT_MISSION_PENDING = 1,
  RDD2_WAYPOINT_MISSION_RUNNING = 2,
  RDD2_WAYPOINT_MISSION_ABORTED = 3,
};

enum rdd2_lockstep_gps_mission_flags {
  RDD2_LOCKSTEP_SOURCE_READY = 1U << 0,
  RDD2_LOCKSTEP_ORIGIN_VALID = 1U << 1,
  RDD2_LOCKSTEP_PLAN_ACCEPTED = 1U << 2,
};

struct rdd2_lockstep_gps_mission_status {
  uint64_t timestamp_ns;
  int32_t plan_sequence;
  uint32_t gnss_generation;
  uint32_t plan_generation;
  uint32_t reference_generation;
  uint8_t mission_state;
  uint8_t flags;
  uint8_t reserved[6];
};

_Static_assert(sizeof(struct rdd2_lockstep_gps_mission_status) == 32U,
               "lockstep GPS mission status ABI size mismatch");
_Static_assert(offsetof(struct rdd2_lockstep_gps_mission_status,
                        plan_sequence) == 8U,
               "lockstep status plan sequence offset mismatch");
_Static_assert(offsetof(struct rdd2_lockstep_gps_mission_status,
                        reference_generation) == 20U,
               "lockstep status reference generation offset mismatch");
_Static_assert(offsetof(struct rdd2_lockstep_gps_mission_status,
                        mission_state) == 24U,
               "lockstep status mission state offset mismatch");
_Static_assert(offsetof(struct rdd2_lockstep_gps_mission_status, reserved) ==
                   26U,
               "lockstep status reserved offset mismatch");

typedef struct {
  int32_t ch[RDD2_RC_CHANNEL_COUNT];
} rdd2_rc_channels_t;

typedef struct {
  int64_t rc_stamp_ms;
  int32_t throttle_us;
  uint8_t rc_link_quality;
  uint8_t flight_mode;
  bool armed;
  bool rc_valid;
  bool rc_stale;
  bool imu_ok;
  bool arm_switch;
  bool failsafe;
} rdd2_control_status_t;

typedef struct {
  float value[4];
} rdd2_motor_values_t;

typedef struct {
  uint16_t value[4];
} rdd2_motor_raw_t;

struct rdd2_topic_flight_state {
  synapse_topic_VehicleHealthData_t vehicle_health;
  synapse_topic_AttitudeEstimateData_t attitude_estimate;
  synapse_topic_AttitudeCommandData_t attitude_command;
  synapse_topic_ControlLoopMetricsData_t control_loop_metrics;
  synapse_topic_OdometryEstimateData_t odometry_estimate;
  synapse_topic_LocalPositionCommandData_t planner_reference;
};

typedef synapse_topic_PwmSignalOutputsData_t rdd2_topic_motor_output_blob_t;
typedef struct rdd2_topic_flight_state rdd2_topic_flight_state_blob_t;

static inline int32_t *rdd2_topic_rc_channels_data(rdd2_rc_channels_t *rc) {
  return rc->ch;
}

static inline const int32_t *
rdd2_topic_rc_channels_data_const(const rdd2_rc_channels_t *rc) {
  return rc->ch;
}

static inline float *rdd2_topic_motor_values_data(rdd2_motor_values_t *motors) {
  return motors->value;
}

static inline const float *
rdd2_topic_motor_values_data_const(const rdd2_motor_values_t *motors) {
  return motors->value;
}

static inline uint16_t *rdd2_topic_motor_raw_data(rdd2_motor_raw_t *raw) {
  return raw->value;
}

static inline const uint16_t *
rdd2_topic_motor_raw_data_const(const rdd2_motor_raw_t *raw) {
  return raw->value;
}

void rdd2_topic_make_vehicle_health(synapse_topic_VehicleHealthData_t *output,
                                    const rdd2_control_status_t *status);
void rdd2_topic_make_control_loop_metrics(
    synapse_topic_ControlLoopMetricsData_t *output,
    uint32_t main_loop_latency_us);
void rdd2_topic_make_pwm_output(rdd2_topic_motor_output_blob_t *output,
                                const rdd2_motor_values_t *motors, bool armed);

#endif /* RDD2_INTERFACES_DATA_H_ */
