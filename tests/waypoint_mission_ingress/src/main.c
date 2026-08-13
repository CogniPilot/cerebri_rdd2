/* SPDX-License-Identifier: Apache-2.0 */

#include <math.h>
#include <string.h>
#include <zephyr/ztest.h>

#define TEST_NS_FROM_US(value) ((uint64_t)(value) * UINT64_C(1000))

#include "../../../src/interfaces/data.h"

static bool planner_fake_source_ready;
static bool planner_fake_publish_failure;
static size_t planner_fake_publish_count;
static size_t planner_fake_startup_count;
static size_t planner_fake_step_count;
static bool planner_fake_plan_seen;
static float planner_fake_waypoint[5][3];
static synapse_topic_LocalPositionCommandData_t planner_fake_last_reference;

enum planner_fake_step_fault {
  PLANNER_FAKE_STEP_OK = 0,
  PLANNER_FAKE_STEP_ERROR,
  PLANNER_FAKE_STEP_NO_REFERENCE,
  PLANNER_FAKE_STEP_NONFINITE,
};

static enum planner_fake_step_fault planner_fake_step_fault;

#define CONFIG_RDD2_GNSS_SOURCE_ONBOARD 1
#define WaypointTrajectoryPlanner_dostep planner_fake_dostep
#define WaypointTrajectoryPlanner_recalibrate planner_fake_recalibrate
#define WaypointTrajectoryPlanner_startup planner_fake_startup
#define rdd2_gnss_onboard_ready_get planner_fake_gnss_onboard_ready_get
#define topic_attitude_estimate planner_fake_topic_attitude_estimate
#define topic_manual_input planner_fake_topic_manual_input
#define topic_navigation_odometry planner_fake_topic_navigation_odometry
#define topic_trajectory_reference planner_fake_topic_trajectory_reference
#define topic_vehicle_health planner_fake_topic_vehicle_health
#define topic_waypoint_plan planner_fake_topic_waypoint_plan
#define zros_node_init planner_fake_zros_node_init
#define zros_pub_init planner_fake_zros_pub_init
#define zros_pub_update planner_fake_zros_pub_update
#define zros_sub_init planner_fake_zros_sub_init
#define zros_sub_update planner_fake_zros_sub_update
#define zros_sub_wait planner_fake_zros_sub_wait

#include "../../../src/processes/waypoint_trajectory_planner.c"

#undef zros_sub_wait
#undef zros_sub_update
#undef zros_sub_init
#undef zros_pub_update
#undef zros_pub_init
#undef zros_node_init
#undef topic_waypoint_plan
#undef topic_vehicle_health
#undef topic_trajectory_reference
#undef topic_navigation_odometry
#undef topic_manual_input
#undef topic_attitude_estimate
#undef rdd2_gnss_onboard_ready_get
#undef WaypointTrajectoryPlanner_startup
#undef WaypointTrajectoryPlanner_recalibrate
#undef WaypointTrajectoryPlanner_dostep
#undef CONFIG_RDD2_GNSS_SOURCE_ONBOARD

struct zros_topic planner_fake_topic_attitude_estimate;
struct zros_topic planner_fake_topic_manual_input;
struct zros_topic planner_fake_topic_navigation_odometry;
struct zros_topic planner_fake_topic_trajectory_reference;
struct zros_topic planner_fake_topic_vehicle_health;
struct zros_topic planner_fake_topic_waypoint_plan;

bool planner_fake_gnss_onboard_ready_get(void) {
  return planner_fake_source_ready;
}

void planner_fake_startup(WaypointTrajectoryPlannerState *self) {
  planner_fake_startup_count++;
  memset(self, 0, sizeof(*self));
  self->maxWaypoints = RDD2_MAX_WAYPOINTS;
  self->previous_sequence = -1;
}

void planner_fake_recalibrate(WaypointTrajectoryPlannerState *self) {
  self->rumoca_galec_error_signal_status = 0U;
}

