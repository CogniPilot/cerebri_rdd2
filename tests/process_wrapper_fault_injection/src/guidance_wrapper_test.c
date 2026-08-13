/* SPDX-License-Identifier: Apache-2.0 */

#include <math.h>
#include <setjmp.h>
#include <string.h>
#include <zephyr/ztest.h>

#define TEST_NS_FROM_US(value) ((uint64_t)(value) * UINT64_C(1000))
#define RDD2_TEST_EVERY_SAMPLE_IS_RELEASE 1

#define CONFIG_RDD2_GNSS_SOURCE_ONBOARD 1
#define zros_node_init guidance_fake_zros_node_init
#define zros_pub_init guidance_fake_zros_pub_init
#define zros_pub_update guidance_fake_zros_pub_update
#define zros_sub_init guidance_fake_zros_sub_init
#define zros_sub_update guidance_fake_zros_sub_update
#define zros_sub_wait guidance_fake_zros_sub_wait
#define rdd2_gnss_onboard_ready_get guidance_fake_gnss_onboard_ready_get
#define topic_attitude_command guidance_fake_topic_attitude_command
#define topic_attitude_estimate guidance_fake_topic_attitude_estimate
#define topic_manual_input guidance_fake_topic_manual_input
#define topic_navigation_odometry guidance_fake_topic_navigation_odometry
#define topic_rate_command guidance_fake_topic_rate_command
#define topic_trajectory_reference guidance_fake_topic_trajectory_reference
#define topic_vehicle_health guidance_fake_topic_vehicle_health

#include "../../../src/processes/guidance_controller.c"

#undef topic_vehicle_health
#undef CONFIG_RDD2_GNSS_SOURCE_ONBOARD
#undef topic_trajectory_reference
#undef topic_rate_command
#undef topic_navigation_odometry
#undef topic_manual_input
#undef topic_attitude_estimate
#undef topic_attitude_command
#undef zros_sub_wait
#undef zros_sub_update
#undef zros_sub_init
#undef rdd2_gnss_onboard_ready_get
#undef zros_pub_update
#undef zros_pub_init
#undef zros_node_init

enum guidance_cycle {
  GUIDANCE_CYCLE_VALID = 0,
  GUIDANCE_CYCLE_FLAG_FAULT,
  GUIDANCE_CYCLE_FLAG_LATCHED,
  GUIDANCE_CYCLE_FLAG_INVALID_LOW,
  GUIDANCE_CYCLE_FLAG_STILL_LATCHED,
  GUIDANCE_CYCLE_FLAG_ACK,
  GUIDANCE_CYCLE_FLAG_RECOVERY,
  GUIDANCE_CYCLE_QUALITY_FAULT,
  GUIDANCE_CYCLE_QUALITY_ACK,
  GUIDANCE_CYCLE_QUALITY_RECOVERY,
  GUIDANCE_CYCLE_NONFINITE_NAV_FAULT,
  GUIDANCE_CYCLE_NONFINITE_NAV_ACK,
  GUIDANCE_CYCLE_NONFINITE_NAV_RECOVERY,
  GUIDANCE_CYCLE_GENERATED_ERROR,
  GUIDANCE_CYCLE_ERROR_LATCHED,
  GUIDANCE_CYCLE_ERROR_ACK,
  GUIDANCE_CYCLE_ERROR_RECOVERY,
  GUIDANCE_CYCLE_GENERATED_LATE_NAN,
  GUIDANCE_CYCLE_NAN_LATCHED,
  GUIDANCE_CYCLE_NAN_ACK,
  GUIDANCE_CYCLE_NAN_RECOVERY,
  GUIDANCE_CYCLE_HEALTH_FAILSAFE,
  GUIDANCE_CYCLE_INVALID_MANUAL,
  GUIDANCE_CYCLE_KILL_SWITCH_ARMED,
  GUIDANCE_CYCLE_POSITION_UNREADY_ARMED_REQUEST,
  GUIDANCE_CYCLE_POSITION_READY_ARMED,
  GUIDANCE_CYCLE_POSITION_LOST_ARMED,
  GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_HIGH,
  GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_HIGH_LATCHED,
  GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_FAILED_LOW,
  GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_INVALID_LOW,
  GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_ACK,
  GUIDANCE_CYCLE_POSITION_READY_ARMED_RECOVERY,
  GUIDANCE_CYCLE_ACRO_UNREADY_ARMED,
  GUIDANCE_CYCLE_ATTITUDE_UNREADY_ARMED,
  GUIDANCE_CYCLE_LEGACY_COUNT,
  GUIDANCE_CYCLE_REFERENCE_NO_SAMPLE_ARMED = GUIDANCE_CYCLE_LEGACY_COUNT,
  GUIDANCE_CYCLE_REFERENCE_RECOVERED_ARMED,
  GUIDANCE_CYCLE_REFERENCE_INVALID_MODE_EXIT_ARMED,
  GUIDANCE_CYCLE_REFERENCE_POSITION_AFTER_INVALID_EXIT_ARMED,
  GUIDANCE_CYCLE_REFERENCE_FAILED_MODE_EXIT_ARMED,
  GUIDANCE_CYCLE_REFERENCE_POSITION_AFTER_FAILED_EXIT_ARMED,
  GUIDANCE_CYCLE_REFERENCE_MODE_EXIT_ARMED,
  GUIDANCE_CYCLE_REFERENCE_RETAINED_MODE_EXIT_ARMED,
  GUIDANCE_CYCLE_REFERENCE_POSITION_AFTER_EXIT_ARMED,
  GUIDANCE_CYCLE_REFERENCE_EXACT_NOW_ARMED,
  GUIDANCE_CYCLE_REFERENCE_EXACT_100_MS_ARMED,
  GUIDANCE_CYCLE_REFERENCE_FUTURE_ARMED,
  GUIDANCE_CYCLE_REFERENCE_STALE_ARMED,
  GUIDANCE_CYCLE_REFERENCE_BAD_FRAME_ARMED,
  GUIDANCE_CYCLE_REFERENCE_BAD_MASK_ARMED,
  GUIDANCE_CYCLE_REFERENCE_NONFINITE_POSITION_ARMED,
  GUIDANCE_CYCLE_REFERENCE_NONFINITE_VELOCITY_ARMED,
  GUIDANCE_CYCLE_REFERENCE_NONFINITE_ACCELERATION_ARMED,
  GUIDANCE_CYCLE_REFERENCE_NONFINITE_YAW_ARMED,
  GUIDANCE_CYCLE_REFERENCE_NONFINITE_YAW_RATE_ARMED,
  GUIDANCE_CYCLE_REFERENCE_PREARM_NO_SAMPLE_HIGH,
  GUIDANCE_CYCLE_REFERENCE_PREARM_VALID_HIGH_LATCHED,
  GUIDANCE_CYCLE_REFERENCE_PREARM_FAILED_LOW,
  GUIDANCE_CYCLE_REFERENCE_PREARM_INVALID_LOW,
  GUIDANCE_CYCLE_REFERENCE_PREARM_VALID_LOW_ACK,
  GUIDANCE_CYCLE_REFERENCE_PREARM_VALID_HIGH_RECOVERY,
  GUIDANCE_CYCLE_REFERENCE_GNSS_LOST_ARMED,
  GUIDANCE_CYCLE_REFERENCE_GNSS_RECOVERED_ARMED,
  GUIDANCE_CYCLE_REFERENCE_GNSS_MODE_EXIT_ARMED,
  GUIDANCE_CYCLE_REFERENCE_GNSS_POSITION_AFTER_EXIT_ARMED,
  GUIDANCE_CYCLE_REFERENCE_ACRO_BAD_REFERENCE,
  GUIDANCE_CYCLE_REFERENCE_ATTITUDE_BAD_REFERENCE,
  GUIDANCE_CYCLE_REFERENCE_ACRO_NO_SAMPLE,
  GUIDANCE_CYCLE_REFERENCE_POSITION_AFTER_ACRO_NO_SAMPLE,
  GUIDANCE_CYCLE_REFERENCE_ATTITUDE_NO_SAMPLE,
  GUIDANCE_CYCLE_REFERENCE_POSITION_AFTER_ATTITUDE_NO_SAMPLE,
  GUIDANCE_CYCLE_RAPID_REARM_LOW_STALE_ARMED_HEALTH,
  GUIDANCE_CYCLE_RAPID_REARM_HIGH_STALE_ARMED_HEALTH,
  GUIDANCE_CYCLE_RAPID_REARM_HIGH_DISARMED_HEALTH,
  GUIDANCE_CYCLE_RAPID_REARM_LOW_DISARMED_ACK,
  GUIDANCE_CYCLE_RAPID_REARM_READY_RECOVERY,
  GUIDANCE_CYCLE_COUNT,
};

