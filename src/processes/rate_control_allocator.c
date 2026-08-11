/* SPDX-License-Identifier: Apache-2.0 */

#include "processes.h"

#include "hotpath_memory.h"
#include "interfaces/drivers.h"
#include "interfaces/zros_topics.h"
#include "scheduling.h"

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zros/private/zros_node_struct.h>
#include <zros/private/zros_pub_struct.h>
#include <zros/private/zros_sub_struct.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>
#include <zros/zros_sub.h>

#include "Vehicles_Rdd2_RateControlAllocator.h"

LOG_MODULE_DECLARE(rdd2, LOG_LEVEL_INF);

#define RC_STALE_TIMEOUT_MS     100
#define THROTTLE_ARM_MAX_US     1050
#define ARM_SWITCH_THRESHOLD_US 1500
#define THROTTLE_CHANNEL_INDEX  2U
#define ARM_CHANNEL_INDEX       4U

struct rate_control_allocator_process {
	RateControlAllocatorState efmu;
	rdd2_vec3f_t gyro;
	rdd2_vec3f_t accel;
	rdd2_rc_channels_t rc;
	rdd2_control_status_t status;
	rdd2_motor_values_t motors;
	rdd2_motor_raw_t raw_test;
	synapse_topic_RateCommandData_t rate_command;
	synapse_topic_AttitudeEstimateData_t navigation;
	synapse_topic_InertialSampleData_t imu_message;
	synapse_topic_VehicleHealthData_t health_message;
	synapse_topic_ControlLoopMetricsData_t metrics_message;
	struct zros_node node;
	struct zros_sub rate_command_sub;
	struct zros_sub navigation_sub;
	struct zros_pub imu_pub;
	struct zros_pub health_pub;
	struct zros_pub metrics_pub;
	float dt;
	uint32_t imu_to_motor_latency_us;
};

static RDD2_HOTPATH_DTCM_BSS struct rate_control_allocator_process g_process;

enum {
	RDD2_MOTOR_COUNT = sizeof(((rdd2_motor_values_t *)0)->value) / sizeof(float),
	EFMU_MOTOR_COUNT = sizeof(((RateControlAllocatorState *)0)->motor) / sizeof(float),
};

_Static_assert(RDD2_MOTOR_COUNT == EFMU_MOTOR_COUNT,
	       "motor driver and generated allocator capacities differ");

static bool loop_divider_expired(uint32_t *countdown, uint32_t divisor)
{
	if (*countdown > 0U) {
		(*countdown)--;
		return false;
	}

	*countdown = divisor - 1U;
	return true;
}

static uint64_t sample_timestamp_ns(uint64_t interrupt_timestamp_ns)
{
	if (interrupt_timestamp_ns != 0U) {
		return interrupt_timestamp_ns;
	}
	return (uint64_t)k_uptime_get() * 1000000ULL;
}

static void publish_imu(struct rate_control_allocator_process *process,
			uint64_t interrupt_timestamp_ns)
{
	process->imu_message = (synapse_topic_InertialSampleData_t){
		.timestamp_ns = sample_timestamp_ns(interrupt_timestamp_ns),
		.accel_flu_m_s2 = process->accel,
		.gyro_flu_rad_s = process->gyro,
		.flags = process->status.imu_ok ? synapse_topic_InertialFieldFlags_Accel |
							  synapse_topic_InertialFieldFlags_Gyro
						: 0U,
	};
	(void)zros_pub_update(&process->imu_pub);
}

static void update_arming_state(struct rate_control_allocator_process *process)
{
	const int32_t *channels = rdd2_topic_rc_channels_data_const(&process->rc);
	bool stale;

#if defined(CONFIG_RDD2_LOCKSTEP)
	stale = !process->status.rc_valid;
#else
	stale = !process->status.rc_valid ||
		((k_uptime_get() - process->status.rc_stamp_ms) > RC_STALE_TIMEOUT_MS);
#endif

	process->status.flight_mode = rdd2_rc_flight_mode(&process->rc);
	process->status.arm_switch = channels[ARM_CHANNEL_INDEX] >= ARM_SWITCH_THRESHOLD_US;
	process->status.throttle_us = channels[THROTTLE_CHANNEL_INDEX];
	process->status.rc_stale = stale;

	if (!process->status.imu_ok || stale || !process->status.arm_switch) {
		process->status.armed = false;
	} else if (!process->status.armed && process->status.throttle_us <= THROTTLE_ARM_MAX_US) {
		process->status.armed = true;
	}
}

static void copy_topic_inputs_to_efmu(struct rate_control_allocator_process *process)
{
	RateControlAllocatorState *efmu = &process->efmu;

	efmu->armed = process->status.armed;
	efmu->thrust_N = process->rate_command.thrust;
	efmu->angularVelocityCommandFlu_rad_s[0] = process->rate_command.body_rate_flu_rad_s.roll;
	efmu->angularVelocityCommandFlu_rad_s[1] = process->rate_command.body_rate_flu_rad_s.pitch;
	efmu->angularVelocityCommandFlu_rad_s[2] = process->rate_command.body_rate_flu_rad_s.yaw;
	efmu->angularVelocityMeasuredFlu_rad_s[0] =
		process->navigation.angular_velocity_flu_rad_s.roll;
	efmu->angularVelocityMeasuredFlu_rad_s[1] =
		process->navigation.angular_velocity_flu_rad_s.pitch;
	efmu->angularVelocityMeasuredFlu_rad_s[2] =
		process->navigation.angular_velocity_flu_rad_s.yaw;
}

