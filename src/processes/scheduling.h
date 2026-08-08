/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_PROCESSES_SCHEDULING_H_
#define RDD2_PROCESSES_SCHEDULING_H_

#define RDD2_CONTROL_RATE_HZ              1600U
#define RDD2_CONTROL_PERIOD_NS            625000ULL
#define RDD2_CONTROL_DT_S                 (1.0f / 1600.0f)
#define RDD2_NAVIGATION_ESTIMATOR_RATE_HZ 1000U
#define RDD2_GUIDANCE_RATE_HZ             200U
#define RDD2_PLANNING_RATE_HZ             50U

/* Lower Zephyr numbers execute first on coincident releases. */
#define RDD2_RATE_PRIORITY       2
#define RDD2_NAVIGATION_PRIORITY 3
#define RDD2_PLANNING_PRIORITY   5
#define RDD2_GUIDANCE_PRIORITY   6

#define RDD2_FLIGHT_STATE_PUBLISH_RATE_HZ 200U
#define RDD2_FLIGHT_STATE_PUBLISH_DIV     (RDD2_CONTROL_RATE_HZ / RDD2_FLIGHT_STATE_PUBLISH_RATE_HZ)
_Static_assert((RDD2_CONTROL_RATE_HZ % RDD2_FLIGHT_STATE_PUBLISH_RATE_HZ) == 0U,
	       "flight-state publish rate must divide the controller rate");
_Static_assert(CONFIG_MAIN_THREAD_PRIORITY == RDD2_RATE_PRIORITY,
	       "main thread must own the highest-priority eFMU process");

#endif /* RDD2_PROCESSES_SCHEDULING_H_ */
