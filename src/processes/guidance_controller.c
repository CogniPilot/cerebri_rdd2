/* SPDX-License-Identifier: Apache-2.0 */

#include "processes.h"

#include "interfaces/zros_topics.h"
#include "scheduling.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zros/private/zros_node_struct.h>
#include <zros/private/zros_pub_struct.h>
#include <zros/private/zros_sub_struct.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>
#include <zros/zros_sub.h>

#include "Vehicles_Rdd2_GuidanceController.h"

LOG_MODULE_DECLARE(rdd2, LOG_LEVEL_INF);

#define GUIDANCE_STACK_SIZE 4096

struct guidance_controller_process {
	GuidanceControllerState efmu;
	synapse_topic_ManualControlData_t manual;
	synapse_topic_VehicleHealthData_t health;
	synapse_topic_AttitudeEstimateData_t attitude;
	synapse_topic_OdometryEstimateData_t odometry;
	synapse_topic_LocalPositionCommandData_t reference;
	synapse_topic_RateCommandData_t rate_command;
	synapse_topic_AttitudeCommandData_t attitude_command;
	struct zros_node node;
	struct zros_sub manual_sub;
	struct zros_sub health_sub;
	struct zros_sub attitude_sub;
	struct zros_sub odometry_sub;
	struct zros_sub reference_sub;
	struct zros_pub rate_command_pub;
	struct zros_pub attitude_command_pub;
};

static struct guidance_controller_process g_process;
static struct k_thread g_thread;
K_THREAD_STACK_DEFINE(g_guidance_stack, GUIDANCE_STACK_SIZE);

static void copy_manual_inputs_to_efmu(GuidanceControllerState *efmu,
				       const synapse_topic_ManualControlData_t *manual,
				       const synapse_topic_VehicleHealthData_t *health)
{
	efmu->mode = manual->flight_mode <= 2U ? manual->flight_mode : 0;
	efmu->armed = (health->flags & synapse_topic_VehicleHealthFlags_Armed) != 0U;
	efmu->stick[0] = 0.001f * (float)manual->roll_milli;
	efmu->stick[1] = 0.001f * (float)manual->pitch_milli;
	efmu->stick[2] = 0.001f * (float)manual->yaw_milli;
	efmu->throttle = 0.001f * (float)manual->throttle_milli;
}

static void copy_navigation_inputs_to_efmu(GuidanceControllerState *efmu,
					   const synapse_topic_AttitudeEstimateData_t *attitude,
					   const synapse_topic_OdometryEstimateData_t *odometry)
{
	efmu->positionWorldEnu_m[0] = odometry->position_enu_m.x;
	efmu->positionWorldEnu_m[1] = odometry->position_enu_m.y;
	efmu->positionWorldEnu_m[2] = odometry->position_enu_m.z;
	efmu->velocityWorldEnu_m_s[0] = odometry->velocity_enu_m_s.x;
	efmu->velocityWorldEnu_m_s[1] = odometry->velocity_enu_m_s.y;
	efmu->velocityWorldEnu_m_s[2] = odometry->velocity_enu_m_s.z;
	efmu->quaternionWorldBody[0] = attitude->attitude.w;
	efmu->quaternionWorldBody[1] = attitude->attitude.x;
	efmu->quaternionWorldBody[2] = attitude->attitude.y;
	efmu->quaternionWorldBody[3] = attitude->attitude.z;
}

static void copy_reference_inputs_to_efmu(GuidanceControllerState *efmu,
					  const synapse_topic_LocalPositionCommandData_t *reference)
{
	efmu->positionWorld_m[0] = reference->position_enu_m.x;
	efmu->positionWorld_m[1] = reference->position_enu_m.y;
	efmu->positionWorld_m[2] = reference->position_enu_m.z;
	efmu->velocityWorld_m_s[0] = reference->velocity_enu_m_s.x;
	efmu->velocityWorld_m_s[1] = reference->velocity_enu_m_s.y;
	efmu->velocityWorld_m_s[2] = reference->velocity_enu_m_s.z;
	efmu->accelerationWorld_m_s2[0] = reference->acceleration_or_force_enu.x;
	efmu->accelerationWorld_m_s2[1] = reference->acceleration_or_force_enu.y;
	efmu->accelerationWorld_m_s2[2] = reference->acceleration_or_force_enu.z;
	efmu->yaw_rad = reference->yaw_rad;
}

