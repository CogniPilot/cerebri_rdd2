/* SPDX-License-Identifier: Apache-2.0 */

#include "processes.h"

#include "interfaces/zros_topics.h"
#include "scheduling.h"

#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zros/private/zros_node_struct.h>
#include <zros/private/zros_pub_struct.h>
#include <zros/private/zros_sub_struct.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>
#include <zros/zros_sub.h>

#include "Planning_Bezier_WaypointTrajectoryPlanner.h"

LOG_MODULE_DECLARE(rdd2, LOG_LEVEL_INF);

#define PLANNER_STACK_SIZE 4096

struct waypoint_trajectory_planner_process {
	WaypointTrajectoryPlannerState efmu;
	rdd2_waypoint_plan_t plan;
	synapse_topic_AttitudeEstimateData_t release_clock;
	synapse_topic_LocalPositionCommandData_t external_reference;
	synapse_topic_LocalPositionCommandData_t reference;
	struct zros_node node;
	struct zros_sub plan_sub;
	struct zros_sub release_sub;
	struct zros_sub external_reference_sub;
	struct zros_pub reference_pub;
	bool have_external_reference;
};

static struct waypoint_trajectory_planner_process g_process;
static struct k_thread g_thread;
K_THREAD_STACK_DEFINE(g_planner_stack, PLANNER_STACK_SIZE);

enum {
	RDD2_WAYPOINT_AXIS_COUNT =
		sizeof(((rdd2_waypoint_plan_t *)0)->origin_geodetic) / sizeof(float),
	EFMU_WAYPOINT_CAPACITY = sizeof(((WaypointTrajectoryPlannerState *)0)->waypoint) /
				 sizeof(((WaypointTrajectoryPlannerState *)0)->waypoint[0]),
	EFMU_VELOCITY_CAPACITY = sizeof(((WaypointTrajectoryPlannerState *)0)->plan_velocityEnu) /
				 sizeof(((WaypointTrajectoryPlannerState *)0)->plan_velocityEnu[0]),
	EFMU_YAW_CAPACITY = sizeof(((WaypointTrajectoryPlannerState *)0)->plan_yaw) / sizeof(float),
	EFMU_WAYPOINT_AXIS_COUNT =
		sizeof(((WaypointTrajectoryPlannerState *)0)->waypoint[0]) / sizeof(float),
	EFMU_VELOCITY_AXIS_COUNT =
		sizeof(((WaypointTrajectoryPlannerState *)0)->plan_velocityEnu[0]) / sizeof(float),
};

_Static_assert(RDD2_MAX_WAYPOINTS == EFMU_WAYPOINT_CAPACITY,
	       "RDD2 waypoint ingress and generated eFMU capacities differ");
_Static_assert(EFMU_WAYPOINT_CAPACITY == EFMU_VELOCITY_CAPACITY,
	       "generated waypoint and velocity capacities differ");
_Static_assert(EFMU_WAYPOINT_CAPACITY == EFMU_YAW_CAPACITY,
	       "generated waypoint and yaw capacities differ");
_Static_assert(RDD2_WAYPOINT_AXIS_COUNT == EFMU_WAYPOINT_AXIS_COUNT,
	       "RDD2 and generated waypoint axis counts differ");
_Static_assert(EFMU_WAYPOINT_AXIS_COUNT == 3U, "generated waypoint vectors must have three axes");
_Static_assert(EFMU_VELOCITY_AXIS_COUNT == 3U, "generated velocity vectors must have three axes");

static int32_t clamp_waypoint_count(int32_t waypoint_count)
{
	if (waypoint_count < 0) {
		return 0;
	}
	if (waypoint_count > (int32_t)RDD2_MAX_WAYPOINTS) {
		return (int32_t)RDD2_MAX_WAYPOINTS;
	}
	return waypoint_count;
}

