/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_PROCESSES_NAVIGATION_OPTICAL_FLOW_RAW_H_
#define RDD2_PROCESSES_NAVIGATION_OPTICAL_FLOW_RAW_H_

#include "interfaces/data.h"

#include <synapse/optical_flow_reader.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * Tightly coupled optical-flow adapter.
 *
 * This adapter consumes the flow node's raw product (synapse OpticalFlowData:
 * integrated flow angles, the node's integrated delta angle over the same
 * window, integration time, range, quality) and builds the estimator's
 * OpticalFlowSample. The estimator then forms the body velocity itself from the
 * gyro-compensated line-of-sight integral, which lets it weight the flow, the
 * rotation compensation and the range independently. The velocity adapter in
 * navigation_optical_flow.{c,h} is kept for logs that carry only the velocity
 * topic and for comparison.
 *
 * Sign convention (derived from the node's stated convention and the ESKF
 * model Estimation/StrapdownINS/ESKF/correctOpticalFlow.mo,
 * Vehicles/Rdd2/simulateOpticalFlowPlane.mo):
 *
 *   The node publishes flow_rad as the integrated angular image flow in body
 *   FLU, expressed in the SAME rotational sense as the body rotation: a pure
 *   body rotation about +x by angle a appears as flow_rad.x ~= +a. That is why
 *   the node computes its rotation-compensated translation flow as
 *   tau = flow_rad - delta_angle_flu, and its body velocity as
 *     v_forward = range * tau_y / dt,  v_left = -range * tau_x / dt.
 *   delta_angle_flu_rad is the genuine +integral of the body FLU rate.
 *
 *   The ESKF forms
 *     compensatedFlow = integratedLineOfSight_rad + integratedGyroscopeBodyFlu_rad[1:2],
 *     v_forward = range * compensatedFlow_y / dt,
 *     v_left    = -range * compensatedFlow_x / dt.
 *   To make the estimator reproduce the node's compensation, the adapter maps
 *   (after applying the mount-yaw rotation R from carrier FLU to vehicle FLU to
 *   both the flow angles and the delta angle):
 *     integratedLineOfSight_rad     =  R * flow_rad
 *     integratedGyroscopeBodyFlu_rad = -(R * delta_angle_flu)   (all three axes)
 *   so compensatedFlow = R * (flow_rad - delta_angle_flu) = R * tau.
 *
 *   The gyro term is the negated body-angle integral because the node reports
 *   flow already sign-aligned with the body rotation while the estimator ADDS
 *   the gyro to remove it. Keeping line-of-sight = flow_rad and gyro =
 *   -delta_angle also keeps the covariance decomposition exact: the line-of-
 *   sight covariance carries only the flow measurement noise and the gyro
 *   covariance carries only the gyro noise, so their sum is the true variance
 *   of the compensated flow.
 *
 *   Invariants pinned by the unit tests: for a pure-translation window
 *   (delta = 0) the compensated flow equals the flow and forward motion gives
 *   line-of-sight y > 0; for a pure-rotation window (tau = 0, so
 *   flow_rad = delta_angle) the compensated flow is zero.
 */

/* OpticalFlowData.flags bit layout, mirroring the synapse schema comment:
 * bit0 FlowValid, bit1 DeltaAngleValid, bit2 DistanceValid, bit3
 * DistanceAmbiguous. */
enum rdd2_optical_flow_raw_flags {
  RDD2_OPTICAL_FLOW_RAW_FLOW_VALID = 1U << 0,
  RDD2_OPTICAL_FLOW_RAW_DELTA_ANGLE_VALID = 1U << 1,
  RDD2_OPTICAL_FLOW_RAW_DISTANCE_VALID = 1U << 2,
  RDD2_OPTICAL_FLOW_RAW_DISTANCE_AMBIGUOUS = 1U << 3,
};

enum rdd2_navigation_optical_flow_raw_status {
  RDD2_OPTICAL_FLOW_RAW_WAITING = 0,
  RDD2_OPTICAL_FLOW_RAW_ACCEPTED,
  RDD2_OPTICAL_FLOW_RAW_HELD,
  RDD2_OPTICAL_FLOW_RAW_REJECTED_CONFIG,
  RDD2_OPTICAL_FLOW_RAW_REJECTED_ZERO_TIMESTAMP,
  RDD2_OPTICAL_FLOW_RAW_REJECTED_SENSOR_ID,
  RDD2_OPTICAL_FLOW_RAW_REJECTED_FLOW_INVALID,
  RDD2_OPTICAL_FLOW_RAW_REJECTED_DELTA_ANGLE_INVALID,
  RDD2_OPTICAL_FLOW_RAW_REJECTED_DISTANCE_INVALID,
  RDD2_OPTICAL_FLOW_RAW_REJECTED_DISTANCE_AMBIGUOUS,
  RDD2_OPTICAL_FLOW_RAW_REJECTED_QUALITY,
  RDD2_OPTICAL_FLOW_RAW_REJECTED_TIME_STATUS,
  RDD2_OPTICAL_FLOW_RAW_REJECTED_NONFINITE,
  RDD2_OPTICAL_FLOW_RAW_REJECTED_DISTANCE,
  RDD2_OPTICAL_FLOW_RAW_REJECTED_INTEGRATION_TIME,
  RDD2_OPTICAL_FLOW_RAW_REJECTED_REPLAY,
  RDD2_OPTICAL_FLOW_RAW_REJECTED_CLOCK_ROLLBACK,
  RDD2_OPTICAL_FLOW_RAW_EXPIRED,
};

