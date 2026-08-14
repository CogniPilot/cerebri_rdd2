/* SPDX-License-Identifier: Apache-2.0 */

#include "processes.h"

#include "control_safety.h"
#include "interfaces/zros_topics.h"
#include "scheduling.h"

#include "gnss_source.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zros/private/zros_node_struct.h>
#include <zros/private/zros_pub_struct.h>
#include <zros/private/zros_sub_struct.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>
#include <zros/zros_sub.h>

#include "Planning_Bezier_WaypointTrajectoryPlanner.h"

LOG_MODULE_DECLARE(rdd2, LOG_LEVEL_INF);

#define PLANNER_STACK_SIZE 4096
#define MISSION_INPUT_TIMEOUT_NS UINT64_C(100000000)
#define MISSION_INPUT_MAX_AGE_CYCLES 5U
#define MISSION_WAYPOINT_COUNT 5
#define MISSION_SIDE_MIN_M 0.5f
#define MISSION_SIDE_MAX_M 3.0f
#define MISSION_SPEED_MIN_M_S 0.1f
#define MISSION_SPEED_MAX_M_S 0.5f
#define MISSION_MIN_SEGMENT_DURATION_S 2.0f
#define MISSION_ZERO_TOLERANCE 1.0e-5f
#define RDD2_FLIGHT_MODE_ACRO 0U
#define RDD2_FLIGHT_MODE_ATTITUDE 1U
#define RDD2_FLIGHT_MODE_POSITION 2U

struct waypoint_trajectory_planner_process {
  WaypointTrajectoryPlannerState efmu;
  rdd2_waypoint_plan_t ingress_plan;
  rdd2_waypoint_plan_t mission_plan;
  synapse_topic_AttitudeEstimateData_t release_clock;
  synapse_topic_ManualControlData_t manual;
  synapse_topic_VehicleHealthData_t health;
  synapse_topic_OdometryEstimateData_t odometry;
  synapse_topic_LocalPositionCommandData_t reference;
  struct zros_node node;
  struct zros_sub plan_sub;
  struct zros_sub release_sub;
  struct zros_sub manual_sub;
  struct zros_sub health_sub;
  struct zros_sub odometry_sub;
  struct zros_pub reference_pub;
  struct rdd2_release_scheduler release_scheduler;
  enum rdd2_waypoint_mission_state mission_state;
  int32_t last_plan_sequence;
  bool plan_sequence_observed;
  bool manual_observed;
  bool health_observed;
  bool odometry_observed;
  uint8_t manual_age_cycles;
  uint8_t health_age_cycles;
};

static struct waypoint_trajectory_planner_process g_process;
static atomic_t g_mission_state;
static struct k_thread g_thread;
K_THREAD_STACK_DEFINE(g_planner_stack, PLANNER_STACK_SIZE);

static void
mission_state_set(struct waypoint_trajectory_planner_process *process,
                  enum rdd2_waypoint_mission_state state) {
  process->mission_state = state;
  atomic_set(&g_mission_state, (atomic_val_t)state);
}

uint8_t rdd2_waypoint_mission_state_get(void) {
  return (uint8_t)atomic_get(&g_mission_state);
}

enum {
  RDD2_WAYPOINT_AXIS_COUNT =
      sizeof(((rdd2_waypoint_plan_t *)0)->origin_geodetic) / sizeof(float),
  EFMU_WAYPOINT_CAPACITY =
      sizeof(((WaypointTrajectoryPlannerState *)0)->waypoint) /
      sizeof(((WaypointTrajectoryPlannerState *)0)->waypoint[0]),
  EFMU_VELOCITY_CAPACITY =
      sizeof(((WaypointTrajectoryPlannerState *)0)->plan_velocityEnu) /
      sizeof(((WaypointTrajectoryPlannerState *)0)->plan_velocityEnu[0]),
  EFMU_YAW_CAPACITY =
      sizeof(((WaypointTrajectoryPlannerState *)0)->plan_yaw) / sizeof(float),
  EFMU_WAYPOINT_AXIS_COUNT =
      sizeof(((WaypointTrajectoryPlannerState *)0)->waypoint[0]) /
      sizeof(float),
  EFMU_VELOCITY_AXIS_COUNT =
      sizeof(((WaypointTrajectoryPlannerState *)0)->plan_velocityEnu[0]) /
      sizeof(float),
};