struct guidance_generated_observation {
  bool armed;
  int32_t mode;
  float stick[3];
  float throttle;
  float position[3];
  float velocity[3];
  float quaternion[4];
  float reference_position[3];
  float reference_velocity[3];
  float reference_acceleration[3];
  float reference_yaw;
  uint32_t error_signal_status;
  float output_rates[3];
  float output_thrust;
};

struct guidance_publication_observation {
  size_t rate_publish_count;
  size_t attitude_publish_count;
  synapse_topic_RateCommandData_t rate;
  synapse_topic_AttitudeCommandData_t attitude;
};

static jmp_buf guidance_loop_escape;
static size_t guidance_active_cycle;
static size_t guidance_next_cycle;
static size_t guidance_stop_cycle;
static size_t guidance_step_count;
static struct guidance_generated_observation
    guidance_generated[GUIDANCE_CYCLE_COUNT];
static struct guidance_publication_observation
    guidance_publications[GUIDANCE_CYCLE_COUNT];

struct zros_topic guidance_fake_topic_attitude_command;
struct zros_topic guidance_fake_topic_attitude_estimate;
struct zros_topic guidance_fake_topic_manual_input;
struct zros_topic guidance_fake_topic_navigation_odometry;
struct zros_topic guidance_fake_topic_rate_command;
struct zros_topic guidance_fake_topic_trajectory_reference;
struct zros_topic guidance_fake_topic_vehicle_health;

static bool guidance_arm_switch_high(size_t cycle) {
  switch (cycle) {
  case GUIDANCE_CYCLE_FLAG_ACK:
  case GUIDANCE_CYCLE_FLAG_INVALID_LOW:
  case GUIDANCE_CYCLE_QUALITY_ACK:
  case GUIDANCE_CYCLE_NONFINITE_NAV_ACK:
  case GUIDANCE_CYCLE_ERROR_ACK:
  case GUIDANCE_CYCLE_NAN_ACK:
  case GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_FAILED_LOW:
  case GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_INVALID_LOW:
  case GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_ACK:
  case GUIDANCE_CYCLE_REFERENCE_PREARM_FAILED_LOW:
  case GUIDANCE_CYCLE_REFERENCE_PREARM_INVALID_LOW:
  case GUIDANCE_CYCLE_REFERENCE_PREARM_VALID_LOW_ACK:
  case GUIDANCE_CYCLE_RAPID_REARM_LOW_STALE_ARMED_HEALTH:
  case GUIDANCE_CYCLE_RAPID_REARM_LOW_DISARMED_ACK:
    return false;
  default:
    return true;
  }
}

bool guidance_fake_gnss_onboard_ready_get(void) {
  switch (guidance_active_cycle) {
  case GUIDANCE_CYCLE_POSITION_UNREADY_ARMED_REQUEST:
  case GUIDANCE_CYCLE_POSITION_LOST_ARMED:
  case GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_HIGH:
  case GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_HIGH_LATCHED:
  case GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_FAILED_LOW:
  case GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_INVALID_LOW:
  case GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_ACK:
  case GUIDANCE_CYCLE_ACRO_UNREADY_ARMED:
  case GUIDANCE_CYCLE_ATTITUDE_UNREADY_ARMED:
  case GUIDANCE_CYCLE_REFERENCE_GNSS_LOST_ARMED:
  case GUIDANCE_CYCLE_REFERENCE_MODE_EXIT_ARMED:
  case GUIDANCE_CYCLE_REFERENCE_RETAINED_MODE_EXIT_ARMED:
  case GUIDANCE_CYCLE_REFERENCE_ACRO_BAD_REFERENCE:
  case GUIDANCE_CYCLE_REFERENCE_ATTITUDE_BAD_REFERENCE:
  case GUIDANCE_CYCLE_RAPID_REARM_LOW_STALE_ARMED_HEALTH:
  case GUIDANCE_CYCLE_RAPID_REARM_HIGH_STALE_ARMED_HEALTH:
  case GUIDANCE_CYCLE_RAPID_REARM_HIGH_DISARMED_HEALTH:
    return false;
  default:
    return true;
  }
}

void GuidanceController_startup(GuidanceControllerState *self) {
  memset(self, 0, sizeof(*self));
}

void GuidanceController_recalibrate(GuidanceControllerState *self) {
  ARG_UNUSED(self);
}