void planner_fake_dostep(WaypointTrajectoryPlannerState *self) {
  planner_fake_step_count++;
  self->rumoca_galec_error_signal_status = 0U;
  if (self->plan_valid) {
    planner_fake_plan_seen = true;
    for (size_t waypoint = 0U; waypoint < ARRAY_SIZE(planner_fake_waypoint);
         ++waypoint) {
      memcpy(planner_fake_waypoint[waypoint], self->waypoint[waypoint],
             sizeof(planner_fake_waypoint[waypoint]));
    }
  }
  self->reference_valid = true;
  self->position[0] = self->waypoint[0][0];
  self->position[1] = self->waypoint[0][1];
  self->position[2] = self->waypoint[0][2];
  self->velocity[0] = 0.1f;
  self->velocity[1] = 0.2f;
  self->velocity[2] = 0.3f;
  self->acceleration[0] = -0.1f;
  self->acceleration[1] = -0.2f;
  self->acceleration[2] = -0.3f;
  self->reference_yaw = 0.4f;
  self->yawRate = -0.5f;

  if (planner_fake_step_fault == PLANNER_FAKE_STEP_ERROR) {
    self->rumoca_galec_error_signal_status = UINT32_C(0x40);
  } else if (planner_fake_step_fault == PLANNER_FAKE_STEP_NO_REFERENCE) {
    self->reference_valid = false;
  } else if (planner_fake_step_fault == PLANNER_FAKE_STEP_NONFINITE) {
    self->acceleration[2] = NAN;
  }
}

void planner_fake_zros_node_init(struct zros_node *node, const char *name) {
  ARG_UNUSED(node);
  ARG_UNUSED(name);
}

int planner_fake_zros_sub_init(struct zros_sub *sub, struct zros_node *node,
                               struct zros_topic *topic, void *data,
                               double rate_limit_hz) {
  ARG_UNUSED(node);
  ARG_UNUSED(rate_limit_hz);
  sub->_topic = topic;
  sub->_data = data;
  return 0;
}

int planner_fake_zros_pub_init(struct zros_pub *pub, struct zros_node *node,
                               struct zros_topic *topic, void *data) {
  ARG_UNUSED(node);
  pub->_topic = topic;
  pub->_data = data;
  return 0;
}

int planner_fake_zros_sub_wait(struct zros_sub *sub, k_timeout_t timeout) {
  ARG_UNUSED(sub);
  ARG_UNUSED(timeout);
  return -1;
}

int planner_fake_zros_sub_update(struct zros_sub *sub) {
  ARG_UNUSED(sub);
  return -1;
}

int planner_fake_zros_pub_update(struct zros_pub *pub) {
  if (pub != &g_process.reference_pub || planner_fake_publish_failure) {
    return -1;
  }
  planner_fake_publish_count++;
  planner_fake_last_reference = g_process.reference;
  return 0;
}

static rdd2_waypoint_plan_t square_plan(int32_t sequence, float side) {
  rdd2_waypoint_plan_t plan = {
      .sequence = sequence,
      .waypoint_count = MISSION_WAYPOINT_COUNT,
      .nominal_speed = 0.25f,
      .min_segment_duration = MISSION_MIN_SEGMENT_DURATION_S,
      .valid = true,
      .global_frame = false,
  };
  const float waypoint[MISSION_WAYPOINT_COUNT][3] = {
      {0.0f, 0.0f, 0.0f}, {side, 0.0f, 0.0f}, {side, side, 0.0f},
      {0.0f, side, 0.0f}, {0.0f, 0.0f, 0.0f},
  };

  memcpy(plan.waypoint, waypoint, sizeof(waypoint));
  return plan;
}

