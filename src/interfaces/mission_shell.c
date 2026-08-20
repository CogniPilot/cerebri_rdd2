/* SPDX-License-Identifier: Apache-2.0 */

#include "synapse_time_status.h"
#include "zros_topics.h"

#include "gnss_source.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <zros/private/zros_node_struct.h>
#include <zros/private/zros_pub_struct.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>

#define MISSION_INPUT_MAX_AGE_NS UINT64_C(100000000)
#define MISSION_BOX_SIDE_MIN_M 0.5f
#define MISSION_BOX_SIDE_MAX_M 3.0f
#define MISSION_BOX_SPEED_MIN_M_S 0.1f
#define MISSION_BOX_SPEED_MAX_M_S 0.5f
#define MISSION_MIN_SEGMENT_DURATION_S 2.0f
#define MISSION_BOX_WAYPOINT_COUNT 5

enum mission_shell_state {
  MISSION_SHELL_EMPTY = 0,
  MISSION_SHELL_REQUEST_PUBLISHED,
  MISSION_SHELL_CANCELLED,
};

struct mission_shell_context {
  struct zros_node node;
  struct zros_pub publisher;
  rdd2_waypoint_plan_t plan;
  enum mission_shell_state state;
  int32_t next_sequence;
  float side_m;
  float speed_m_s;
  bool node_ready;
  bool publisher_ready;
};

struct mission_shell_inputs {
  synapse_topic_ManualControlData_t manual;
  synapse_topic_VehicleHealthData_t health;
  synapse_topic_OdometryEstimateData_t navigation;
  synapse_topic_AttitudeEstimateData_t attitude;
};

static struct mission_shell_context g_mission = {
    .next_sequence = 1,
};

static bool timestamp_is_recent(uint64_t timestamp_ns, uint64_t now_ns) {
  return timestamp_ns != 0U && timestamp_ns <= now_ns &&
         now_ns - timestamp_ns <= MISSION_INPUT_MAX_AGE_NS;
}

static bool
navigation_is_valid(const synapse_topic_OdometryEstimateData_t *navigation,
                    const synapse_topic_AttitudeEstimateData_t *attitude) {
  const float values[] = {
      navigation->position_enu_m.x,
      navigation->position_enu_m.y,
      navigation->position_enu_m.z,
      navigation->attitude.w,
      navigation->attitude.x,
      navigation->attitude.y,
      navigation->attitude.z,
      navigation->velocity_enu_m_s.x,
      navigation->velocity_enu_m_s.y,
      navigation->velocity_enu_m_s.z,
      navigation->angular_velocity_flu_rad_s.roll,
      navigation->angular_velocity_flu_rad_s.pitch,
      navigation->angular_velocity_flu_rad_s.yaw,
      attitude->attitude.w,
      attitude->attitude.x,
      attitude->attitude.y,
      attitude->attitude.z,
  };
  const uint8_t attitude_required =
      synapse_topic_AttitudeEstimateFlags_AttitudeValid;

  if (navigation->timestamp_ns == 0U || navigation->quality_pct <= 0 ||
      attitude->timestamp_ns == 0U ||
      (attitude->flags & attitude_required) != attitude_required ||
      navigation->timestamp_ns > attitude->timestamp_ns ||
      attitude->timestamp_ns - navigation->timestamp_ns >
          MISSION_INPUT_MAX_AGE_NS) {
    return false;
  }
  for (size_t index = 0U; index < ARRAY_SIZE(values); ++index) {
    if (!isfinite(values[index])) {
      return false;
    }
  }
  return true;
}