void GuidanceController_dostep(GuidanceControllerState *self) {
  struct guidance_generated_observation *observation =
      &guidance_generated[guidance_active_cycle];

  observation->armed = self->armed;
  observation->mode = self->mode;
  memcpy(observation->stick, self->stick, sizeof(observation->stick));
  observation->throttle = self->throttle;
  memcpy(observation->position, self->positionWorldEnu_m,
         sizeof(observation->position));
  memcpy(observation->velocity, self->velocityWorldEnu_m_s,
         sizeof(observation->velocity));
  memcpy(observation->quaternion, self->quaternionWorldBody,
         sizeof(observation->quaternion));
  memcpy(observation->reference_position, self->positionWorld_m,
         sizeof(observation->reference_position));
  memcpy(observation->reference_velocity, self->velocityWorld_m_s,
         sizeof(observation->reference_velocity));
  memcpy(observation->reference_acceleration, self->accelerationWorld_m_s2,
         sizeof(observation->reference_acceleration));
  observation->reference_yaw = self->yaw_rad;

  self->rumoca_galec_error_signal_status = 0U;
  self->angularVelocityCommandFlu_rad_s[0] = self->armed ? 0.1f : 0.0f;
  self->angularVelocityCommandFlu_rad_s[1] = self->armed ? -0.2f : 0.0f;
  self->angularVelocityCommandFlu_rad_s[2] = self->armed ? 0.3f : 0.0f;
  self->thrust_N = self->armed ? 4.5f : 0.0f;
  if (guidance_active_cycle == GUIDANCE_CYCLE_GENERATED_ERROR) {
    self->rumoca_galec_error_signal_status = UINT32_C(0x80);
  } else if (guidance_active_cycle == GUIDANCE_CYCLE_GENERATED_LATE_NAN) {
    self->angularVelocityCommandFlu_rad_s[2] = NAN;
  }

  observation->error_signal_status = self->rumoca_galec_error_signal_status;
  memcpy(observation->output_rates, self->angularVelocityCommandFlu_rad_s,
         sizeof(observation->output_rates));
  observation->output_thrust = self->thrust_N;
  guidance_step_count++;
}

void guidance_fake_zros_node_init(struct zros_node *node, const char *name) {
  ARG_UNUSED(node);
  ARG_UNUSED(name);
}

int guidance_fake_zros_sub_init(struct zros_sub *sub, struct zros_node *node,
                                struct zros_topic *topic, void *data,
                                double rate_limit_hz) {
  ARG_UNUSED(node);
  ARG_UNUSED(rate_limit_hz);
  sub->_topic = topic;
  sub->_data = data;
  return 0;
}

int guidance_fake_zros_pub_init(struct zros_pub *pub, struct zros_node *node,
                                struct zros_topic *topic, void *data) {
  ARG_UNUSED(node);
  pub->_topic = topic;
  pub->_data = data;
  return 0;
}

int guidance_fake_zros_sub_wait(struct zros_sub *sub, k_timeout_t timeout) {
  ARG_UNUSED(sub);
  ARG_UNUSED(timeout);
  if (guidance_next_cycle == guidance_stop_cycle) {
    longjmp(guidance_loop_escape, 1);
  }
  guidance_active_cycle = guidance_next_cycle++;
  return 0;
}

static void guidance_fill_manual(void) {
  uint8_t flags = synapse_topic_ManualControlFlags_Valid |
                  synapse_topic_ManualControlFlags_Active;

  if (guidance_arm_switch_high(guidance_active_cycle)) {
    flags |= synapse_topic_ManualControlFlags_ArmSwitch;
  }
  if (guidance_active_cycle == GUIDANCE_CYCLE_FLAG_INVALID_LOW ||
      guidance_active_cycle == GUIDANCE_CYCLE_INVALID_MANUAL ||
      guidance_active_cycle ==
          GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_INVALID_LOW ||
      guidance_active_cycle ==
          GUIDANCE_CYCLE_REFERENCE_INVALID_MODE_EXIT_ARMED ||
      guidance_active_cycle == GUIDANCE_CYCLE_REFERENCE_PREARM_INVALID_LOW) {
    flags = 0U;
  } else if (guidance_active_cycle == GUIDANCE_CYCLE_KILL_SWITCH_ARMED) {
    flags |= synapse_topic_ManualControlFlags_KillSwitch;
  }
  uint8_t flight_mode = 2U;

  if (guidance_active_cycle == GUIDANCE_CYCLE_ACRO_UNREADY_ARMED ||
      guidance_active_cycle == GUIDANCE_CYCLE_REFERENCE_ACRO_NO_SAMPLE ||
      guidance_active_cycle == GUIDANCE_CYCLE_REFERENCE_ACRO_BAD_REFERENCE) {
    flight_mode = 0U;
  } else if (guidance_active_cycle == GUIDANCE_CYCLE_ATTITUDE_UNREADY_ARMED ||
             guidance_active_cycle ==
                 GUIDANCE_CYCLE_REFERENCE_INVALID_MODE_EXIT_ARMED ||
             guidance_active_cycle ==
                 GUIDANCE_CYCLE_REFERENCE_FAILED_MODE_EXIT_ARMED ||
             guidance_active_cycle ==
                 GUIDANCE_CYCLE_REFERENCE_MODE_EXIT_ARMED ||
             guidance_active_cycle ==
                 GUIDANCE_CYCLE_REFERENCE_ATTITUDE_NO_SAMPLE ||
             guidance_active_cycle ==
                 GUIDANCE_CYCLE_REFERENCE_GNSS_MODE_EXIT_ARMED ||
             guidance_active_cycle ==
                 GUIDANCE_CYCLE_REFERENCE_ATTITUDE_BAD_REFERENCE) {
    flight_mode = 1U;
  }
  g_process.manual = (synapse_topic_ManualControlData_t){
      .timestamp_ns =
          TEST_NS_FROM_US(UINT64_C(300000) + guidance_active_cycle * 20000U),
      .roll_milli = 125,
      .pitch_milli = -250,
      .yaw_milli = 375,
      .throttle_milli = 500,
      .flight_mode = flight_mode,
      .flags = flags,
  };
}

static void guidance_fill_navigation(void) {
  g_process.attitude = (synapse_topic_AttitudeEstimateData_t){
      .timestamp_ns =
          TEST_NS_FROM_US(UINT64_C(300000) + guidance_active_cycle * 20000U),
      .attitude = {.w = 0.9f, .x = 0.1f, .y = -0.2f, .z = 0.3f},
      .angular_velocity_flu_rad_s = {.roll = 0.01f,
                                     .pitch = -0.02f,
                                     .yaw = 0.03f},
      .flags = synapse_topic_AttitudeEstimateFlags_AttitudeValid |
               synapse_topic_AttitudeEstimateFlags_RatesValid,
  };
  g_process.odometry = (synapse_topic_OdometryEstimateData_t){
      .timestamp_ns = g_process.attitude.timestamp_ns,
      .position_enu_m = {.x = 1.0f, .y = 2.0f, .z = 3.0f},
      .velocity_enu_m_s = {.x = 0.4f, .y = 0.5f, .z = 0.6f},
      .quality_pct = 80,
  };

  switch (guidance_active_cycle) {
  case GUIDANCE_CYCLE_FLAG_FAULT:
    g_process.attitude.flags = synapse_topic_AttitudeEstimateFlags_RatesValid;
    break;
  case GUIDANCE_CYCLE_QUALITY_FAULT:
    g_process.odometry.quality_pct = 0;
    break;
  case GUIDANCE_CYCLE_NONFINITE_NAV_FAULT:
    g_process.odometry.velocity_enu_m_s.z = INFINITY;
    break;
  default:
    break;
  }
}

