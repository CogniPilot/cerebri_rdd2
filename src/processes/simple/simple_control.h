/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_PROCESSES_SIMPLE_CONTROL_H_
#define RDD2_PROCESSES_SIMPLE_CONTROL_H_

/*
 * Shared tuning constants for the hand-written stand-in control backend.
 *
 * Every gain here is an intentionally conservative bench placeholder chosen for
 * props-off verification of sign conventions and data flow, not for flight
 * performance. They are collected in one place so a bench operator can find and
 * adjust them without hunting through the process files.
 *
 * Coordinate frames used throughout the stand-in backend:
 *   - Body frame is FLU: +x forward, +y left, +z up.
 *   - World frame is ENU: +x east, +y north, +z up.
 *   - Attitude quaternion is world-from-body in (w, x, y, z) order, so it
 *     rotates a vector expressed in the body frame into the world frame.
 *   - Euler angles use the body-to-world sequence Rz(yaw) * Ry(pitch) * Rx(roll):
 *       roll  is rotation about body +x (forward), +roll is right-side-down.
 *       pitch is rotation about body +y (left),    +pitch is nose-down.
 *       yaw   is rotation about world +z (up),     +yaw turns the nose left
 *                                                  (east toward north).
 *   - The ManualControl pitch stick is nose-up positive per the schema, which
 *     is opposite to +pitch above, so guidance negates it where consumed.
 */

/* Standard gravity magnitude, m/s^2. */
#define SIMPLE_GRAVITY_M_S2 9.80665f

/*
 * Estimator complementary-filter blend applied to the accelerometer gravity
 * reference each estimator step. At the 800 Hz estimator release this gives a
 * correction time constant of roughly 62 ms, fast enough that sustained
 * lateral acceleration inside the trust band biases the tilt estimate within
 * a fraction of a second. Acceptable for props-off sign checks, retune before
 * any powered test.
 */
#define SIMPLE_ACCEL_BLEND 0.02f

/*
 * Accelerometer magnitude acceptance band, as a fraction of gravity. The
 * gravity reference is only blended in when the measured specific-force
 * magnitude is close to 1 g, so linear acceleration does not corrupt the tilt
 * estimate.
 */
#define SIMPLE_ACCEL_TRUST_LOW 0.7f
#define SIMPLE_ACCEL_TRUST_HIGH 1.3f

/* Maximum integration step the estimator will trust between IMU samples, s. */
#define SIMPLE_MAX_DT_S 0.02f

/* Stick-to-lean-angle scale for attitude mode, rad at full stick (~20 deg). */
#define SIMPLE_MAX_TILT_RAD 0.35f

/* Stick-to-yaw-rate scale, rad/s at full stick. */
#define SIMPLE_MAX_YAW_RATE_RAD_S 1.5f

/* Stick-to-body-rate scale for acro (rate) mode, rad/s at full stick. */
#define SIMPLE_MAX_ACRO_RATE_RAD_S 3.0f

/* Attitude-P gain: attitude error (rad) to commanded body rate (rad/s). */
#define SIMPLE_ATT_P 3.0f

/* Rate-P gains: body-rate error (rad/s) to normalized moment demand. */
#define SIMPLE_RATE_P_ROLL 0.05f
#define SIMPLE_RATE_P_PITCH 0.05f
#define SIMPLE_RATE_P_YAW 0.05f

/* Optical-flow freshness window, ns (100 ms, matching the tree sensor gates). */
#define SIMPLE_OPTICAL_FLOW_TIMEOUT_NS UINT64_C(100000000)

#endif /* RDD2_PROCESSES_SIMPLE_CONTROL_H_ */
