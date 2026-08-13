/* SPDX-License-Identifier: Apache-2.0 */

#include <math.h>
#include <string.h>
#include <zephyr/ztest.h>

#include "Planning_Bezier_WaypointTrajectoryPlanner.h"

#define GENERATED_MISSION_WAYPOINT_COUNT 5

static void set_square_plan(WaypointTrajectoryPlannerState *state,
                            int32_t sequence, float origin_x, float origin_y,
                            float origin_z, float side) {
  const float waypoint[GENERATED_MISSION_WAYPOINT_COUNT][3] = {
      {origin_x, origin_y, origin_z},
      {origin_x + side, origin_y, origin_z},
      {origin_x + side, origin_y + side, origin_z},
      {origin_x, origin_y + side, origin_z},
      {origin_x, origin_y, origin_z},
  };

  state->plan_valid = true;
  state->plan_sequence = sequence;
  state->plan_waypointCount = GENERATED_MISSION_WAYPOINT_COUNT;
  state->globalFrame = false;
  memcpy(state->waypoint, waypoint, sizeof(waypoint));
  state->nominalSpeed = 0.25f;
  state->minSegmentDuration = 2.0f;
}

static void expect_real_generated_reference_usable(
    const WaypointTrajectoryPlannerState *state) {
  const float values[] = {
      state->reference_trajectoryTime,
      state->reference_totalDuration,
      state->position[0],
      state->position[1],
      state->position[2],
      state->velocity[0],
      state->velocity[1],
      state->velocity[2],
      state->acceleration[0],
      state->acceleration[1],
      state->acceleration[2],
      state->jerk[0],
      state->jerk[1],
      state->jerk[2],
      state->snap[0],
      state->snap[1],
      state->snap[2],
      state->reference_yaw,
      state->yawRate,
      state->yawAcceleration,
  };

  zexpect_equal(state->rumoca_galec_error_signal_status, 0U);
  zexpect_true(state->reference_valid);
  for (size_t index = 0U; index < ARRAY_SIZE(values); ++index) {
    zexpect_true(isfinite(values[index]), "generated output %zu is nonfinite",
                 index);
  }
}

ZTEST(waypoint_mission_ingress,
      test_real_v3_planner_accepts_advances_and_restarts) {
  WaypointTrajectoryPlannerState state;
  float first_total_duration;
  float previous_trajectory_time;

  memset(&state, 0, sizeof(state));
  WaypointTrajectoryPlanner_startup(&state);
  WaypointTrajectoryPlanner_recalibrate(&state);
  set_square_plan(&state, 900, 10.0f, -4.0f, 2.0f, 2.0f);
  WaypointTrajectoryPlanner_dostep(&state);
  expect_real_generated_reference_usable(&state);
  zexpect_equal(state.reference_sequence, 900);
  zexpect_within(state.reference_trajectoryTime, 0.0f, 1.0e-6f);
  zexpect_within(state.position[0], 10.0f, 1.0e-5f);
  zexpect_within(state.position[1], -4.0f, 1.0e-5f);
  zexpect_within(state.position[2], 2.0f, 1.0e-5f);
  first_total_duration = state.reference_totalDuration;
  zexpect_true(first_total_duration > 0.0f);

  state.plan_valid = false;
  previous_trajectory_time = state.reference_trajectoryTime;
  for (size_t tick = 0U; tick < 8U; ++tick) {
    WaypointTrajectoryPlanner_dostep(&state);
    expect_real_generated_reference_usable(&state);
    zexpect_equal(state.reference_sequence, 900);
    zexpect_true(state.reference_trajectoryTime > previous_trajectory_time);
    zexpect_within(state.reference_trajectoryTime, 0.02f * (float)(tick + 1U),
                   1.0e-5f);
    previous_trajectory_time = state.reference_trajectoryTime;
  }
  zexpect_true(state.position[0] > 10.0f,
               "real generated reference did not advance");

  set_square_plan(&state, 901, -3.0f, 5.0f, 1.5f, 1.0f);
  WaypointTrajectoryPlanner_dostep(&state);
  expect_real_generated_reference_usable(&state);
  zexpect_equal(state.reference_sequence, 901);
  zexpect_within(state.reference_trajectoryTime, 0.0f, 1.0e-6f);
  zexpect_within(state.position[0], -3.0f, 1.0e-5f);
  zexpect_within(state.position[1], 5.0f, 1.0e-5f);
  zexpect_within(state.position[2], 1.5f, 1.0e-5f);
  zexpect_true(state.reference_totalDuration > 0.0f);
  zexpect_true(state.reference_totalDuration < first_total_duration);
}