static void guidance_fill_reference(void) {
  uint64_t control_now_ns =
      TEST_NS_FROM_US(UINT64_C(300000) + guidance_active_cycle * 20000U);

  g_process.reference = (synapse_topic_LocalPositionCommandData_t){
      .timestamp_ns = control_now_ns - TEST_NS_FROM_US(UINT64_C(1000)),
      .position_enu_m = {.x = 4.0f, .y = 5.0f, .z = 6.0f},
      .velocity_enu_m_s = {.x = 0.7f, .y = 0.8f, .z = 0.9f},
      .acceleration_or_force_enu = {.x = -0.1f, .y = -0.2f, .z = -0.3f},
      .yaw_rad = 0.75f,
      .yaw_rate_rad_s = -0.25f,
      .coordinate_frame = synapse_types_LocalFrame_LocalEnu,
  };

  switch (guidance_active_cycle) {
  case GUIDANCE_CYCLE_REFERENCE_EXACT_NOW_ARMED:
    g_process.reference.timestamp_ns = control_now_ns;
    break;
  case GUIDANCE_CYCLE_REFERENCE_EXACT_100_MS_ARMED:
    g_process.reference.timestamp_ns =
        control_now_ns - TEST_NS_FROM_US(UINT64_C(100000));
    break;
  case GUIDANCE_CYCLE_REFERENCE_FUTURE_ARMED:
    g_process.reference.timestamp_ns =
        control_now_ns + TEST_NS_FROM_US(UINT64_C(1));
    break;
  case GUIDANCE_CYCLE_REFERENCE_STALE_ARMED:
    g_process.reference.timestamp_ns =
        control_now_ns - TEST_NS_FROM_US(UINT64_C(100001));
    break;
  case GUIDANCE_CYCLE_REFERENCE_BAD_FRAME_ARMED:
    g_process.reference.coordinate_frame = synapse_types_LocalFrame_BodyFlu;
    break;
  case GUIDANCE_CYCLE_REFERENCE_BAD_MASK_ARMED:
    g_process.reference.type_mask =
        synapse_topic_LocalPositionCommandMask_IgnoreYawRate;
    break;
  case GUIDANCE_CYCLE_REFERENCE_NONFINITE_POSITION_ARMED:
    g_process.reference.position_enu_m.z = NAN;
    break;
  case GUIDANCE_CYCLE_REFERENCE_NONFINITE_VELOCITY_ARMED:
    g_process.reference.velocity_enu_m_s.x = INFINITY;
    break;
  case GUIDANCE_CYCLE_REFERENCE_NONFINITE_ACCELERATION_ARMED:
    g_process.reference.acceleration_or_force_enu.y = -INFINITY;
    break;
  case GUIDANCE_CYCLE_REFERENCE_NONFINITE_YAW_ARMED:
    g_process.reference.yaw_rad = NAN;
    break;
  case GUIDANCE_CYCLE_REFERENCE_NONFINITE_YAW_RATE_ARMED:
    g_process.reference.yaw_rate_rad_s = NAN;
    break;
  case GUIDANCE_CYCLE_REFERENCE_ACRO_BAD_REFERENCE:
  case GUIDANCE_CYCLE_REFERENCE_ATTITUDE_BAD_REFERENCE:
    g_process.reference.position_enu_m.x = NAN;
    break;
  default:
    break;
  }
}

int guidance_fake_zros_sub_update(struct zros_sub *sub) {
  if (sub == &g_process.manual_sub) {
    if (guidance_active_cycle ==
        GUIDANCE_CYCLE_REFERENCE_RETAINED_MODE_EXIT_ARMED) {
      return -1;
    }
    guidance_fill_manual();
    if (guidance_active_cycle ==
            GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_FAILED_LOW ||
        guidance_active_cycle == GUIDANCE_CYCLE_REFERENCE_PREARM_FAILED_LOW ||
        guidance_active_cycle ==
            GUIDANCE_CYCLE_REFERENCE_FAILED_MODE_EXIT_ARMED) {
      return -1;
    }
    return 0;
  }
  if (sub == &g_process.health_sub) {
    g_process.health = (synapse_topic_VehicleHealthData_t){
        .flags = synapse_topic_VehicleHealthFlags_Armed,
    };
    switch (guidance_active_cycle) {
    case GUIDANCE_CYCLE_FLAG_ACK:
    case GUIDANCE_CYCLE_QUALITY_ACK:
    case GUIDANCE_CYCLE_NONFINITE_NAV_ACK:
    case GUIDANCE_CYCLE_ERROR_ACK:
    case GUIDANCE_CYCLE_NAN_ACK:
    case GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_HIGH:
    case GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_HIGH_LATCHED:
    case GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_FAILED_LOW:
    case GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_INVALID_LOW:
    case GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_ACK:
    case GUIDANCE_CYCLE_REFERENCE_PREARM_NO_SAMPLE_HIGH:
    case GUIDANCE_CYCLE_REFERENCE_PREARM_VALID_HIGH_LATCHED:
    case GUIDANCE_CYCLE_REFERENCE_PREARM_FAILED_LOW:
    case GUIDANCE_CYCLE_REFERENCE_PREARM_INVALID_LOW:
    case GUIDANCE_CYCLE_REFERENCE_PREARM_VALID_LOW_ACK:
    case GUIDANCE_CYCLE_RAPID_REARM_HIGH_DISARMED_HEALTH:
    case GUIDANCE_CYCLE_RAPID_REARM_LOW_DISARMED_ACK:
      g_process.health.flags = 0U;
      break;
    default:
      break;
    }
    if (guidance_active_cycle == GUIDANCE_CYCLE_HEALTH_FAILSAFE) {
      g_process.health.flags |= synapse_topic_VehicleHealthFlags_Failsafe;
    }
    return 0;
  }
  if (sub == &g_process.attitude_sub) {
    guidance_fill_navigation();
    return 0;
  }
  if (sub == &g_process.odometry_sub) {
    return 0;
  }
  if (sub == &g_process.reference_sub) {
    if (guidance_active_cycle == GUIDANCE_CYCLE_REFERENCE_NO_SAMPLE_ARMED ||
        guidance_active_cycle ==
            GUIDANCE_CYCLE_REFERENCE_PREARM_NO_SAMPLE_HIGH ||
        guidance_active_cycle == GUIDANCE_CYCLE_REFERENCE_ACRO_NO_SAMPLE ||
        guidance_active_cycle == GUIDANCE_CYCLE_REFERENCE_ATTITUDE_NO_SAMPLE) {
      guidance_fill_reference();
      return -1;
    }
    guidance_fill_reference();
    return 0;
  }
  return -1;
}

int guidance_fake_zros_pub_update(struct zros_pub *pub) {
  struct guidance_publication_observation *observation =
      &guidance_publications[guidance_active_cycle];

  if (pub == &g_process.rate_command_pub) {
    observation->rate_publish_count++;
    observation->rate = g_process.rate_command;
    return 0;
  }
  if (pub == &g_process.attitude_command_pub) {
    observation->attitude_publish_count++;
    observation->attitude = g_process.attitude_command;
    return 0;
  }
  return -1;
}

static void guidance_expect_not_published(size_t cycle) {
  zexpect_equal(guidance_publications[cycle].rate_publish_count, 0U,
                "cycle %zu unexpectedly published a rate command", cycle);
  zexpect_equal(guidance_publications[cycle].attitude_publish_count, 0U,
                "cycle %zu unexpectedly published an attitude command", cycle);
}

