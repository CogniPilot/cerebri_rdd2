/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_PROCESSES_IMU_PREINTEGRATION_H_
#define RDD2_PROCESSES_IMU_PREINTEGRATION_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Closed-form IMU preintegration, the firmware half of the ESKF packet
 * boundary.
 *
 * The estimator on modelica_models main consumes an SE_2(3) preintegral
 * (delta angle, delta velocity, delta position, delta quaternion, the
 * integration time, and five bias sensitivities) rather than the raw
 * instantaneous sample the previous MixedInvariant estimator took. The
 * estimator thread already sees every IMU sample and used to discard the ones
 * between its releases; this accumulator consumes all of them and closes one
 * packet per estimator release, so no sample is thrown away and the estimator
 * rate becomes a free choice.
 *
 * Structure follows PX4 src/modules/sensors/Integrator.hpp: accumulate on
 * every sample against the previous sample and its timestamp, close and reset
 * on demand, carry the previous sample across the close so the interval that
 * spans a packet boundary is still integrated exactly once. ArduPilot's
 * AP_InertialSensor backend keeps the same shape.
 *
 * The per-interval composition is a transcription of
 * Estimation.StrapdownINS.preintegrateImuStep from modelica_models, not an
 * independent derivation. Keeping the same closed form is the point: the
 * packet the flight code hands the generated estimator is bit-for-bit the
 * packet the model hands the same block in simulation, so every mission,
 * consistency, and hold-order result measured on the model applies to the
 * artifact that flies. The zero-order hold is the model's flight default
 * (Vehicles.Rdd2.WaypointVehicleSystem useFirstOrderHoldImu = false); the
 * first-order hold reproduces Vehicles.Rdd2.Test.FohGlobalWaypointMission and
 * is the trapezoid-plus-coning form PX4 and ArduPilot use.
 */

/* Bounds on the measured sample interval. A sample outside them is a
 * transport fault, not a measurement, so it advances the sample history
 * without contributing an interval. */
#define RDD2_IMU_PREINTEGRATION_MIN_DT_S 1.0e-5f
#define RDD2_IMU_PREINTEGRATION_MAX_DT_S 0.02f

struct rdd2_imu_preintegrator {
  float delta_position_m[3];
  float delta_velocity_m_s[3];
  float delta_quaternion[4];
  float rotation_gyroscope_bias_jacobian_s[3][3];
  float velocity_gyroscope_bias_jacobian_m[3][3];
  float velocity_accelerometer_bias_jacobian_s[3][3];
  float position_gyroscope_bias_jacobian_m_s[3][3];
  float position_accelerometer_bias_jacobian_s2[3][3];
  float gyroscope_bias_linearization_rad_s[3];
  float accelerometer_bias_linearization_m_s2[3];
  float previous_gyroscope_rad_s[3];
  float previous_accelerometer_m_s2[3];
  float integration_time_s;
  /* Interval to attribute to the first sample after a reset, where there is
   * no earlier timestamp to difference against. The model integrates every
   * sample over its nominal period including the first, so dropping it here
   * would make the first window one interval short of the model's. Zero
   * disables the fallback and the first sample only starts the history. */
  float nominal_sample_period_s;
  uint64_t previous_timestamp_ns;
  uint32_t sample_count;
  uint32_t rejected_sample_count;
  bool previous_sample_valid;
  bool first_order_hold;
};

struct rdd2_imu_packet {
  float delta_angle_rad[3];
  float delta_velocity_m_s[3];
  float delta_position_m[3];
  float delta_quaternion[4];
  float rotation_gyroscope_bias_jacobian_s[3][3];
  float velocity_gyroscope_bias_jacobian_m[3][3];
  float velocity_accelerometer_bias_jacobian_s[3][3];
  float position_gyroscope_bias_jacobian_m_s[3][3];
  float position_accelerometer_bias_jacobian_s2[3][3];
  float gyroscope_bias_linearization_rad_s[3];
  float accelerometer_bias_linearization_m_s2[3];
  /* Packet-mean rate and specific force, formed the way
   * Vehicles.Rdd2.WaypointVehicleSystem forms them at the estimator
   * boundary: the accumulated increment divided by the integration time. */
  float angular_velocity_rad_s[3];
  float specific_force_m_s2[3];
  float integration_time_s;
  uint64_t timestamp_ns;
  uint32_t sample_count;
  bool valid;
};

/* Start a new integration window anchored on the supplied bias estimate.
 * The sample history is deliberately preserved so the interval spanning the
 * packet boundary is integrated into the new packet. */
void rdd2_imu_preintegrator_restart(struct rdd2_imu_preintegrator *state,
                                    const float gyroscope_bias_rad_s[3],
                                    const float accelerometer_bias_m_s2[3]);

/* Forget the sample history as well, for an estimator reset or a stream
 * dropout: the next sample starts a fresh interval rather than integrating
 * across the gap. */
void rdd2_imu_preintegrator_reset(struct rdd2_imu_preintegrator *state,
                                  const float gyroscope_bias_rad_s[3],
                                  const float accelerometer_bias_m_s2[3]);

/* Compose one sample interval into the running preintegral. Returns true when
 * the sample produced an interval. */
bool rdd2_imu_preintegrator_accumulate(struct rdd2_imu_preintegrator *state,
                                       const float gyroscope_rad_s[3],
                                       const float accelerometer_m_s2[3],
                                       uint64_t timestamp_ns);

/* Publish the accumulated window and restart it against the bias estimate the
 * consumer produced on its previous step. */
void rdd2_imu_preintegrator_close(struct rdd2_imu_preintegrator *state,
                                  struct rdd2_imu_packet *packet,
                                  uint64_t timestamp_ns,
                                  const float next_gyroscope_bias_rad_s[3],
                                  const float next_accelerometer_bias_m_s2[3]);

#endif /* RDD2_PROCESSES_IMU_PREINTEGRATION_H_ */