static void publish_efmu_outputs(struct guidance_controller_process *process)
{
	GuidanceControllerState *efmu = &process->efmu;
	uint64_t timestamp_us = (uint64_t)k_uptime_get() * 1000U;

	process->rate_command = (synapse_topic_RateCommandData_t){
		.timestamp_us = timestamp_us,
		.body_rate_flu_rad_s =
			{
				.roll = efmu->angularVelocityCommandFlu_rad_s[0],
				.pitch = efmu->angularVelocityCommandFlu_rad_s[1],
				.yaw = efmu->angularVelocityCommandFlu_rad_s[2],
			},
		.thrust = efmu->thrust_N,
	};
	process->attitude_command = (synapse_topic_AttitudeCommandData_t){
		.timestamp_us = timestamp_us,
		.attitude = process->attitude.attitude,
		.body_rate_flu_rad_s = process->rate_command.body_rate_flu_rad_s,
		.thrust = process->rate_command.thrust,
	};
	(void)zros_pub_update(&process->rate_command_pub);
	(void)zros_pub_update(&process->attitude_command_pub);
}

static void guidance_controller_thread(void *arg1, void *arg2, void *arg3)
{
	struct guidance_controller_process *process = arg1;

	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		if (zros_sub_wait(&process->attitude_sub, K_FOREVER) != 0) {
			continue;
		}

		(void)zros_sub_update(&process->manual_sub);
		(void)zros_sub_update(&process->health_sub);
		(void)zros_sub_update(&process->attitude_sub);
		(void)zros_sub_update(&process->odometry_sub);
		(void)zros_sub_update(&process->reference_sub);

		copy_manual_inputs_to_efmu(&process->efmu, &process->manual, &process->health);
		copy_navigation_inputs_to_efmu(&process->efmu, &process->attitude,
					       &process->odometry);
		copy_reference_inputs_to_efmu(&process->efmu, &process->reference);
		GuidanceController_dostep(&process->efmu);
		publish_efmu_outputs(process);
	}
}

int rdd2_guidance_controller_process_start(void)
{
	struct guidance_controller_process *process = &g_process;
	int rc;

	*process = (struct guidance_controller_process){0};
	process->attitude.attitude.w = 1.0f;
	GuidanceController_startup(&process->efmu);
	GuidanceController_recalibrate(&process->efmu);
	zros_node_init(&process->node, "efmu_guidance");

	rc = zros_sub_init(&process->manual_sub, &process->node, &topic_manual_input,
			   &process->manual, 0.0);
	if (rc != 0) {
		return rc;
	}
	rc = zros_sub_init(&process->health_sub, &process->node, &topic_vehicle_health,
			   &process->health, 0.0);
	if (rc != 0) {
		return rc;
	}
	rc = zros_sub_init(&process->attitude_sub, &process->node, &topic_attitude_estimate,
			   &process->attitude, RDD2_GUIDANCE_RATE_HZ);
	if (rc != 0) {
		return rc;
	}
	rc = zros_sub_init(&process->odometry_sub, &process->node, &topic_navigation_odometry,
			   &process->odometry, 0.0);
	if (rc != 0) {
		return rc;
	}
	rc = zros_sub_init(&process->reference_sub, &process->node, &topic_trajectory_reference,
			   &process->reference, 0.0);
	if (rc != 0) {
		return rc;
	}

	rc = zros_pub_init(&process->rate_command_pub, &process->node, &topic_rate_command,
			   &process->rate_command);
	if (rc == 0) {
		rc = zros_pub_init(&process->attitude_command_pub, &process->node,
				   &topic_attitude_command, &process->attitude_command);
	}
	if (rc != 0) {
		return rc;
	}

	k_thread_create(&g_thread, g_guidance_stack, K_THREAD_STACK_SIZEOF(g_guidance_stack),
			guidance_controller_thread, process, NULL, NULL, RDD2_GUIDANCE_PRIORITY, 0,
			K_NO_WAIT);
	k_thread_name_set(&g_thread, "efmu_guidance");
	return 0;
}
