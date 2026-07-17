/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "attitude_estimator.h"

static void set_inputs(Estimation_ComplementaryAttitudeState *model,
                       const rdd2_vec3f_t *gyro, const rdd2_vec3f_t *accel) {
  model->gyro_rad_s[0] = (double)gyro->x;
  model->gyro_rad_s[1] = (double)gyro->y;
  model->gyro_rad_s[2] = (double)gyro->z;
  model->accel_m_s2[0] = (double)accel->x;
  model->accel_m_s2[1] = (double)accel->y;
  model->accel_m_s2[2] = (double)accel->z;
}

void rdd2_attitude_estimator_init(struct rdd2_attitude_estimator *estimator) {
  if (estimator == NULL) {
    return;
  }

  Estimation_ComplementaryAttitude_startup(&estimator->model);
  Estimation_ComplementaryAttitude_recalibrate(&estimator->model);
}

void rdd2_attitude_estimator_reset_from_accel(
    struct rdd2_attitude_estimator *estimator, const rdd2_vec3f_t *accel) {
  static const rdd2_vec3f_t zero_gyro;

  if (estimator == NULL || accel == NULL) {
    return;
  }

  set_inputs(&estimator->model, &zero_gyro, accel);
  estimator->model.reset = true;
  Estimation_ComplementaryAttitude_dostep(&estimator->model);
}

void rdd2_attitude_estimator_predict(struct rdd2_attitude_estimator *estimator,
                                     const rdd2_vec3f_t *gyro,
                                     const rdd2_vec3f_t *accel, float dt) {
  if (estimator == NULL || gyro == NULL || accel == NULL || dt <= 0.0f) {
    return;
  }

  set_inputs(&estimator->model, gyro, accel);
  estimator->model.samplePeriod = (double)dt;
  estimator->model.reset = false;
  Estimation_ComplementaryAttitude_dostep(&estimator->model);
}

void rdd2_attitude_estimator_get_attitude(
    const struct rdd2_attitude_estimator *estimator,
    rdd2_attitude_euler_t *attitude) {
  if (estimator == NULL || attitude == NULL) {
    return;
  }

  attitude->roll = (float)estimator->model.euler_rad[0];
  attitude->pitch = (float)estimator->model.euler_rad[1];
  attitude->yaw = (float)estimator->model.euler_rad[2];
}
