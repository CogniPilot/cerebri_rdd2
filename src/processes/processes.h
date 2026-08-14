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
bool rdd2_navigation_optical_flow_diagnostics_get(
    struct rdd2_navigation_optical_flow_diagnostics *diagnostics);
uint8_t rdd2_waypoint_mission_state_get(void);

/* Navigation keeps finite attitude/rate output usable during estimator
 * recovery, while quality 1 explicitly withdraws POSITION capability. */
#define RDD2_NAVIGATION_POSITION_QUALITY_MIN_PCT INT8_C(2)

/* Keep a previously verified finite IMU payload through at most 20
 * consecutive Navigation releases (nominally 20 ms at 1 kHz). */
#define RDD2_NAVIGATION_IMU_HOLD_MAX_RELEASES UINT16_C(20)

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