_Static_assert(RDD2_MAX_WAYPOINTS == EFMU_WAYPOINT_CAPACITY,
               "RDD2 waypoint ingress and generated eFMU capacities differ");
_Static_assert(EFMU_WAYPOINT_CAPACITY == EFMU_VELOCITY_CAPACITY,
               "generated waypoint and velocity capacities differ");
_Static_assert(EFMU_WAYPOINT_CAPACITY == EFMU_YAW_CAPACITY,
               "generated waypoint and yaw capacities differ");
_Static_assert(RDD2_WAYPOINT_AXIS_COUNT == EFMU_WAYPOINT_AXIS_COUNT,
               "RDD2 and generated waypoint axis counts differ");
_Static_assert(EFMU_WAYPOINT_AXIS_COUNT == 3U,
               "generated waypoint vectors must have three axes");
_Static_assert(EFMU_VELOCITY_AXIS_COUNT == 3U,
               "generated velocity vectors must have three axes");

static bool value_is_near(float value, float expected) {
  return fabsf(value - expected) <= MISSION_ZERO_TOLERANCE;
}

static bool waypoint_plan_values_are_finite(const rdd2_waypoint_plan_t *plan) {
  if (!isfinite(plan->nominal_speed) || !isfinite(plan->min_segment_duration)) {
    return false;
  }
  for (size_t axis = 0U; axis < RDD2_WAYPOINT_AXIS_COUNT; ++axis) {
    if (!isfinite(plan->origin_geodetic[axis])) {
      return false;
    }
  }
  for (size_t waypoint = 0U; waypoint < RDD2_MAX_WAYPOINTS; ++waypoint) {
    if (!isfinite(plan->yaw[waypoint])) {
      return false;
    }
    for (size_t axis = 0U; axis < RDD2_WAYPOINT_AXIS_COUNT; ++axis) {
      if (!isfinite(plan->waypoint[waypoint][axis]) ||
          !isfinite(plan->velocity_enu[waypoint][axis])) {
        return false;
      }
    }
  }
  return true;
}

static bool waypoint_plan_is_bounded_square(const rdd2_waypoint_plan_t *plan) {
  float side = plan->waypoint[1][0];
  const float expected_xy[MISSION_WAYPOINT_COUNT][2] = {
      {0.0f, 0.0f}, {side, 0.0f}, {side, side}, {0.0f, side}, {0.0f, 0.0f},
  };

  if (side < MISSION_SIDE_MIN_M || side > MISSION_SIDE_MAX_M) {
    return false;
  }
  for (size_t waypoint = 0U; waypoint < MISSION_WAYPOINT_COUNT; ++waypoint) {
    if (!value_is_near(plan->waypoint[waypoint][0], expected_xy[waypoint][0]) ||
        !value_is_near(plan->waypoint[waypoint][1], expected_xy[waypoint][1]) ||
        !value_is_near(plan->waypoint[waypoint][2], 0.0f) ||
        !value_is_near(plan->yaw[waypoint], 0.0f)) {
      return false;
    }
    for (size_t axis = 0U; axis < RDD2_WAYPOINT_AXIS_COUNT; ++axis) {
      if (!value_is_near(plan->velocity_enu[waypoint][axis], 0.0f)) {
        return false;
      }
    }
  }
  for (size_t waypoint = MISSION_WAYPOINT_COUNT; waypoint < RDD2_MAX_WAYPOINTS;
       ++waypoint) {
    if (!value_is_near(plan->yaw[waypoint], 0.0f)) {
      return false;
    }
    for (size_t axis = 0U; axis < RDD2_WAYPOINT_AXIS_COUNT; ++axis) {
      if (!value_is_near(plan->waypoint[waypoint][axis], 0.0f) ||
          !value_is_near(plan->velocity_enu[waypoint][axis], 0.0f)) {
        return false;
      }
    }
  }
  return true;
}

