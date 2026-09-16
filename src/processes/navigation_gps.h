/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_PROCESSES_NAVIGATION_GPS_H_
#define RDD2_PROCESSES_NAVIGATION_GPS_H_

#include "interfaces/data.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * A GNSS measurement projected into the local ENU frame.
 *
 * velocity_valid is set when the fix reports a usable horizontal velocity
 * (course over ground and ground speed). The vertical component of
 * velocity_enu_m_s is populated only when the fix also reports a valid vertical
 * velocity; otherwise it is zero and its variance in
 * velocity_covariance_enu_m2_s2[2][2] is set to a large declared value
 * (unobserved vertical velocity), leaving the velocity correction effectively
 * horizontal only. The horizontal velocity variances follow the accuracy
 * floor-with-sigma rule regardless.
 */
struct rdd2_navigation_gps_measurement {
  bool valid;
  bool fresh;
  bool position_valid;
  bool velocity_valid;
  uint64_t timestamp_ns;
  float geodetic_deg_m[3];
  float position_enu_m[3];
  float velocity_enu_m_s[3];
  float position_covariance_enu_m2[3][3];
  float velocity_covariance_enu_m2_s2[3][3];
};

struct rdd2_navigation_gps_adapter {
  synapse_topic_GnssFixData_t pending_fix;
  synapse_topic_GnssFixData_t successor_fix;
  synapse_topic_VehicleHealthData_t health;
  uint64_t last_consumed_timestamp_ns;
  int32_t origin_latitude_deg_e7;
  int32_t origin_longitude_deg_e7;
  int32_t origin_altitude_msl_mm;
  bool pending;
  bool pending_origin_eligible;
  bool successor_pending;
  bool successor_origin_eligible;
  bool health_observed;
  bool has_consumed_timestamp;
  bool origin_valid;
};

void rdd2_navigation_gps_init(struct rdd2_navigation_gps_adapter *adapter);

bool rdd2_navigation_gps_step(
    struct rdd2_navigation_gps_adapter *adapter,
    struct rdd2_navigation_gps_measurement *measurement,
    const synapse_topic_GnssFixData_t *fix, bool fix_fresh,
    const synapse_topic_VehicleHealthData_t *health, bool health_fresh,
    uint64_t imu_timestamp_ns);

#endif /* RDD2_PROCESSES_NAVIGATION_GPS_H_ */
