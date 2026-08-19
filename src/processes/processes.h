/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_PROCESSES_H_
#define RDD2_PROCESSES_H_

#include <stdbool.h>
#include <stdint.h>

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
int rdd2_comms_stub_process_run(void);
bool rdd2_navigation_origin_valid_get(void);
uint8_t rdd2_waypoint_mission_state_get(void);

#endif /* RDD2_PROCESSES_H_ */
