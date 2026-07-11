/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "topic_bus.h"

#include <csyn/csyn.h>

#include <zephyr/sys/atomic.h>

#include <zros/private/zros_topic_struct.h>

ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(rc, rdd2_rc_channels_t);

/* RDD2 owns its synapse_fbs 0.7 topic contract. CSyn resolves these compact
 * keys through the generated catalog and rejects mismatched payload sizes at
 * initialization. The lockstep transport remains direct shared memory; these
 * registrations support independent realtime Ethernet communication. */
CSYN_TOPIC_DEFINE(manual, "manual", CSYN_DIR_RX,
                  sizeof(synapse_topic_ManualControlData_t));
CSYN_TOPIC_DEFINE(imu, "imu", CSYN_DIR_RX,
                  sizeof(synapse_topic_InertialSampleData_t));
CSYN_TOPIC_DEFINE(pwm, "pwm", CSYN_DIR_TX,
                  sizeof(synapse_topic_PwmSignalOutputsData_t));
CSYN_TOPIC_DEFINE(health, "health", CSYN_DIR_TX,
                  sizeof(synapse_topic_VehicleHealthData_t));
CSYN_TOPIC_DEFINE(att, "att", CSYN_DIR_TX,
                  sizeof(synapse_topic_AttitudeEstimateData_t));
CSYN_TOPIC_DEFINE(att_sp, "att_sp", CSYN_DIR_TX,
                  sizeof(synapse_topic_AttitudeCommandData_t));
CSYN_TOPIC_DEFINE(loop, "loop", CSYN_DIR_TX,
                  sizeof(synapse_topic_ControlLoopMetricsData_t));
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(manual_control, struct csyn_manual_control);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(inertial_sample,
                                   synapse_topic_InertialSampleData_t);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(pwm_signal_outputs,
                                   synapse_topic_PwmSignalOutputsData_t);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(vehicle_health,
                                   synapse_topic_VehicleHealthData_t);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(attitude_estimate,
                                   synapse_topic_AttitudeEstimateData_t);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(attitude_command,
                                   synapse_topic_AttitudeCommandData_t);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(control_loop_metrics,
                                   synapse_topic_ControlLoopMetricsData_t);

uint32_t rdd2_topic_generation(const struct zros_topic *topic) {
  return (uint32_t)atomic_get((atomic_t *)&topic->_lockless_generation);
}

bool rdd2_topic_has_sample(const struct zros_topic *topic) {
  return rdd2_topic_generation(topic) != 0U;
}

bool rdd2_topic_copy_blob(const struct zros_topic *topic, uint8_t *buf,
                          size_t buf_size, size_t *len) {
  if (topic == NULL || buf == NULL || len == NULL) {
    return false;
  }

  if (!rdd2_topic_has_sample(topic) || (size_t)topic->_size > buf_size) {
    return false;
  }

  if (zros_topic_read((struct zros_topic *)topic, buf) != 0) {
    return false;
  }

  *len = (size_t)topic->_size;
  return true;
}

uint32_t rdd2_topic_flight_state_generation(void) {
  return rdd2_topic_generation(&topic_vehicle_health);
}

bool rdd2_topic_flight_state_copy_blob(uint8_t *buf, size_t buf_size,
                                       size_t *len) {
  rdd2_topic_flight_state_blob_t *state = (rdd2_topic_flight_state_blob_t *)buf;

  if (buf == NULL || len == NULL || buf_size < sizeof(*state) ||
      zros_topic_read(&topic_vehicle_health, &state->vehicle_health) != 0 ||
      zros_topic_read(&topic_attitude_estimate, &state->attitude_estimate) !=
          0 ||
      zros_topic_read(&topic_attitude_command, &state->attitude_command) != 0 ||
      zros_topic_read(&topic_control_loop_metrics,
                      &state->control_loop_metrics) != 0) {
    return false;
  }
  *len = sizeof(*state);
  return true;
}

uint32_t rdd2_topic_motor_output_generation(void) {
  return rdd2_topic_generation(&topic_pwm_signal_outputs);
}

bool rdd2_topic_motor_output_copy_blob(uint8_t *buf, size_t buf_size,
                                       size_t *len) {
  return rdd2_topic_copy_blob(&topic_pwm_signal_outputs, buf, buf_size, len);
}
