/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_PROCESSES_NAVIGATION_OPTICAL_FLOW_H_
#define RDD2_PROCESSES_NAVIGATION_OPTICAL_FLOW_H_

#include "interfaces/data.h"

#include <stdbool.h>
#include <stdint.h>

enum rdd2_optical_flow_velocity_flags {
  RDD2_OPTICAL_FLOW_VELOCITY_VALID = 1U << 0,
  RDD2_OPTICAL_FLOW_TILT_COMPENSATED = 1U << 1,
  RDD2_OPTICAL_FLOW_RANGE_TRUSTED = 1U << 2,
};

struct rdd2_navigation_optical_flow_config {
  uint64_t max_age_ns;
  float min_distance_m;
  float max_distance_m;
  float max_tilt_rad;
  float max_speed_m_s;
  float best_stddev_m_s;
  float worst_stddev_m_s;
  uint8_t sensor_id;
  uint8_t min_quality;
  bool require_gptp;
};

struct rdd2_navigation_optical_flow_measurement {
  bool valid;
  bool fresh;
  float timestamp_s;
  float velocity_body_flu_m_s[2];
  float velocity_covariance_body_m2_s2[2][2];
  float integrated_line_of_sight_rad[2];
  float integration_time_s;
  float ground_distance_m;
  float quality;
};

struct rdd2_navigation_optical_flow_adapter {
  synapse_topic_OpticalFlowVelocityData_t sample;
  uint64_t last_source_timestamp_ns;
  uint64_t last_valid_control_timestamp_ns;
  bool source_timestamp_observed;
  bool sample_valid;
};

void rdd2_navigation_optical_flow_init(
    struct rdd2_navigation_optical_flow_adapter *adapter);

void rdd2_navigation_optical_flow_step(
    struct rdd2_navigation_optical_flow_adapter *adapter,
    struct rdd2_navigation_optical_flow_measurement *measurement,
    const synapse_topic_OpticalFlowVelocityData_t *sample, bool sample_fresh,
    uint64_t control_timestamp_ns,
    const struct rdd2_navigation_optical_flow_config *config);

#endif /* RDD2_PROCESSES_NAVIGATION_OPTICAL_FLOW_H_ */