static int read_admission_inputs(struct mission_shell_inputs *inputs) {
  const uint8_t manual_required = synapse_topic_ManualControlFlags_Valid |
                                  synapse_topic_ManualControlFlags_Active;
  const uint8_t manual_blocking = synapse_topic_ManualControlFlags_ArmSwitch |
                                  synapse_topic_ManualControlFlags_KillSwitch;
  const uint32_t rc_sensor = synapse_topic_SensorComponentFlags_RadioControl;
  const uint8_t health_blocking = synapse_topic_VehicleHealthFlags_Armed |
                                  synapse_topic_VehicleHealthFlags_Failsafe;
  /* Compare against the same full-precision boot clock the manual, health, and
   * estimate stamps now carry. The old millisecond-floored now read a sample
   * taken later in the current millisecond as if it were from the future, which
   * failed timestamp_is_recent and stalled admission. */
  uint64_t now_ns = synapse_time_boot_ns();

  if (!rdd2_topic_has_sample(&topic_manual_input) ||
      !rdd2_topic_has_sample(&topic_vehicle_health) ||
      !rdd2_topic_has_sample(&topic_navigation_odometry) ||
      !rdd2_topic_has_sample(&topic_attitude_estimate) ||
      zros_topic_read(&topic_manual_input, &inputs->manual) != 0 ||
      zros_topic_read(&topic_vehicle_health, &inputs->health) != 0 ||
      zros_topic_read(&topic_navigation_odometry, &inputs->navigation) != 0 ||
      zros_topic_read(&topic_attitude_estimate, &inputs->attitude) != 0) {
    return -ENODATA;
  }

  if (!timestamp_is_recent(inputs->manual.timestamp_ns, now_ns) ||
      (inputs->manual.flags & manual_required) != manual_required) {
    return -EAGAIN;
  }
  if ((inputs->manual.flags & manual_blocking) != 0U) {
    return -EBUSY;
  }
  if (!timestamp_is_recent(inputs->health.timestamp_ns, now_ns) ||
      (inputs->health.flags & health_blocking) != 0U ||
      (inputs->health.sensors_health & rc_sensor) == 0U) {
    return -EBUSY;
  }
  if (!navigation_is_valid(&inputs->navigation, &inputs->attitude) ||
      !rdd2_position_source_ready_get()) {
    return -EAGAIN;
  }
  return 0;
}

static int parse_bounded_float(const char *text, float minimum, float maximum,
                               float *value) {
  char *end = NULL;
  float parsed;

  errno = 0;
  parsed = strtof(text, &end);
  if (end == text || end == NULL || *end != '\0' || errno == ERANGE ||
      !isfinite(parsed) || parsed < minimum || parsed > maximum) {
    return -EINVAL;
  }
  *value = parsed;
  return 0;
}

static int32_t take_sequence(void) {
  int32_t sequence = g_mission.next_sequence;

  g_mission.next_sequence = sequence == INT32_MAX ? 1 : sequence + 1;
  return sequence;
}

static int ensure_publisher(void) {
  int rc;

  if (g_mission.publisher_ready) {
    return 0;
  }
  if (!g_mission.node_ready) {
    zros_node_init(&g_mission.node, "rdd2_mission_shell");
    g_mission.node_ready = true;
  }
  rc = zros_pub_init(&g_mission.publisher, &g_mission.node,
                     &topic_waypoint_plan, &g_mission.plan);
  if (rc == 0) {
    g_mission.publisher_ready = true;
  }
  return rc;
}

static rdd2_waypoint_plan_t make_box_plan(int32_t sequence, float side_m,
                                          float speed_m_s) {
  rdd2_waypoint_plan_t plan = {
      .sequence = sequence,
      .waypoint_count = MISSION_BOX_WAYPOINT_COUNT,
      .nominal_speed = speed_m_s,
      .min_segment_duration = MISSION_MIN_SEGMENT_DURATION_S,
      .valid = true,
      .global_frame = false,
  };

  plan.waypoint[1][0] = side_m;
  plan.waypoint[2][0] = side_m;
  plan.waypoint[2][1] = side_m;
  plan.waypoint[3][1] = side_m;
  return plan;
}