static void guidance_expect_armed_publication(size_t cycle) {
  const struct guidance_publication_observation *observation =
      &guidance_publications[cycle];

  const uint64_t timestamp_ns =
      TEST_NS_FROM_US(UINT64_C(300000) + cycle * 20000U);

  zexpect_equal(observation->rate_publish_count, 1U,
                "cycle %zu rate publish count mismatch", cycle);
  zexpect_equal(observation->attitude_publish_count, 1U,
                "cycle %zu attitude publish count mismatch", cycle);
  zexpect_equal(observation->rate.timestamp_ns, timestamp_ns);
  zexpect_equal(observation->attitude.timestamp_ns, timestamp_ns);
  zexpect_equal(observation->rate.type_mask, 0U);
  zexpect_equal(observation->attitude.type_mask, 0U);
  zexpect_equal(observation->rate.body_rate_flu_rad_s.roll, 0.1f);
  zexpect_equal(observation->rate.body_rate_flu_rad_s.pitch, -0.2f);
  zexpect_equal(observation->rate.body_rate_flu_rad_s.yaw, 0.3f);
  zexpect_equal(observation->rate.thrust, 4.5f);
  zexpect_equal(observation->attitude.thrust, 4.5f);
  zexpect_equal(observation->attitude.attitude.w, 0.9f);
}

static void guidance_expect_disarmed_publication(size_t cycle) {
  const struct guidance_publication_observation *observation =
      &guidance_publications[cycle];

  const uint64_t timestamp_ns =
      TEST_NS_FROM_US(UINT64_C(300000) + cycle * 20000U);

  zexpect_equal(observation->rate_publish_count, 1U,
                "cycle %zu safe rate publish count mismatch", cycle);
  zexpect_equal(observation->attitude_publish_count, 1U,
                "cycle %zu safe attitude publish count mismatch", cycle);
  zexpect_equal(observation->rate.timestamp_ns, timestamp_ns);
  zexpect_equal(observation->attitude.timestamp_ns, timestamp_ns);
  zexpect_equal(observation->rate.type_mask, 0U);
  zexpect_equal(observation->attitude.type_mask, 0U);
  zexpect_equal(observation->rate.body_rate_flu_rad_s.roll, 0.0f);
  zexpect_equal(observation->rate.body_rate_flu_rad_s.pitch, 0.0f);
  zexpect_equal(observation->rate.body_rate_flu_rad_s.yaw, 0.0f);
  zexpect_equal(observation->rate.thrust, 0.0f);
}

static void guidance_expect_navigation_zeroed(size_t cycle) {
  const struct guidance_generated_observation *observation =
      &guidance_generated[cycle];

  for (size_t axis = 0U; axis < ARRAY_SIZE(observation->position); ++axis) {
    zexpect_equal(observation->position[axis], 0.0f,
                  "cycle %zu position axis %zu was not zero", cycle, axis);
    zexpect_equal(observation->velocity[axis], 0.0f,
                  "cycle %zu velocity axis %zu was not zero", cycle, axis);
  }
  zexpect_equal(observation->quaternion[0], 1.0f);
  zexpect_equal(observation->quaternion[1], 0.0f);
  zexpect_equal(observation->quaternion[2], 0.0f);
  zexpect_equal(observation->quaternion[3], 0.0f);
  zexpect_false(observation->armed);
}

