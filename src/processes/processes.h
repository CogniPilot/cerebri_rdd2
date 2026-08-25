/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_PROCESSES_H_
#define RDD2_PROCESSES_H_

#include <stdbool.h>
#include <stdint.h>

struct rdd2_navigation_optical_flow_diagnostics;

/*
 * Zephyr deployment boundary for the generated eFMUs.
 *
 * NavigationEstimator, WaypointTrajectoryPlanner, and GuidanceController
 * create their own worker threads. RateControlAllocator owns the calling
 * (main) thread so the IMU-to-motor path has no scheduler handoff.
 */
int rdd2_navigation_estimator_process_start(void);
int rdd2_waypoint_trajectory_planner_process_start(void);
int rdd2_guidance_controller_process_start(void);
int rdd2_rate_control_allocator_process_run(void);
bool rdd2_navigation_origin_valid_get(void);
/* Latest gyroscope bias estimate, for the IMU-rate bias correction the rate
 * loop applies to the raw gyro sample. Returns false until the estimator has
 * published a usable estimate, in which case the caller must use the
 * uncorrected sample rather than a stale bias. */
bool rdd2_navigation_gyroscope_bias_get(float bias_body_flu_rad_s[3]);
bool rdd2_navigation_optical_flow_diagnostics_get(
    struct rdd2_navigation_optical_flow_diagnostics *diagnostics);
uint8_t rdd2_waypoint_mission_state_get(void);

/* Navigation keeps finite attitude/rate output usable during estimator
 * recovery, while quality 1 explicitly withdraws POSITION capability. */
#define RDD2_NAVIGATION_POSITION_QUALITY_MIN_PCT INT8_C(2)

/* Keep a previously verified finite IMU payload for at most this long. The
 * deadline is wall clock, so it is converted into releases from the estimator
 * rate rather than written as a release count: the count that meant 20 ms at
 * 1 kHz would mean 200 ms at the 100 Hz release rate the preintegrated
 * estimator runs at, silently extending the deadline by ten. */
#define RDD2_NAVIGATION_IMU_HOLD_DEADLINE_MS UINT16_C(20)
#define RDD2_NAVIGATION_IMU_HOLD_MAX_RELEASES UINT16_C(2)

static inline bool
rdd2_navigation_position_quality_is_usable(int8_t quality_pct) {
  return quality_pct >= RDD2_NAVIGATION_POSITION_QUALITY_MIN_PCT;
}

static inline bool
rdd2_navigation_imu_hold_is_usable(bool usable_payload_observed,
                                   uint16_t consecutive_held_releases) {
  return usable_payload_observed && consecutive_held_releases > 0U &&
         consecutive_held_releases <= RDD2_NAVIGATION_IMU_HOLD_MAX_RELEASES;
}

#endif /* RDD2_PROCESSES_H_ */