static bool waypoint_plan_is_admissible(const rdd2_waypoint_plan_t *plan) {
  bool origin_is_zero = true;

  for (size_t axis = 0U; axis < RDD2_WAYPOINT_AXIS_COUNT; ++axis) {
    origin_is_zero =
        origin_is_zero && value_is_near(plan->origin_geodetic[axis], 0.0f);
  }
  return plan->valid && plan->sequence != -1 &&
         plan->waypoint_count == MISSION_WAYPOINT_COUNT &&
         !plan->global_frame && origin_is_zero &&
         waypoint_plan_values_are_finite(plan) &&
         waypoint_plan_is_bounded_square(plan) &&
         plan->nominal_speed >= MISSION_SPEED_MIN_M_S &&
         plan->nominal_speed <= MISSION_SPEED_MAX_M_S &&
         value_is_near(plan->min_segment_duration,
                       MISSION_MIN_SEGMENT_DURATION_S);
}

static bool
manual_is_current(const struct waypoint_trajectory_planner_process *process) {
  const uint8_t required = synapse_topic_ManualControlFlags_Valid |
                           synapse_topic_ManualControlFlags_Active;

  return (process->manual.flags & required) == required &&
         (process->manual.flags &
          synapse_topic_ManualControlFlags_KillSwitch) == 0U &&
         process->manual_observed &&
         process->manual_age_cycles <= MISSION_INPUT_MAX_AGE_CYCLES;
}

static bool
odometry_is_current(const struct waypoint_trajectory_planner_process *process,
                    uint64_t control_now_ns) {
  const float values[] = {
      process->odometry.position_enu_m.x,
      process->odometry.position_enu_m.y,
      process->odometry.position_enu_m.z,
      process->odometry.velocity_enu_m_s.x,
      process->odometry.velocity_enu_m_s.y,
      process->odometry.velocity_enu_m_s.z,
  };

  return rdd2_navigation_position_quality_is_usable(
             process->odometry.quality_pct) &&
         rdd2_control_values_are_finite(values, ARRAY_SIZE(values)) &&
         rdd2_control_timestamp_is_fresh(
             process->odometry_observed, process->odometry.timestamp_ns,
             control_now_ns, MISSION_INPUT_TIMEOUT_NS);
}

static bool
health_is_disarmed(const struct waypoint_trajectory_planner_process *process) {
  const uint8_t rejected = synapse_topic_VehicleHealthFlags_Armed |
                           synapse_topic_VehicleHealthFlags_Failsafe;

  return process->health_observed &&
         process->health_age_cycles <= MISSION_INPUT_MAX_AGE_CYCLES &&
         (process->health.flags & rejected) == 0U;
}

static bool
health_is_armed(const struct waypoint_trajectory_planner_process *process) {
  const uint8_t required = synapse_topic_VehicleHealthFlags_Armed;
  const uint8_t rejected = synapse_topic_VehicleHealthFlags_Failsafe;

  return process->health_observed &&
         process->health_age_cycles <= MISSION_INPUT_MAX_AGE_CYCLES &&
         (process->health.flags & required) == required &&
         (process->health.flags & rejected) == 0U;
}

static bool health_is_current_without_failsafe(
    const struct waypoint_trajectory_planner_process *process) {
  return process->health_observed &&
         process->health_age_cycles <= MISSION_INPUT_MAX_AGE_CYCLES &&
         (process->health.flags & synapse_topic_VehicleHealthFlags_Failsafe) ==
             0U;
}

static bool mission_load_is_allowed(
    const struct waypoint_trajectory_planner_process *process,
    uint64_t control_now_ns, bool source_ready) {
  return manual_is_current(process) &&
         (process->manual.flags & synapse_topic_ManualControlFlags_ArmSwitch) ==
             0U &&
         health_is_disarmed(process) &&
         odometry_is_current(process, control_now_ns) && source_ready;
}

