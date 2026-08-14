/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_LOCKSTEP_SHARED_H_
#define RDD2_LOCKSTEP_SHARED_H_

#include <stddef.h>
#include <stdint.h>

#include "interfaces/data.h"

#include <synapse/control_reader.h>
#include <synapse/sensors_reader.h>
#include <synapse/state_reader.h>

#define RDD2_LOCKSTEP_MAGIC UINT32_C(0x52444734)

/* Vehicle-owned storage containing only generated synapse_fbs 0.9 payloads. */
struct rdd2_lockstep_shared {
  uint32_t magic;
  uint32_t input_sequence;
  uint32_t response_sequence;
  uint32_t terminate;
  synapse_topic_InertialSampleData_t inertial_sample;
  synapse_topic_ManualControlData_t manual_control;
  synapse_topic_GnssFixData_t gnss_fix;
  rdd2_waypoint_plan_t waypoint_plan;
  synapse_topic_PwmSignalOutputsData_t pwm_signal_outputs;
  synapse_topic_VehicleHealthData_t vehicle_health;
  synapse_topic_AttitudeEstimateData_t attitude_estimate;
  synapse_topic_AttitudeCommandData_t attitude_command;
  synapse_topic_ControlLoopMetricsData_t control_loop_metrics;
  synapse_topic_OdometryEstimateData_t odometry_estimate;
  synapse_topic_LocalPositionCommandData_t planner_reference;
  struct rdd2_lockstep_gps_mission_status mission_status;
};

_Static_assert(sizeof(struct rdd2_lockstep_shared) == 1192,
               "native SIL shared layout mismatch");
_Static_assert(_Alignof(struct rdd2_lockstep_shared) == 8,
               "native SIL shared alignment mismatch");
_Static_assert(offsetof(struct rdd2_lockstep_shared, inertial_sample) == 16,
               "inertial sample ABI offset mismatch");
_Static_assert(offsetof(struct rdd2_lockstep_shared, manual_control) == 56,
               "manual control ABI offset mismatch");
_Static_assert(offsetof(struct rdd2_lockstep_shared, gnss_fix) == 96,
               "GNSS fix ABI offset mismatch");
_Static_assert(offsetof(struct rdd2_lockstep_shared, waypoint_plan) == 160,
               "waypoint plan ABI offset mismatch");
_Static_assert(offsetof(struct rdd2_lockstep_shared, pwm_signal_outputs) == 640,
               "PWM output ABI offset mismatch");
_Static_assert(offsetof(struct rdd2_lockstep_shared, vehicle_health) == 688,
               "vehicle health ABI offset mismatch");
_Static_assert(offsetof(struct rdd2_lockstep_shared, attitude_estimate) == 744,
               "attitude estimate ABI offset mismatch");
_Static_assert(offsetof(struct rdd2_lockstep_shared, attitude_command) == 784,
               "attitude command ABI offset mismatch");
_Static_assert(offsetof(struct rdd2_lockstep_shared, control_loop_metrics) ==
                   832,
               "control loop metrics ABI offset mismatch");
_Static_assert(offsetof(struct rdd2_lockstep_shared, odometry_estimate) == 856,
               "odometry estimate ABI offset mismatch");
_Static_assert(offsetof(struct rdd2_lockstep_shared, planner_reference) == 1088,
               "planner reference ABI offset mismatch");
_Static_assert(offsetof(struct rdd2_lockstep_shared, mission_status) == 1144,
               "mission status ABI offset mismatch");

#endif
