/* SPDX-License-Identifier: Apache-2.0 */

#include "imu_preintegration.h"

#include <math.h>
#include <string.h>

/*
 * Transcription of Estimation.StrapdownINS.preintegrateImuStep and the
 * LieGroups helpers it calls, from modelica_models. Every branch threshold and
 * series expansion below is the one the model uses, because the packet handed
 * to the generated estimator has to be the packet the model would have handed
 * it. Where a name differs from the model it is spelled out in a comment.
 */

/* LieGroups.SO3.Quat.exp_map */
#define QUATERNION_EXP_SERIES_LIMIT 1.0e-8f
/* LieGroups.SO3.Quat.log_map */
#define QUATERNION_LOG_SERIES_LIMIT 1.0e-10f
/* LieGroups.SO3.Quat.normalize */
#define QUATERNION_NORM_TOLERANCE 1.0e-12f
/* LieGroups.SO3.Quat.right_jacobian and LieGroups.SE23.Quat.exp_mixed both
 * retain the series below 0.1 rad, where the closed forms lose their
 * numerators to cancellation in single precision. */
#define ROTATION_SERIES_LIMIT 1.0e-2f

static void vector_copy(float destination[3], const float source[3]) {
  destination[0] = source[0];
  destination[1] = source[1];
  destination[2] = source[2];
}

static void matrix_identity(float result[3][3]) {
  for (size_t row = 0U; row < 3U; ++row) {
    for (size_t column = 0U; column < 3U; ++column) {
      result[row][column] = (row == column) ? 1.0f : 0.0f;
    }
  }
}

static void matrix_zero(float result[3][3]) {
  memset(result, 0, 9U * sizeof(float));
}

static void matrix_copy(float destination[3][3], const float source[3][3]) {
  memcpy(destination, source, 9U * sizeof(float));
}

static void matrix_multiply(float result[3][3], const float left[3][3],
                            const float right[3][3]) {
  for (size_t row = 0U; row < 3U; ++row) {
    for (size_t column = 0U; column < 3U; ++column) {
      float sum = 0.0f;

      for (size_t inner = 0U; inner < 3U; ++inner) {
        sum += left[row][inner] * right[inner][column];
      }
      result[row][column] = sum;
    }
  }
}

static void matrix_transpose_multiply(float result[3][3],
                                      const float left[3][3],
                                      const float right[3][3]) {
  for (size_t row = 0U; row < 3U; ++row) {
    for (size_t column = 0U; column < 3U; ++column) {
      float sum = 0.0f;

      for (size_t inner = 0U; inner < 3U; ++inner) {
        sum += left[inner][row] * right[inner][column];
      }
      result[row][column] = sum;
    }
  }
}

/* destination += scale * addend */
static void matrix_scaled_add(float destination[3][3],
                              const float addend[3][3], float scale) {
  for (size_t row = 0U; row < 3U; ++row) {
    for (size_t column = 0U; column < 3U; ++column) {
      destination[row][column] += scale * addend[row][column];
    }
  }
}

static void matrix_apply(float result[3], const float matrix[3][3],
                         const float vector[3]) {
  for (size_t row = 0U; row < 3U; ++row) {
    result[row] = matrix[row][0] * vector[0] + matrix[row][1] * vector[1] +
                  matrix[row][2] * vector[2];
  }
}

/* LieGroups.SO3.Quat.wedge */
static void skew(float result[3][3], const float vector[3]) {
  result[0][0] = 0.0f;
  result[0][1] = -vector[2];
  result[0][2] = vector[1];
  result[1][0] = vector[2];
  result[1][1] = 0.0f;
  result[1][2] = -vector[0];
  result[2][0] = -vector[1];
  result[2][1] = vector[0];
  result[2][2] = 0.0f;
}

static void cross(float result[3], const float left[3], const float right[3]) {
  result[0] = left[1] * right[2] - left[2] * right[1];
  result[1] = left[2] * right[0] - left[0] * right[2];
  result[2] = left[0] * right[1] - left[1] * right[0];
}

