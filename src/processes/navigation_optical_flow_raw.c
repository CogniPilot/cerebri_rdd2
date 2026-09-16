/* SPDX-License-Identifier: Apache-2.0 */

#include "navigation_optical_flow_raw.h"

#include <math.h>
#include <string.h>

static bool config_valid(
    const struct rdd2_navigation_optical_flow_raw_config *config) {
  return config != NULL && config->max_age_ns > 0U &&
         config->min_quality > 0U && config->min_distance_m > 0.0f &&
         config->max_distance_m >= config->min_distance_m &&
         config->min_integration_time_s > 0.0f &&
         config->max_integration_time_s >= config->min_integration_time_s &&
         config->los_floor_rad >= 0.0f && config->los_sens_best_frac >= 0.0f &&
         config->los_sens_worst_frac >= config->los_sens_best_frac &&
         config->gyro_rate_noise_rad_s_rthz > 0.0f &&
         config->range_best_stddev_m >= 0.0f &&
         config->range_worst_stddev_m >= config->range_best_stddev_m &&
         (config->mount_yaw_deg == 0 || config->mount_yaw_deg == 90 ||
          config->mount_yaw_deg == 180 || config->mount_yaw_deg == 270);
}

bool rdd2_navigation_optical_flow_raw_config_valid(
    const struct rdd2_navigation_optical_flow_raw_config *config) {
  return config_valid(config);
}

/* Rotate an in-plane vector from carrier FLU into vehicle FLU by the mount yaw.
 * Rz(phi): x' = x cos - y sin, y' = x sin + y cos; only the four right-angle
 * cases are supported so the mapping stays exact. */
static void apply_mount_yaw(float x, float y, int yaw_deg, float *out_x,
                            float *out_y) {
  switch (yaw_deg) {
  case 90:
    *out_x = -y;
    *out_y = x;
    break;
  case 180:
    *out_x = -x;
    *out_y = -y;
    break;
  case 270:
    *out_x = y;
    *out_y = -x;
    break;
  default:
    *out_x = x;
    *out_y = y;
    break;
  }
}

static bool time_status_usable(
    const synapse_topic_OpticalFlowData_t *sample,
    const struct rdd2_navigation_optical_flow_raw_config *config) {
  if (!config->require_gptp) {
    return synapse_types_TimeStatus_is_known_value(sample->time_status);
  }
  return sample->time_status == synapse_types_TimeStatus_GptpSynced ||
         sample->time_status == synapse_types_TimeStatus_GptpHoldover;
}

static float normalized_quality(uint8_t quality, uint8_t min_quality) {
  float span = (float)(UINT8_MAX - min_quality);

  if (span <= 0.0f) {
    return 1.0f;
  }
  if (quality <= min_quality) {
    return 0.0f;
  }
  return (float)(quality - min_quality) / span;
}

