/* SPDX-License-Identifier: Apache-2.0 */

#include "navigation_gps.h"

#include <math.h>
#include <string.h>

#define EARTH_RADIUS_M 6378137.0f
#define DEG_TO_RAD (3.14159265358979323846f / 180.0f)
#define GPS_MAX_FUTURE_NS UINT64_C(100000000)
#define GPS_MAX_AGE_NS UINT64_C(500000000)
#define HEALTH_MAX_AGE_NS UINT64_C(25000000)
#define GPS_MAX_HORIZONTAL_ACC_MM UINT16_C(10000)
#define GPS_MAX_VERTICAL_ACC_MM UINT16_C(15000)
#define GPS_MAX_VELOCITY_ACC_MM_S UINT16_C(5000)
#define GPS_MIN_ALTITUDE_MSL_MM (-INT32_C(1000000))
#define GPS_MAX_ALTITUDE_MSL_MM INT32_C(20000000)
#define GPS_MAX_LOCAL_COMPONENT_M 10000.0f
#define GPS_POSITION_SIGMA_FLOOR_M 0.5f
#define GPS_VELOCITY_SIGMA_FLOOR_M_S 0.1f

static bool timestamp_due(uint64_t timestamp_ns, uint64_t now_ns) {
  return timestamp_ns <= now_ns;
}

static bool timestamp_too_far_future(uint64_t timestamp_ns, uint64_t now_ns) {
  return timestamp_ns > now_ns && timestamp_ns - now_ns > GPS_MAX_FUTURE_NS;
}

static bool timestamp_stale(uint64_t timestamp_ns, uint64_t now_ns,
                            uint64_t max_age_ns) {
  return timestamp_ns <= now_ns && now_ns - timestamp_ns > max_age_ns;
}

static bool timestamp_near(uint64_t timestamp_ns, uint64_t now_ns,
                           uint64_t tolerance_ns) {
  return timestamp_ns <= now_ns ? now_ns - timestamp_ns <= tolerance_ns
                                : timestamp_ns - now_ns <= tolerance_ns;
}

static bool supported_fix_type(synapse_types_GnssFixType_enum_t fix_type) {
  return fix_type == synapse_types_GnssFixType_Fix3d ||
         fix_type == synapse_types_GnssFixType_Dgnss ||
         fix_type == synapse_types_GnssFixType_RtkFloat ||
         fix_type == synapse_types_GnssFixType_RtkFixed;
}

static bool position_usable(const synapse_topic_GnssFixData_t *fix) {
  return supported_fix_type(fix->fix_type) &&
         fix->latitude_deg_e7 >= -INT32_C(900000000) &&
         fix->latitude_deg_e7 <= INT32_C(900000000) &&
         fix->longitude_deg_e7 >= -INT32_C(1800000000) &&
         fix->longitude_deg_e7 <= INT32_C(1800000000) &&
         fix->altitude_msl_mm >= GPS_MIN_ALTITUDE_MSL_MM &&
         fix->altitude_msl_mm <= GPS_MAX_ALTITUDE_MSL_MM &&
         fix->horizontal_accuracy_mm <= GPS_MAX_HORIZONTAL_ACC_MM &&
         fix->vertical_accuracy_mm <= GPS_MAX_VERTICAL_ACC_MM;
}

static bool velocity_usable(const synapse_topic_GnssFixData_t *fix) {
  const uint8_t required = synapse_topic_GnssFixFlags_CourseValid |
                           synapse_topic_GnssFixFlags_VelocityUpValid;

  return (fix->flags & required) == required &&
         fix->course_over_ground_cdeg < UINT16_C(36000) &&
         fix->velocity_accuracy_mm_s <= GPS_MAX_VELOCITY_ACC_MM_S;
}

static bool
disarmed_health_eligible(const struct rdd2_navigation_gps_adapter *adapter,
                         uint64_t now_ns) {
  const uint8_t unusable = synapse_topic_VehicleHealthFlags_Armed |
                           synapse_topic_VehicleHealthFlags_Failsafe;

  return adapter->health_observed && adapter->health.timestamp_ns <= now_ns &&
         timestamp_near(adapter->health.timestamp_ns, now_ns,
                        HEALTH_MAX_AGE_NS) &&
         (adapter->health.flags & unusable) == 0U;
}

static float variance_with_floor(uint16_t sigma_milli, float floor) {
  float sigma = (float)sigma_milli * 1.0e-3f;

  if (sigma < floor) {
    sigma = floor;
  }
  return sigma * sigma;
}