/* LieGroups.SO3.Quat.exp_map */
static void quaternion_exp(float quaternion[4], const float rotation[3]) {
  float theta_squared = rotation[0] * rotation[0] +
                        rotation[1] * rotation[1] + rotation[2] * rotation[2];
  float scalar;
  float vector_scale;

  if (theta_squared < QUATERNION_EXP_SERIES_LIMIT) {
    scalar = 1.0f - theta_squared / 8.0f;
    vector_scale = 0.5f - theta_squared / 48.0f;
  } else {
    float theta = sqrtf(theta_squared);

    scalar = cosf(0.5f * theta);
    vector_scale = sinf(0.5f * theta) / theta;
  }
  quaternion[0] = scalar;
  quaternion[1] = vector_scale * rotation[0];
  quaternion[2] = vector_scale * rotation[1];
  quaternion[3] = vector_scale * rotation[2];
}

/* LieGroups.SO3.Quat.normalize */
static void quaternion_normalize(float quaternion[4]) {
  float norm = sqrtf(quaternion[0] * quaternion[0] +
                     quaternion[1] * quaternion[1] +
                     quaternion[2] * quaternion[2] +
                     quaternion[3] * quaternion[3]);

  if (norm > QUATERNION_NORM_TOLERANCE) {
    float inverse = 1.0f / norm;

    quaternion[0] *= inverse;
    quaternion[1] *= inverse;
    quaternion[2] *= inverse;
    quaternion[3] *= inverse;
  } else {
    quaternion[0] = 1.0f;
    quaternion[1] = 0.0f;
    quaternion[2] = 0.0f;
    quaternion[3] = 0.0f;
  }
}

/* LieGroups.SO3.Quat.log_map */
static void quaternion_log(float rotation[3], const float quaternion[4]) {
  float normalized[4] = {quaternion[0], quaternion[1], quaternion[2],
                         quaternion[3]};
  float vector_norm;

  quaternion_normalize(normalized);
  if (normalized[0] < 0.0f) {
    normalized[0] = -normalized[0];
    normalized[1] = -normalized[1];
    normalized[2] = -normalized[2];
    normalized[3] = -normalized[3];
  }
  vector_norm = sqrtf(normalized[1] * normalized[1] +
                      normalized[2] * normalized[2] +
                      normalized[3] * normalized[3]);
  if (vector_norm < QUATERNION_LOG_SERIES_LIMIT) {
    rotation[0] = 2.0f * normalized[1];
    rotation[1] = 2.0f * normalized[2];
    rotation[2] = 2.0f * normalized[3];
  } else {
    float scalar = normalized[0];
    float theta;
    float scale;

    if (scalar > 1.0f) {
      scalar = 1.0f;
    } else if (scalar < -1.0f) {
      scalar = -1.0f;
    } else {
      /* already inside the principal range */
    }
    theta = 2.0f * atan2f(vector_norm, scalar);
    scale = theta / vector_norm;
    rotation[0] = scale * normalized[1];
    rotation[1] = scale * normalized[2];
    rotation[2] = scale * normalized[3];
  }
}

/* LieGroups.SO3.Quat.product */
static void quaternion_multiply(float result[4], const float left[4],
                                const float right[4]) {
  result[0] = left[0] * right[0] - left[1] * right[1] - left[2] * right[2] -
              left[3] * right[3];
  result[1] = left[1] * right[0] + left[0] * right[1] - left[3] * right[2] +
              left[2] * right[3];
  result[2] = left[2] * right[0] + left[3] * right[1] + left[0] * right[2] -
              left[1] * right[3];
  result[3] = left[3] * right[0] - left[2] * right[1] + left[1] * right[2] +
              left[0] * right[3];
}

/* LieGroups.SO3.Quat.to_DCM */
static void quaternion_to_rotation(float result[3][3],
                                   const float quaternion[4]) {
  float a = quaternion[0];
  float b = quaternion[1];
  float c = quaternion[2];
  float d = quaternion[3];

  result[0][0] = a * a + b * b - c * c - d * d;
  result[0][1] = 2.0f * (b * c - a * d);
  result[0][2] = 2.0f * (b * d + a * c);
  result[1][0] = 2.0f * (b * c + a * d);
  result[1][1] = a * a - b * b + c * c - d * d;
  result[1][2] = 2.0f * (c * d - a * b);
  result[2][0] = 2.0f * (b * d - a * c);
  result[2][1] = 2.0f * (c * d + a * b);
  result[2][2] = a * a - b * b - c * c + d * d;
}