static enum rdd2_navigation_optical_flow_raw_status sample_status(
    const synapse_topic_OpticalFlowData_t *sample,
    const struct rdd2_navigation_optical_flow_raw_config *config) {
  const float values[] = {
      sample->flow_rad.x,          sample->flow_rad.y,
      sample->delta_angle_flu_rad.x, sample->delta_angle_flu_rad.y,
      sample->delta_angle_flu_rad.z, sample->distance_m,
  };
  float integration_time_s;

  if (sample->timestamp_ns == 0U) {
    return RDD2_OPTICAL_FLOW_RAW_REJECTED_ZERO_TIMESTAMP;
  }
  if (sample->id != config->sensor_id) {
    return RDD2_OPTICAL_FLOW_RAW_REJECTED_SENSOR_ID;
  }
  if ((sample->flags & RDD2_OPTICAL_FLOW_RAW_FLOW_VALID) == 0U) {
    return RDD2_OPTICAL_FLOW_RAW_REJECTED_FLOW_INVALID;
  }
  if ((sample->flags & RDD2_OPTICAL_FLOW_RAW_DELTA_ANGLE_VALID) == 0U) {
    return RDD2_OPTICAL_FLOW_RAW_REJECTED_DELTA_ANGLE_INVALID;
  }
  if ((sample->flags & RDD2_OPTICAL_FLOW_RAW_DISTANCE_VALID) == 0U) {
    return RDD2_OPTICAL_FLOW_RAW_REJECTED_DISTANCE_INVALID;
  }
  if ((sample->flags & RDD2_OPTICAL_FLOW_RAW_DISTANCE_AMBIGUOUS) != 0U) {
    return RDD2_OPTICAL_FLOW_RAW_REJECTED_DISTANCE_AMBIGUOUS;
  }
  if (sample->quality < config->min_quality) {
    return RDD2_OPTICAL_FLOW_RAW_REJECTED_QUALITY;
  }
  if (!time_status_usable(sample, config)) {
    return RDD2_OPTICAL_FLOW_RAW_REJECTED_TIME_STATUS;
  }
  for (size_t i = 0U; i < sizeof(values) / sizeof(values[0]); ++i) {
    if (!isfinite(values[i])) {
      return RDD2_OPTICAL_FLOW_RAW_REJECTED_NONFINITE;
    }
  }
  if (sample->distance_m < config->min_distance_m ||
      sample->distance_m > config->max_distance_m) {
    return RDD2_OPTICAL_FLOW_RAW_REJECTED_DISTANCE;
  }
  integration_time_s = (float)sample->integration_timespan_ns * 1.0e-9f;
  if (integration_time_s < config->min_integration_time_s ||
      integration_time_s > config->max_integration_time_s) {
    return RDD2_OPTICAL_FLOW_RAW_REJECTED_INTEGRATION_TIME;
  }
  return RDD2_OPTICAL_FLOW_RAW_ACCEPTED;
}

static void build_measurement(
    struct rdd2_navigation_optical_flow_raw_measurement *measurement,
    const synapse_topic_OpticalFlowData_t *sample, float integration_time_s,
    const struct rdd2_navigation_optical_flow_raw_config *config) {
  float los_x;
  float los_y;
  float gyro_x;
  float gyro_y;
  float norm_q = normalized_quality(sample->quality, config->min_quality);
  float sens = config->los_sens_worst_frac -
               norm_q * (config->los_sens_worst_frac -
                         config->los_sens_best_frac);
  float gyro_var = config->gyro_rate_noise_rad_s_rthz *
                   config->gyro_rate_noise_rad_s_rthz * integration_time_s;
  float norm_dq =
      normalized_quality(sample->distance_quality, 0U);
  float range_stddev =
      config->range_worst_stddev_m -
      norm_dq * (config->range_worst_stddev_m - config->range_best_stddev_m);

  /* Line-of-sight is the flow angle rotated into vehicle FLU; the estimator's
   * additive gyro term is the negated body-angle integral so that adding it
   * subtracts the rotation the node saw. */
  apply_mount_yaw(sample->flow_rad.x, sample->flow_rad.y, config->mount_yaw_deg,
                  &los_x, &los_y);
  apply_mount_yaw(sample->delta_angle_flu_rad.x, sample->delta_angle_flu_rad.y,
                  config->mount_yaw_deg, &gyro_x, &gyro_y);

  measurement->integrated_line_of_sight_rad[0] = los_x;
  measurement->integrated_line_of_sight_rad[1] = los_y;
  measurement->integrated_gyro_body_flu_rad[0] = -gyro_x;
  measurement->integrated_gyro_body_flu_rad[1] = -gyro_y;
  measurement->integrated_gyro_body_flu_rad[2] = -sample->delta_angle_flu_rad.z;

  for (size_t row = 0U; row < 2U; ++row) {
    float angle = measurement->integrated_line_of_sight_rad[row];
    float sigma = config->los_floor_rad + sens * fabsf(angle);

    for (size_t column = 0U; column < 2U; ++column) {
      measurement->integrated_line_of_sight_cov_rad2[row][column] =
          row == column ? sigma * sigma : 0.0f;
    }
  }
  for (size_t row = 0U; row < 3U; ++row) {
    for (size_t column = 0U; column < 3U; ++column) {
      measurement->integrated_gyro_cov_rad2[row][column] =
          row == column ? gyro_var : 0.0f;
    }
  }

  measurement->integration_time_s = integration_time_s;
  measurement->ground_distance_m = sample->distance_m;
  measurement->ground_distance_variance_m2 = range_stddev * range_stddev;
  measurement->quality = (float)sample->quality / (float)UINT8_MAX;
}

