/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rate_control.h"

#include "rdd2_efmi_control.h"

static void quadrotor_from_rc(QuadrotorState *state, const rdd2_rc_channels_t *rc)
{
	rdd2_efmi_quadrotor_init_from_rc(state, rc, RDD2_ROLL_CHANNEL_INDEX,
					 RDD2_PITCH_CHANNEL_INDEX, RDD2_THROTTLE_CHANNEL_INDEX,
					 RDD2_YAW_CHANNEL_INDEX, RDD2_ARM_CHANNEL_INDEX);
}

void rdd2_rate_controller_init(struct rdd2_rate_controller *controller)
{
	rdd2_efmi_pid_axis_init(&controller->roll);
	rdd2_efmi_pid_axis_init(&controller->pitch);
	rdd2_efmi_pid_axis_init(&controller->yaw);
	controller->yaw.kp = 0.20f;
	controller->yaw.ki = 0.20f;
	controller->yaw.kd = 0.0f;
	rdd2_efmi_pid_axis_recalibrate(&controller->yaw);
	rdd2_rate_controller_reset(controller);
}

void rdd2_rate_controller_reset(struct rdd2_rate_controller *controller)
{
	rdd2_efmi_pid_axis_reset(&controller->roll);
	rdd2_efmi_pid_axis_reset(&controller->pitch);
	rdd2_efmi_pid_axis_reset(&controller->yaw);
}

bool rdd2_rate_arm_switch_high(const rdd2_rc_channels_t *rc)
{
	QuadrotorState state;

	quadrotor_from_rc(&state, rc);
	rdd2_efmi_quadrotor_step(&state);
	return state.armSwitchHigh;
}

int32_t rdd2_rate_throttle_us(const rdd2_rc_channels_t *rc)
{
	return rdd2_topic_rc_channels_data_const(rc)[RDD2_THROTTLE_CHANNEL_INDEX];
}

float rdd2_rate_throttle_input_from_rc(const rdd2_rc_channels_t *rc)
{
	QuadrotorState state;

	quadrotor_from_rc(&state, rc);
	rdd2_efmi_quadrotor_step(&state);
	return (float)state.throttleInput;
}

float rdd2_rate_throttle_command(float throttle_input, bool armed)
{
	QuadrotorState state;

	rdd2_efmi_quadrotor_init(&state);
	state.throttleInputForCommand = (double)throttle_input;
	state.armed = armed;
	rdd2_efmi_quadrotor_step(&state);
	return (float)state.throttleCommand;
}

bool rdd2_rate_pid_integrate(float throttle_input, bool armed)
{
	QuadrotorState state;

	rdd2_efmi_quadrotor_init(&state);
	state.throttleInputForCommand = (double)throttle_input;
	state.armed = armed;
	rdd2_efmi_quadrotor_step(&state);
	return state.ratePidIntegrate;
}

float rdd2_rate_yaw_desired_from_rc(const rdd2_rc_channels_t *rc)
{
	QuadrotorState state;

	quadrotor_from_rc(&state, rc);
	rdd2_efmi_quadrotor_step(&state);
	return (float)state.yawRateDesired;
}

void rdd2_rate_desired_from_rc(const rdd2_rc_channels_t *rc,
			       rdd2_rate_triplet_t *rate_desired)
{
	QuadrotorState state;

	quadrotor_from_rc(&state, rc);
	rdd2_efmi_quadrotor_step(&state);
	rate_desired->roll = (float)state.acroRateDesiredRoll;
	rate_desired->pitch = (float)state.acroRateDesiredPitch;
	rate_desired->yaw = (float)state.acroRateDesiredYaw;
}

void rdd2_rate_controller_step(struct rdd2_rate_controller *controller,
			       const rdd2_rate_triplet_t *rate_desired,
			       const rdd2_vec3f_t *gyro, float dt, bool integrate,
			       rdd2_rate_triplet_t *rate_cmd)
{
	if (dt <= 0.0f) {
		*rate_cmd = (rdd2_rate_triplet_t){0};
		return;
	}

	rate_cmd->roll =
		rdd2_efmi_pid_axis_step(&controller->roll, rate_desired->roll, gyro->x, dt,
					 integrate);

	rate_cmd->pitch =
		rdd2_efmi_pid_axis_step(&controller->pitch, rate_desired->pitch, gyro->y, dt,
					 integrate);

	rate_cmd->yaw =
		rdd2_efmi_pid_axis_step(&controller->yaw, rate_desired->yaw, gyro->z, dt,
					 integrate);
}

void rdd2_mix_quad_x(float throttle, const rdd2_rate_triplet_t *rate_cmd,
		     rdd2_motor_values_t *motors)
{
	QuadrotorState state;

	rdd2_efmi_quadrotor_init(&state);
	state.throttle = (double)throttle;
	state.rateCmdRoll = (double)rate_cmd->roll;
	state.rateCmdPitch = (double)rate_cmd->pitch;
	state.rateCmdYaw = (double)rate_cmd->yaw;
	rdd2_efmi_quadrotor_step(&state);
	motors->m0 = (float)state.motor0;
	motors->m1 = (float)state.motor1;
	motors->m2 = (float)state.motor2;
	motors->m3 = (float)state.motor3;
}