static bool mission_run_is_allowed(
    const struct waypoint_trajectory_planner_process *process,
    uint64_t control_now_ns, bool source_ready) {
  return manual_is_current(process) &&
         (process->manual.flags & synapse_topic_ManualControlFlags_ArmSwitch) !=
             0U &&
         process->manual.flight_mode == RDD2_FLIGHT_MODE_POSITION &&
         health_is_armed(process) &&
         odometry_is_current(process, control_now_ns) && source_ready;
}

static bool mission_pending_is_allowed(
    const struct waypoint_trajectory_planner_process *process,
    uint64_t control_now_ns, bool source_ready) {
  bool disarmed_acro = process->manual.flight_mode == RDD2_FLIGHT_MODE_ACRO &&
                       (process->manual.flags &
                        synapse_topic_ManualControlFlags_ArmSwitch) == 0U &&
                       health_is_disarmed(process);
  bool takeoff_or_position_mode =
      process->manual.flight_mode == RDD2_FLIGHT_MODE_ATTITUDE ||
      process->manual.flight_mode == RDD2_FLIGHT_MODE_POSITION;

  return manual_is_current(process) &&
         (disarmed_acro || takeoff_or_position_mode) &&
         health_is_current_without_failsafe(process) &&
         odometry_is_current(process, control_now_ns) && source_ready;
}

static void
reset_planner_efmu(struct waypoint_trajectory_planner_process *process) {
  WaypointTrajectoryPlanner_startup(&process->efmu);
  process->efmu.maxWaypoints = RDD2_MAX_WAYPOINTS;
  WaypointTrajectoryPlanner_recalibrate(&process->efmu);
}

static void abort_mission(struct waypoint_trajectory_planner_process *process) {
  process->reference = (synapse_topic_LocalPositionCommandData_t){
      .timestamp_ns = process->release_clock.timestamp_ns,
      .type_mask = synapse_topic_LocalPositionCommandMask_IgnorePositionX,
      .coordinate_frame = synapse_types_LocalFrame_LocalEnu,
  };
  (void)zros_pub_update(&process->reference_pub);
  reset_planner_efmu(process);
  process->mission_plan = (rdd2_waypoint_plan_t){0};
  mission_state_set(process, RDD2_WAYPOINT_MISSION_ABORTED);
}

static void
copy_waypoint_plan_input_to_efmu(WaypointTrajectoryPlannerState *efmu,
                                 const rdd2_waypoint_plan_t *plan) {
  efmu->plan_valid = true;
  efmu->plan_sequence = plan->sequence;
  efmu->plan_waypointCount = plan->waypoint_count;
  efmu->globalFrame = false;
  for (size_t axis = 0U; axis < RDD2_WAYPOINT_AXIS_COUNT; ++axis) {
    efmu->originGeodetic[axis] = plan->origin_geodetic[axis];
  }
  for (size_t waypoint = 0U; waypoint < RDD2_MAX_WAYPOINTS; ++waypoint) {
    for (size_t axis = 0U; axis < RDD2_WAYPOINT_AXIS_COUNT; ++axis) {
      efmu->waypoint[waypoint][axis] = plan->waypoint[waypoint][axis];
      efmu->plan_velocityEnu[waypoint][axis] =
          plan->velocity_enu[waypoint][axis];
    }
    efmu->plan_yaw[waypoint] = plan->yaw[waypoint];
  }
  efmu->nominalSpeed = plan->nominal_speed;
  efmu->minSegmentDuration = plan->min_segment_duration;
}

static void
rebase_mission_plan(struct waypoint_trajectory_planner_process *process) {
  const float origin[RDD2_WAYPOINT_AXIS_COUNT] = {
      process->odometry.position_enu_m.x,
      process->odometry.position_enu_m.y,
      process->odometry.position_enu_m.z,
  };

  for (size_t waypoint = 0U; waypoint < MISSION_WAYPOINT_COUNT; ++waypoint) {
    for (size_t axis = 0U; axis < RDD2_WAYPOINT_AXIS_COUNT; ++axis) {
      process->mission_plan.waypoint[waypoint][axis] += origin[axis];
    }
  }
  for (size_t axis = 0U; axis < RDD2_WAYPOINT_AXIS_COUNT; ++axis) {
    process->mission_plan.waypoint[0][axis] = origin[axis];
  }
}

