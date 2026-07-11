/* SPDX-License-Identifier: Apache-2.0 */

#include "synapse_messages.h"

#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

BUILD_ASSERT(sizeof(synapse_topic_InertialSampleData_t) == 56U);
BUILD_ASSERT(sizeof(synapse_topic_ManualControlData_t) == 40U);
BUILD_ASSERT(sizeof(synapse_topic_PwmSignalOutputsData_t) == 48U);
BUILD_ASSERT(sizeof(synapse_topic_VehicleHealthData_t) == 48U);
BUILD_ASSERT(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);

static uint64_t timestamp_us(void)
{
	return (uint64_t)k_uptime_get() * 1000U;
}

static synapse_types_Quaternionf_t quaternion_from_euler(const rdd2_attitude_euler_t *euler)
{
	float cr = cosf(euler->roll * 0.5f);
	float sr = sinf(euler->roll * 0.5f);
	float cp = cosf(euler->pitch * 0.5f);
	float sp = sinf(euler->pitch * 0.5f);
	float cy = cosf(euler->yaw * 0.5f);
	float sy = sinf(euler->yaw * 0.5f);

	return (synapse_types_Quaternionf_t){
		.w = cr * cp * cy + sr * sp * sy,
		.x = sr * cp * cy - cr * sp * sy,
		.y = cr * sp * cy + sr * cp * sy,
		.z = cr * cp * sy - sr * sp * cy,
	};
}

void rdd2_topic_make_flight_state(rdd2_topic_flight_state_blob_t *output, const rdd2_vec3f_t *gyro,
				  const rdd2_control_status_t *status,
				  const rdd2_attitude_euler_t *attitude,
				  const rdd2_attitude_euler_t *attitude_desired,
				  const rdd2_rate_triplet_t *rate_desired,
				  uint32_t main_loop_latency_us)
{
	uint64_t now_us = timestamp_us();
	uint32_t sensors = synapse_topic_SensorComponentFlags_Gyro |
			   synapse_topic_SensorComponentFlags_Accel |
			   synapse_topic_SensorComponentFlags_RadioControl |
			   synapse_topic_SensorComponentFlags_MotorOutputs |
			   synapse_topic_SensorComponentFlags_Estimator;
	uint32_t healthy = synapse_topic_SensorComponentFlags_MotorOutputs |
			   synapse_topic_SensorComponentFlags_Estimator;

	if (status->imu_ok) {
		healthy |= synapse_topic_SensorComponentFlags_Gyro |
			   synapse_topic_SensorComponentFlags_Accel;
	}
	if (status->rc_valid && !status->rc_stale) {
		healthy |= synapse_topic_SensorComponentFlags_RadioControl;
	}

	memset(output, 0, sizeof(*output));
	output->vehicle_health = (synapse_topic_VehicleHealthData_t){
		.timestamp_us = now_us,
		.sensors_present = sensors,
		.sensors_enabled = sensors,
		.sensors_health = healthy,
		.flight_mode = status->flight_mode,
		.link_quality_pct = status->rc_link_quality,
		.flags = status->armed ? synapse_topic_VehicleHealthFlags_Armed : 0U,
	};
	output->attitude_estimate = (synapse_topic_AttitudeEstimateData_t){
		.timestamp_us = now_us,
		.attitude = quaternion_from_euler(attitude),
		.angular_velocity_flu_rad_s =
			{
				.roll = gyro->x,
				.pitch = gyro->y,
				.yaw = gyro->z,
			},
		.flags = status->imu_ok ? synapse_topic_AttitudeEstimateFlags_AttitudeValid |
						  synapse_topic_AttitudeEstimateFlags_RatesValid
					: 0U,
	};
	output->attitude_command = (synapse_topic_AttitudeCommandData_t){
		.timestamp_us = now_us,
		.attitude = quaternion_from_euler(attitude_desired),
		.body_rate_flu_rad_s = *rate_desired,
	};
	output->control_loop_metrics = (synapse_topic_ControlLoopMetricsData_t){
		.timestamp_us = now_us,
		.period_us = 625U,
		.latency_us = main_loop_latency_us,
	};
}

void rdd2_topic_make_pwm_output(rdd2_topic_motor_output_blob_t *output,
				const rdd2_motor_values_t *motors, bool armed)
{
	const float *values = rdd2_topic_motor_values_data_const(motors);
	uint16_t *pwm = &output->output0_us;

	memset(output, 0, sizeof(*output));
	output->timestamp_us = timestamp_us();
	output->active_mask = 0x0fU;
	for (size_t i = 0; i < 4U; ++i) {
		float value = armed ? CLAMP(values[i], 0.0f, 1.0f) : 0.0f;
		pwm[i] = (uint16_t)(1000.0f + value * 1000.0f + 0.5f);
	}
}