static int cmd_mission_box(const struct shell *sh, size_t argc, char **argv) {
  struct mission_shell_inputs inputs;
  rdd2_waypoint_plan_t plan;
  float side_m;
  float speed_m_s;
  int rc;

  ARG_UNUSED(argc);
  rc = parse_bounded_float(argv[1], MISSION_BOX_SIDE_MIN_M,
                           MISSION_BOX_SIDE_MAX_M, &side_m);
  if (rc == 0) {
    rc = parse_bounded_float(argv[2], MISSION_BOX_SPEED_MIN_M_S,
                             MISSION_BOX_SPEED_MAX_M_S, &speed_m_s);
  }
  if (rc != 0) {
    shell_error(sh,
                "box requires finite side 0.5..3.0 m and speed 0.1..0.5 m/s");
    return rc;
  }

  rc = read_admission_inputs(&inputs);
  if (rc != 0) {
    shell_error(sh, "mission request blocked: require disarmed fresh "
                    "RC/health, valid navigation, and ready position source");
    return rc;
  }
  rc = ensure_publisher();
  if (rc != 0) {
    shell_error(sh, "mission publisher unavailable: %d", rc);
    return rc;
  }

  plan = make_box_plan(take_sequence(), side_m, speed_m_s);
  g_mission.plan = plan;
  rc = zros_pub_update(&g_mission.publisher);
  if (rc != 0) {
    shell_error(sh, "mission plan publish failed: %d", rc);
    return rc;
  }
  g_mission.state = MISSION_SHELL_REQUEST_PUBLISHED;
  g_mission.side_m = side_m;
  g_mission.speed_m_s = speed_m_s;
  shell_print(sh,
              "published current-altitude relative box sequence=%d side_mm=%d "
              "speed_mm_s=%d",
              plan.sequence, (int)(side_m * 1000.0f + 0.5f),
              (int)(speed_m_s * 1000.0f + 0.5f));
  return 0;
}

static int cmd_mission_cancel(const struct shell *sh, size_t argc,
                              char **argv) {
  rdd2_waypoint_plan_t cancellation = {
      .sequence = take_sequence(),
      .global_frame = false,
  };
  int rc;

  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  rc = ensure_publisher();
  if (rc != 0) {
    shell_error(sh, "mission publisher unavailable: %d", rc);
    return rc;
  }
  g_mission.plan = cancellation;
  rc = zros_pub_update(&g_mission.publisher);
  if (rc != 0) {
    shell_error(sh, "mission cancellation publish failed: %d", rc);
    return rc;
  }
  g_mission.state = MISSION_SHELL_CANCELLED;
  g_mission.side_m = 0.0f;
  g_mission.speed_m_s = 0.0f;
  shell_print(sh, "mission cancelled sequence=%d", cancellation.sequence);
  return 0;
}

static int cmd_mission_status(const struct shell *sh, size_t argc,
                              char **argv) {
  static const char *const names[] = {
      [MISSION_SHELL_EMPTY] = "empty",
      [MISSION_SHELL_REQUEST_PUBLISHED] = "published",
      [MISSION_SHELL_CANCELLED] = "cancelled",
  };

  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  shell_print(sh,
              "ingress=%s planner=unknown sequence=%d side_mm=%d speed_mm_s=%d "
              "publisher=%s",
              names[g_mission.state], g_mission.plan.sequence,
              (int)(g_mission.side_m * 1000.0f + 0.5f),
              (int)(g_mission.speed_m_s * 1000.0f + 0.5f),
              g_mission.publisher_ready ? "ready" : "idle");
  return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    mission_commands,
    SHELL_CMD_ARG(
        box, NULL,
        "Publish relative current-altitude square: box <side_m> <speed_m_s>",
        cmd_mission_box, 3, 0),
    SHELL_CMD(cancel, NULL, "Publish mission cancellation.",
              cmd_mission_cancel),
    SHELL_CMD(status, NULL, "Show shell ingress publication state.",
              cmd_mission_status),
    SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(mission, &mission_commands,
                   "Bounded local demo mission ingress.", NULL);
