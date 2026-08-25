/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The firmware preintegrator is a transcription of
 * Estimation.StrapdownINS.preintegrateImuStep, so it is checked the way the
 * model checks itself: the nominal composition against an independent
 * fine-grid integration of the same differential equation, and the bias
 * sensitivities against finite differences of the composition they linearize.
 * Neither reference reuses the closed form under test.
 */

#include "imu_preintegration.h"

#include <math.h>
#include <string.h>
#include <zephyr/ztest.h>

#define SAMPLE_DT_S 0.00125 /* 800 Hz */
#define PACKET_SAMPLES 8    /* 100 Hz packet */

static const float g_gyro[3] = {0.35f, -0.22f, 0.18f};
static const float g_accel[3] = {0.7f, -0.4f, 9.2f};
static const float g_zero[3] = {0.0f, 0.0f, 0.0f};

static void run_packet(struct rdd2_imu_packet *packet,
                       const float gyroscope_bias[3],
                       const float accelerometer_bias[3], bool first_order_hold) {
  struct rdd2_imu_preintegrator state;
  uint64_t timestamp_ns = UINT64_C(1000000000);
  const uint64_t step_ns = (uint64_t)(SAMPLE_DT_S * 1.0e9 + 0.5);

  memset(&state, 0, sizeof(state));
  state.first_order_hold = first_order_hold;
  rdd2_imu_preintegrator_reset(&state, gyroscope_bias, accelerometer_bias);
  /* Prime the sample history the way flight does, where it carries over from
   * the previous packet, so all PACKET_SAMPLES intervals are integrated. */
  (void)rdd2_imu_preintegrator_accumulate(&state, g_gyro, g_accel,
                                          timestamp_ns);
  for (int sample = 0; sample < PACKET_SAMPLES; ++sample) {
    timestamp_ns += step_ns;
    (void)rdd2_imu_preintegrator_accumulate(&state, g_gyro, g_accel,
                                            timestamp_ns);
  }
  rdd2_imu_preintegrator_close(&state, packet, timestamp_ns, gyroscope_bias,
                               accelerometer_bias);
}

/* Independent double-precision reference: dR/dt = R*wedge(w), dv/dt = R*a,
 * dp/dt = v, stepped far finer than the sample interval. */
static void reference_integrate(double total_time_s, long steps,
                                double position[3], double velocity[3],
                                double rotation[3][3]) {
  const double h = total_time_s / (double)steps;
  const double skew[3][3] = {
      {0.0, -(double)g_gyro[2], (double)g_gyro[1]},
      {(double)g_gyro[2], 0.0, -(double)g_gyro[0]},
      {-(double)g_gyro[1], (double)g_gyro[0], 0.0},
  };

  memset(position, 0, 3 * sizeof(double));
  memset(velocity, 0, 3 * sizeof(double));
  memset(rotation, 0, 9 * sizeof(double));
  rotation[0][0] = 1.0;
  rotation[1][1] = 1.0;
  rotation[2][2] = 1.0;
  for (long step = 0; step < steps; ++step) {
    double world_acceleration[3];
    double increment[3][3];
    double skew_squared[3][3];
    double updated[3][3];

    for (int row = 0; row < 3; ++row) {
      world_acceleration[row] = rotation[row][0] * (double)g_accel[0] +
                                rotation[row][1] * (double)g_accel[1] +
                                rotation[row][2] * (double)g_accel[2];
    }
    for (int row = 0; row < 3; ++row) {
      position[row] += velocity[row] * h + 0.5 * world_acceleration[row] * h * h;
      velocity[row] += world_acceleration[row] * h;
    }
    for (int row = 0; row < 3; ++row) {
      for (int column = 0; column < 3; ++column) {
        skew_squared[row][column] = 0.0;
        for (int inner = 0; inner < 3; ++inner) {
          skew_squared[row][column] += skew[row][inner] * skew[inner][column];
        }
      }
    }
    for (int row = 0; row < 3; ++row) {
      for (int column = 0; column < 3; ++column) {
        increment[row][column] = (row == column) ? 1.0 : 0.0;
        increment[row][column] +=
            skew[row][column] * h + 0.5 * skew_squared[row][column] * h * h;
      }
    }
    for (int row = 0; row < 3; ++row) {
      for (int column = 0; column < 3; ++column) {
        updated[row][column] = 0.0;
        for (int inner = 0; inner < 3; ++inner) {
          updated[row][column] +=
              rotation[row][inner] * increment[inner][column];
        }
      }
    }
    memcpy(rotation, updated, sizeof(updated));
  }
}

