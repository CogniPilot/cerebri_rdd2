/* SPDX-License-Identifier: Apache-2.0 */

#include <math.h>
#include <string.h>

#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

#include <zros/private/zros_node_struct.h>
#include <zros/private/zros_pub_struct.h>
#include <zros/private/zros_topic_struct.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>

#include "interfaces/zros_topics.h"

#define TEST_NS_FROM_US(value) ((uint64_t)(value) * UINT64_C(1000))

ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(manual_input,
                                   synapse_topic_ManualControlData_t);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(vehicle_health,
                                   synapse_topic_VehicleHealthData_t);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(navigation_odometry,
                                   synapse_topic_OdometryEstimateData_t);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(attitude_estimate,
                                   synapse_topic_AttitudeEstimateData_t);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(waypoint_plan, rdd2_waypoint_plan_t);

static bool position_source_ready;
static uint8_t planner_state;

bool rdd2_gnss_onboard_ready_get(void) { return position_source_ready; }

uint8_t rdd2_waypoint_mission_state_get(void) { return planner_state; }

bool rdd2_topic_has_sample(const struct zros_topic *topic) {
  return rdd2_topic_generation(topic) != 0U;
}

uint32_t rdd2_topic_generation(const struct zros_topic *topic) {
  return (uint32_t)atomic_get((atomic_t *)&topic->_lockless_generation);
}

#define CONFIG_RDD2_GNSS_SOURCE_ONBOARD 1
#include "../../../src/interfaces/mission_shell.c"
#undef CONFIG_RDD2_GNSS_SOURCE_ONBOARD

struct input_publishers {
  struct zros_node node;
  struct zros_pub manual_pub;
  struct zros_pub health_pub;
  struct zros_pub navigation_pub;
  struct zros_pub attitude_pub;
  synapse_topic_ManualControlData_t manual;
  synapse_topic_VehicleHealthData_t health;
  synapse_topic_OdometryEstimateData_t navigation;
  synapse_topic_AttitudeEstimateData_t attitude;
};

static struct input_publishers inputs;

static uint64_t wall_timestamp_ns(void) {
  uint64_t timestamp_ns = (uint64_t)k_uptime_get() * 1000000ULL;

  if (timestamp_ns == 0U) {
    k_sleep(K_MSEC(1));
    timestamp_ns = (uint64_t)k_uptime_get() * 1000000ULL;
  }
  return timestamp_ns;
}

static void publish_inputs(void) {
  zassert_ok(zros_pub_update(&inputs.manual_pub));
  zassert_ok(zros_pub_update(&inputs.health_pub));
  zassert_ok(zros_pub_update(&inputs.navigation_pub));
  zassert_ok(zros_pub_update(&inputs.attitude_pub));
}

static void valid_inputs(void) {
  uint64_t now_ns = wall_timestamp_ns();

  inputs.manual = (synapse_topic_ManualControlData_t){
      .timestamp_ns = now_ns,
      .flight_mode = 1U,
      .flags = synapse_topic_ManualControlFlags_Valid |
               synapse_topic_ManualControlFlags_Active,
  };
  inputs.health = (synapse_topic_VehicleHealthData_t){
      .timestamp_ns = now_ns,
      .sensors_health = synapse_topic_SensorComponentFlags_RadioControl,
  };
  inputs.navigation = (synapse_topic_OdometryEstimateData_t){
      .timestamp_ns = TEST_NS_FROM_US(UINT64_C(500000)),
      .position_enu_m = {.x = 12.0f, .y = -4.0f, .z = 2.0f},
      .attitude = {.w = 1.0f},
      .velocity_enu_m_s = {.x = 0.1f, .y = -0.1f, .z = 0.0f},
      .angular_velocity_flu_rad_s = {.roll = 0.01f,
                                     .pitch = 0.02f,
                                     .yaw = 0.03f},
      .quality_pct = 90U,
  };
  inputs.attitude = (synapse_topic_AttitudeEstimateData_t){
      .timestamp_ns = inputs.navigation.timestamp_ns,
      .attitude = {.w = 1.0f},
      .flags = synapse_topic_AttitudeEstimateFlags_AttitudeValid |
               synapse_topic_AttitudeEstimateFlags_RatesValid,
  };
  position_source_ready = true;
  publish_inputs();
}

static void *mission_shell_setup(void) {
  zros_node_init(&inputs.node, "mission_shell_test_inputs");
  zassert_ok(zros_pub_init(&inputs.manual_pub, &inputs.node,
                           &topic_manual_input, &inputs.manual));
  zassert_ok(zros_pub_init(&inputs.health_pub, &inputs.node,
                           &topic_vehicle_health, &inputs.health));
  zassert_ok(zros_pub_init(&inputs.navigation_pub, &inputs.node,
                           &topic_navigation_odometry, &inputs.navigation));
  zassert_ok(zros_pub_init(&inputs.attitude_pub, &inputs.node,
                           &topic_attitude_estimate, &inputs.attitude));
  return NULL;
}

static void mission_shell_before(void *fixture) {
  ARG_UNUSED(fixture);
  planner_state = RDD2_WAYPOINT_MISSION_EMPTY;
  valid_inputs();
}

