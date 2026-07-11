#ifndef RDD2_ATTITUDE_ESTIMATOR_H_
#define RDD2_ATTITUDE_ESTIMATOR_H_

#include "synapse_messages.h"

struct rdd2_attitude_estimator {
	float q_w;
	float q_x;
	float q_y;
	float q_z;
	float gyro_bias_x;
	float gyro_bias_y;
	float gyro_bias_z;
};

void rdd2_attitude_estimator_init(struct rdd2_attitude_estimator *estimator);
void rdd2_attitude_estimator_reset_from_accel(struct rdd2_attitude_estimator *estimator,
					      const rdd2_vec3f_t *accel);
void rdd2_attitude_estimator_predict(struct rdd2_attitude_estimator *estimator,
				     const rdd2_vec3f_t *gyro,
				     const rdd2_vec3f_t *accel, float dt);
void rdd2_attitude_estimator_get_attitude(const struct rdd2_attitude_estimator *estimator,
					  rdd2_attitude_euler_t *attitude);

#endif