static void planner_test_reset(uint64_t control_now_ns) {
  memset(&g_process, 0, sizeof(g_process));
  planner_fake_source_ready = true;
  planner_fake_publish_failure = false;
  planner_fake_publish_count = 0U;
  planner_fake_startup_count = 0U;
  planner_fake_step_count = 0U;
  planner_fake_plan_seen = false;
  planner_fake_step_fault = PLANNER_FAKE_STEP_OK;
  memset(planner_fake_waypoint, 0, sizeof(planner_fake_waypoint));
  memset(&planner_fake_last_reference, 0, sizeof(planner_fake_last_reference));
  reset_planner_efmu(&g_process);

  g_process.release_clock.timestamp_ns = control_now_ns;
  g_process.manual = (synapse_topic_ManualControlData_t){
      .timestamp_ns = control_now_ns - TEST_NS_FROM_US(UINT64_C(1000)),
      .flight_mode = 1U,
      .flags = synapse_topic_ManualControlFlags_Valid |
               synapse_topic_ManualControlFlags_Active,
  };
  g_process.health = (synapse_topic_VehicleHealthData_t){0};
  g_process.odometry = (synapse_topic_OdometryEstimateData_t){
      .timestamp_ns = control_now_ns - TEST_NS_FROM_US(UINT64_C(2000)),
      .position_enu_m = {.x = 10.0f, .y = -4.0f, .z = 2.0f},
      .velocity_enu_m_s = {.x = 0.1f, .y = -0.1f, .z = 0.0f},
      .quality_pct = 80,
  };
  g_process.manual_observed = true;
  g_process.health_observed = true;
  g_process.odometry_observed = true;
}

static void advance_control_time(uint64_t control_now_ns) {
  g_process.release_clock.timestamp_ns = control_now_ns;
  g_process.manual.timestamp_ns =
      control_now_ns - TEST_NS_FROM_US(UINT64_C(1000));
  g_process.odometry.timestamp_ns =
      control_now_ns - TEST_NS_FROM_US(UINT64_C(2000));
}

static void load_pending(int32_t sequence) {
  g_process.ingress_plan = square_plan(sequence, 2.0f);
  waypoint_mission_cycle(&g_process, true);
  zassert_equal(g_process.mission_state, WAYPOINT_MISSION_PENDING);
}

static void start_running(int32_t sequence) {
  planner_test_reset(TEST_NS_FROM_US(UINT64_C(1000000)));
  load_pending(sequence);
  advance_control_time(TEST_NS_FROM_US(UINT64_C(1020000)));
  g_process.manual.flags |= synapse_topic_ManualControlFlags_ArmSwitch;
  g_process.manual.flight_mode = RDD2_FLIGHT_MODE_POSITION;
  g_process.health.flags = synapse_topic_VehicleHealthFlags_Armed;
  waypoint_mission_cycle(&g_process, false);
  zassert_equal(g_process.mission_state, WAYPOINT_MISSION_RUNNING);
}

static void expect_abort_invalidation(uint64_t timestamp_ns) {
  const float values[] = {
      planner_fake_last_reference.position_enu_m.x,
      planner_fake_last_reference.position_enu_m.y,
      planner_fake_last_reference.position_enu_m.z,
      planner_fake_last_reference.velocity_enu_m_s.x,
      planner_fake_last_reference.velocity_enu_m_s.y,
      planner_fake_last_reference.velocity_enu_m_s.z,
      planner_fake_last_reference.acceleration_or_force_enu.x,
      planner_fake_last_reference.acceleration_or_force_enu.y,
      planner_fake_last_reference.acceleration_or_force_enu.z,
      planner_fake_last_reference.yaw_rad,
      planner_fake_last_reference.yaw_rate_rad_s,
  };

  zexpect_equal(planner_fake_last_reference.timestamp_ns, timestamp_ns);
  zexpect_equal(planner_fake_last_reference.coordinate_frame,
                synapse_types_LocalFrame_LocalEnu);
  zexpect_equal(planner_fake_last_reference.type_mask,
                synapse_topic_LocalPositionCommandMask_IgnorePositionX);
  for (size_t index = 0U; index < ARRAY_SIZE(values); ++index) {
    zexpect_true(isfinite(values[index]));
    zexpect_equal(values[index], 0.0f);
  }
}

