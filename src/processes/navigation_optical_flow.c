/* SPDX-License-Identifier: Apache-2.0 */

#include "navigation_optical_flow.h"

#include <math.h>
#include <string.h>

static bool
config_valid(const struct rdd2_navigation_optical_flow_config *config) {
  return config != NULL && config->max_age_ns > 0U &&
         config->min_quality > 0U && config->min_distance_m > 0.0f &&
         config->max_distance_m >= config->min_distance_m &&
         config->max_tilt_rad > 0.0f && config->max_speed_m_s > 0.0f &&
         config->best_stddev_m_s > 0.0f &&
         config->worst_stddev_m_s >= config->best_stddev_m_s;
}

static bool
time_status_usable(const synapse_topic_OpticalFlowVelocityData_t *sample,
                   const struct rdd2_navigation_optical_flow_config *config) {
  if (!config->require_gptp) {
    return synapse_types_TimeStatus_is_known_value(sample->time_status);
  }
  return sample->time_status == synapse_types_TimeStatus_GptpSynced ||
         sample->time_status == synapse_types_TimeStatus_GptpHoldover;
}

static enum rdd2_navigation_optical_flow_status
sample_status(const synapse_topic_OpticalFlowVelocityData_t *sample,
              const struct rdd2_navigation_optical_flow_config *config) {
  const uint8_t required = RDD2_OPTICAL_FLOW_VELOCITY_VALID |
                           RDD2_OPTICAL_FLOW_TILT_COMPENSATED |
                           RDD2_OPTICAL_FLOW_RANGE_TRUSTED;
  const float values[] = {
      sample->velocity_flu_m_s.x, sample->velocity_flu_m_s.y,
      sample->distance_m,         sample->roll_rad,
      sample->pitch_rad,
  };

  if (sample->timestamp_ns == 0U) {
    return RDD2_OPTICAL_FLOW_REJECTED_ZERO_TIMESTAMP;
  }
  if (sample->id != config->sensor_id) {
    return RDD2_OPTICAL_FLOW_REJECTED_SENSOR_ID;
  }
  if ((sample->flags & required) != required) {
    return RDD2_OPTICAL_FLOW_REJECTED_FLAGS;
  }
  if (sample->quality < config->min_quality) {
    return RDD2_OPTICAL_FLOW_REJECTED_QUALITY;
  }
  if (!time_status_usable(sample, config)) {
    return RDD2_OPTICAL_FLOW_REJECTED_TIME_STATUS;
  }
  for (size_t i = 0U; i < sizeof(values) / sizeof(values[0]); ++i) {
    if (!isfinite(values[i])) {
      return RDD2_OPTICAL_FLOW_REJECTED_NONFINITE;
    }
  }
  if (sample->distance_m < config->min_distance_m ||
      sample->distance_m > config->max_distance_m) {
    return RDD2_OPTICAL_FLOW_REJECTED_DISTANCE;
  }
  if (fabsf(sample->roll_rad) > config->max_tilt_rad ||
      fabsf(sample->pitch_rad) > config->max_tilt_rad) {
    return RDD2_OPTICAL_FLOW_REJECTED_TILT;
  }
  if (fabsf(sample->velocity_flu_m_s.x) > config->max_speed_m_s ||
      fabsf(sample->velocity_flu_m_s.y) > config->max_speed_m_s) {
    return RDD2_OPTICAL_FLOW_REJECTED_SPEED;
  }
  return RDD2_OPTICAL_FLOW_ACCEPTED;
}

static float
velocity_variance(uint8_t quality,
                  const struct rdd2_navigation_optical_flow_config *config) {
  float quality_span = (float)(UINT8_MAX - config->min_quality);
  float normalized_quality =
      quality_span > 0.0f
          ? (float)(quality - config->min_quality) / quality_span
          : 1.0f;
  float sigma =
      config->worst_stddev_m_s -
      normalized_quality * (config->worst_stddev_m_s - config->best_stddev_m_s);

  return sigma * sigma;
}