/* Shared series coefficients of the SO(3) closed forms, evaluated on the
 * squared rotation angle exactly as LieGroups.SE23.Quat.exp_mixed does. */
static void rotation_series(float theta_squared, float *first, float *second,
                            float *third) {
  if (theta_squared < ROTATION_SERIES_LIMIT) {
    *first = 0.5f - theta_squared / 24.0f;
    *second = 1.0f / 6.0f - theta_squared / 120.0f;
    *third = 1.0f / 24.0f - theta_squared / 720.0f;
  } else {
    float theta = sqrtf(theta_squared);
    float cosine = cosf(theta);

    *first = (1.0f - cosine) / theta_squared;
    *second = (theta - sinf(theta)) / (theta_squared * theta);
    *third = (0.5f * theta_squared + cosine - 1.0f) /
             (theta_squared * theta_squared);
  }
}

/* LieGroups.SO3.Quat.right_jacobian */
static void right_jacobian(float result[3][3], const float rotation[3]) {
  float theta_squared = rotation[0] * rotation[0] +
                        rotation[1] * rotation[1] + rotation[2] * rotation[2];
  float skew_matrix[3][3];
  float skew_squared[3][3];
  float first;
  float second;
  float third;

  skew(skew_matrix, rotation);
  matrix_multiply(skew_squared, skew_matrix, skew_matrix);
  rotation_series(theta_squared, &first, &second, &third);
  matrix_identity(result);
  matrix_scaled_add(result, skew_matrix, -first);
  matrix_scaled_add(result, skew_squared, second);
}

/*
 * LieGroups.SE23.Quat.exp_mixed specialised to the preintegration call:
 * the right (world) algebra element is zero and B is the strictly upper
 * nilpotent [0, dt; 0, 0], so the world rotation is the identity, N_r
 * vanishes, and the composition reduces to
 *
 *   dv <- dv + dR * (I + C1*Om + C2*Om2) * u
 *   dp <- dp + dv * dt + dR * (w + dt/2 * u + C1*Om*w + C2*Om2*w
 *                              + dt * (C2*Om*u + C3*Om2*u))
 *   dR <- dR * Exp(theta)
 *
 * with u the body velocity increment, w the body position increment (zero
 * under the zero-order hold), and Om the wedge of the rotation increment.
 */
static void compose_interval(struct rdd2_imu_preintegrator *state,
                             const float rotation_increment[3],
                             const float velocity_increment[3],
                             const float position_increment[3], float dt,
                             const float previous_rotation[3][3],
                             const float increment_quaternion[4]) {
  float theta_squared = rotation_increment[0] * rotation_increment[0] +
                        rotation_increment[1] * rotation_increment[1] +
                        rotation_increment[2] * rotation_increment[2];
  float first;
  float second;
  float third;
  float skew_matrix[3][3];
  float skew_velocity[3];
  float skew_squared_velocity[3];
  float skew_position[3];
  float skew_squared_position[3];
  float velocity_column[3];
  float position_column[3];
  float rotated[3];
  float updated_quaternion[4];

  rotation_series(theta_squared, &first, &second, &third);
  skew(skew_matrix, rotation_increment);
  matrix_apply(skew_velocity, skew_matrix, velocity_increment);
  matrix_apply(skew_squared_velocity, skew_matrix, skew_velocity);
  matrix_apply(skew_position, skew_matrix, position_increment);
  matrix_apply(skew_squared_position, skew_matrix, skew_position);
  for (size_t axis = 0U; axis < 3U; ++axis) {
    velocity_column[axis] = velocity_increment[axis] +
                            first * skew_velocity[axis] +
                            second * skew_squared_velocity[axis];
    position_column[axis] =
        position_increment[axis] + 0.5f * dt * velocity_increment[axis] +
        first * skew_position[axis] + second * skew_squared_position[axis] +
        dt * (second * skew_velocity[axis] + third * skew_squared_velocity[axis]);
  }

  matrix_apply(rotated, previous_rotation, position_column);
  for (size_t axis = 0U; axis < 3U; ++axis) {
    state->delta_position_m[axis] +=
        state->delta_velocity_m_s[axis] * dt + rotated[axis];
  }
  matrix_apply(rotated, previous_rotation, velocity_column);
  for (size_t axis = 0U; axis < 3U; ++axis) {
    state->delta_velocity_m_s[axis] += rotated[axis];
  }
  quaternion_multiply(updated_quaternion, state->delta_quaternion,
                      increment_quaternion);
  quaternion_normalize(updated_quaternion);
  for (size_t element = 0U; element < 4U; ++element) {
    state->delta_quaternion[element] = updated_quaternion[element];
  }
}