ZTEST(waypoint_mission_ingress, test_plan_admission_is_exactly_bounded_square) {
  rdd2_waypoint_plan_t plan = square_plan(1, MISSION_SIDE_MIN_M);

  zexpect_true(waypoint_plan_is_admissible(&plan));
  plan = square_plan(2, MISSION_SIDE_MAX_M);
  zexpect_true(waypoint_plan_is_admissible(&plan));
  plan = square_plan(-1, 1.0f);
  zexpect_false(waypoint_plan_is_admissible(&plan));
  plan = square_plan(3, MISSION_SIDE_MIN_M - 0.01f);
  zexpect_false(waypoint_plan_is_admissible(&plan));
  plan = square_plan(4, MISSION_SIDE_MAX_M + 0.01f);
  zexpect_false(waypoint_plan_is_admissible(&plan));
  plan = square_plan(5, 1.0f);
  plan.waypoint_count = 4;
  zexpect_false(waypoint_plan_is_admissible(&plan));
  plan = square_plan(6, 1.0f);
  plan.global_frame = true;
  zexpect_false(waypoint_plan_is_admissible(&plan));
  plan = square_plan(7, 1.0f);
  plan.waypoint[2][1] = 0.9f;
  zexpect_false(waypoint_plan_is_admissible(&plan));
  plan = square_plan(8, 1.0f);
  plan.waypoint[3][2] = 0.1f;
  zexpect_false(waypoint_plan_is_admissible(&plan));
  plan = square_plan(9, 1.0f);
  plan.velocity_enu[1][0] = 0.1f;
  zexpect_false(waypoint_plan_is_admissible(&plan));
  plan = square_plan(10, 1.0f);
  plan.yaw[2] = 0.1f;
  zexpect_false(waypoint_plan_is_admissible(&plan));
  plan = square_plan(11, 1.0f);
  plan.nominal_speed = MISSION_SPEED_MAX_M_S + 0.01f;
  zexpect_false(waypoint_plan_is_admissible(&plan));
  plan = square_plan(12, 1.0f);
  plan.min_segment_duration = 2.1f;
  zexpect_false(waypoint_plan_is_admissible(&plan));
  plan = square_plan(13, 1.0f);
  plan.origin_geodetic[0] = NAN;
  zexpect_false(waypoint_plan_is_admissible(&plan));
  plan = square_plan(14, 1.0f);
  plan.origin_geodetic[0] = 1.0f;
  zexpect_false(waypoint_plan_is_admissible(&plan));
  plan = square_plan(15, 1.0f);
  plan.waypoint[RDD2_MAX_WAYPOINTS - 1U][0] = NAN;
  zexpect_false(waypoint_plan_is_admissible(&plan));
  plan = square_plan(16, 1.0f);
  plan.yaw[RDD2_MAX_WAYPOINTS - 1U] = 0.1f;
  zexpect_false(waypoint_plan_is_admissible(&plan));
}

