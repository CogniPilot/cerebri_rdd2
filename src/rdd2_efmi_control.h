#ifndef RDD2_EFMI_CONTROL_H_
#define RDD2_EFMI_CONTROL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Vehicles_Rdd2_Controller.h"
#include "efmi_wrapper.h"
#include "synapse_messages.h"

static inline void rdd2_efmi_controller_init(Vehicles_Rdd2_ControllerState *state)
{
	RDD2_EFMI_INIT(Vehicles_Rdd2_Controller, state);
}

static inline void rdd2_efmi_controller_recalibrate(Vehicles_Rdd2_ControllerState *state)
{
	RDD2_EFMI_RECALIBRATE(Vehicles_Rdd2_Controller, state);
}

static inline void rdd2_efmi_controller_step(Vehicles_Rdd2_ControllerState *state)
{
	RDD2_EFMI_STEP(Vehicles_Rdd2_Controller, state);
}

static inline void rdd2_efmi_pid_axis_init(Vehicles_Rdd2_ControllerState *state)
{
	rdd2_efmi_controller_init(state);
}

static inline void rdd2_efmi_pid_axis_recalibrate(Vehicles_Rdd2_ControllerState *state)
{
	rdd2_efmi_controller_recalibrate(state);
}

static inline void rdd2_efmi_pid_axis_reset(Vehicles_Rdd2_ControllerState *state)
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

static inline float rdd2_efmi_pid_axis_step(Vehicles_Rdd2_ControllerState *state, float setpoint,
					    float measurement, float dt, bool integrate)
{
	state->setpoint = (double)setpoint;
	state->measurement = (double)measurement;
	state->integrate = integrate ? 1.0 : 0.0;
	state->samplePeriod = (double)dt;
	rdd2_efmi_controller_step(state);
	return (float)state->pidOutput;
}

static inline void rdd2_efmi_controller_init_from_rc(Vehicles_Rdd2_ControllerState *state,
						     const rdd2_rc_channels_t *rc,
						     size_t roll_index, size_t pitch_index,
						     size_t throttle_index, size_t yaw_index,
						     size_t arm_index)
{
	const int32_t *channels = rdd2_topic_rc_channels_data_const(rc);

	rdd2_efmi_controller_init(state);
	state->rcRollUs = (double)channels[roll_index];
	state->rcPitchUs = (double)channels[pitch_index];
	state->rcThrottleUs = (double)channels[throttle_index];
	state->rcYawUs = (double)channels[yaw_index];
	state->rcArmUs = (double)channels[arm_index];
}

#endif