/*
 * First variation of the same composition with respect to the bias anchors,
 * evaluated at the interval midpoint so the sensitivities stay second-order
 * accurate while the nominal preintegral remains closed-form. Mirrors the
 * Jacobian half of preintegrateImuStep.
 */
static void compose_jacobians(struct rdd2_imu_preintegrator *state,
                              const float rotation_increment[3],
                              const float midpoint_specific_force[3],
                              const float angular_velocity_delta[3],
                              const float specific_force_delta[3], float dt,
                              const float previous_rotation[3][3],
                              const float increment_quaternion[4]) {
  float half_rotation[3];
  float half_quaternion[4];
  float half_matrix[3][3];
  float increment_matrix[3][3];
  float midpoint_rotation[3][3];
  float midpoint_jacobian[3][3];
  float right_jacobian_half[3][3];
  float right_jacobian_full[3][3];
  float force_skew[3][3];
  float rotated_force_skew[3][3];
  float sensitivity[3][3];
  float previous_velocity_gyroscope[3][3];
  float previous_velocity_accelerometer[3][3];
  float scratch[3][3];

  for (size_t axis = 0U; axis < 3U; ++axis) {
    half_rotation[axis] = 0.5f * rotation_increment[axis];
  }
  quaternion_exp(half_quaternion, half_rotation);
  quaternion_to_rotation(half_matrix, half_quaternion);
  quaternion_to_rotation(increment_matrix, increment_quaternion);
  matrix_multiply(midpoint_rotation, previous_rotation, half_matrix);
  right_jacobian(right_jacobian_half, half_rotation);
  right_jacobian(right_jacobian_full, rotation_increment);

  /* rotationJacobianAtMidpoint_s */
  matrix_transpose_multiply(midpoint_jacobian, half_matrix,
                            state->rotation_gyroscope_bias_jacobian_s);
  matrix_scaled_add(midpoint_jacobian, right_jacobian_half, -0.5f * dt);

  /* velocity and position sensitivity to the gyroscope bias share the term
   * rotationAtMidpoint * wedge(force) * rotationJacobianAtMidpoint_s */
  skew(force_skew, midpoint_specific_force);
  matrix_multiply(rotated_force_skew, midpoint_rotation, force_skew);
  matrix_multiply(sensitivity, rotated_force_skew, midpoint_jacobian);

  matrix_copy(previous_velocity_gyroscope,
              state->velocity_gyroscope_bias_jacobian_m);
  matrix_copy(previous_velocity_accelerometer,
              state->velocity_accelerometer_bias_jacobian_s);

  /* deltaRotationGyroscopeBiasJacobian_s */
  matrix_transpose_multiply(scratch, increment_matrix,
                            state->rotation_gyroscope_bias_jacobian_s);
  matrix_copy(state->rotation_gyroscope_bias_jacobian_s, scratch);
  if (state->first_order_hold) {
    float bracket[3][3];
    float delta_skew[3][3];

    /* Jr * ((dt^2/12) * wedge(delta omega) - dt * I), factored as
     * -dt * Jr * (I - (dt/12) * wedge(delta omega)). */
    matrix_identity(bracket);
    skew(delta_skew, angular_velocity_delta);
    matrix_scaled_add(bracket, delta_skew, -dt / 12.0f);
    matrix_multiply(scratch, right_jacobian_full, bracket);
    matrix_scaled_add(state->rotation_gyroscope_bias_jacobian_s, scratch, -dt);
  } else {
    matrix_scaled_add(state->rotation_gyroscope_bias_jacobian_s,
                      right_jacobian_full, -dt);
  }

  /* deltaVelocity and deltaPosition sensitivities */
  matrix_scaled_add(state->velocity_gyroscope_bias_jacobian_m, sensitivity,
                    -dt);
  matrix_scaled_add(state->velocity_accelerometer_bias_jacobian_s,
                    midpoint_rotation, -dt);
  matrix_scaled_add(state->position_gyroscope_bias_jacobian_m_s,
                    previous_velocity_gyroscope, dt);
  matrix_scaled_add(state->position_gyroscope_bias_jacobian_m_s, sensitivity,
                    -0.5f * dt * dt);
  matrix_scaled_add(state->position_accelerometer_bias_jacobian_s2,
                    previous_velocity_accelerometer, dt);
  matrix_scaled_add(state->position_accelerometer_bias_jacobian_s2,
                    midpoint_rotation, -0.5f * dt * dt);
  if (state->first_order_hold) {
    float force_delta_skew[3][3];
    float angular_delta_skew[3][3];

    skew(force_delta_skew, specific_force_delta);
    matrix_multiply(scratch, midpoint_rotation, force_delta_skew);
    matrix_scaled_add(state->velocity_gyroscope_bias_jacobian_m, scratch,
                      dt * dt / 12.0f);
    matrix_scaled_add(state->position_gyroscope_bias_jacobian_m_s, scratch,
                      0.5f * dt * dt * dt / 12.0f);
    skew(angular_delta_skew, angular_velocity_delta);
    matrix_multiply(scratch, midpoint_rotation, angular_delta_skew);
    matrix_scaled_add(state->velocity_accelerometer_bias_jacobian_s, scratch,
                      dt * dt / 12.0f);
    matrix_scaled_add(state->position_accelerometer_bias_jacobian_s2, scratch,
                      0.5f * dt * dt * dt / 12.0f);
  }
}