static int run(const char *command) {
  return shell_execute_cmd(shell_backend_dummy_get_ptr(), command);
}

static void expect_rejected_without_publication(const char *command) {
  uint32_t generation = rdd2_topic_generation(&topic_waypoint_plan);

  zassert_not_equal(run(command), 0);
  zassert_equal(rdd2_topic_generation(&topic_waypoint_plan), generation);
}

ZTEST(mission_shell, test_first_command_uses_preexisting_input_publications) {
  rdd2_waypoint_plan_t plan;

  zassert_ok(run("mission box 1.5 0.25"));
  zassert_ok(zros_topic_read(&topic_waypoint_plan, &plan));
  zassert_true(plan.valid);
  zassert_false(plan.global_frame);
  zassert_equal(plan.waypoint_count, 5);
  zassert_equal(plan.nominal_speed, 0.25f);
  zassert_equal(plan.min_segment_duration, 2.0f);
  zassert_mem_equal(plan.waypoint[0], ((float[3]){0.0f, 0.0f, 0.0f}),
                    sizeof(plan.waypoint[0]));
  zassert_mem_equal(plan.waypoint[1], ((float[3]){1.5f, 0.0f, 0.0f}),
                    sizeof(plan.waypoint[1]));
  zassert_mem_equal(plan.waypoint[2], ((float[3]){1.5f, 1.5f, 0.0f}),
                    sizeof(plan.waypoint[2]));
  zassert_mem_equal(plan.waypoint[3], ((float[3]){0.0f, 1.5f, 0.0f}),
                    sizeof(plan.waypoint[3]));
  zassert_mem_equal(plan.waypoint[4], ((float[3]){0.0f, 0.0f, 0.0f}),
                    sizeof(plan.waypoint[4]));
  for (size_t index = 0U; index < RDD2_MAX_WAYPOINTS; ++index) {
    zassert_mem_equal(plan.velocity_enu[index], ((float[3]){0.0f, 0.0f, 0.0f}),
                      sizeof(plan.velocity_enu[index]));
    zassert_equal(plan.yaw[index], 0.0f);
  }
}

ZTEST(mission_shell, test_armed_and_failsafe_states_are_rejected) {
  inputs.health.flags = synapse_topic_VehicleHealthFlags_Armed;
  publish_inputs();
  expect_rejected_without_publication("mission box 1.0 0.2");

  valid_inputs();
  inputs.health.flags = synapse_topic_VehicleHealthFlags_Failsafe;
  publish_inputs();
  expect_rejected_without_publication("mission box 1.0 0.2");
}

ZTEST(mission_shell, test_stale_or_invalid_manual_and_health_are_rejected) {
  uint64_t now_ns = wall_timestamp_ns();

  inputs.manual.timestamp_ns = now_ns - MISSION_INPUT_MAX_AGE_NS - 1U;
  publish_inputs();
  expect_rejected_without_publication("mission box 1.0 0.2");

  valid_inputs();
  inputs.health.timestamp_ns =
      wall_timestamp_ns() - MISSION_INPUT_MAX_AGE_NS - 1U;
  publish_inputs();
  expect_rejected_without_publication("mission box 1.0 0.2");

  valid_inputs();
  inputs.manual.flags = 0U;
  publish_inputs();
  expect_rejected_without_publication("mission box 1.0 0.2");

  valid_inputs();
  inputs.health.sensors_health = 0U;
  publish_inputs();
  expect_rejected_without_publication("mission box 1.0 0.2");
}

ZTEST(mission_shell, test_arm_switch_high_and_unready_source_are_rejected) {
  inputs.manual.flags |= synapse_topic_ManualControlFlags_ArmSwitch;
  publish_inputs();
  expect_rejected_without_publication("mission box 1.0 0.2");

  valid_inputs();
  position_source_ready = false;
  expect_rejected_without_publication("mission box 1.0 0.2");
}

ZTEST(mission_shell, test_kill_switch_is_rejected) {
  inputs.manual.flags |= synapse_topic_ManualControlFlags_KillSwitch;
  publish_inputs();
  expect_rejected_without_publication("mission box 1.0 0.2");
}

ZTEST(mission_shell, test_invalid_navigation_is_rejected) {
  inputs.navigation.quality_pct = 0U;
  publish_inputs();
  expect_rejected_without_publication("mission box 1.0 0.2");

  valid_inputs();
  inputs.navigation.position_enu_m.x = NAN;
  publish_inputs();
  expect_rejected_without_publication("mission box 1.0 0.2");

  valid_inputs();
  inputs.navigation.timestamp_ns = 0U;
  publish_inputs();
  expect_rejected_without_publication("mission box 1.0 0.2");

  valid_inputs();
  inputs.navigation.quality_pct = -1;
  publish_inputs();
  expect_rejected_without_publication("mission box 1.0 0.2");
}

