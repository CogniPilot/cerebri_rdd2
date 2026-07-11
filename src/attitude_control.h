#ifndef RDD2_ATTITUDE_CONTROL_H_
#define RDD2_ATTITUDE_CONTROL_H_

#include "Quadrotor.h"
#include "synapse_messages.h"

struct rdd2_attitude_controller {
	QuadrotorState roll;
	QuadrotorState pitch;
};

void rdd2_attitude_controller_init(struct rdd2_attitude_controller *controller);
void rdd2_attitude_controller_reset(struct rdd2_attitude_controller *controller);
void rdd2_attitude_desired_from_rc(const rdd2_rc_channels_t *rc,
				   const rdd2_attitude_euler_t *attitude,
				   rdd2_attitude_euler_t *attitude_desired);
void rdd2_attitude_controller_step(struct rdd2_attitude_controller *controller,
				   const rdd2_attitude_euler_t *attitude,
				   const rdd2_attitude_euler_t *attitude_desired,
				   const rdd2_rc_channels_t *rc, float dt,
				   rdd2_rate_triplet_t *rate_desired);

#endif