void rdd2_imu_preintegrator_restart(struct rdd2_imu_preintegrator *state,
                                    const float gyroscope_bias_rad_s[3],
                                    const float accelerometer_bias_m_s2[3]) {
  state->delta_position_m[0] = 0.0f;
  state->delta_position_m[1] = 0.0f;
  state->delta_position_m[2] = 0.0f;
  state->delta_velocity_m_s[0] = 0.0f;
  state->delta_velocity_m_s[1] = 0.0f;
  state->delta_velocity_m_s[2] = 0.0f;
  state->delta_quaternion[0] = 1.0f;
  state->delta_quaternion[1] = 0.0f;
  state->delta_quaternion[2] = 0.0f;
  state->delta_quaternion[3] = 0.0f;
  matrix_zero(state->rotation_gyroscope_bias_jacobian_s);
  matrix_zero(state->velocity_gyroscope_bias_jacobian_m);
  matrix_zero(state->velocity_accelerometer_bias_jacobian_s);
  matrix_zero(state->position_gyroscope_bias_jacobian_m_s);
  matrix_zero(state->position_accelerometer_bias_jacobian_s2);
  vector_copy(state->gyroscope_bias_linearization_rad_s, gyroscope_bias_rad_s);
  vector_copy(state->accelerometer_bias_linearization_m_s2,
              accelerometer_bias_m_s2);
  state->integration_time_s = 0.0f;
  state->sample_count = 0U;
}

void rdd2_imu_preintegrator_reset(struct rdd2_imu_preintegrator *state,
                                  const float gyroscope_bias_rad_s[3],
                                  const float accelerometer_bias_m_s2[3]) {
  rdd2_imu_preintegrator_restart(state, gyroscope_bias_rad_s,
                                 accelerometer_bias_m_s2);
  state->previous_gyroscope_rad_s[0] = 0.0f;
  state->previous_gyroscope_rad_s[1] = 0.0f;
  state->previous_gyroscope_rad_s[2] = 0.0f;
  state->previous_accelerometer_m_s2[0] = 0.0f;
  state->previous_accelerometer_m_s2[1] = 0.0f;
  state->previous_accelerometer_m_s2[2] = 0.0f;
  state->previous_timestamp_ns = 0U;
  state->previous_sample_valid = false;
  state->rejected_sample_count = 0U;
}

