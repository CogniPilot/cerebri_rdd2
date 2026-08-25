/* SPDX-License-Identifier: Apache-2.0 */

#include <math.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "Vehicles_Rdd2_GuidanceController.h"

struct guidance_case {
  int32_t mode;
  float reference_z_m;
};

static float expect_valid_step(const struct guidance_case *test_case) {
  GuidanceControllerState state;

  memset(&state, 0, sizeof(state));
  GuidanceController_startup(&state);
  zassert_equal(state.rumoca_galec_error_signal_status, 0U,
                "Guidance startup reported generated status %u",
                state.rumoca_galec_error_signal_status);
  GuidanceController_recalibrate(&state);
  zassert_equal(state.rumoca_galec_error_signal_status, 0U,
                "Guidance recalibration reported generated status %u",
                state.rumoca_galec_error_signal_status);

  state.mode = test_case->mode;
  state.quaternionWorldBody[0] = 1.0f;
  state.positionWorld_m[2] = test_case->reference_z_m;
  GuidanceController_dostep(&state);

  zassert_equal(state.rumoca_galec_error_signal_status, 0U,
                "Guidance mode %d reported generated status %u",
                test_case->mode, state.rumoca_galec_error_signal_status);
  zassert_true(isfinite(state.thrust_N),
               "Guidance mode %d produced nonfinite thrust",
               test_case->mode);
  for (size_t axis = 0U; axis < 3U; ++axis) {
    zassert_true(isfinite(state.angularVelocityCommandFlu_rad_s[axis]),
                 "Guidance mode %d produced nonfinite rate axis %u",
                 test_case->mode, (unsigned int)axis);
    zassert_true(fabsf(state.angularVelocityCommandFlu_rad_s[axis]) < 1.0e-5f,
                 "Guidance mode %d produced unexpected rate axis %u: %g",
                 test_case->mode, (unsigned int)axis,
                 (double)state.angularVelocityCommandFlu_rad_s[axis]);
  }
  return state.thrust_N;
}

ZTEST(generated_guidance_validity,
      test_all_modes_report_success_and_finite_commands) {
  const struct guidance_case cases[] = {
      {.mode = 0, .reference_z_m = 0.0f},
      {.mode = 1, .reference_z_m = 0.0f},
      {.mode = 2, .reference_z_m = 0.0f},
      {.mode = 2, .reference_z_m = 1.0f},
  };

  const float manual_thrust = expect_valid_step(&cases[0]);
  const float attitude_thrust = expect_valid_step(&cases[1]);
  const float hover_thrust = expect_valid_step(&cases[2]);
  const float climb_thrust = expect_valid_step(&cases[3]);

  zassert_true(fabsf(manual_thrust) < 1.0e-5f,
               "ACRO zero-input thrust changed: %g", (double)manual_thrust);
  zassert_true(fabsf(attitude_thrust) < 1.0e-5f,
               "ATTITUDE zero-input thrust changed: %g",
               (double)attitude_thrust);
  zassert_true(fabsf(hover_thrust - 19.6f) < 1.0e-3f,
               "POSITION hover thrust changed: %g", (double)hover_thrust);
  /* 23.6 N is m * (g + positionGain_s2[3] * 1 m) = 2 * (9.8 + 2.0). The
   * position law is now stated as separate position and velocity gains in
   * Control.Multirotor.LogLinear rather than one feedback gain, and the
   * vertical position gain that falls out of it is 2.0 per second squared. */
  zassert_true(fabsf(climb_thrust - 23.6f) < 1.0e-3f,
               "POSITION altitude response changed: %g",
               (double)climb_thrust);
  zassert_true(climb_thrust > hover_thrust + 1.0f,
               "POSITION altitude reference did not increase thrust");
}

ZTEST(generated_guidance_validity,
      test_small_flight_attitude_stays_finite_in_float32) {
  GuidanceControllerState state;

  memset(&state, 0, sizeof(state));
  GuidanceController_startup(&state);
  GuidanceController_recalibrate(&state);

  state.mode = 1;
  state.armed = true;
  state.stick[0] = 0.25f;
  state.throttle = 0.703f;
  state.quaternionWorldBody[0] = 1.0f;
  state.quaternionWorldBody[1] = 8.3571314e-05f;
  state.positionWorldEnu_m[2] = 0.18f;

  for (size_t step = 0U; step < 10U; ++step) {
    GuidanceController_dostep(&state);
    zassert_equal(state.rumoca_galec_error_signal_status, 0U,
                  "small flight attitude failed at step %u with status %u",
                  (unsigned int)step,
                  state.rumoca_galec_error_signal_status);
    zassert_true(isfinite(state.thrust_N),
                 "small flight attitude produced nonfinite thrust");
    for (size_t axis = 0U; axis < 3U; ++axis) {
      zassert_true(isfinite(state.angularVelocityCommandFlu_rad_s[axis]),
                   "small flight attitude produced nonfinite rate axis %u",
                   (unsigned int)axis);
    }
  }

  zassert_true(fabsf(state.thrust_N - 20.4479f) < 1.0e-3f,
               "small flight attitude thrust changed: %g",
               (double)state.thrust_N);
  zassert_true(
      fabsf(state.angularVelocityCommandFlu_rad_s[0] - 0.305098f) < 1.0e-4f,
      "small flight attitude roll-rate response changed: %g",
      (double)state.angularVelocityCommandFlu_rad_s[0]);
  zassert_true(fabsf(state.angularVelocityCommandFlu_rad_s[1]) < 1.0e-5f &&
                   fabsf(state.angularVelocityCommandFlu_rad_s[2]) < 1.0e-5f,
               "small flight attitude leaked into pitch/yaw rates: %g, %g",
               (double)state.angularVelocityCommandFlu_rad_s[1],
               (double)state.angularVelocityCommandFlu_rad_s[2]);
}

ZTEST_SUITE(generated_guidance_validity, NULL, NULL, NULL, NULL, NULL);