static void quaternion_to_rotation(double rotation[3][3],
                                   const float quaternion[4]) {
  const double a = quaternion[0];
  const double b = quaternion[1];
  const double c = quaternion[2];
  const double d = quaternion[3];

  rotation[0][0] = a * a + b * b - c * c - d * d;
  rotation[0][1] = 2.0 * (b * c - a * d);
  rotation[0][2] = 2.0 * (b * d + a * c);
  rotation[1][0] = 2.0 * (b * c + a * d);
  rotation[1][1] = a * a - b * b + c * c - d * d;
  rotation[1][2] = 2.0 * (c * d - a * b);
  rotation[2][0] = 2.0 * (b * d - a * c);
  rotation[2][1] = 2.0 * (c * d + a * b);
  rotation[2][2] = a * a - b * b - c * c + d * d;
}

ZTEST(process_wrapper_fault_injection,
      test_preintegration_matches_a_fine_grid_integration) {
  struct rdd2_imu_packet packet;
  double position[3];
  double velocity[3];
  double rotation[3][3];
  double packet_rotation[3][3];

  run_packet(&packet, g_zero, g_zero, false);
  reference_integrate(SAMPLE_DT_S * PACKET_SAMPLES, 200000, position, velocity,
                      rotation);

  zexpect_true(packet.valid);
  zexpect_within(packet.integration_time_s,
                 (float)(SAMPLE_DT_S * PACKET_SAMPLES), 1.0e-7f);
  zexpect_equal(packet.sample_count, (uint32_t)PACKET_SAMPLES);
  quaternion_to_rotation(packet_rotation, packet.delta_quaternion);
  for (int row = 0; row < 3; ++row) {
    zexpect_true(fabs((double)packet.delta_position_m[row] - position[row]) <
                 1.0e-7);
    zexpect_true(fabs((double)packet.delta_velocity_m_s[row] - velocity[row]) <
                 1.0e-4);
    /* The packet mean rate is what the block receives as
     * angularVelocityBodyFlu_rad_s; on a constant stream it is the input. */
    zexpect_within(packet.angular_velocity_rad_s[row], g_gyro[row], 1.0e-4f);
    for (int column = 0; column < 3; ++column) {
      zexpect_true(fabs(packet_rotation[row][column] -
                        rotation[row][column]) < 1.0e-5);
    }
  }
}

ZTEST(process_wrapper_fault_injection,
      test_first_order_hold_reduces_to_the_zero_order_hold_on_a_held_input) {
  struct rdd2_imu_packet zero_order;
  struct rdd2_imu_packet first_order;

  run_packet(&zero_order, g_zero, g_zero, false);
  run_packet(&first_order, g_zero, g_zero, true);
  for (int axis = 0; axis < 3; ++axis) {
    zexpect_within(first_order.delta_angle_rad[axis],
                   zero_order.delta_angle_rad[axis], 1.0e-6f);
    zexpect_within(first_order.delta_velocity_m_s[axis],
                   zero_order.delta_velocity_m_s[axis], 1.0e-6f);
  }
}