ZTEST(waypoint_mission_ingress,
      test_pending_hold_and_start_rebase_use_control_time) {
  planner_test_reset(TEST_NS_FROM_US(UINT64_C(1000000)));
  load_pending(21);
  zexpect_equal(planner_fake_publish_count, 1U);
  zexpect_equal(planner_fake_last_reference.timestamp_ns,
                TEST_NS_FROM_US(UINT64_C(998000)));
  zexpect_equal(planner_fake_last_reference.coordinate_frame,
                synapse_types_LocalFrame_LocalEnu);
  zexpect_equal(planner_fake_last_reference.type_mask, 0U);
  zexpect_within(planner_fake_last_reference.position_enu_m.x, 10.0f, 1.0e-6f);
  zexpect_within(planner_fake_last_reference.position_enu_m.y, -4.0f, 1.0e-6f);
  zexpect_within(planner_fake_last_reference.position_enu_m.z, 2.0f, 1.0e-6f);
  zexpect_equal(planner_fake_step_count, 0U);

  advance_control_time(TEST_NS_FROM_US(UINT64_C(1020000)));
  g_process.manual.flags |= synapse_topic_ManualControlFlags_ArmSwitch;
  g_process.manual.flight_mode = 1U;
  g_process.health.flags = synapse_topic_VehicleHealthFlags_Armed;
  waypoint_mission_cycle(&g_process, false);
  zexpect_equal(g_process.mission_state, WAYPOINT_MISSION_PENDING);
  zexpect_equal(planner_fake_step_count, 0U);
  zexpect_equal(planner_fake_publish_count, 2U);

  advance_control_time(TEST_NS_FROM_US(UINT64_C(1040000)));
  g_process.manual.flight_mode = RDD2_FLIGHT_MODE_POSITION;
  waypoint_mission_cycle(&g_process, false);
  zexpect_equal(g_process.mission_state, WAYPOINT_MISSION_RUNNING);
  zexpect_equal(planner_fake_step_count, 1U);
  zexpect_true(planner_fake_plan_seen);
  zexpect_equal(planner_fake_publish_count, 3U);
  zexpect_equal(planner_fake_last_reference.timestamp_ns,
                TEST_NS_FROM_US(UINT64_C(1038000)));
  zexpect_within(planner_fake_waypoint[0][0], 10.0f, 1.0e-6f);
  zexpect_within(planner_fake_waypoint[0][1], -4.0f, 1.0e-6f);
  zexpect_within(planner_fake_waypoint[0][2], 2.0f, 1.0e-6f);
  zexpect_within(planner_fake_waypoint[1][0], 12.0f, 1.0e-6f);
  zexpect_within(planner_fake_waypoint[2][1], -2.0f, 1.0e-6f);
  zexpect_within(planner_fake_waypoint[3][0], 10.0f, 1.0e-6f);
  zexpect_within(planner_fake_waypoint[4][2], 2.0f, 1.0e-6f);

  planner_fake_plan_seen = false;
  advance_control_time(TEST_NS_FROM_US(UINT64_C(1060000)));
  waypoint_mission_cycle(&g_process, false);
  zexpect_equal(planner_fake_step_count, 2U);
  zexpect_false(planner_fake_plan_seen,
                "plan was reissued after its start transition");
}

ZTEST(waypoint_mission_ingress,
      test_disarmed_acro_can_wait_but_armed_acro_aborts_without_resume) {
  planner_test_reset(TEST_NS_FROM_US(UINT64_C(1000000)));
  g_process.manual.flight_mode = RDD2_FLIGHT_MODE_ACRO;
  load_pending(22);
  zexpect_equal(planner_fake_publish_count, 1U);
  zexpect_equal(planner_fake_step_count, 0U);

  advance_control_time(TEST_NS_FROM_US(UINT64_C(1020000)));
  g_process.manual.flags |= synapse_topic_ManualControlFlags_ArmSwitch;
  g_process.health.flags = synapse_topic_VehicleHealthFlags_Armed;
  waypoint_mission_cycle(&g_process, false);
  zassert_equal(g_process.mission_state, WAYPOINT_MISSION_ABORTED);
  zexpect_equal(planner_fake_publish_count, 2U);
  zexpect_equal(planner_fake_step_count, 0U);

  advance_control_time(TEST_NS_FROM_US(UINT64_C(1040000)));
  g_process.manual.flight_mode = RDD2_FLIGHT_MODE_POSITION;
  waypoint_mission_cycle(&g_process, false);
  zexpect_equal(g_process.mission_state, WAYPOINT_MISSION_ABORTED,
                "armed ACRO abort unexpectedly resumed in POSITION");
  zexpect_equal(planner_fake_step_count, 0U);
}

enum runtime_fault {
  RUNTIME_DISARMED = 0,
  RUNTIME_MODE_EXIT,
  RUNTIME_MANUAL_INVALID,
  RUNTIME_MANUAL_STALE,
  RUNTIME_HEALTH_STALE,
  RUNTIME_HEALTH_FAILSAFE,
  RUNTIME_NAV_INVALID,
  RUNTIME_NAV_STALE,
  RUNTIME_SOURCE_LOST,
  RUNTIME_FAULT_COUNT,
};