bool rdd2_imu_preintegrator_accumulate(struct rdd2_imu_preintegrator *state,
                                       const float gyroscope_rad_s[3],
                                       const float accelerometer_m_s2[3],
                                       uint64_t timestamp_ns) {
  float corrected_angular_velocity[3];
  float corrected_specific_force[3];
  float start_angular_velocity[3];
  float start_specific_force[3];
  float angular_velocity_delta[3];
  float specific_force_delta[3];
  float midpoint_specific_force[3];
  float rotation_increment[3];
  float velocity_increment[3];
  float position_increment[3] = {0.0f, 0.0f, 0.0f};
  float previous_rotation[3][3];
  float increment_quaternion[4];
  float dt = 0.0f;
  bool measured_interval = false;
  bool integrated = false;

  for (size_t axis = 0U; axis < 3U; ++axis) {
    corrected_angular_velocity[axis] =
        gyroscope_rad_s[axis] - state->gyroscope_bias_linearization_rad_s[axis];
    corrected_specific_force[axis] =
        accelerometer_m_s2[axis] -
        state->accelerometer_bias_linearization_m_s2[axis];
  }

  if (state->previous_sample_valid) {
    if (timestamp_ns > state->previous_timestamp_ns) {
      dt = (float)(timestamp_ns - state->previous_timestamp_ns) * 1.0e-9f;
      measured_interval = true;
    }
  } else if (state->nominal_sample_period_s > 0.0f) {
    /* No earlier timestamp to difference against: the sample still covers one
     * nominal period, which is what the model integrates it over. */
    dt = state->nominal_sample_period_s;
    measured_interval = true;
  } else {
    /* fallback disabled: this sample only starts the history */
  }

  if (measured_interval) {
    if (dt >= RDD2_IMU_PREINTEGRATION_MIN_DT_S &&
        dt <= RDD2_IMU_PREINTEGRATION_MAX_DT_S) {
      if (state->first_order_hold && state->previous_sample_valid) {
        float bracket[3];

        for (size_t axis = 0U; axis < 3U; ++axis) {
          start_angular_velocity[axis] =
              state->previous_gyroscope_rad_s[axis] -
              state->gyroscope_bias_linearization_rad_s[axis];
          start_specific_force[axis] =
              state->previous_accelerometer_m_s2[axis] -
              state->accelerometer_bias_linearization_m_s2[axis];
          /* Sample differences: the constant bias anchor cancels, so these
           * never subtract two nearly parallel corrected samples. */
          angular_velocity_delta[axis] =
              gyroscope_rad_s[axis] - state->previous_gyroscope_rad_s[axis];
          specific_force_delta[axis] = accelerometer_m_s2[axis] -
                                       state->previous_accelerometer_m_s2[axis];
        }
        /* Trapezoid plus the truncated Magnus bracket: the first term is
         * exactly the PX4/ArduPilot trapezoidal increment and the second is
         * the classical coning correction. */
        cross(bracket, start_angular_velocity, angular_velocity_delta);
        for (size_t axis = 0U; axis < 3U; ++axis) {
          rotation_increment[axis] =
              (start_angular_velocity[axis] +
               0.5f * angular_velocity_delta[axis]) *
                  dt +
              (dt * dt / 12.0f) * bracket[axis];
        }
        {
          float sculling_first[3];
          float sculling_second[3];

          cross(sculling_first, start_angular_velocity, specific_force_delta);
          cross(sculling_second, angular_velocity_delta, start_specific_force);
          for (size_t axis = 0U; axis < 3U; ++axis) {
            velocity_increment[axis] =
                (start_specific_force[axis] +
                 0.5f * specific_force_delta[axis]) *
                    dt +
                (dt * dt / 12.0f) *
                    (sculling_first[axis] - sculling_second[axis]);
            position_increment[axis] =
                -(dt * dt / 12.0f) * specific_force_delta[axis];
            midpoint_specific_force[axis] = start_specific_force[axis] +
                                            0.5f * specific_force_delta[axis];
          }
        }
      } else {
        for (size_t axis = 0U; axis < 3U; ++axis) {
          angular_velocity_delta[axis] = 0.0f;
          specific_force_delta[axis] = 0.0f;
          rotation_increment[axis] = corrected_angular_velocity[axis] * dt;
          velocity_increment[axis] = corrected_specific_force[axis] * dt;
          position_increment[axis] = 0.0f;
          midpoint_specific_force[axis] = corrected_specific_force[axis];
        }
      }

      quaternion_to_rotation(previous_rotation, state->delta_quaternion);
      quaternion_exp(increment_quaternion, rotation_increment);
      /* The Jacobians read the pre-update preintegral, so compose them
       * before the nominal state advances. */
      compose_jacobians(state, rotation_increment, midpoint_specific_force,
                        angular_velocity_delta, specific_force_delta, dt,
                        previous_rotation, increment_quaternion);
      compose_interval(state, rotation_increment, velocity_increment,
                       position_increment, dt, previous_rotation,
                       increment_quaternion);
      state->integration_time_s += dt;
      state->sample_count++;
      integrated = true;
    } else {
      state->rejected_sample_count++;
    }
  }

  vector_copy(state->previous_gyroscope_rad_s, gyroscope_rad_s);
  vector_copy(state->previous_accelerometer_m_s2, accelerometer_m_s2);
  state->previous_timestamp_ns = timestamp_ns;
  state->previous_sample_valid = true;
  return integrated;
}