ZTEST(process_wrapper_fault_injection,
      test_guidance_wrapper_latches_and_requires_low_switch_ack) {
  memset(&g_process, 0, sizeof(g_process));
  memset(guidance_generated, 0, sizeof(guidance_generated));
  memset(guidance_publications, 0, sizeof(guidance_publications));
  guidance_active_cycle = 0U;
  guidance_next_cycle = 0U;
  guidance_stop_cycle = GUIDANCE_CYCLE_LEGACY_COUNT;
  guidance_step_count = 0U;

  if (setjmp(guidance_loop_escape) == 0) {
    guidance_controller_thread(&g_process, NULL, NULL);
    zassert_unreachable(
        "guidance wrapper returned before the script completed");
  }

  zexpect_equal(guidance_step_count, GUIDANCE_CYCLE_LEGACY_COUNT);
  guidance_expect_armed_publication(GUIDANCE_CYCLE_VALID);
  guidance_expect_not_published(GUIDANCE_CYCLE_FLAG_FAULT);
  guidance_expect_not_published(GUIDANCE_CYCLE_FLAG_LATCHED);
  guidance_expect_not_published(GUIDANCE_CYCLE_FLAG_INVALID_LOW);
  guidance_expect_not_published(GUIDANCE_CYCLE_FLAG_STILL_LATCHED);
  guidance_expect_disarmed_publication(GUIDANCE_CYCLE_FLAG_ACK);
  guidance_expect_armed_publication(GUIDANCE_CYCLE_FLAG_RECOVERY);
  guidance_expect_not_published(GUIDANCE_CYCLE_QUALITY_FAULT);
  guidance_expect_disarmed_publication(GUIDANCE_CYCLE_QUALITY_ACK);
  guidance_expect_armed_publication(GUIDANCE_CYCLE_QUALITY_RECOVERY);
  guidance_expect_not_published(GUIDANCE_CYCLE_NONFINITE_NAV_FAULT);
  guidance_expect_disarmed_publication(GUIDANCE_CYCLE_NONFINITE_NAV_ACK);
  guidance_expect_armed_publication(GUIDANCE_CYCLE_NONFINITE_NAV_RECOVERY);
  guidance_expect_not_published(GUIDANCE_CYCLE_GENERATED_ERROR);
  guidance_expect_not_published(GUIDANCE_CYCLE_ERROR_LATCHED);
  guidance_expect_disarmed_publication(GUIDANCE_CYCLE_ERROR_ACK);
  guidance_expect_armed_publication(GUIDANCE_CYCLE_ERROR_RECOVERY);
  guidance_expect_not_published(GUIDANCE_CYCLE_GENERATED_LATE_NAN);
  guidance_expect_not_published(GUIDANCE_CYCLE_NAN_LATCHED);
  guidance_expect_disarmed_publication(GUIDANCE_CYCLE_NAN_ACK);
  guidance_expect_armed_publication(GUIDANCE_CYCLE_NAN_RECOVERY);
  guidance_expect_disarmed_publication(GUIDANCE_CYCLE_HEALTH_FAILSAFE);
  guidance_expect_disarmed_publication(GUIDANCE_CYCLE_INVALID_MANUAL);
  guidance_expect_disarmed_publication(GUIDANCE_CYCLE_KILL_SWITCH_ARMED);
  guidance_expect_armed_publication(
      GUIDANCE_CYCLE_POSITION_UNREADY_ARMED_REQUEST);
  guidance_expect_armed_publication(GUIDANCE_CYCLE_POSITION_READY_ARMED);
  guidance_expect_armed_publication(GUIDANCE_CYCLE_POSITION_LOST_ARMED);
  guidance_expect_not_published(GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_HIGH);
  guidance_expect_not_published(
      GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_HIGH_LATCHED);
  guidance_expect_not_published(
      GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_FAILED_LOW);
  guidance_expect_not_published(
      GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_INVALID_LOW);
  guidance_expect_disarmed_publication(
      GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_ACK);
  guidance_expect_armed_publication(
      GUIDANCE_CYCLE_POSITION_READY_ARMED_RECOVERY);
  guidance_expect_armed_publication(GUIDANCE_CYCLE_ACRO_UNREADY_ARMED);
  guidance_expect_armed_publication(GUIDANCE_CYCLE_ATTITUDE_UNREADY_ARMED);

  guidance_expect_navigation_zeroed(GUIDANCE_CYCLE_FLAG_FAULT);
  guidance_expect_navigation_zeroed(GUIDANCE_CYCLE_QUALITY_FAULT);
  guidance_expect_navigation_zeroed(GUIDANCE_CYCLE_NONFINITE_NAV_FAULT);
  zexpect_true(guidance_generated[GUIDANCE_CYCLE_VALID].armed);
  zexpect_equal(guidance_generated[GUIDANCE_CYCLE_VALID].mode, 2);
  zexpect_within(guidance_generated[GUIDANCE_CYCLE_VALID].stick[0], 0.125f,
                 1.0e-6f);
  zexpect_within(guidance_generated[GUIDANCE_CYCLE_VALID].stick[1], -0.25f,
                 1.0e-6f);
  zexpect_within(guidance_generated[GUIDANCE_CYCLE_VALID].stick[2], 0.375f,
                 1.0e-6f);
  zexpect_within(guidance_generated[GUIDANCE_CYCLE_VALID].throttle, 0.5f,
                 1.0e-6f);
  zexpect_equal(guidance_generated[GUIDANCE_CYCLE_VALID].position[2], 3.0f);
  zexpect_equal(guidance_generated[GUIDANCE_CYCLE_VALID].velocity[1], 0.5f);
  zexpect_equal(guidance_generated[GUIDANCE_CYCLE_VALID].quaternion[2], -0.2f);
  zexpect_equal(guidance_generated[GUIDANCE_CYCLE_VALID].reference_position[0],
                4.0f);
  zexpect_equal(guidance_generated[GUIDANCE_CYCLE_VALID].reference_velocity[2],
                0.9f);
  zexpect_equal(
      guidance_generated[GUIDANCE_CYCLE_VALID].reference_acceleration[1],
      -0.2f);
  zexpect_equal(guidance_generated[GUIDANCE_CYCLE_VALID].reference_yaw, 0.75f);
  zexpect_false(guidance_generated[GUIDANCE_CYCLE_FLAG_LATCHED].armed);
  zexpect_false(guidance_generated[GUIDANCE_CYCLE_FLAG_INVALID_LOW].armed);
  zexpect_false(guidance_generated[GUIDANCE_CYCLE_FLAG_STILL_LATCHED].armed);
  zexpect_equal(
      guidance_generated[GUIDANCE_CYCLE_GENERATED_ERROR].error_signal_status,
      UINT32_C(0x80));
  zexpect_true(isfinite(
      guidance_generated[GUIDANCE_CYCLE_GENERATED_LATE_NAN].output_rates[0]));
  zexpect_true(isfinite(
      guidance_generated[GUIDANCE_CYCLE_GENERATED_LATE_NAN].output_rates[1]));
  zexpect_true(isnan(
      guidance_generated[GUIDANCE_CYCLE_GENERATED_LATE_NAN].output_rates[2]));
  zexpect_true(isfinite(
      guidance_generated[GUIDANCE_CYCLE_GENERATED_LATE_NAN].output_thrust));
  zexpect_false(guidance_generated[GUIDANCE_CYCLE_HEALTH_FAILSAFE].armed);
  zexpect_false(guidance_generated[GUIDANCE_CYCLE_INVALID_MANUAL].armed);
  zexpect_false(guidance_generated[GUIDANCE_CYCLE_KILL_SWITCH_ARMED].armed);
  zexpect_equal(
      guidance_generated[GUIDANCE_CYCLE_POSITION_UNREADY_ARMED_REQUEST].mode,
      1);
  zexpect_true(
      guidance_generated[GUIDANCE_CYCLE_POSITION_UNREADY_ARMED_REQUEST].armed);
  zexpect_equal(guidance_generated[GUIDANCE_CYCLE_POSITION_READY_ARMED].mode,
                1);
  zexpect_true(guidance_generated[GUIDANCE_CYCLE_POSITION_READY_ARMED].armed);
  zexpect_equal(guidance_generated[GUIDANCE_CYCLE_POSITION_LOST_ARMED].mode, 1);
  zexpect_true(guidance_generated[GUIDANCE_CYCLE_POSITION_LOST_ARMED].armed);
  zexpect_false(
      guidance_generated[GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_HIGH].armed);
  zexpect_false(
      guidance_generated[GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_HIGH_LATCHED]
          .armed);
  zexpect_false(
      guidance_generated[GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_FAILED_LOW]
          .armed);
  zexpect_false(
      guidance_generated[GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_INVALID_LOW]
          .armed);
  zexpect_false(
      guidance_generated[GUIDANCE_CYCLE_POSITION_UNREADY_DISARMED_ACK].armed);
  zexpect_equal(
      guidance_generated[GUIDANCE_CYCLE_POSITION_READY_ARMED_RECOVERY].mode, 1);
  zexpect_true(
      guidance_generated[GUIDANCE_CYCLE_POSITION_READY_ARMED_RECOVERY].armed);
  zexpect_equal(guidance_generated[GUIDANCE_CYCLE_ACRO_UNREADY_ARMED].mode, 0);
  zexpect_true(guidance_generated[GUIDANCE_CYCLE_ACRO_UNREADY_ARMED].armed);
  zexpect_equal(guidance_generated[GUIDANCE_CYCLE_ATTITUDE_UNREADY_ARMED].mode,
                1);
  zexpect_true(guidance_generated[GUIDANCE_CYCLE_ATTITUDE_UNREADY_ARMED].armed);
}

static void guidance_run_cycles(size_t first_cycle, size_t stop_cycle) {
  memset(&g_process, 0, sizeof(g_process));
  memset(guidance_generated, 0, sizeof(guidance_generated));
  memset(guidance_publications, 0, sizeof(guidance_publications));
  guidance_active_cycle = first_cycle;
  guidance_next_cycle = first_cycle;
  guidance_stop_cycle = stop_cycle;
  guidance_step_count = 0U;

  if (setjmp(guidance_loop_escape) == 0) {
    guidance_controller_thread(&g_process, NULL, NULL);
    zassert_unreachable(
        "guidance wrapper returned before the script completed");
  }

  zexpect_equal(guidance_step_count, stop_cycle - first_cycle);
}