static bool
publish_pending_hold(struct waypoint_trajectory_planner_process *process) {
  process->reference = (synapse_topic_LocalPositionCommandData_t){
      .timestamp_ns = process->odometry.timestamp_ns,
      .position_enu_m = process->odometry.position_enu_m,
      .coordinate_frame = synapse_types_LocalFrame_LocalEnu,
  };
  return zros_pub_update(&process->reference_pub) == 0;
}

static bool
generated_reference_is_usable(const WaypointTrajectoryPlannerState *efmu) {
  const float values[] = {
      efmu->position[0],     efmu->position[1],     efmu->position[2],
      efmu->velocity[0],     efmu->velocity[1],     efmu->velocity[2],
      efmu->acceleration[0], efmu->acceleration[1], efmu->acceleration[2],
      efmu->reference_yaw,   efmu->yawRate,
  };

  return rdd2_generated_step_ok(efmu->rumoca_galec_error_signal_status) &&
         efmu->reference_valid &&
         rdd2_control_values_are_finite(values, ARRAY_SIZE(values));
}

static bool publish_generated_reference(
    struct waypoint_trajectory_planner_process *process) {
  WaypointTrajectoryPlannerState *efmu = &process->efmu;

  process->reference = (synapse_topic_LocalPositionCommandData_t){
      .timestamp_ns = process->odometry.timestamp_ns,
      .position_enu_m =
          {
              .x = efmu->position[0],
              .y = efmu->position[1],
              .z = efmu->position[2],
          },
      .velocity_enu_m_s =
          {
              .x = efmu->velocity[0],
              .y = efmu->velocity[1],
              .z = efmu->velocity[2],
          },
      .acceleration_or_force_enu =
          {
              .x = efmu->acceleration[0],
              .y = efmu->acceleration[1],
              .z = efmu->acceleration[2],
          },
      .yaw_rad = efmu->reference_yaw,
      .yaw_rate_rad_s = efmu->yawRate,
      .coordinate_frame = synapse_types_LocalFrame_LocalEnu,
  };
  return zros_pub_update(&process->reference_pub) == 0;
}

static void
handle_plan_update(struct waypoint_trajectory_planner_process *process,
                   uint64_t control_now_ns, bool source_ready) {
  if (process->plan_sequence_observed &&
      process->ingress_plan.sequence == process->last_plan_sequence) {
    return;
  }
  process->last_plan_sequence = process->ingress_plan.sequence;
  process->plan_sequence_observed = true;

  if (!waypoint_plan_is_admissible(&process->ingress_plan) ||
      !mission_load_is_allowed(process, control_now_ns, source_ready)) {
    abort_mission(process);
    return;
  }

  reset_planner_efmu(process);
  process->mission_plan = process->ingress_plan;
  mission_state_set(process, RDD2_WAYPOINT_MISSION_PENDING);
}

static void
waypoint_mission_cycle(struct waypoint_trajectory_planner_process *process,
                       bool plan_updated) {
  uint64_t control_now_ns = process->release_clock.timestamp_ns;
  bool navigation_current = odometry_is_current(process, control_now_ns);
  bool source_ready = rdd2_position_source_ready_get();

  if (plan_updated) {
    handle_plan_update(process, control_now_ns, source_ready);
  }

  if (process->mission_state == RDD2_WAYPOINT_MISSION_PENDING) {
    if (!mission_pending_is_allowed(process, control_now_ns, source_ready)) {
      abort_mission(process);
      return;
    }
    if (mission_run_is_allowed(process, control_now_ns, source_ready)) {
      rebase_mission_plan(process);
      copy_waypoint_plan_input_to_efmu(&process->efmu, &process->mission_plan);
      mission_state_set(process, RDD2_WAYPOINT_MISSION_RUNNING);
    } else {
      if (navigation_current && source_ready &&
          !publish_pending_hold(process)) {
        abort_mission(process);
      }
      return;
    }
  }

  if (process->mission_state != RDD2_WAYPOINT_MISSION_RUNNING) {
    return;
  }
  if (!mission_run_is_allowed(process, control_now_ns, source_ready)) {
    abort_mission(process);
    return;
  }

  WaypointTrajectoryPlanner_dostep(&process->efmu);
  process->efmu.plan_valid = false;
  if (!generated_reference_is_usable(&process->efmu) ||
      !publish_generated_reference(process)) {
    abort_mission(process);
  }
}

