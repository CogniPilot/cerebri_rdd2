/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "topic_bus.h"

/* Only this file bridges the two buses: it declares the CSyn side of the
 * Ethernet path and defines the zros storage its bridge mirrors into. */
#include <csyn/csyn.h>
#include <csyn/csyn_zros.h>

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
/* GNSS is internal-bus only. The serial transport carries it to and from the
 * ground directly off this topic; nothing mirrors it onto CSyn, so a fix does
 * not appear on the Ethernet/Zenoh side. */
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(gnss_fix, synapse_topic_GnssFixData_t);

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

/* SPEC_0005 forbids GNSS in the rate loop, so every caller of this is off the
 * hot path by construction. The generation is sampled after the read so it
 * describes the sample the caller actually got: the single-publisher backend
 * retries the copy under a concurrent publish, which would leave a
 * before-the-read generation describing data that was already replaced. */
bool rdd2_topic_gnss_copy(synapse_topic_GnssFixData_t *fix, uint32_t *generation) {
  if (fix == NULL || !rdd2_topic_has_sample(&topic_gnss_fix) ||
      zros_topic_read(&topic_gnss_fix, fix) != 0) {
    return false;
  }

  if (generation != NULL) {
    *generation = rdd2_topic_generation(&topic_gnss_fix);
  }
  return true;
}

uint32_t rdd2_topic_motor_output_generation(void) {
  return rdd2_topic_generation(&topic_pwm_signal_outputs);
}

bool rdd2_topic_motor_output_copy_blob(uint8_t *buf, size_t buf_size,
                                       size_t *len) {
  return rdd2_topic_copy_blob(&topic_pwm_signal_outputs, buf, buf_size, len);
}