ZTEST(process_wrapper_fault_injection,
      test_bias_jacobians_match_finite_differences) {
  const double perturbation = 1.0e-4;
  struct rdd2_imu_packet nominal;

  run_packet(&nominal, g_zero, g_zero, false);
  for (int column = 0; column < 3; ++column) {
    struct rdd2_imu_packet perturbed;
    float anchor[3] = {0.0f, 0.0f, 0.0f};
    double nominal_rotation[3][3];
    double perturbed_rotation[3][3];
    double relative[3][3];
    double trace;
    double angle;
    double scale;
    double numeric[3];

    anchor[column] = (float)perturbation;
    run_packet(&perturbed, anchor, g_zero, false);
    quaternion_to_rotation(nominal_rotation, nominal.delta_quaternion);
    quaternion_to_rotation(perturbed_rotation, perturbed.delta_quaternion);
    for (int row = 0; row < 3; ++row) {
      for (int inner_column = 0; inner_column < 3; ++inner_column) {
        relative[row][inner_column] = 0.0;
        for (int inner = 0; inner < 3; ++inner) {
          relative[row][inner_column] +=
              nominal_rotation[inner][row] * perturbed_rotation[inner][inner_column];
        }
      }
    }
    trace = relative[0][0] + relative[1][1] + relative[2][2];
    angle = acos(fmin(1.0, fmax(-1.0, 0.5 * (trace - 1.0))));
    scale = (angle < 1.0e-9) ? 0.5 : angle / (2.0 * sin(angle));
    numeric[0] = scale * (relative[2][1] - relative[1][2]) / perturbation;
    numeric[1] = scale * (relative[0][2] - relative[2][0]) / perturbation;
    numeric[2] = scale * (relative[1][0] - relative[0][1]) / perturbation;
    for (int row = 0; row < 3; ++row) {
      zexpect_true(
          fabs(numeric[row] -
               (double)nominal.rotation_gyroscope_bias_jacobian_s[row][column]) <
          1.0e-3);
      zexpect_true(
          fabs(((double)perturbed.delta_velocity_m_s[row] -
                (double)nominal.delta_velocity_m_s[row]) / perturbation -
               (double)nominal.velocity_gyroscope_bias_jacobian_m[row][column]) <
          1.0e-2);
      zexpect_true(
          fabs(((double)perturbed.delta_position_m[row] -
                (double)nominal.delta_position_m[row]) / perturbation -
               (double)nominal.position_gyroscope_bias_jacobian_m_s[row][column]) <
          1.0e-4);
    }

    run_packet(&perturbed, g_zero, anchor, false);
    for (int row = 0; row < 3; ++row) {
      zexpect_true(
          fabs(((double)perturbed.delta_velocity_m_s[row] -
                (double)nominal.delta_velocity_m_s[row]) / perturbation -
               (double)
                   nominal.velocity_accelerometer_bias_jacobian_s[row][column]) <
          1.0e-3);
      zexpect_true(
          fabs(((double)perturbed.delta_position_m[row] -
                (double)nominal.delta_position_m[row]) / perturbation -
               (double)
                   nominal.position_accelerometer_bias_jacobian_s2[row][column]) <
          1.0e-4);
    }
  }
}

ZTEST(process_wrapper_fault_injection,
      test_packet_boundary_integrates_every_interval_exactly_once) {
  struct rdd2_imu_preintegrator state;
  struct rdd2_imu_packet first;
  struct rdd2_imu_packet second;
  uint64_t timestamp_ns = UINT64_C(5000000000);
  const uint64_t step_ns = (uint64_t)(SAMPLE_DT_S * 1.0e9 + 0.5);

  memset(&state, 0, sizeof(state));
  rdd2_imu_preintegrator_reset(&state, g_zero, g_zero);
  (void)rdd2_imu_preintegrator_accumulate(&state, g_gyro, g_accel,
                                          timestamp_ns);
  for (int sample = 0; sample < PACKET_SAMPLES; ++sample) {
    timestamp_ns += step_ns;
    (void)rdd2_imu_preintegrator_accumulate(&state, g_gyro, g_accel,
                                            timestamp_ns);
  }
  rdd2_imu_preintegrator_close(&state, &first, timestamp_ns, g_zero, g_zero);
  for (int sample = 0; sample < PACKET_SAMPLES; ++sample) {
    timestamp_ns += step_ns;
    (void)rdd2_imu_preintegrator_accumulate(&state, g_gyro, g_accel,
                                            timestamp_ns);
  }
  rdd2_imu_preintegrator_close(&state, &second, timestamp_ns, g_zero, g_zero);

  /* The interval spanning the boundary belongs to the second packet, so the
   * two windows tile the stream with no gap and no double count. */
  zexpect_equal(second.sample_count, (uint32_t)PACKET_SAMPLES);
  zexpect_within(first.integration_time_s + second.integration_time_s,
                 (float)(2.0 * SAMPLE_DT_S * PACKET_SAMPLES), 1.0e-6f);
}

ZTEST(process_wrapper_fault_injection,
      test_an_empty_window_produces_an_invalid_packet) {
  struct rdd2_imu_preintegrator state;
  struct rdd2_imu_packet packet;

  memset(&state, 0, sizeof(state));
  rdd2_imu_preintegrator_reset(&state, g_zero, g_zero);
  rdd2_imu_preintegrator_close(&state, &packet, UINT64_C(1000000000), g_zero,
                               g_zero);
  zexpect_false(packet.valid);
  zexpect_equal(packet.sample_count, 0U);
  zexpect_equal(packet.integration_time_s, 0.0f);
}
