/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Stand-in waypoint trajectory planner.
 *
 * Replaces the generated WaypointTrajectoryPlanner eFMU (a Bezier mission
 * generator) with a deliberately simple position-hold planner. It keeps the
 * same process entrypoint, mission-state accessor, thread cadence, subscription
 * set, and trajectory_reference publication so the ZROS surface is unchanged and
 * guidance can consume a valid position reference.
 *
 * Behavior: when odometry is current, it publishes a LocalEnu position-hold
 * reference at the current estimated position with zero velocity and
 * acceleration. It does not execute uploaded waypoint missions, so the mission
 * state stays EMPTY. This is sufficient for bench verification of the estimator,
 * guidance, and allocator sign conventions, where position missions are not
 * exercised. Frames follow simple_control.h (world ENU, body FLU).
 */

#include "processes.h"

#include "control_safety.h"
#include "interfaces/zros_topics.h"
#include "scheduling.h"

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

LOG_MODULE_DECLARE(rdd2, LOG_LEVEL_INF);

#define PLANNER_STACK_SIZE 4096
#define HOLD_INPUT_TIMEOUT_NS UINT64_C(100000000)

struct simple_waypoint_trajectory_planner_process {
  rdd2_waypoint_plan_t ingress_plan;
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
  bool odometry_observed;
};

static struct simple_waypoint_trajectory_planner_process g_process;
static atomic_t g_mission_state;
static struct k_thread g_thread;
K_THREAD_STACK_DEFINE(g_planner_stack, PLANNER_STACK_SIZE);

uint8_t rdd2_waypoint_mission_state_get(void) {
  return (uint8_t)atomic_get(&g_mission_state);
}

static bool odometry_is_current(
    const struct simple_waypoint_trajectory_planner_process *process,
    uint64_t control_now_ns) {
  const float values[] = {
      process->odometry.position_enu_m.x,
      process->odometry.position_enu_m.y,
      process->odometry.position_enu_m.z,
      process->odometry.velocity_enu_m_s.x,
      process->odometry.velocity_enu_m_s.y,
      process->odometry.velocity_enu_m_s.z,
  };

  return process->odometry.quality_pct > 0 &&
         rdd2_control_values_are_finite(values, ARRAY_SIZE(values)) &&
         rdd2_control_timestamp_is_fresh(
             process->odometry_observed, process->odometry.timestamp_ns,
             control_now_ns, HOLD_INPUT_TIMEOUT_NS);
}

static void
publish_position_hold(struct simple_waypoint_trajectory_planner_process *process) {
  process->reference = (synapse_topic_LocalPositionCommandData_t){
      .timestamp_ns = process->odometry.timestamp_ns,
      .position_enu_m = process->odometry.position_enu_m,
      .coordinate_frame = synapse_types_LocalFrame_LocalEnu,
  };
  (void)zros_pub_update(&process->reference_pub);
}

static void simple_waypoint_trajectory_planner_thread(void *arg1, void *arg2,
                                                      void *arg3) {
  struct simple_waypoint_trajectory_planner_process *process = arg1;

  ARG_UNUSED(arg2);
  ARG_UNUSED(arg3);

  while (true) {
    if (zros_sub_wait(&process->release_sub, K_FOREVER) != 0 ||
        zros_sub_update(&process->release_sub) != 0) {
      continue;
    }
    if (!rdd2_release_due(&process->release_scheduler,
                          RDD2_NAVIGATION_ESTIMATOR_RATE_HZ,
                          RDD2_PLANNING_RATE_HZ)) {
      continue;
    }

    /* Drain the plan and RC/health topics to keep the ZROS surface identical to
     * the generated planner, even though missions are not executed here. */
    (void)zros_sub_update(&process->plan_sub);
    (void)zros_sub_update(&process->manual_sub);
    (void)zros_sub_update(&process->health_sub);
    if (zros_sub_update(&process->odometry_sub) == 0) {
      process->odometry_observed = true;
    }

    if (odometry_is_current(process, process->release_clock.timestamp_ns)) {
      publish_position_hold(process);
    }
  }
}

int rdd2_waypoint_trajectory_planner_process_start(void) {
  struct simple_waypoint_trajectory_planner_process *process = &g_process;
  int rc;

  *process = (struct simple_waypoint_trajectory_planner_process){0};
  atomic_set(&g_mission_state, (atomic_val_t)RDD2_WAYPOINT_MISSION_EMPTY);
  zros_node_init(&process->node, "simple_planner");

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
                  simple_waypoint_trajectory_planner_thread, process, NULL, NULL,
                  RDD2_PLANNING_PRIORITY, 0, K_NO_WAIT);
  k_thread_name_set(&g_thread, "simple_planner");
  return 0;
}
