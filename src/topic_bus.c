/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "topic_bus.h"

#include <zephyr/sys/atomic.h>

#include <zros/private/zros_topic_struct.h>

ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(rc, rdd2_rc_channels_t);

/* Lockstep owns its in-process controller topics directly. The CSyn/ZROS
 * bridge defines these topics only for realtime communication builds and is
 * never scheduled in the deterministic lockstep hot path. */
#if defined(CONFIG_RDD2_LOCKSTEP)
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
#endif

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
