/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_SYNAPSE_MESSAGES_H_
#define RDD2_SYNAPSE_MESSAGES_H_

#include "rdd2_control_types.h"

#include <synapse/control_reader.h>
#include <synapse/sensors_reader.h>
#include <synapse/state_reader.h>

/* Every member below is an unwrapped, generated synapse_fbs v0.5 payload. */
struct rdd2_topic_flight_state {
	synapse_topic_VehicleHealthData_t vehicle_health;
	synapse_topic_AttitudeEstimateData_t attitude_estimate;
	synapse_topic_AttitudeCommandData_t attitude_command;
	synapse_topic_ControlLoopMetricsData_t control_loop_metrics;
};

typedef synapse_topic_PwmSignalOutputsData_t rdd2_topic_motor_output_blob_t;
typedef struct rdd2_topic_flight_state rdd2_topic_flight_state_blob_t;

static inline float *rdd2_topic_vec3f_data(rdd2_vec3f_t *vec)
{
	return &vec->x;
}

static inline const float *rdd2_topic_vec3f_data_const(const rdd2_vec3f_t *vec)
{
	return &vec->x;
}

static inline int32_t *rdd2_topic_rc_channels_data(rdd2_rc_channels_t *rc)
{
	return &rc->ch0;
}

static inline const int32_t *rdd2_topic_rc_channels_data_const(const rdd2_rc_channels_t *rc)
{
	return &rc->ch0;
}

static inline float *rdd2_topic_rate_triplet_data(rdd2_rate_triplet_t *rate)
{
	return &rate->roll;
}

static inline const float *rdd2_topic_rate_triplet_data_const(const rdd2_rate_triplet_t *rate)
{
	return &rate->roll;
}

static inline float *rdd2_topic_attitude_euler_data(rdd2_attitude_euler_t *attitude)
{
	return &attitude->roll;
}

static inline const float *
rdd2_topic_attitude_euler_data_const(const rdd2_attitude_euler_t *attitude)
{
	return &attitude->roll;
}

static inline float *rdd2_topic_motor_values_data(rdd2_motor_values_t *motors)
{
	return &motors->m0;
}

static inline const float *rdd2_topic_motor_values_data_const(const rdd2_motor_values_t *motors)
{
	return &motors->m0;
}

static inline uint16_t *rdd2_topic_motor_raw_data(rdd2_motor_raw_t *raw)
{
	return &raw->m0;
}

static inline const uint16_t *rdd2_topic_motor_raw_data_const(const rdd2_motor_raw_t *raw)
{
	return &raw->m0;
}

void rdd2_topic_make_flight_state(rdd2_topic_flight_state_blob_t *output, const rdd2_vec3f_t *gyro,
				  const rdd2_control_status_t *status,
				  const rdd2_attitude_euler_t *attitude,
				  const rdd2_attitude_euler_t *attitude_desired,
				  const rdd2_rate_triplet_t *rate_desired,
				  uint32_t main_loop_latency_us);

void rdd2_topic_make_pwm_output(rdd2_topic_motor_output_blob_t *output,
				const rdd2_motor_values_t *motors, bool armed);

#endif