static void apply_runtime_fault(enum runtime_fault fault) {
  advance_control_time(TEST_NS_FROM_US(UINT64_C(1060000)));
  switch (fault) {
  case RUNTIME_DISARMED:
    g_process.health.flags = 0U;
    break;
  case RUNTIME_MODE_EXIT:
    g_process.manual.flight_mode = 1U;
    break;
  case RUNTIME_MANUAL_INVALID:
    g_process.manual.flags = synapse_topic_ManualControlFlags_ArmSwitch;
    break;
  case RUNTIME_MANUAL_STALE:
    g_process.manual_age_cycles = MISSION_INPUT_MAX_AGE_CYCLES + 1U;
    break;
  case RUNTIME_HEALTH_STALE:
    g_process.health_age_cycles = MISSION_INPUT_MAX_AGE_CYCLES + 1U;
    break;
  case RUNTIME_HEALTH_FAILSAFE:
    g_process.health.flags |= synapse_topic_VehicleHealthFlags_Failsafe;
    break;
  case RUNTIME_NAV_INVALID:
    g_process.odometry.quality_pct = 0;
    break;
  case RUNTIME_NAV_STALE:
    g_process.odometry.timestamp_ns -= MISSION_INPUT_TIMEOUT_NS + 1U;
    break;
  case RUNTIME_SOURCE_LOST:
    planner_fake_source_ready = false;
    break;
  case RUNTIME_FAULT_COUNT:
    break;
  }
}

ZTEST(waypoint_mission_ingress, test_running_faults_abort_and_never_resume) {
  for (enum runtime_fault fault = RUNTIME_DISARMED; fault < RUNTIME_FAULT_COUNT;
       ++fault) {
    start_running(30 + fault);
    planner_fake_publish_count = 0U;
    planner_fake_step_count = 0U;
    apply_runtime_fault(fault);
    waypoint_mission_cycle(&g_process, false);
    zexpect_equal(g_process.mission_state, WAYPOINT_MISSION_ABORTED,
                  "runtime fault %d did not abort", fault);
    zexpect_equal(planner_fake_publish_count, 1U);
    expect_abort_invalidation(TEST_NS_FROM_US(UINT64_C(1060000)));
    zexpect_equal(planner_fake_step_count, 0U);

    planner_fake_source_ready = true;
    advance_control_time(TEST_NS_FROM_US(UINT64_C(1080000)));
    g_process.manual.flags = synapse_topic_ManualControlFlags_Valid |
                             synapse_topic_ManualControlFlags_Active |
                             synapse_topic_ManualControlFlags_ArmSwitch;
    g_process.manual.flight_mode = RDD2_FLIGHT_MODE_POSITION;
    g_process.health.flags = synapse_topic_VehicleHealthFlags_Armed;
    g_process.odometry.quality_pct = 80;
    waypoint_mission_cycle(&g_process, false);
    zexpect_equal(g_process.mission_state, WAYPOINT_MISSION_ABORTED,
                  "runtime fault %d resumed after recovery", fault);
    zexpect_equal(planner_fake_publish_count, 1U);
    zexpect_equal(planner_fake_step_count, 0U);
  }
}

ZTEST(waypoint_mission_ingress,
      test_replay_is_rejected_but_new_disarmed_load_recovers) {
  start_running(51);
  advance_control_time(TEST_NS_FROM_US(UINT64_C(1060000)));
  g_process.manual.flight_mode = 1U;
  waypoint_mission_cycle(&g_process, false);
  zassert_equal(g_process.mission_state, WAYPOINT_MISSION_ABORTED);

  advance_control_time(TEST_NS_FROM_US(UINT64_C(1080000)));
  g_process.manual.flags = synapse_topic_ManualControlFlags_Valid |
                           synapse_topic_ManualControlFlags_Active;
  g_process.health.flags = 0U;
  g_process.ingress_plan = square_plan(51, 2.0f);
  waypoint_mission_cycle(&g_process, true);
  zexpect_equal(g_process.mission_state, WAYPOINT_MISSION_ABORTED,
                "same sequence unexpectedly reloaded an aborted mission");

  g_process.ingress_plan = square_plan(52, 2.0f);
  waypoint_mission_cycle(&g_process, true);
  zexpect_equal(g_process.mission_state, WAYPOINT_MISSION_PENDING);

  g_process.ingress_plan =
      (rdd2_waypoint_plan_t){.sequence = 53, .valid = false};
  waypoint_mission_cycle(&g_process, true);
  zexpect_equal(g_process.mission_state, WAYPOINT_MISSION_ABORTED);
  expect_abort_invalidation(TEST_NS_FROM_US(UINT64_C(1080000)));
}