struct rdd2_navigation_optical_flow_raw_config {
  uint64_t max_age_ns;
  float min_distance_m;
  float max_distance_m;
  float min_integration_time_s;
  float max_integration_time_s;
  /* Line-of-sight measurement noise. sigma = floor + fraction * |angle|, where
   * the sensitivity fraction runs from best_frac at quality 255 to worst_frac
   * at min_quality. The floor keeps a very small flow angle from claiming an
   * unrealistically tight variance. */
  float los_floor_rad;
  float los_sens_best_frac;
  float los_sens_worst_frac;
  /* Integrated-gyro noise: the ICM45686 rate-noise density integrated over the
   * exposure window as an angle random walk, so variance = density^2 * window. */
  float gyro_rate_noise_rad_s_rthz;
  /* Range noise. sigma = best_stddev at distance quality 255, worst_stddev at
   * distance quality 0. */
  float range_best_stddev_m;
  float range_worst_stddev_m;
  /* Mount yaw from vehicle FLU to carrier FLU, one of 0, 90, 180, 270 degrees,
   * applied to both the flow angles and the delta angle. */
  int mount_yaw_deg;
  uint8_t sensor_id;
  uint8_t min_quality;
  bool require_gptp;
};

struct rdd2_navigation_optical_flow_raw_measurement {
  bool valid;
  bool fresh;
  uint64_t timestamp_ns;
  float integrated_line_of_sight_rad[2];
  float integrated_line_of_sight_cov_rad2[2][2];
  float integrated_gyro_body_flu_rad[3];
  float integrated_gyro_cov_rad2[3][3];
  float integration_time_s;
  float ground_distance_m;
  float ground_distance_variance_m2;
  float quality;
};

struct rdd2_navigation_optical_flow_raw_adapter {
  synapse_topic_OpticalFlowData_t sample;
  uint64_t last_source_timestamp_ns;
  uint64_t last_valid_control_timestamp_ns;
  float integration_time_s;
  uint32_t accepted_count;
  uint32_t rejected_count;
  enum rdd2_navigation_optical_flow_raw_status status;
  bool source_timestamp_observed;
  bool sample_valid;
};

struct rdd2_navigation_optical_flow_raw_diagnostics {
  uint64_t control_timestamp_ns;
  uint64_t source_timestamp_ns;
  uint64_t accepted_age_ns;
  uint64_t last_fusion_control_timestamp_ns;
  uint32_t source_generation;
  uint32_t accepted_count;
  uint32_t rejected_count;
  uint32_t fusion_accepted_count;
  int32_t consecutive_estimator_rejections;
  float line_of_sight_rad[2];
  float gyro_body_flu_rad[2];
  float integration_time_s;
  float ground_distance_m;
  float quality;
  uint8_t flags;
  uint8_t time_status;
  int32_t correction_outcome;
  int32_t correction_source;
  int32_t recovery_stage;
  enum rdd2_navigation_optical_flow_raw_status adapter_status;
  bool measurement_valid;
  bool measurement_fresh;
  bool correction_accepted;
};

void rdd2_navigation_optical_flow_raw_init(
    struct rdd2_navigation_optical_flow_raw_adapter *adapter);

/* Validate and hold the latest raw OpticalFlowData sample and emit the
 * measurement the estimator adapter consumes. The integration time comes
 * directly from the sample's integration_timespan_ns (no interval
 * reconstruction is needed, unlike the velocity adapter). A window is rejected
 * when FlowValid, DeltaAngleValid or DistanceValid is clear, when the distance
 * is flagged ambiguous, when the integration time is outside
 * [min_integration_time_s, max_integration_time_s], or on the usual timestamp,
 * quality, time-status, distance and finiteness faults. Held (non-fresh)
 * measurements reuse the last accepted sample. */
void rdd2_navigation_optical_flow_raw_step(
    struct rdd2_navigation_optical_flow_raw_adapter *adapter,
    struct rdd2_navigation_optical_flow_raw_measurement *measurement,
    const synapse_topic_OpticalFlowData_t *sample, bool sample_fresh,
    uint64_t control_timestamp_ns,
    const struct rdd2_navigation_optical_flow_raw_config *config);

bool rdd2_navigation_optical_flow_raw_config_valid(
    const struct rdd2_navigation_optical_flow_raw_config *config);

const char *rdd2_navigation_optical_flow_raw_status_name(
    enum rdd2_navigation_optical_flow_raw_status status);

#endif /* RDD2_PROCESSES_NAVIGATION_OPTICAL_FLOW_RAW_H_ */