void rdd2_imu_preintegrator_close(struct rdd2_imu_preintegrator *state,
                                  struct rdd2_imu_packet *packet,
                                  uint64_t timestamp_ns,
                                  const float next_gyroscope_bias_rad_s[3],
                                  const float next_accelerometer_bias_m_s2[3]) {
  float inverse_integration_time;

  /* deltaAngle is the logarithm of the accumulated rotation, exactly as
   * Vehicles.Rdd2.WaypointVehicleSystem forms it at the packet boundary. */
  quaternion_log(packet->delta_angle_rad, state->delta_quaternion);
  vector_copy(packet->delta_velocity_m_s, state->delta_velocity_m_s);
  vector_copy(packet->delta_position_m, state->delta_position_m);
  for (size_t element = 0U; element < 4U; ++element) {
    packet->delta_quaternion[element] = state->delta_quaternion[element];
  }
  matrix_copy(packet->rotation_gyroscope_bias_jacobian_s,
              state->rotation_gyroscope_bias_jacobian_s);
  matrix_copy(packet->velocity_gyroscope_bias_jacobian_m,
              state->velocity_gyroscope_bias_jacobian_m);
  matrix_copy(packet->velocity_accelerometer_bias_jacobian_s,
              state->velocity_accelerometer_bias_jacobian_s);
  matrix_copy(packet->position_gyroscope_bias_jacobian_m_s,
              state->position_gyroscope_bias_jacobian_m_s);
  matrix_copy(packet->position_accelerometer_bias_jacobian_s2,
              state->position_accelerometer_bias_jacobian_s2);
  vector_copy(packet->gyroscope_bias_linearization_rad_s,
              state->gyroscope_bias_linearization_rad_s);
  vector_copy(packet->accelerometer_bias_linearization_m_s2,
              state->accelerometer_bias_linearization_m_s2);
  packet->integration_time_s = state->integration_time_s;
  packet->timestamp_ns = timestamp_ns;
  packet->sample_count = state->sample_count;
  packet->valid = state->integration_time_s > 0.0f;

  inverse_integration_time =
      packet->valid ? (1.0f / state->integration_time_s) : 0.0f;
  for (size_t axis = 0U; axis < 3U; ++axis) {
    packet->angular_velocity_rad_s[axis] =
        packet->delta_angle_rad[axis] * inverse_integration_time;
    packet->specific_force_m_s2[axis] =
        packet->delta_velocity_m_s[axis] * inverse_integration_time;
  }

  rdd2_imu_preintegrator_restart(state, next_gyroscope_bias_rad_s,
                                 next_accelerometer_bias_m_s2);
}