enum generated_fault {
  GENERATED_STATUS = 0,
  GENERATED_REFERENCE_INVALID,
  GENERATED_NONFINITE,
  GENERATED_PUBLISH_FAILURE,
  GENERATED_FAULT_COUNT,
};

ZTEST(waypoint_mission_ingress,
      test_generated_faults_abort_with_reference_invalidation) {
  for (enum generated_fault fault = GENERATED_STATUS;
       fault < GENERATED_FAULT_COUNT; ++fault) {
    start_running(60 + fault);
    planner_fake_publish_count = 0U;
    planner_fake_step_count = 0U;
    advance_control_time(TEST_NS_FROM_US(UINT64_C(1060000)));
    planner_fake_step_fault =
        fault == GENERATED_STATUS              ? PLANNER_FAKE_STEP_ERROR
        : fault == GENERATED_REFERENCE_INVALID ? PLANNER_FAKE_STEP_NO_REFERENCE
        : fault == GENERATED_NONFINITE         ? PLANNER_FAKE_STEP_NONFINITE
                                               : PLANNER_FAKE_STEP_OK;
    planner_fake_publish_failure = fault == GENERATED_PUBLISH_FAILURE;
    waypoint_mission_cycle(&g_process, false);
    zexpect_equal(g_process.mission_state, WAYPOINT_MISSION_ABORTED,
                  "generated fault %d did not abort", fault);
    zexpect_equal(planner_fake_step_count, 1U);
    zexpect_equal(planner_fake_publish_count,
                  fault == GENERATED_PUBLISH_FAILURE ? 0U : 1U);
    zexpect_false(g_process.efmu.reference_valid,
                  "generated state was not reset after fault %d", fault);
  }
}

ZTEST(waypoint_mission_ingress, test_load_requires_current_disarmed_inputs) {
  enum load_fault {
    LOAD_ARM_SWITCH_HIGH = 0,
    LOAD_HEALTH_ARMED,
    LOAD_MANUAL_STALE,
    LOAD_HEALTH_STALE,
    LOAD_MANUAL_INVALID,
    LOAD_KILL_SWITCH,
    LOAD_NAV_STALE,
    LOAD_NAV_FUTURE,
    LOAD_NAV_INVALID,
    LOAD_NAV_NONFINITE,
    LOAD_SOURCE_UNREADY,
    LOAD_FAULT_COUNT,
  };

  for (enum load_fault fault = LOAD_ARM_SWITCH_HIGH; fault < LOAD_FAULT_COUNT;
       ++fault) {
    planner_test_reset(TEST_NS_FROM_US(UINT64_C(1000000)));
    switch (fault) {
    case LOAD_ARM_SWITCH_HIGH:
      g_process.manual.flags |= synapse_topic_ManualControlFlags_ArmSwitch;
      break;
    case LOAD_HEALTH_ARMED:
      g_process.health.flags = synapse_topic_VehicleHealthFlags_Armed;
      break;
    case LOAD_MANUAL_STALE:
      g_process.manual_age_cycles = MISSION_INPUT_MAX_AGE_CYCLES + 1U;
      break;
    case LOAD_HEALTH_STALE:
      g_process.health_age_cycles = MISSION_INPUT_MAX_AGE_CYCLES + 1U;
      break;
    case LOAD_MANUAL_INVALID:
      g_process.manual.flags = 0U;
      break;
    case LOAD_KILL_SWITCH:
      g_process.manual.flags |= synapse_topic_ManualControlFlags_KillSwitch;
      break;
    case LOAD_NAV_STALE:
      g_process.odometry.timestamp_ns = TEST_NS_FROM_US(UINT64_C(899999));
      break;
    case LOAD_NAV_FUTURE:
      g_process.odometry.timestamp_ns = TEST_NS_FROM_US(UINT64_C(1000001));
      break;
    case LOAD_NAV_INVALID:
      g_process.odometry.quality_pct = 0;
      break;
    case LOAD_NAV_NONFINITE:
      g_process.odometry.position_enu_m.z = INFINITY;
      break;
    case LOAD_SOURCE_UNREADY:
      planner_fake_source_ready = false;
      break;
    case LOAD_FAULT_COUNT:
      break;
    }
    g_process.ingress_plan = square_plan(80 + fault, 1.0f);
    waypoint_mission_cycle(&g_process, true);
    zexpect_equal(g_process.mission_state, WAYPOINT_MISSION_ABORTED,
                  "load fault %d was admitted", fault);
    zexpect_equal(planner_fake_publish_count, 1U);
    zexpect_equal(planner_fake_step_count, 0U);
  }
}