static void copy_waypoint_plan_input_to_efmu(WaypointTrajectoryPlannerState *efmu,
					     const rdd2_waypoint_plan_t *plan)
{
	efmu->plan_valid = plan->valid;
	efmu->plan_sequence = plan->sequence;
	efmu->plan_waypointCount = clamp_waypoint_count(plan->waypoint_count);
	efmu->globalFrame = plan->global_frame;
	for (size_t axis = 0U; axis < RDD2_WAYPOINT_AXIS_COUNT; ++axis) {
		efmu->originGeodetic[axis] = plan->origin_geodetic[axis];
	}

	for (size_t waypoint = 0U; waypoint < RDD2_MAX_WAYPOINTS; ++waypoint) {
		for (size_t axis = 0U; axis < RDD2_WAYPOINT_AXIS_COUNT; ++axis) {
			efmu->waypoint[waypoint][axis] = plan->waypoint[waypoint][axis];
			efmu->plan_velocityEnu[waypoint][axis] = plan->velocity_enu[waypoint][axis];
		}
		efmu->plan_yaw[waypoint] = plan->yaw[waypoint];
	}
	efmu->nominalSpeed = plan->nominal_speed;
	efmu->minSegmentDuration = plan->min_segment_duration;
}

static void publish_efmu_reference(struct waypoint_trajectory_planner_process *process)
{
	WaypointTrajectoryPlannerState *efmu = &process->efmu;

	if (!efmu->reference_valid) {
		if (process->have_external_reference) {
			process->reference = process->external_reference;
			(void)zros_pub_update(&process->reference_pub);
		}
		return;
	}

	process->reference = (synapse_topic_LocalPositionCommandData_t){
		.timestamp_us = (uint64_t)k_uptime_get() * 1000U,
		.position_enu_m =
			{
				.x = efmu->position[0],
				.y = efmu->position[1],
				.z = efmu->position[2],
			},
		.velocity_enu_m_s =
			{
				.x = efmu->velocity[0],
				.y = efmu->velocity[1],
				.z = efmu->velocity[2],
			},
		.acceleration_or_force_enu =
			{
				.x = efmu->acceleration[0],
				.y = efmu->acceleration[1],
				.z = efmu->acceleration[2],
			},
		.yaw_rad = efmu->reference_yaw,
		.yaw_rate_rad_s = efmu->yawRate,
		.coordinate_frame = synapse_types_LocalFrame_LocalEnu,
	};
	(void)zros_pub_update(&process->reference_pub);
}

static void waypoint_trajectory_planner_thread(void *arg1, void *arg2, void *arg3)
{
	struct waypoint_trajectory_planner_process *process = arg1;

	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		if (zros_sub_wait(&process->release_sub, K_FOREVER) != 0 ||
		    zros_sub_update(&process->release_sub) != 0) {
			continue;
		}

		(void)zros_sub_update(&process->plan_sub);
		if (zros_sub_update(&process->external_reference_sub) == 0) {
			process->have_external_reference = true;
		}
		copy_waypoint_plan_input_to_efmu(&process->efmu, &process->plan);
		WaypointTrajectoryPlanner_dostep(&process->efmu);
		publish_efmu_reference(process);
	}
}

int rdd2_waypoint_trajectory_planner_process_start(void)
{
	struct waypoint_trajectory_planner_process *process = &g_process;
	int rc;

	*process = (struct waypoint_trajectory_planner_process){0};
	WaypointTrajectoryPlanner_startup(&process->efmu);
	process->efmu.maxWaypoints = RDD2_MAX_WAYPOINTS;
	WaypointTrajectoryPlanner_recalibrate(&process->efmu);
	zros_node_init(&process->node, "efmu_planner");

	rc = zros_sub_init(&process->plan_sub, &process->node, &topic_waypoint_plan, &process->plan,
			   0.0);
	if (rc == 0) {
		rc = zros_sub_init(&process->release_sub, &process->node, &topic_attitude_estimate,
				   &process->release_clock, RDD2_PLANNING_RATE_HZ);
	}
	if (rc == 0) {
		rc = zros_sub_init(&process->external_reference_sub, &process->node,
				   &topic_local_position_command, &process->external_reference,
				   0.0);
	}
	if (rc == 0) {
		rc = zros_pub_init(&process->reference_pub, &process->node,
				   &topic_trajectory_reference, &process->reference);
	}
	if (rc != 0) {
		return rc;
	}

	k_thread_create(&g_thread, g_planner_stack, K_THREAD_STACK_SIZEOF(g_planner_stack),
			waypoint_trajectory_planner_thread, process, NULL, NULL,
			RDD2_PLANNING_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&g_thread, "efmu_planner");
	return 0;
}