static void copy_efmu_outputs_to_motor_buffer(struct rate_control_allocator_process *process)
{
	for (size_t motor = 0U; motor < RDD2_MOTOR_COUNT; ++motor) {
		process->motors.value[motor] = process->efmu.motor[motor];
	}
}

static void step_efmu(struct rate_control_allocator_process *process)
{
	(void)zros_sub_update(&process->rate_command_sub);
	(void)zros_sub_update(&process->navigation_sub);
	copy_topic_inputs_to_efmu(process);

	RateControlAllocator_dostep(&process->efmu);
	copy_efmu_outputs_to_motor_buffer(process);
}

static uint32_t imu_to_motor_latency_us(uint64_t imu_timestamp_ns, uint64_t motor_timestamp_ns)
{
	uint64_t latency_ns;

#if defined(CONFIG_RDD2_LOCKSTEP)
	ARG_UNUSED(imu_timestamp_ns);
	ARG_UNUSED(motor_timestamp_ns);
	return 0U;
#endif

	if (imu_timestamp_ns == 0U || motor_timestamp_ns == 0U ||
	    motor_timestamp_ns <= imu_timestamp_ns) {
		return 0U;
	}

	latency_ns = motor_timestamp_ns - imu_timestamp_ns;
	if (latency_ns >= ((uint64_t)UINT32_MAX * 1000U)) {
		return UINT32_MAX;
	}
	return (uint32_t)(latency_ns / 1000U);
}

static void publish_status(struct rate_control_allocator_process *process)
{
	static uint32_t publish_countdown;

#if defined(CONFIG_RDD2_LOCKSTEP)
	if (!rdd2_imu_stream_lockstep_at_target()) {
		return;
	}
#endif
	if (!loop_divider_expired(&publish_countdown, RDD2_FLIGHT_STATE_PUBLISH_DIV)) {
		return;
	}

	rdd2_topic_make_vehicle_health(&process->health_message, &process->status);
	rdd2_topic_make_control_loop_metrics(&process->metrics_message,
					     process->imu_to_motor_latency_us);
	(void)zros_pub_update(&process->health_pub);
	(void)zros_pub_update(&process->metrics_pub);
}

static int process_zros_init(struct rate_control_allocator_process *process)
{
	int rc;

	zros_node_init(&process->node, "efmu_rate");
	rc = zros_sub_init(&process->rate_command_sub, &process->node, &topic_rate_command,
			   &process->rate_command, 0.0);
	if (rc == 0) {
		rc = zros_sub_init(&process->navigation_sub, &process->node,
				   &topic_attitude_estimate, &process->navigation, 0.0);
	}
	if (rc == 0) {
		rc = zros_pub_init(&process->imu_pub, &process->node, &topic_control_imu,
				   &process->imu_message);
	}
	if (rc == 0) {
		rc = zros_pub_init(&process->health_pub, &process->node, &topic_vehicle_health,
				   &process->health_message);
	}
	if (rc == 0) {
		rc = zros_pub_init(&process->metrics_pub, &process->node,
				   &topic_control_loop_metrics, &process->metrics_message);
	}
	return rc;
}

static int process_drivers_init(void)
{
	int rc;

	rc = rdd2_rc_input_init();
	if (rc == 0) {
		rc = rdd2_motor_output_init();
	}
	if (rc == 0) {
		rc = rdd2_imu_stream_init();
	}
	return rc;
}

int rdd2_rate_control_allocator_process_run(void)
{
	struct rate_control_allocator_process *process = &g_process;
	int rc;

	*process = (struct rate_control_allocator_process){0};
	RateControlAllocator_startup(&process->efmu);
	RateControlAllocator_recalibrate(&process->efmu);

	rc = process_zros_init(process);
	if (rc != 0) {
		return rc;
	}
	rc = process_drivers_init();
	if (rc != 0) {
		return rc;
	}

	LOG_INF("RateControlAllocator eFMU process running");
	while (true) {
		uint64_t imu_timestamp_ns = 0U;
		uint64_t motor_timestamp_ns;

		process->status.imu_ok = rdd2_imu_stream_wait_next(&process->gyro, &process->accel,
								   &process->dt, &imu_timestamp_ns);
		process->status.rc_link_quality = rdd2_rc_input_link_quality_get();
		rdd2_rc_input_latest_get(&process->rc, &process->status.rc_stamp_ms,
					 &process->status.rc_valid);
		update_arming_state(process);
		publish_imu(process, imu_timestamp_ns);

		process->motors = (rdd2_motor_values_t){0};
		step_efmu(process);
		if (rdd2_motor_test_get(&process->motors)) {
			motor_timestamp_ns =
				rdd2_motor_output_write_all(&process->motors, true, true);
		} else if (rdd2_motor_raw_test_get(&process->raw_test)) {
			motor_timestamp_ns =
				rdd2_motor_output_write_all_raw(&process->raw_test, true);
		} else {
			motor_timestamp_ns = rdd2_motor_output_write_all(
				&process->motors, process->status.armed, false);
		}

		process->imu_to_motor_latency_us =
			imu_to_motor_latency_us(imu_timestamp_ns, motor_timestamp_ns);
		publish_status(process);
	}
}
