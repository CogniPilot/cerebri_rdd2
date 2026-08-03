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
/* The fix lands on one 64-byte catalog contract whatever produced it, so
 * consumers never care which source filled it. The direction is the source
 * selector, which makes the two mutually exclusive by construction: injected
 * over the radio it is RX and the transport accepts it inbound; produced by
 * the onboard receiver it is TX and the transport streams it out. */
#if defined(CONFIG_RDD2_GNSS_SOURCE_ONBOARD)
#define RDD2_GNSS_TOPIC_DIR CSYN_DIR_TX
#else
#define RDD2_GNSS_TOPIC_DIR CSYN_DIR_RX
#endif
CSYN_TOPIC_DEFINE(gnss, "gnss", RDD2_GNSS_TOPIC_DIR, sizeof(synapse_topic_GnssFixData_t));
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

/* Called from the GNSS driver's own workqueue, never the control loop. */
bool rdd2_topic_gnss_publish(const synapse_topic_GnssFixData_t *fix) {
  struct csyn_topic *topic = csyn_topic_find("gnss");

  if (fix == NULL || topic == NULL) {
    return false;
  }

  return csyn_topic_publish(topic, fix, sizeof(*fix));
}

/* The CSyn store is already the latest-value store for the fix, so GNSS gets
 * no zros mirror and no thread of its own until something in the firmware
 * actually consumes it. SPEC_0005 forbids GNSS in the rate loop, so every
 * caller of this is off the hot path by construction. */
bool rdd2_topic_gnss_copy(synapse_topic_GnssFixData_t *fix, uint32_t *generation) {
  struct csyn_topic *topic = csyn_topic_find("gnss");
  size_t len = 0U;

  if (fix == NULL || topic == NULL) {
    return false;
  }

  return csyn_topic_copy(topic, fix, sizeof(*fix), &len, generation) &&
         len == sizeof(*fix);
}

uint32_t rdd2_topic_motor_output_generation(void) {
  return rdd2_topic_generation(&topic_pwm_signal_outputs);
}

bool rdd2_topic_motor_output_copy_blob(uint8_t *buf, size_t buf_size,
                                       size_t *len) {
  return rdd2_topic_copy_blob(&topic_pwm_signal_outputs, buf, buf_size, len);
}