void rdd2_navigation_optical_flow_raw_init(
    struct rdd2_navigation_optical_flow_raw_adapter *adapter) {
  memset(adapter, 0, sizeof(*adapter));
  adapter->status = RDD2_OPTICAL_FLOW_RAW_WAITING;
}

void rdd2_navigation_optical_flow_raw_step(
    struct rdd2_navigation_optical_flow_raw_adapter *adapter,
    struct rdd2_navigation_optical_flow_raw_measurement *measurement,
    const synapse_topic_OpticalFlowData_t *sample, bool sample_fresh,
    uint64_t control_timestamp_ns,
    const struct rdd2_navigation_optical_flow_raw_config *config) {
  bool accepted = false;

  if (measurement == NULL) {
    return;
  }
  memset(measurement, 0, sizeof(*measurement));
  if (adapter == NULL || sample == NULL || !config_valid(config)) {
    if (adapter != NULL) {
      adapter->status = RDD2_OPTICAL_FLOW_RAW_REJECTED_CONFIG;
    }
    return;
  }

  if (sample_fresh) {
    if (sample->timestamp_ns == 0U) {
      adapter->sample_valid = false;
      adapter->status = RDD2_OPTICAL_FLOW_RAW_REJECTED_ZERO_TIMESTAMP;
      adapter->rejected_count++;
    } else if (adapter->source_timestamp_observed &&
               sample->timestamp_ns <= adapter->last_source_timestamp_ns) {
      adapter->status = RDD2_OPTICAL_FLOW_RAW_REJECTED_REPLAY;
      adapter->rejected_count++;
    } else {
      adapter->last_source_timestamp_ns = sample->timestamp_ns;
      adapter->source_timestamp_observed = true;
      adapter->status = sample_status(sample, config);
      adapter->sample_valid = adapter->status == RDD2_OPTICAL_FLOW_RAW_ACCEPTED;
      if (adapter->sample_valid) {
        adapter->integration_time_s =
            (float)sample->integration_timespan_ns * 1.0e-9f;
        adapter->sample = *sample;
        adapter->last_valid_control_timestamp_ns = control_timestamp_ns;
        adapter->accepted_count++;
        accepted = true;
      } else {
        adapter->rejected_count++;
      }
    }
  }

  if (!adapter->sample_valid) {
    return;
  }
  if (control_timestamp_ns < adapter->last_valid_control_timestamp_ns) {
    adapter->status = RDD2_OPTICAL_FLOW_RAW_REJECTED_CLOCK_ROLLBACK;
    return;
  }
  if (control_timestamp_ns - adapter->last_valid_control_timestamp_ns >
      config->max_age_ns) {
    adapter->status = RDD2_OPTICAL_FLOW_RAW_EXPIRED;
    return;
  }

  if (!accepted && !sample_fresh) {
    adapter->status = RDD2_OPTICAL_FLOW_RAW_HELD;
  }

  measurement->valid = true;
  measurement->fresh = accepted;
  /* The producer timestamp proves ordering, but the gPTP and IMU clocks have
   * different epochs, so the estimator receives the control-clock time at which
   * this sample became usable. */
  measurement->timestamp_ns = control_timestamp_ns;
  build_measurement(measurement, &adapter->sample, adapter->integration_time_s,
                    config);
}

const char *rdd2_navigation_optical_flow_raw_status_name(
    enum rdd2_navigation_optical_flow_raw_status status) {
  static const char *const names[] = {
      "waiting",
      "accepted",
      "held",
      "bad-config",
      "zero-timestamp",
      "sensor-id",
      "flow-invalid",
      "delta-angle-invalid",
      "distance-invalid",
      "distance-ambiguous",
      "quality",
      "time-status",
      "nonfinite",
      "distance",
      "integration-time",
      "replay",
      "clock-rollback",
      "expired",
  };

  return (unsigned int)status < sizeof(names) / sizeof(names[0])
             ? names[status]
             : "unknown";
}
