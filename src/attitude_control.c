/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "attitude_control.h"

#include "rate_control.h"
#include "rdd2_efmi_control.h"

#define AUTO_LEVEL_ROLL_P_GAIN  4.0f
#define AUTO_LEVEL_PITCH_P_GAIN 4.0f

static void controller_from_rc(Vehicles_Rdd2_ControllerState *state,
			       const rdd2_rc_channels_t *rc)
{
	rdd2_efmi_controller_init_from_rc(state, rc, RDD2_ROLL_CHANNEL_INDEX,
					  RDD2_PITCH_CHANNEL_INDEX,
					  RDD2_THROTTLE_CHANNEL_INDEX,
					  RDD2_YAW_CHANNEL_INDEX, RDD2_ARM_CHANNEL_INDEX);
}

void rdd2_attitude_controller_init(struct rdd2_attitude_controller *controller)
{
	rdd2_efmi_pid_axis_init(&controller->roll);
	rdd2_efmi_pid_axis_init(&controller->pitch);

	controller->roll.kp = AUTO_LEVEL_ROLL_P_GAIN;
	controller->roll.ki = 0.0f;
	controller->roll.kd = 0.0f;
	controller->roll.i_limit = 0.0f;
	controller->roll.output_limit = RDD2_MAX_ROLL_PITCH_RATE_RAD_S;
	rdd2_efmi_pid_axis_recalibrate(&controller->roll);

	controller->pitch.kp = AUTO_LEVEL_PITCH_P_GAIN;
	controller->pitch.ki = 0.0f;
	controller->pitch.kd = 0.0f;
	controller->pitch.i_limit = 0.0f;
	controller->pitch.output_limit = RDD2_MAX_ROLL_PITCH_RATE_RAD_S;
	rdd2_efmi_pid_axis_recalibrate(&controller->pitch);

	rdd2_attitude_controller_reset(controller);
}

void rdd2_attitude_controller_reset(struct rdd2_attitude_controller *controller)
{
	rdd2_efmi_pid_axis_reset(&controller->roll);
	rdd2_efmi_pid_axis_reset(&controller->pitch);
}

void rdd2_attitude_desired_from_rc(const rdd2_rc_channels_t *rc,
				   const rdd2_attitude_euler_t *attitude,
				   rdd2_attitude_euler_t *attitude_desired)
{
	Vehicles_Rdd2_ControllerState state;

	if (rc == NULL || attitude == NULL || attitude_desired == NULL) {
		return;
	}

	controller_from_rc(&state, rc);
	state.attitudeRoll = (double)attitude->roll;
	state.attitudePitch = (double)attitude->pitch;
	state.attitudeYaw = (double)attitude->yaw;
	rdd2_efmi_controller_step(&state);
	attitude_desired->roll = (float)state.attitudeDesiredRoll;
	attitude_desired->pitch = (float)state.attitudeDesiredPitch;
	attitude_desired->yaw = (float)state.attitudeDesiredYaw;
}

void rdd2_attitude_controller_step(struct rdd2_attitude_controller *controller,
				   const rdd2_attitude_euler_t *attitude,
				   const rdd2_attitude_euler_t *attitude_desired,
				   const rdd2_rc_channels_t *rc, float dt,
				   rdd2_rate_triplet_t *rate_desired)
{
	Vehicles_Rdd2_ControllerState state;

	if (controller == NULL || attitude == NULL || attitude_desired == NULL || rc == NULL ||
	    rate_desired == NULL || dt <= 0.0f) {
		return;
	}

	rate_desired->roll =
		rdd2_efmi_pid_axis_step(&controller->roll, attitude_desired->roll,
					 attitude->roll, dt, false);

	rate_desired->pitch =
		rdd2_efmi_pid_axis_step(&controller->pitch, attitude_desired->pitch,
					 attitude->pitch, dt, false);

	controller_from_rc(&state, rc);
	rdd2_efmi_controller_step(&state);
	rate_desired->yaw = (float)state.yawRateDesired;
}
