/* SPDX-License-Identifier: Apache-2.0 */

#include "interfaces/zros_topics.h"
#include "processes/processes.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(rdd2, LOG_LEVEL_INF);

int main(void)
{
	rdd2_topic_shell_formatters_init();

#if defined(CONFIG_RDD2_COMMS_STUB)
	LOG_INF("PASSIVE COMMS ENDPOINT: FLIGHT CONTROL AND OUTPUTS ARE NOT BUILT");
	return rdd2_comms_stub_process_run();
#else
	int rc;

	/* Start consumers before the IMU-paced rate process begins publishing. */
	rc = rdd2_navigation_estimator_process_start();
	if (rc != 0) {
		LOG_ERR("NavigationEstimator process failed to start: %d", rc);
		return rc;
	}

	rc = rdd2_waypoint_trajectory_planner_process_start();
	if (rc != 0) {
		LOG_ERR("WaypointTrajectoryPlanner process failed to start: %d", rc);
		return rc;
	}

	rc = rdd2_guidance_controller_process_start();
	if (rc != 0) {
		LOG_ERR("GuidanceController process failed to start: %d", rc);
		return rc;
	}
	LOG_INF("RDD2 eFMU deployment starting");
	return rdd2_rate_control_allocator_process_run();
#endif
}