ZTEST(mission_shell, test_navigation_control_time_freshness_is_bounded) {
  inputs.attitude.timestamp_ns = inputs.navigation.timestamp_ns - 1U;
  publish_inputs();
  expect_rejected_without_publication("mission box 1.0 0.2");

  valid_inputs();
  inputs.attitude.timestamp_ns =
      inputs.navigation.timestamp_ns + MISSION_INPUT_MAX_AGE_NS;
  publish_inputs();
  zassert_ok(run("mission box 1.0 0.2"));

  valid_inputs();
  inputs.attitude.timestamp_ns =
      inputs.navigation.timestamp_ns + MISSION_INPUT_MAX_AGE_NS + 1U;
  publish_inputs();
  expect_rejected_without_publication("mission box 1.0 0.2");
}

ZTEST(mission_shell, test_invalid_attitude_release_is_rejected) {
  inputs.attitude.flags = 0U;
  publish_inputs();
  expect_rejected_without_publication("mission box 1.0 0.2");

  valid_inputs();
  inputs.attitude.timestamp_ns = 0U;
  publish_inputs();
  expect_rejected_without_publication("mission box 1.0 0.2");

  valid_inputs();
  inputs.attitude.attitude.x = INFINITY;
  publish_inputs();
  expect_rejected_without_publication("mission box 1.0 0.2");
}

ZTEST(mission_shell, test_numeric_syntax_and_bounds_are_rejected) {
  const char *const commands[] = {
      "mission box nan 0.2",  "mission box inf 0.2",  "mission box 1oops 0.2",
      "mission box 0.49 0.2", "mission box 3.01 0.2", "mission box 1.0 0.09",
      "mission box 1.0 0.51",
  };

  for (size_t index = 0U; index < ARRAY_SIZE(commands); ++index) {
    expect_rejected_without_publication(commands[index]);
  }
}

ZTEST(mission_shell, test_bounds_are_inclusive) {
  zassert_ok(run("mission box 0.5 0.1"));
  zassert_ok(run("mission box 3.0 0.5"));
}

ZTEST(mission_shell, test_status_reports_authoritative_planner_state) {
  const struct shell *sh = shell_backend_dummy_get_ptr();
  const char *output;
  size_t output_size;

  zassert_ok(run("mission box 2.0 0.3"));
  shell_backend_dummy_clear_output(sh);
  zassert_ok(run("mission status"));
  output = shell_backend_dummy_get_output(sh, &output_size);
  zassert_not_null(output);
  zassert_not_null(strstr(output, "ingress=published"));
  zassert_not_null(strstr(output, "planner=empty"));
  zassert_not_null(strstr(output, "ingress_sequence="));
  zassert_is_null(strstr(output, " planner=empty sequence="));
  zassert_is_null(strstr(output, "loaded"));
  zassert_is_null(strstr(output, "accepted"));
  zassert_is_null(strstr(output, "pending"));
  zassert_is_null(strstr(output, "running"));
  zassert_not_null(strstr(output, "side_mm=2000"));
  zassert_not_null(strstr(output, "speed_mm_s=300"));

  planner_state = RDD2_WAYPOINT_MISSION_PENDING;
  shell_backend_dummy_clear_output(sh);
  zassert_ok(run("mission status"));
  output = shell_backend_dummy_get_output(sh, &output_size);
  zassert_not_null(output);
  zassert_not_null(strstr(output, "planner=pending"));

  planner_state = RDD2_WAYPOINT_MISSION_RUNNING;
  shell_backend_dummy_clear_output(sh);
  zassert_ok(run("mission status"));
  output = shell_backend_dummy_get_output(sh, &output_size);
  zassert_not_null(output);
  zassert_not_null(strstr(output, "planner=running"));

  planner_state = RDD2_WAYPOINT_MISSION_ABORTED;
  shell_backend_dummy_clear_output(sh);
  zassert_ok(run("mission status"));
  output = shell_backend_dummy_get_output(sh, &output_size);
  zassert_not_null(output);
  zassert_not_null(strstr(output, "planner=aborted"));
}

ZTEST(mission_shell,
      test_cancel_publishes_invalid_new_sequence_and_updates_status) {
  const struct shell *sh = shell_backend_dummy_get_ptr();
  rdd2_waypoint_plan_t loaded;
  rdd2_waypoint_plan_t cancelled;
  const char *output;
  size_t output_size;

  zassert_ok(run("mission box 1.0 0.2"));
  zassert_ok(zros_topic_read(&topic_waypoint_plan, &loaded));
  zassert_ok(run("mission cancel"));
  zassert_ok(zros_topic_read(&topic_waypoint_plan, &cancelled));
  zassert_false(cancelled.valid);
  zassert_false(cancelled.global_frame);
  zassert_equal(cancelled.waypoint_count, 0);
  zassert_not_equal(cancelled.sequence, loaded.sequence);
  shell_backend_dummy_clear_output(sh);
  zassert_ok(run("mission status"));
  output = shell_backend_dummy_get_output(sh, &output_size);
  zassert_not_null(output);
  zassert_not_null(strstr(output, "ingress=cancelled"));
}

ZTEST_SUITE(mission_shell, NULL, mission_shell_setup, mission_shell_before,
            NULL, NULL);