static void guidance_expect_reference_zeroed(size_t cycle) {
  const struct guidance_generated_observation *observation =
      &guidance_generated[cycle];

  for (size_t axis = 0U; axis < ARRAY_SIZE(observation->reference_position);
       ++axis) {
    zexpect_equal(observation->reference_position[axis], 0.0f,
                  "cycle %zu position reference axis %zu was not zero", cycle,
                  axis);
    zexpect_equal(observation->reference_velocity[axis], 0.0f,
                  "cycle %zu velocity reference axis %zu was not zero", cycle,
                  axis);
    zexpect_equal(observation->reference_acceleration[axis], 0.0f,
                  "cycle %zu acceleration reference axis %zu was not zero",
                  cycle, axis);
  }
  zexpect_equal(observation->reference_yaw, 0.0f);
}

static void guidance_expect_position_fallback(size_t cycle) {
  zexpect_equal(guidance_generated[cycle].mode, 1,
                "cycle %zu did not use ATTITUDE", cycle);
  zexpect_true(guidance_generated[cycle].armed,
               "cycle %zu unexpectedly disarmed", cycle);
  guidance_expect_armed_publication(cycle);
}

ZTEST(process_wrapper_fault_injection,
      test_guidance_reference_timestamp_boundaries) {
  const size_t usable_cycles[] = {
      GUIDANCE_CYCLE_REFERENCE_EXACT_NOW_ARMED,
      GUIDANCE_CYCLE_REFERENCE_EXACT_100_MS_ARMED,
  };
  const size_t unusable_cycles[] = {
      GUIDANCE_CYCLE_REFERENCE_FUTURE_ARMED,
      GUIDANCE_CYCLE_REFERENCE_STALE_ARMED,
      GUIDANCE_CYCLE_REFERENCE_BAD_FRAME_ARMED,
      GUIDANCE_CYCLE_REFERENCE_BAD_MASK_ARMED,
  };

  for (size_t index = 0U; index < ARRAY_SIZE(usable_cycles); ++index) {
    size_t cycle = usable_cycles[index];

    guidance_run_cycles(cycle, cycle + 1U);
    zexpect_equal(guidance_generated[cycle].mode, 2);
    zexpect_true(guidance_generated[cycle].armed);
    guidance_expect_armed_publication(cycle);
  }
  for (size_t index = 0U; index < ARRAY_SIZE(unusable_cycles); ++index) {
    size_t cycle = unusable_cycles[index];

    guidance_run_cycles(cycle, cycle + 1U);
    guidance_expect_position_fallback(cycle);
    guidance_expect_reference_zeroed(cycle);
  }
}

ZTEST(process_wrapper_fault_injection,
      test_guidance_nonfinite_reference_falls_back_safely) {
  const size_t cycles[] = {
      GUIDANCE_CYCLE_REFERENCE_NONFINITE_POSITION_ARMED,
      GUIDANCE_CYCLE_REFERENCE_NONFINITE_VELOCITY_ARMED,
      GUIDANCE_CYCLE_REFERENCE_NONFINITE_ACCELERATION_ARMED,
      GUIDANCE_CYCLE_REFERENCE_NONFINITE_YAW_ARMED,
      GUIDANCE_CYCLE_REFERENCE_NONFINITE_YAW_RATE_ARMED,
  };

  for (size_t index = 0U; index < ARRAY_SIZE(cycles); ++index) {
    size_t cycle = cycles[index];

    guidance_run_cycles(cycle, cycle + 1U);
    guidance_expect_position_fallback(cycle);
    guidance_expect_reference_zeroed(cycle);
    zexpect_true(isfinite(guidance_generated[cycle].output_rates[0]));
    zexpect_true(isfinite(guidance_generated[cycle].output_rates[1]));
    zexpect_true(isfinite(guidance_generated[cycle].output_rates[2]));
    zexpect_true(isfinite(guidance_generated[cycle].output_thrust));
  }
}

ZTEST(process_wrapper_fault_injection,
      test_guidance_reference_prearm_latch_requires_current_low_ack) {
  guidance_run_cycles(GUIDANCE_CYCLE_REFERENCE_PREARM_NO_SAMPLE_HIGH,
                      GUIDANCE_CYCLE_REFERENCE_PREARM_VALID_HIGH_RECOVERY + 1U);

  guidance_expect_not_published(GUIDANCE_CYCLE_REFERENCE_PREARM_NO_SAMPLE_HIGH);
  guidance_expect_not_published(
      GUIDANCE_CYCLE_REFERENCE_PREARM_VALID_HIGH_LATCHED);
  guidance_expect_not_published(GUIDANCE_CYCLE_REFERENCE_PREARM_FAILED_LOW);
  guidance_expect_not_published(GUIDANCE_CYCLE_REFERENCE_PREARM_INVALID_LOW);
  guidance_expect_disarmed_publication(
      GUIDANCE_CYCLE_REFERENCE_PREARM_VALID_LOW_ACK);
  guidance_expect_armed_publication(
      GUIDANCE_CYCLE_REFERENCE_PREARM_VALID_HIGH_RECOVERY);
  zexpect_false(
      guidance_generated[GUIDANCE_CYCLE_REFERENCE_PREARM_NO_SAMPLE_HIGH].armed);
  zexpect_false(
      guidance_generated[GUIDANCE_CYCLE_REFERENCE_PREARM_VALID_HIGH_LATCHED]
          .armed);
  zexpect_equal(
      guidance_generated[GUIDANCE_CYCLE_REFERENCE_PREARM_VALID_HIGH_RECOVERY]
          .mode,
      2);
}

ZTEST(process_wrapper_fault_injection,
      test_guidance_airborne_reference_loss_requires_mode_exit) {
  guidance_run_cycles(GUIDANCE_CYCLE_REFERENCE_NO_SAMPLE_ARMED,
                      GUIDANCE_CYCLE_REFERENCE_POSITION_AFTER_EXIT_ARMED + 1U);

  guidance_expect_position_fallback(GUIDANCE_CYCLE_REFERENCE_NO_SAMPLE_ARMED);
  guidance_expect_reference_zeroed(GUIDANCE_CYCLE_REFERENCE_NO_SAMPLE_ARMED);
  guidance_expect_position_fallback(GUIDANCE_CYCLE_REFERENCE_RECOVERED_ARMED);
  guidance_expect_disarmed_publication(
      GUIDANCE_CYCLE_REFERENCE_INVALID_MODE_EXIT_ARMED);
  guidance_expect_position_fallback(
      GUIDANCE_CYCLE_REFERENCE_POSITION_AFTER_INVALID_EXIT_ARMED);
  guidance_expect_armed_publication(
      GUIDANCE_CYCLE_REFERENCE_FAILED_MODE_EXIT_ARMED);
  guidance_expect_position_fallback(
      GUIDANCE_CYCLE_REFERENCE_POSITION_AFTER_FAILED_EXIT_ARMED);
  guidance_expect_armed_publication(GUIDANCE_CYCLE_REFERENCE_MODE_EXIT_ARMED);
  guidance_expect_armed_publication(
      GUIDANCE_CYCLE_REFERENCE_RETAINED_MODE_EXIT_ARMED);
  guidance_expect_armed_publication(
      GUIDANCE_CYCLE_REFERENCE_POSITION_AFTER_EXIT_ARMED);
  zexpect_equal(
      guidance_generated[GUIDANCE_CYCLE_REFERENCE_RECOVERED_ARMED].mode, 1);
  zexpect_equal(guidance_generated
                    [GUIDANCE_CYCLE_REFERENCE_POSITION_AFTER_INVALID_EXIT_ARMED]
                        .mode,
                1);
  zexpect_equal(guidance_generated
                    [GUIDANCE_CYCLE_REFERENCE_POSITION_AFTER_FAILED_EXIT_ARMED]
                        .mode,
                1);
  zexpect_equal(
      guidance_generated[GUIDANCE_CYCLE_REFERENCE_MODE_EXIT_ARMED].mode, 1);
  zexpect_equal(
      guidance_generated[GUIDANCE_CYCLE_REFERENCE_RETAINED_MODE_EXIT_ARMED]
          .mode,
      1);
  zexpect_equal(
      guidance_generated[GUIDANCE_CYCLE_REFERENCE_POSITION_AFTER_EXIT_ARMED]
          .mode,
      2);
}