static bool project_to_enu(int32_t origin_latitude_deg_e7,
                           int32_t origin_longitude_deg_e7,
                           int32_t origin_altitude_msl_mm,
                           const synapse_topic_GnssFixData_t *fix,
                           float enu_m[3]) {
  int64_t delta_latitude_e7 =
      (int64_t)fix->latitude_deg_e7 - origin_latitude_deg_e7;
  int64_t delta_longitude_e7 =
      (int64_t)fix->longitude_deg_e7 - origin_longitude_deg_e7;
  float lat0 = (float)origin_latitude_deg_e7 * 1.0e-7f * DEG_TO_RAD;
  float dlat = (float)delta_latitude_e7 * 1.0e-7f * DEG_TO_RAD;
  float dlon = (float)delta_longitude_e7 * 1.0e-7f * DEG_TO_RAD;
  float sin_lat0 = sinf(lat0);
  float cos_lat0 = cosf(lat0);
  float sin_dlat = sinf(dlat);
  float cos_dlat = cosf(dlat);
  float sin_dlon = sinf(dlon);
  float cos_dlon = cosf(dlon);
  float cos_lat = cos_lat0 * cos_dlat - sin_lat0 * sin_dlat;
  float sin_half_lat = sinf(dlat * 0.5f);
  float sin_half_lon = sinf(dlon * 0.5f);
  float haversine = sin_half_lat * sin_half_lat +
                    cos_lat0 * cos_lat * sin_half_lon * sin_half_lon;
  float central_angle;
  float north_m = 0.0f;
  float east_m = 0.0f;

  if (haversine < 0.0f) {
    haversine = 0.0f;
  } else if (haversine > 1.0f) {
    haversine = 1.0f;
  }
  central_angle = 2.0f * asinf(sqrtf(haversine));
  if (central_angle > 1.0e-12f) {
    float bearing_y = sin_dlon * cos_lat;
    float bearing_x =
        sin_lat0 * cos_lat0 * cos_dlat * (1.0f - cos_dlon) +
        (cos_lat0 * cos_lat0 + sin_lat0 * sin_lat0 * cos_dlon) * sin_dlat;
    float bearing = atan2f(bearing_y, bearing_x);
    float distance_m = EARTH_RADIUS_M * central_angle;

    north_m = distance_m * cosf(bearing);
    east_m = distance_m * sinf(bearing);
  }
  enu_m[0] = east_m;
  enu_m[1] = north_m;
  enu_m[2] =
      ((float)fix->altitude_msl_mm - (float)origin_altitude_msl_mm) * 1.0e-3f;
  return isfinite(enu_m[0]) && isfinite(enu_m[1]) && isfinite(enu_m[2]) &&
         fabsf(enu_m[0]) <= GPS_MAX_LOCAL_COMPONENT_M &&
         fabsf(enu_m[1]) <= GPS_MAX_LOCAL_COMPONENT_M &&
         fabsf(enu_m[2]) <= GPS_MAX_LOCAL_COMPONENT_M;
}

static void
fill_covariances(struct rdd2_navigation_gps_measurement *measurement,
                 const synapse_topic_GnssFixData_t *fix) {
  float horizontal = variance_with_floor(fix->horizontal_accuracy_mm,
                                         GPS_POSITION_SIGMA_FLOOR_M);
  float vertical = variance_with_floor(fix->vertical_accuracy_mm,
                                       GPS_POSITION_SIGMA_FLOOR_M);
  float velocity = variance_with_floor(fix->velocity_accuracy_mm_s,
                                       GPS_VELOCITY_SIGMA_FLOOR_M_S);

  measurement->position_covariance_enu_m2[0][0] = horizontal;
  measurement->position_covariance_enu_m2[1][1] = horizontal;
  measurement->position_covariance_enu_m2[2][2] = vertical;
  measurement->velocity_covariance_enu_m2_s2[0][0] = velocity;
  measurement->velocity_covariance_enu_m2_s2[1][1] = velocity;
  measurement->velocity_covariance_enu_m2_s2[2][2] = velocity;
}

static void fill_velocity(struct rdd2_navigation_gps_measurement *measurement,
                          const synapse_topic_GnssFixData_t *fix) {
  float course_rad = (float)fix->course_over_ground_cdeg * (DEG_TO_RAD * 0.01f);
  float ground_speed_m_s = (float)fix->ground_speed_cm_s * 0.01f;

  measurement->velocity_enu_m_s[0] = ground_speed_m_s * sinf(course_rad);
  measurement->velocity_enu_m_s[1] = ground_speed_m_s * cosf(course_rad);
  measurement->velocity_enu_m_s[2] = (float)fix->velocity_up_cm_s * 0.01f;
}

void rdd2_navigation_gps_init(struct rdd2_navigation_gps_adapter *adapter) {
  memset(adapter, 0, sizeof(*adapter));
}

static bool
timestamp_not_consumed(const struct rdd2_navigation_gps_adapter *adapter,
                       uint64_t timestamp_ns) {
  if (adapter->has_consumed_timestamp &&
      timestamp_ns <= adapter->last_consumed_timestamp_ns) {
    return false;
  }
  return true;
}

static bool
origin_eligible_at_receipt(const struct rdd2_navigation_gps_adapter *adapter,
                           uint64_t imu_timestamp_ns) {
  return adapter->origin_valid ||
         disarmed_health_eligible(adapter, imu_timestamp_ns);
}

