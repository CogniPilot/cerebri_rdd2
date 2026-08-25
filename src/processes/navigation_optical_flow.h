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

enum rdd2_navigation_optical_flow_status {
  RDD2_OPTICAL_FLOW_WAITING = 0,
  RDD2_OPTICAL_FLOW_ACCEPTED,
  RDD2_OPTICAL_FLOW_HELD,
  RDD2_OPTICAL_FLOW_REJECTED_CONFIG,
  RDD2_OPTICAL_FLOW_REJECTED_ZERO_TIMESTAMP,
  RDD2_OPTICAL_FLOW_REJECTED_SENSOR_ID,
  RDD2_OPTICAL_FLOW_REJECTED_FLAGS,
  RDD2_OPTICAL_FLOW_REJECTED_QUALITY,
  RDD2_OPTICAL_FLOW_REJECTED_TIME_STATUS,
  RDD2_OPTICAL_FLOW_REJECTED_NONFINITE,
  RDD2_OPTICAL_FLOW_REJECTED_DISTANCE,
  RDD2_OPTICAL_FLOW_REJECTED_TILT,
  RDD2_OPTICAL_FLOW_REJECTED_SPEED,
  RDD2_OPTICAL_FLOW_REJECTED_REPLAY,
  RDD2_OPTICAL_FLOW_REJECTED_CLOCK_ROLLBACK,
  RDD2_OPTICAL_FLOW_EXPIRED,
};

struct rdd2_navigation_optical_flow_config {
  uint64_t max_age_ns;
  float min_distance_m;
  float max_distance_m;
  float max_tilt_rad;
  float max_speed_m_s;
  float best_stddev_m_s;
  float worst_stddev_m_s;
  float range_variance_m2;
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
  float ground_distance_variance_m2;
  float quality;
};

struct rdd2_navigation_optical_flow_adapter {
  synapse_topic_OpticalFlowVelocityData_t sample;
  uint64_t last_source_timestamp_ns;
  uint64_t last_valid_control_timestamp_ns;
  uint32_t accepted_count;
  uint32_t rejected_count;
  enum rdd2_navigation_optical_flow_status status;
  bool source_timestamp_observed;
  bool sample_valid;
};

struct rdd2_navigation_optical_flow_diagnostics {
  uint64_t control_timestamp_ns;
  uint64_t source_timestamp_ns;
  uint64_t accepted_age_ns;
  uint64_t last_fusion_control_timestamp_ns;
  uint32_t source_generation;
  uint32_t accepted_count;
  uint32_t rejected_count;
  uint32_t fusion_accepted_count;
  int32_t consecutive_estimator_rejections;
  float velocity_body_flu_m_s[2];
  float ground_distance_m;
  uint8_t quality;
  uint8_t flags;
  uint8_t time_status;
  int32_t correction_outcome;
  int32_t correction_source;
  int32_t recovery_stage;
  enum rdd2_navigation_optical_flow_status adapter_status;
  bool measurement_valid;
  bool measurement_fresh;
  bool correction_accepted;
};

void rdd2_navigation_optical_flow_init(
    struct rdd2_navigation_optical_flow_adapter *adapter);

void rdd2_navigation_optical_flow_step(
    struct rdd2_navigation_optical_flow_adapter *adapter,
    struct rdd2_navigation_optical_flow_measurement *measurement,
    const synapse_topic_OpticalFlowVelocityData_t *sample, bool sample_fresh,
    uint64_t control_timestamp_ns,
    const struct rdd2_navigation_optical_flow_config *config);

const char *rdd2_navigation_optical_flow_status_name(
    enum rdd2_navigation_optical_flow_status status);

#endif /* RDD2_PROCESSES_NAVIGATION_OPTICAL_FLOW_H_ */