ZTEST(process_wrapper_fault_injection,
      test_guidance_gnss_loss_uses_same_fallback_lifecycle) {
  guidance_run_cycles(GUIDANCE_CYCLE_REFERENCE_GNSS_LOST_ARMED,
                      GUIDANCE_CYCLE_REFERENCE_GNSS_POSITION_AFTER_EXIT_ARMED +
                          1U);

  guidance_expect_position_fallback(GUIDANCE_CYCLE_REFERENCE_GNSS_LOST_ARMED);
  guidance_expect_position_fallback(
      GUIDANCE_CYCLE_REFERENCE_GNSS_RECOVERED_ARMED);
  guidance_expect_armed_publication(
      GUIDANCE_CYCLE_REFERENCE_GNSS_MODE_EXIT_ARMED);
  guidance_expect_armed_publication(
      GUIDANCE_CYCLE_REFERENCE_GNSS_POSITION_AFTER_EXIT_ARMED);
  zexpect_equal(
      guidance_generated[GUIDANCE_CYCLE_REFERENCE_GNSS_RECOVERED_ARMED].mode,
      1);
  zexpect_equal(
      guidance_generated[GUIDANCE_CYCLE_REFERENCE_GNSS_MODE_EXIT_ARMED].mode,
      1);
  zexpect_equal(guidance_generated
                    [GUIDANCE_CYCLE_REFERENCE_GNSS_POSITION_AFTER_EXIT_ARMED]
                        .mode,
                2);
}

ZTEST(process_wrapper_fault_injection,
      test_guidance_acro_attitude_ignore_position_capability) {
  guidance_run_cycles(GUIDANCE_CYCLE_REFERENCE_ACRO_BAD_REFERENCE,
                      GUIDANCE_CYCLE_REFERENCE_ATTITUDE_BAD_REFERENCE + 1U);

  guidance_expect_armed_publication(
      GUIDANCE_CYCLE_REFERENCE_ACRO_BAD_REFERENCE);
  guidance_expect_armed_publication(
      GUIDANCE_CYCLE_REFERENCE_ATTITUDE_BAD_REFERENCE);
  zexpect_equal(
      guidance_generated[GUIDANCE_CYCLE_REFERENCE_ACRO_BAD_REFERENCE].mode, 0);
  zexpect_equal(
      guidance_generated[GUIDANCE_CYCLE_REFERENCE_ATTITUDE_BAD_REFERENCE].mode,
      1);
  zexpect_true(
      guidance_generated[GUIDANCE_CYCLE_REFERENCE_ACRO_BAD_REFERENCE].armed);
  zexpect_true(
      guidance_generated[GUIDANCE_CYCLE_REFERENCE_ATTITUDE_BAD_REFERENCE]
          .armed);
  guidance_expect_reference_zeroed(GUIDANCE_CYCLE_REFERENCE_ACRO_BAD_REFERENCE);
  guidance_expect_reference_zeroed(
      GUIDANCE_CYCLE_REFERENCE_ATTITUDE_BAD_REFERENCE);

  guidance_run_cycles(GUIDANCE_CYCLE_REFERENCE_ACRO_NO_SAMPLE,
                      GUIDANCE_CYCLE_REFERENCE_POSITION_AFTER_ACRO_NO_SAMPLE +
                          1U);
  guidance_expect_armed_publication(GUIDANCE_CYCLE_REFERENCE_ACRO_NO_SAMPLE);
  guidance_expect_armed_publication(
      GUIDANCE_CYCLE_REFERENCE_POSITION_AFTER_ACRO_NO_SAMPLE);
  zexpect_equal(
      guidance_generated[GUIDANCE_CYCLE_REFERENCE_ACRO_NO_SAMPLE].mode, 0);
  zexpect_equal(
      guidance_generated[GUIDANCE_CYCLE_REFERENCE_POSITION_AFTER_ACRO_NO_SAMPLE]
          .mode,
      2);

  guidance_run_cycles(
      GUIDANCE_CYCLE_REFERENCE_ATTITUDE_NO_SAMPLE,
      GUIDANCE_CYCLE_REFERENCE_POSITION_AFTER_ATTITUDE_NO_SAMPLE + 1U);
  guidance_expect_armed_publication(
      GUIDANCE_CYCLE_REFERENCE_ATTITUDE_NO_SAMPLE);
  guidance_expect_armed_publication(
      GUIDANCE_CYCLE_REFERENCE_POSITION_AFTER_ATTITUDE_NO_SAMPLE);
  zexpect_equal(
      guidance_generated[GUIDANCE_CYCLE_REFERENCE_ATTITUDE_NO_SAMPLE].mode, 1);
  zexpect_equal(guidance_generated
                    [GUIDANCE_CYCLE_REFERENCE_POSITION_AFTER_ATTITUDE_NO_SAMPLE]
                        .mode,
                2);
}

ZTEST(process_wrapper_fault_injection,
      test_guidance_rapid_rearm_waits_for_disarmed_health) {
  guidance_run_cycles(GUIDANCE_CYCLE_RAPID_REARM_LOW_STALE_ARMED_HEALTH,
                      GUIDANCE_CYCLE_RAPID_REARM_READY_RECOVERY + 1U);

  guidance_expect_disarmed_publication(
      GUIDANCE_CYCLE_RAPID_REARM_LOW_STALE_ARMED_HEALTH);
  guidance_expect_not_published(
      GUIDANCE_CYCLE_RAPID_REARM_HIGH_STALE_ARMED_HEALTH);
  guidance_expect_not_published(
      GUIDANCE_CYCLE_RAPID_REARM_HIGH_DISARMED_HEALTH);
  guidance_expect_disarmed_publication(
      GUIDANCE_CYCLE_RAPID_REARM_LOW_DISARMED_ACK);
  guidance_expect_armed_publication(GUIDANCE_CYCLE_RAPID_REARM_READY_RECOVERY);
  zexpect_equal(
      guidance_generated[GUIDANCE_CYCLE_RAPID_REARM_READY_RECOVERY].mode, 2);
}