ZTEST(waypoint_mission_ingress,
      test_manual_health_age_uses_release_cycles_not_payload_time) {
  planner_test_reset(TEST_NS_FROM_US(UINT64_C(1000000)));
  g_process.manual.timestamp_ns = UINT64_MAX;
  g_process.health.timestamp_ns = UINT64_MAX;
  g_process.manual_age_cycles = MISSION_INPUT_MAX_AGE_CYCLES;
  g_process.health_age_cycles = MISSION_INPUT_MAX_AGE_CYCLES;
  g_process.ingress_plan = square_plan(100, 1.0f);
  waypoint_mission_cycle(&g_process, true);
  zassert_equal(g_process.mission_state, WAYPOINT_MISSION_PENDING,
                "payload clocks were incorrectly compared with control time");

  g_process.manual_age_cycles = MISSION_INPUT_MAX_AGE_CYCLES + 1U;
  waypoint_mission_cycle(&g_process, false);
  zexpect_equal(g_process.mission_state, WAYPOINT_MISSION_ABORTED,
                "pending mission survived a sixth missed manual update");

  planner_test_reset(TEST_NS_FROM_US(UINT64_C(1000000)));
  g_process.health_age_cycles = MISSION_INPUT_MAX_AGE_CYCLES + 1U;
  g_process.ingress_plan = square_plan(101, 1.0f);
  waypoint_mission_cycle(&g_process, true);
  zexpect_equal(g_process.mission_state, WAYPOINT_MISSION_ABORTED,
                "stale health was admitted");
}

ZTEST(waypoint_mission_ingress,
      test_pending_loss_and_hold_failure_abort_permanently) {
  planner_test_reset(TEST_NS_FROM_US(UINT64_C(1000000)));
  load_pending(110);
  advance_control_time(TEST_NS_FROM_US(UINT64_C(1020000)));
  g_process.manual.flags = 0U;
  waypoint_mission_cycle(&g_process, false);
  zassert_equal(g_process.mission_state, WAYPOINT_MISSION_ABORTED);

  advance_control_time(TEST_NS_FROM_US(UINT64_C(1040000)));
  g_process.manual.flags = synapse_topic_ManualControlFlags_Valid |
                           synapse_topic_ManualControlFlags_Active |
                           synapse_topic_ManualControlFlags_ArmSwitch;
  g_process.manual.flight_mode = RDD2_FLIGHT_MODE_POSITION;
  g_process.health.flags = synapse_topic_VehicleHealthFlags_Armed;
  waypoint_mission_cycle(&g_process, false);
  zexpect_equal(g_process.mission_state, WAYPOINT_MISSION_ABORTED,
                "pending mission resumed after manual recovery");

  planner_test_reset(TEST_NS_FROM_US(UINT64_C(1000000)));
  planner_fake_publish_failure = true;
  g_process.ingress_plan = square_plan(111, 1.0f);
  waypoint_mission_cycle(&g_process, true);
  zexpect_equal(g_process.mission_state, WAYPOINT_MISSION_ABORTED,
                "pending hold publication failure did not abort");
}

ZTEST_SUITE(waypoint_mission_ingress, NULL, NULL, NULL, NULL, NULL);
