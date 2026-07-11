/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_NATIVE_SIL_TRANSPORT_H_
#define RDD2_NATIVE_SIL_TRANSPORT_H_

#include <stddef.h>
#include <stdint.h>

#include <synapse/control_reader.h>
#include <synapse/sensors_reader.h>
#include <synapse/state_reader.h>

#define RDD2_NATIVE_SIL_MAGIC UINT32_C(0x52444432)

/* CUBS2-style lockstep layout containing only generated v0.5 payloads. */
struct rdd2_native_sil_shared {
  uint32_t magic;
  uint32_t input_sequence;
  uint32_t response_sequence;
  uint32_t terminate;
  synapse_topic_InertialSampleData_t inertial_sample;
  synapse_topic_ManualControlData_t manual_control;
  synapse_topic_PwmSignalOutputsData_t pwm_signal_outputs;
  synapse_topic_VehicleHealthData_t vehicle_health;
  synapse_topic_AttitudeEstimateData_t attitude_estimate;
  synapse_topic_AttitudeCommandData_t attitude_command;
  synapse_topic_ControlLoopMetricsData_t control_loop_metrics;
};

_Static_assert(sizeof(struct rdd2_native_sil_shared) == 320,
               "native SIL shared layout mismatch");
_Static_assert(offsetof(struct rdd2_native_sil_shared, inertial_sample) == 16,
               "inertial sample ABI offset mismatch");
_Static_assert(offsetof(struct rdd2_native_sil_shared, manual_control) == 72,
               "manual control ABI offset mismatch");
_Static_assert(offsetof(struct rdd2_native_sil_shared, pwm_signal_outputs) ==
                   112,
               "PWM output ABI offset mismatch");
_Static_assert(offsetof(struct rdd2_native_sil_shared, vehicle_health) == 160,
               "vehicle health ABI offset mismatch");
_Static_assert(offsetof(struct rdd2_native_sil_shared, attitude_estimate) ==
                   208,
               "attitude estimate ABI offset mismatch");
_Static_assert(offsetof(struct rdd2_native_sil_shared, attitude_command) == 248,
               "attitude command ABI offset mismatch");
_Static_assert(offsetof(struct rdd2_native_sil_shared, control_loop_metrics) ==
                   296,
               "control loop metrics ABI offset mismatch");

#endif