void rdd2_navigation_optical_flow_init(
    struct rdd2_navigation_optical_flow_adapter *adapter) {
  memset(adapter, 0, sizeof(*adapter));
  adapter->status = RDD2_OPTICAL_FLOW_WAITING;
}

void rdd2_navigation_optical_flow_step(
    struct rdd2_navigation_optical_flow_adapter *adapter,
    struct rdd2_navigation_optical_flow_measurement *measurement,
    const synapse_topic_OpticalFlowVelocityData_t *sample, bool sample_fresh,
    uint64_t control_timestamp_ns,
    const struct rdd2_navigation_optical_flow_config *config) {
  bool accepted = false;

  if (measurement == NULL) {
    return;
  }
  memset(measurement, 0, sizeof(*measurement));
  if (adapter == NULL || sample == NULL || !config_valid(config)) {
    if (adapter != NULL) {
      adapter->status = RDD2_OPTICAL_FLOW_REJECTED_CONFIG;
    }
    return;
  }

  if (sample_fresh) {
    if (sample->timestamp_ns == 0U) {
      adapter->sample_valid = false;
      adapter->status = RDD2_OPTICAL_FLOW_REJECTED_ZERO_TIMESTAMP;
      adapter->rejected_count++;
    } else if (adapter->source_timestamp_observed &&
               sample->timestamp_ns <= adapter->last_source_timestamp_ns) {
      adapter->status = RDD2_OPTICAL_FLOW_REJECTED_REPLAY;
      adapter->rejected_count++;
    } else {
      adapter->last_source_timestamp_ns = sample->timestamp_ns;
      adapter->source_timestamp_observed = true;
      adapter->status = sample_status(sample, config);
      adapter->sample_valid = adapter->status == RDD2_OPTICAL_FLOW_ACCEPTED;
      if (adapter->sample_valid) {
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
    adapter->status = RDD2_OPTICAL_FLOW_REJECTED_CLOCK_ROLLBACK;
    return;
  }
  if (control_timestamp_ns - adapter->last_valid_control_timestamp_ns >
      config->max_age_ns) {
    adapter->status = RDD2_OPTICAL_FLOW_EXPIRED;
    return;
  }

  if (!accepted && !sample_fresh) {
    adapter->status = RDD2_OPTICAL_FLOW_HELD;
  }

  measurement->valid = true;
  measurement->fresh = accepted;
  /* The source timestamp proves producer ordering, but gPTP and the IMU
   * sensor clock have different epochs. The estimator therefore receives the
   * control-clock time at which this sample became usable. */
  measurement->timestamp_s = (float)control_timestamp_ns * 1.0e-9f;
  measurement->velocity_body_flu_m_s[0] = adapter->sample.velocity_flu_m_s.x;
  measurement->velocity_body_flu_m_s[1] = adapter->sample.velocity_flu_m_s.y;
  measurement->velocity_covariance_body_m2_s2[0][0] =
      velocity_variance(adapter->sample.quality, config);
  measurement->velocity_covariance_body_m2_s2[1][1] =
      measurement->velocity_covariance_body_m2_s2[0][0];
  measurement->ground_distance_m = adapter->sample.distance_m;
  measurement->ground_distance_variance_m2 = config->range_variance_m2;
  measurement->quality = (float)adapter->sample.quality / (float)UINT8_MAX;
}

const char *rdd2_navigation_optical_flow_status_name(
    enum rdd2_navigation_optical_flow_status status) {
  static const char *const names[] = {
      "waiting",        "accepted",  "held",           "bad-config",
      "zero-timestamp", "sensor-id", "flags",          "quality",
      "time-status",    "nonfinite", "distance",       "tilt",
      "speed",          "replay",    "clock-rollback", "expired",
  };

  return (unsigned int)status < sizeof(names) / sizeof(names[0]) ? names[status]
                                                                 : "unknown";
}