static void stage_fresh_fix(struct rdd2_navigation_gps_adapter *adapter,
                            const synapse_topic_GnssFixData_t *fix,
                            uint64_t imu_timestamp_ns) {
  bool origin_eligible;

  if (!timestamp_not_consumed(adapter, fix->timestamp_ns)) {
    return;
  }
  origin_eligible = origin_eligible_at_receipt(adapter, imu_timestamp_ns);
  if (!adapter->pending) {
    adapter->pending_fix = *fix;
    adapter->pending = true;
    adapter->pending_origin_eligible = origin_eligible;
    return;
  }
  if (fix->timestamp_ns <= adapter->pending_fix.timestamp_ns) {
    return;
  }
  if (!adapter->successor_pending ||
      fix->timestamp_ns < adapter->successor_fix.timestamp_ns) {
    adapter->successor_fix = *fix;
    adapter->successor_pending = true;
    adapter->successor_origin_eligible = origin_eligible;
  }
}

static void promote_successor(struct rdd2_navigation_gps_adapter *adapter) {
  if (adapter->pending || !adapter->successor_pending) {
    return;
  }
  adapter->pending_fix = adapter->successor_fix;
  adapter->pending = true;
  adapter->pending_origin_eligible = adapter->successor_origin_eligible;
  adapter->successor_pending = false;
}

static bool consume_pending(struct rdd2_navigation_gps_adapter *adapter,
                            struct rdd2_navigation_gps_measurement *measurement,
                            uint64_t imu_timestamp_ns) {
  const synapse_topic_GnssFixData_t *pending;
  int32_t origin_latitude_deg_e7;
  int32_t origin_longitude_deg_e7;
  int32_t origin_altitude_msl_mm;
  bool origin_captured = false;

  if (!adapter->pending) {
    return false;
  }
  pending = &adapter->pending_fix;
  if (timestamp_too_far_future(pending->timestamp_ns, imu_timestamp_ns) ||
      timestamp_stale(pending->timestamp_ns, imu_timestamp_ns,
                      GPS_MAX_AGE_NS) ||
      !position_usable(pending)) {
    adapter->pending = false;
    return false;
  }
  if (!timestamp_due(pending->timestamp_ns, imu_timestamp_ns)) {
    return false;
  }
  if (!adapter->origin_valid) {
    if (!adapter->pending_origin_eligible ||
        !disarmed_health_eligible(adapter, imu_timestamp_ns)) {
      adapter->pending = false;
      return false;
    }
    origin_latitude_deg_e7 = pending->latitude_deg_e7;
    origin_longitude_deg_e7 = pending->longitude_deg_e7;
    origin_altitude_msl_mm = pending->altitude_msl_mm;
    origin_captured = true;
  } else {
    origin_latitude_deg_e7 = adapter->origin_latitude_deg_e7;
    origin_longitude_deg_e7 = adapter->origin_longitude_deg_e7;
    origin_altitude_msl_mm = adapter->origin_altitude_msl_mm;
  }
  if (!project_to_enu(origin_latitude_deg_e7, origin_longitude_deg_e7,
                      origin_altitude_msl_mm, pending,
                      measurement->position_enu_m)) {
    adapter->pending = false;
    return false;
  }
  if (origin_captured) {
    adapter->origin_latitude_deg_e7 = origin_latitude_deg_e7;
    adapter->origin_longitude_deg_e7 = origin_longitude_deg_e7;
    adapter->origin_altitude_msl_mm = origin_altitude_msl_mm;
    adapter->origin_valid = true;
  }
  measurement->valid = true;
  measurement->fresh = true;
  measurement->position_valid = true;
  measurement->velocity_valid = velocity_usable(pending);
  measurement->timestamp_ns = pending->timestamp_ns;
  measurement->geodetic_deg_m[0] = (float)pending->latitude_deg_e7 * 1.0e-7f;
  measurement->geodetic_deg_m[1] = (float)pending->longitude_deg_e7 * 1.0e-7f;
  measurement->geodetic_deg_m[2] = (float)pending->altitude_msl_mm * 1.0e-3f;
  if (measurement->velocity_valid) {
    fill_velocity(measurement, pending);
  }
  fill_covariances(measurement, pending);
  adapter->last_consumed_timestamp_ns = pending->timestamp_ns;
  adapter->has_consumed_timestamp = true;
  adapter->pending = false;
  return origin_captured;
}

bool rdd2_navigation_gps_step(
    struct rdd2_navigation_gps_adapter *adapter,
    struct rdd2_navigation_gps_measurement *measurement,
    const synapse_topic_GnssFixData_t *fix, bool fix_fresh,
    const synapse_topic_VehicleHealthData_t *health, bool health_fresh,
    uint64_t imu_timestamp_ns) {
  bool origin_captured;

  memset(measurement, 0, sizeof(*measurement));
  if (health_fresh) {
    adapter->health = *health;
    adapter->health_observed = true;
  }
  if (fix_fresh) {
    stage_fresh_fix(adapter, fix, imu_timestamp_ns);
  }

  origin_captured = consume_pending(adapter, measurement, imu_timestamp_ns);
  if (adapter->pending) {
    return false;
  }
  promote_successor(adapter);
  if (measurement->valid || !adapter->pending) {
    return origin_captured;
  }
  return consume_pending(adapter, measurement, imu_timestamp_ns);
}
