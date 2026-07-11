#ifndef RDD2_EFMI_CONTROL_H_
#define RDD2_EFMI_CONTROL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Quadrotor.h"
#include "efmi_wrapper.h"
#include "synapse_messages.h"

static inline void rdd2_efmi_quadrotor_init(QuadrotorState *state)
{
	RDD2_EFMI_INIT(Quadrotor, state);
}

static inline void rdd2_efmi_quadrotor_recalibrate(QuadrotorState *state)
{
	RDD2_EFMI_RECALIBRATE(Quadrotor, state);
}

static inline void rdd2_efmi_quadrotor_step(QuadrotorState *state)
{
	RDD2_EFMI_STEP(Quadrotor, state);
}

static inline void rdd2_efmi_pid_axis_init(QuadrotorState *state)
{
	rdd2_efmi_quadrotor_init(state);
}

static inline void rdd2_efmi_pid_axis_recalibrate(QuadrotorState *state)
{
	rdd2_efmi_quadrotor_recalibrate(state);
}

static inline void rdd2_efmi_pid_axis_reset(QuadrotorState *state)
{
	state->setpoint = 0.0;
	state->measurement = 0.0;
	state->integrate = 0.0;
	state->previous_meas_filt_valid = false;
	state->previous_meas_filt = 0.0;
	state->previous_e_int = 0.0;
	state->e_int = 0.0;
	state->meas_filt = 0.0;
	state->derivative = 0.0;
	state->pidError = 0.0;
	state->pidOutput = 0.0;
	state->meas_filt_valid = false;
}

static inline float rdd2_efmi_pid_axis_step(QuadrotorState *state, float setpoint,
					    float measurement, float dt, bool integrate)
{
	state->setpoint = (double)setpoint;
	state->measurement = (double)measurement;
	state->integrate = integrate ? 1.0 : 0.0;
	state->samplePeriod = (double)dt;
	rdd2_efmi_quadrotor_step(state);
	return (float)state->pidOutput;
}

static inline void rdd2_efmi_quadrotor_init_from_rc(QuadrotorState *state,
						    const rdd2_rc_channels_t *rc,
						    size_t roll_index, size_t pitch_index,
						    size_t throttle_index, size_t yaw_index,
						    size_t arm_index)
{
	const int32_t *channels = rdd2_topic_rc_channels_data_const(rc);

	rdd2_efmi_quadrotor_init(state);
	state->rcRollUs = (double)channels[roll_index];
	state->rcPitchUs = (double)channels[pitch_index];
	state->rcThrottleUs = (double)channels[throttle_index];
	state->rcYawUs = (double)channels[yaw_index];
	state->rcArmUs = (double)channels[arm_index];
}

#endif