static void waypoint_trajectory_planner_thread(void *arg1, void *arg2,
                                               void *arg3) {
  struct waypoint_trajectory_planner_process *process = arg1;

  ARG_UNUSED(arg2);
  ARG_UNUSED(arg3);

  while (true) {
    bool plan_updated;

    if (zros_sub_wait(&process->release_sub, K_FOREVER) != 0 ||
        zros_sub_update(&process->release_sub) != 0) {
      continue;
    }
    if (!rdd2_release_due(&process->release_scheduler,
                          RDD2_NAVIGATION_ESTIMATOR_RATE_HZ,
                          RDD2_PLANNING_RATE_HZ)) {
      continue;
    }

    plan_updated = zros_sub_update(&process->plan_sub) == 0;
    if (zros_sub_update(&process->manual_sub) == 0) {
      process->manual_observed = true;
      process->manual_age_cycles = 0U;
    } else if (process->manual_observed &&
               process->manual_age_cycles < UINT8_MAX) {
      process->manual_age_cycles++;
    }
    if (zros_sub_update(&process->health_sub) == 0) {
      process->health_observed = true;
      process->health_age_cycles = 0U;
    } else if (process->health_observed &&
               process->health_age_cycles < UINT8_MAX) {
      process->health_age_cycles++;
    }
    if (zros_sub_update(&process->odometry_sub) == 0) {
      process->odometry_observed = true;
    }
    waypoint_mission_cycle(process, plan_updated);
  }
}

int rdd2_waypoint_trajectory_planner_process_start(void) {
  struct waypoint_trajectory_planner_process *process = &g_process;
  int rc;

  *process = (struct waypoint_trajectory_planner_process){0};
  mission_state_set(process, RDD2_WAYPOINT_MISSION_EMPTY);
  reset_planner_efmu(process);
  zros_node_init(&process->node, "efmu_planner");

  rc = zros_sub_init(&process->plan_sub, &process->node, &topic_waypoint_plan,
                     &process->ingress_plan, 0.0);
  if (rc == 0) {
    rc = zros_sub_init(&process->release_sub, &process->node,
                       &topic_attitude_estimate, &process->release_clock, 0.0);
  }
  if (rc == 0) {
    rc = zros_sub_init(&process->manual_sub, &process->node,
                       &topic_manual_input, &process->manual, 0.0);
  }
  if (rc == 0) {
    rc = zros_sub_init(&process->health_sub, &process->node,
                       &topic_vehicle_health, &process->health, 0.0);
  }
  if (rc == 0) {
    rc = zros_sub_init(&process->odometry_sub, &process->node,
                       &topic_navigation_odometry, &process->odometry, 0.0);
  }
  if (rc == 0) {
    rc = zros_pub_init(&process->reference_pub, &process->node,
                       &topic_trajectory_reference, &process->reference);
  }
  if (rc != 0) {
    return rc;
  }

  k_thread_create(&g_thread, g_planner_stack,
                  K_THREAD_STACK_SIZEOF(g_planner_stack),
                  waypoint_trajectory_planner_thread, process, NULL, NULL,
                  RDD2_PLANNING_PRIORITY, 0, K_NO_WAIT);
  k_thread_name_set(&g_thread, "efmu_planner");
  return 0;
}
