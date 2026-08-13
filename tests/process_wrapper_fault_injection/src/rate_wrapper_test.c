/* SPDX-License-Identifier: Apache-2.0 */

#include <math.h>
#include <setjmp.h>
#include <string.h>
#include <zephyr/ztest.h>

#include "../../../src/processes/rate_control_allocator.c"

enum {
  CYCLE_INITIAL_UNOBSERVED = 0,
  CYCLE_INITIAL_ACK,
  CYCLE_INITIAL_RECOVERY,
  CYCLE_FUTURE_COMMAND,
  CYCLE_LATCHED_AFTER_FUTURE,
  CYCLE_FUTURE_ACK,
  CYCLE_FUTURE_RECOVERY,
  CYCLE_MASKED_COMMAND,
  CYCLE_MASK_ACK,
  CYCLE_MASK_RECOVERY,
  CYCLE_NONFINITE_COMMAND,
  CYCLE_COMMAND_ACK,
  CYCLE_COMMAND_RECOVERY,
  CYCLE_INVALID_NAV_FLAGS,
  CYCLE_NAV_FLAGS_ACK,
  CYCLE_NAV_FLAGS_RECOVERY,
  CYCLE_NONFINITE_NAV_RATE,
  CYCLE_NAV_RATE_ACK,
  CYCLE_NAV_RATE_RECOVERY,
  CYCLE_STALE_COMMAND,
  CYCLE_STALE_LOW_WHILE_STALLED,
  CYCLE_STALE_HIGH_REARM_WHILE_STALLED,
  CYCLE_STALE_FRESH_ACK,
  CYCLE_STALE_RECOVERY,
  CYCLE_GENERATED_ERROR,
  CYCLE_GENERATED_ERROR_ACK,
  CYCLE_GENERATED_ERROR_RECOVERY,
  CYCLE_GENERATED_LATE_NAN,
  CYCLE_GENERATED_NAN_ACK,
  CYCLE_GENERATED_NAN_RECOVERY,
  CYCLE_ARM_RACE_LOW_FRESH,
  CYCLE_ARM_RACE_HIGH_OLD_UPDATED,
  CYCLE_ARM_RACE_LOW_FUTURE_PRELOAD,
  CYCLE_ARM_RACE_HIGH_EQUAL_RETAINED,
  CYCLE_ARM_RACE_LOW_FRESH_ACK,
  CYCLE_ARM_RACE_HIGH_ANNOUNCE_FRESH,
  CYCLE_ARM_RACE_HIGH_AUTHORIZED_FRESH,
  TEST_CYCLE_COUNT,
};

struct motor_observation {
  rdd2_motor_values_t motors;
  bool armed;
  bool arm_switch;
  bool failsafe;
  bool test_mode;
};

struct generated_observation {
  bool armed;
  float thrust;
  float command[3];
  float measured[3];
  uint32_t error_signal_status;
  float motor[EFMU_MOTOR_COUNT];
};

static jmp_buf g_loop_escape;
static size_t g_cycle;
static struct motor_observation g_motor_observations[TEST_CYCLE_COUNT];
static struct generated_observation g_generated_observations[TEST_CYCLE_COUNT];
static size_t g_health_publish_count[TEST_CYCLE_COUNT];
static bool g_health_publish_armed[TEST_CYCLE_COUNT];

struct zros_topic topic_rate_command;
struct zros_topic topic_attitude_estimate;
struct zros_topic topic_control_imu;
struct zros_topic topic_vehicle_health;
struct zros_topic topic_control_loop_metrics;

static uint64_t control_time_ns(size_t cycle) {
  if (cycle <= CYCLE_NAV_RATE_RECOVERY) {
    return (UINT64_C(100000) + cycle * UINT64_C(5000)) * UINT64_C(1000);
  }
  return (UINT64_C(125001) + cycle * UINT64_C(5000)) * UINT64_C(1000);
}

static bool arm_switch_high(size_t cycle) {
  switch (cycle) {
  case CYCLE_INITIAL_UNOBSERVED:
  case CYCLE_INITIAL_ACK:
  case CYCLE_FUTURE_ACK:
  case CYCLE_MASK_ACK:
  case CYCLE_COMMAND_ACK:
  case CYCLE_NAV_FLAGS_ACK:
  case CYCLE_NAV_RATE_ACK:
  case CYCLE_STALE_LOW_WHILE_STALLED:
  case CYCLE_STALE_FRESH_ACK:
  case CYCLE_GENERATED_ERROR_ACK:
  case CYCLE_GENERATED_NAN_ACK:
  case CYCLE_ARM_RACE_LOW_FRESH:
  case CYCLE_ARM_RACE_LOW_FUTURE_PRELOAD:
  case CYCLE_ARM_RACE_LOW_FRESH_ACK:
    return false;
  default:
    return true;
  }
}

void RateControlAllocator_startup(RateControlAllocatorState *self) {
  memset(self, 0, sizeof(*self));
}

void RateControlAllocator_recalibrate(RateControlAllocatorState *self) {
  ARG_UNUSED(self);
}

void RateControlAllocator_dostep(RateControlAllocatorState *self) {
  struct generated_observation *observation =
      &g_generated_observations[g_cycle];

  observation->armed = self->armed;
  observation->thrust = self->thrust_N;
  memcpy(observation->command, self->angularVelocityCommandFlu_rad_s,
         sizeof(observation->command));
  memcpy(observation->measured, self->angularVelocityMeasuredFlu_rad_s,
         sizeof(observation->measured));

  self->rumoca_galec_error_signal_status =
      g_cycle == CYCLE_GENERATED_ERROR ? UINT32_C(0x80) : 0U;
  for (size_t motor = 0U; motor < ARRAY_SIZE(self->motor); ++motor) {
    self->motor[motor] = self->armed ? 0.2f + 0.01f * (float)motor : 0.0f;
  }
  if (g_cycle == CYCLE_GENERATED_LATE_NAN) {
    self->motor[2] = NAN;
  }

  observation->error_signal_status = self->rumoca_galec_error_signal_status;
  memcpy(observation->motor, self->motor, sizeof(observation->motor));
}

void zros_node_init(struct zros_node *node, const char *name) {
  ARG_UNUSED(node);
  ARG_UNUSED(name);
}

int zros_sub_init(struct zros_sub *sub, struct zros_node *node,
                  struct zros_topic *topic, void *data, double rate_limit_hz) {
  ARG_UNUSED(node);
  ARG_UNUSED(rate_limit_hz);
  sub->_topic = topic;
  sub->_data = data;
  return 0;
}

int zros_pub_init(struct zros_pub *pub, struct zros_node *node,
                  struct zros_topic *topic, void *data) {
  ARG_UNUSED(node);
  pub->_topic = topic;
  pub->_data = data;
  return 0;
}

int zros_sub_update(struct zros_sub *sub) {
  if (sub == &g_process.rate_command_sub) {
    if (g_cycle == CYCLE_INITIAL_UNOBSERVED ||
        (g_cycle >= CYCLE_STALE_COMMAND &&
         g_cycle <= CYCLE_STALE_HIGH_REARM_WHILE_STALLED) ||
        g_cycle == CYCLE_ARM_RACE_HIGH_EQUAL_RETAINED) {
      return -1;
    }
    g_process.rate_command = (synapse_topic_RateCommandData_t){
        .timestamp_ns = control_time_ns(g_cycle) +
                        (g_cycle == CYCLE_FUTURE_COMMAND ? 1U : 0U),
        .body_rate_flu_rad_s = {.roll = 0.1f, .pitch = -0.1f, .yaw = 0.2f},
        .thrust = 4.0f,
        .type_mask = g_cycle == CYCLE_MASKED_COMMAND ? 1U : 0U,
    };
    if (g_cycle == CYCLE_NONFINITE_COMMAND) {
      g_process.rate_command.body_rate_flu_rad_s.pitch = NAN;
    } else if (g_cycle == CYCLE_ARM_RACE_HIGH_OLD_UPDATED) {
      g_process.rate_command.timestamp_ns--;
    } else if (g_cycle == CYCLE_ARM_RACE_LOW_FUTURE_PRELOAD) {
      g_process.rate_command.timestamp_ns = control_time_ns(g_cycle + 1U);
    }
    return 0;
  }
  if (sub == &g_process.navigation_sub) {
    g_process.navigation = (synapse_topic_AttitudeEstimateData_t){
        .timestamp_ns = control_time_ns(g_cycle),
        .angular_velocity_flu_rad_s = {.roll = 0.01f,
                                       .pitch = 0.02f,
                                       .yaw = 0.03f},
        .flags = synapse_topic_AttitudeEstimateFlags_RatesValid,
    };
    if (g_cycle == CYCLE_INVALID_NAV_FLAGS) {
      g_process.navigation.flags = 0U;
    } else if (g_cycle == CYCLE_NONFINITE_NAV_RATE) {
      g_process.navigation.angular_velocity_flu_rad_s.yaw = INFINITY;
    }
    return 0;
  }
  return -1;
}

int zros_pub_update(struct zros_pub *pub) {
  if (pub == &g_process.health_pub) {
    g_health_publish_count[g_cycle]++;
    g_health_publish_armed[g_cycle] = g_process.status.armed;
    if (g_cycle == CYCLE_ARM_RACE_LOW_FRESH_ACK) {
      return -1;
    }
  }
  return 0;
}

int rdd2_rc_input_init(void) { return 0; }

int rdd2_motor_output_init(void) { return 0; }

int rdd2_imu_stream_init(void) { return 0; }

bool rdd2_imu_stream_wait_next(rdd2_vec3f_t *gyro, rdd2_vec3f_t *accel,
                               float *dt, uint64_t *interrupt_timestamp_ns) {
  *gyro = (rdd2_vec3f_t){0};
  *accel = (rdd2_vec3f_t){0};
  *dt = RDD2_CONTROL_DT_S;
  *interrupt_timestamp_ns = control_time_ns(g_cycle);
  return true;
}

bool rdd2_imu_stream_lockstep_at_target(void) { return true; }

void rdd2_rc_input_latest_get(rdd2_rc_channels_t *rc, int64_t *stamp_ms,
                              bool *valid) {
  *rc = (rdd2_rc_channels_t){0};
  rc->ch[THROTTLE_CHANNEL_INDEX] = 1000;
  rc->ch[ARM_CHANNEL_INDEX] = arm_switch_high(g_cycle) ? 2000 : 1000;
  *stamp_ms = (int64_t)(control_time_ns(g_cycle) / UINT64_C(1000000));
  *valid = true;
}

uint8_t rdd2_rc_input_link_quality_get(void) { return 100U; }

uint8_t rdd2_rc_flight_mode(const rdd2_rc_channels_t *rc) {
  ARG_UNUSED(rc);
  return g_cycle >= CYCLE_ARM_RACE_LOW_FRESH ? 2U : 0U;
}

bool rdd2_motor_test_get(rdd2_motor_values_t *motors) {
  ARG_UNUSED(motors);
  return false;
}

bool rdd2_motor_raw_test_get(rdd2_motor_raw_t *raw) {
  ARG_UNUSED(raw);
  return false;
}

uint64_t rdd2_motor_output_write_all(const rdd2_motor_values_t *motors,
                                     bool armed, bool test_mode) {
  g_motor_observations[g_cycle] = (struct motor_observation){
      .motors = *motors,
      .armed = armed,
      .arm_switch = g_process.status.arm_switch,
      .failsafe = g_process.status.failsafe,
      .test_mode = test_mode,
  };
  g_cycle++;
  if (g_cycle == TEST_CYCLE_COUNT) {
    longjmp(g_loop_escape, 1);
  }
  return control_time_ns(g_cycle - 1U) + UINT64_C(1000);
}

uint64_t rdd2_motor_output_write_all_raw(const rdd2_motor_raw_t *raw,
                                         bool test_mode) {
  ARG_UNUSED(raw);
  ARG_UNUSED(test_mode);
  zassert_unreachable("raw motor test path was not expected");
  return 0U;
}

void rdd2_topic_make_vehicle_health(synapse_topic_VehicleHealthData_t *output,
                                    const rdd2_control_status_t *status) {
  ARG_UNUSED(output);
  ARG_UNUSED(status);
}

void rdd2_topic_make_control_loop_metrics(
    synapse_topic_ControlLoopMetricsData_t *output,
    uint32_t main_loop_latency_us) {
  ARG_UNUSED(output);
  ARG_UNUSED(main_loop_latency_us);
}

static void expect_zero_and_disarmed(size_t cycle) {
  zexpect_false(g_motor_observations[cycle].armed,
                "cycle %zu unexpectedly armed", cycle);
  zexpect_false(g_motor_observations[cycle].test_mode,
                "cycle %zu unexpectedly used motor test mode", cycle);
  for (size_t motor = 0U; motor < RDD2_MOTOR_COUNT; ++motor) {
    zexpect_equal(g_motor_observations[cycle].motors.value[motor], 0.0f,
                  "cycle %zu motor %zu was not zero", cycle, motor);
  }
}

static void expect_armed_finite_output(size_t cycle) {
  zexpect_true(g_motor_observations[cycle].armed, "cycle %zu was not armed",
               cycle);
  zexpect_false(g_motor_observations[cycle].test_mode,
                "cycle %zu unexpectedly used motor test mode", cycle);
  for (size_t motor = 0U; motor < RDD2_MOTOR_COUNT; ++motor) {
    zexpect_true(isfinite(g_motor_observations[cycle].motors.value[motor]));
    zexpect_true(g_motor_observations[cycle].motors.value[motor] > 0.0f);
  }
}

static void expect_failsafe(size_t cycle, bool expected) {
  zexpect_equal(g_motor_observations[cycle].failsafe, expected,
                "cycle %zu failsafe state mismatch", cycle);
}

static void expect_generated_inputs_zero(size_t cycle) {
  const struct generated_observation *observation =
      &g_generated_observations[cycle];

  zexpect_false(observation->armed, "cycle %zu generated armed input was true",
                cycle);
  zexpect_equal(observation->thrust, 0.0f,
                "cycle %zu generated thrust was not zero", cycle);
  for (size_t axis = 0U; axis < ARRAY_SIZE(observation->command); ++axis) {
    zexpect_equal(observation->command[axis], 0.0f,
                  "cycle %zu generated command axis %zu was not zero", cycle,
                  axis);
    zexpect_equal(observation->measured[axis], 0.0f,
                  "cycle %zu generated measurement axis %zu was not zero",
                  cycle, axis);
  }
}

static void expect_generated_inputs_forwarded(size_t cycle, bool armed) {
  const struct generated_observation *observation =
      &g_generated_observations[cycle];
  const float expected_command[] = {0.1f, -0.1f, 0.2f};
  const float expected_measured[] = {0.01f, 0.02f, 0.03f};

  zexpect_equal(observation->armed, armed,
                "cycle %zu generated armed input mismatch", cycle);
  zexpect_equal(observation->thrust, 4.0f,
                "cycle %zu generated thrust mismatch", cycle);
  for (size_t axis = 0U; axis < ARRAY_SIZE(observation->command); ++axis) {
    zexpect_equal(observation->command[axis], expected_command[axis],
                  "cycle %zu generated command axis %zu mismatch", cycle, axis);
    zexpect_equal(observation->measured[axis], expected_measured[axis],
                  "cycle %zu generated measurement axis %zu mismatch", cycle,
                  axis);
  }
}

ZTEST(process_wrapper_fault_injection,
      test_rate_wrapper_fails_closed_and_requires_ack) {
  memset(g_motor_observations, 0, sizeof(g_motor_observations));
  memset(g_generated_observations, 0, sizeof(g_generated_observations));
  g_cycle = 0U;
  if (setjmp(g_loop_escape) == 0) {
    (void)rdd2_rate_control_allocator_process_run();
    zassert_unreachable(
        "rate wrapper returned before the scripted sequence completed");
  }

  zexpect_equal(g_cycle, TEST_CYCLE_COUNT);

  expect_zero_and_disarmed(CYCLE_INITIAL_UNOBSERVED);
  expect_zero_and_disarmed(CYCLE_INITIAL_ACK);
  expect_armed_finite_output(CYCLE_INITIAL_RECOVERY);
  expect_zero_and_disarmed(CYCLE_FUTURE_COMMAND);
  expect_zero_and_disarmed(CYCLE_LATCHED_AFTER_FUTURE);
  expect_zero_and_disarmed(CYCLE_FUTURE_ACK);
  expect_armed_finite_output(CYCLE_FUTURE_RECOVERY);
  expect_zero_and_disarmed(CYCLE_MASKED_COMMAND);
  expect_zero_and_disarmed(CYCLE_MASK_ACK);
  expect_armed_finite_output(CYCLE_MASK_RECOVERY);
  expect_zero_and_disarmed(CYCLE_NONFINITE_COMMAND);
  expect_zero_and_disarmed(CYCLE_COMMAND_ACK);
  expect_armed_finite_output(CYCLE_COMMAND_RECOVERY);
  expect_zero_and_disarmed(CYCLE_INVALID_NAV_FLAGS);
  expect_zero_and_disarmed(CYCLE_NAV_FLAGS_ACK);
  expect_armed_finite_output(CYCLE_NAV_FLAGS_RECOVERY);
  expect_zero_and_disarmed(CYCLE_NONFINITE_NAV_RATE);
  expect_zero_and_disarmed(CYCLE_NAV_RATE_ACK);
  expect_armed_finite_output(CYCLE_NAV_RATE_RECOVERY);
  expect_zero_and_disarmed(CYCLE_STALE_COMMAND);
  expect_zero_and_disarmed(CYCLE_STALE_LOW_WHILE_STALLED);
  expect_zero_and_disarmed(CYCLE_STALE_HIGH_REARM_WHILE_STALLED);
  expect_zero_and_disarmed(CYCLE_STALE_FRESH_ACK);
  expect_armed_finite_output(CYCLE_STALE_RECOVERY);
  expect_zero_and_disarmed(CYCLE_GENERATED_ERROR);
  expect_zero_and_disarmed(CYCLE_GENERATED_ERROR_ACK);
  expect_armed_finite_output(CYCLE_GENERATED_ERROR_RECOVERY);
  expect_zero_and_disarmed(CYCLE_GENERATED_LATE_NAN);
  expect_zero_and_disarmed(CYCLE_GENERATED_NAN_ACK);
  expect_armed_finite_output(CYCLE_GENERATED_NAN_RECOVERY);
  expect_zero_and_disarmed(CYCLE_ARM_RACE_LOW_FRESH);
  expect_zero_and_disarmed(CYCLE_ARM_RACE_HIGH_OLD_UPDATED);
  expect_zero_and_disarmed(CYCLE_ARM_RACE_LOW_FUTURE_PRELOAD);
  expect_zero_and_disarmed(CYCLE_ARM_RACE_HIGH_EQUAL_RETAINED);
  expect_zero_and_disarmed(CYCLE_ARM_RACE_LOW_FRESH_ACK);
  expect_zero_and_disarmed(CYCLE_ARM_RACE_HIGH_ANNOUNCE_FRESH);
  expect_armed_finite_output(CYCLE_ARM_RACE_HIGH_AUTHORIZED_FRESH);

  const size_t unusable_input_cycles[] = {
      CYCLE_INITIAL_UNOBSERVED,
      CYCLE_FUTURE_COMMAND,
      CYCLE_MASKED_COMMAND,
      CYCLE_NONFINITE_COMMAND,
      CYCLE_INVALID_NAV_FLAGS,
      CYCLE_NONFINITE_NAV_RATE,
      CYCLE_STALE_COMMAND,
      CYCLE_STALE_LOW_WHILE_STALLED,
      CYCLE_STALE_HIGH_REARM_WHILE_STALLED,
      CYCLE_ARM_RACE_LOW_FUTURE_PRELOAD,
  };
  for (size_t index = 0U; index < ARRAY_SIZE(unusable_input_cycles); ++index) {
    expect_generated_inputs_zero(unusable_input_cycles[index]);
  }

  expect_generated_inputs_forwarded(CYCLE_LATCHED_AFTER_FUTURE, false);
  expect_generated_inputs_forwarded(CYCLE_STALE_FRESH_ACK, false);
  expect_generated_inputs_forwarded(CYCLE_STALE_RECOVERY, true);
  expect_failsafe(CYCLE_FUTURE_COMMAND, true);
  expect_failsafe(CYCLE_LATCHED_AFTER_FUTURE, true);
  expect_failsafe(CYCLE_FUTURE_ACK, false);
  expect_failsafe(CYCLE_FUTURE_RECOVERY, false);
  expect_failsafe(CYCLE_STALE_COMMAND, true);
  expect_failsafe(CYCLE_STALE_LOW_WHILE_STALLED, false);
  expect_failsafe(CYCLE_STALE_HIGH_REARM_WHILE_STALLED, true);
  expect_failsafe(CYCLE_STALE_FRESH_ACK, false);
  expect_failsafe(CYCLE_GENERATED_ERROR, true);
  expect_failsafe(CYCLE_GENERATED_ERROR_ACK, false);
  expect_failsafe(CYCLE_GENERATED_LATE_NAN, true);
  expect_failsafe(CYCLE_GENERATED_NAN_ACK, false);
  zexpect_true(g_motor_observations[CYCLE_STALE_COMMAND].arm_switch,
               "stale-command fault did not occur with the arm switch high");
  zexpect_false(g_motor_observations[CYCLE_STALE_LOW_WHILE_STALLED].arm_switch,
                "stalled low-switch attempt was not observed by the wrapper");
  zexpect_true(
      g_motor_observations[CYCLE_STALE_HIGH_REARM_WHILE_STALLED].arm_switch,
      "stalled high-switch rearm was not observed by the wrapper");
  zexpect_false(g_motor_observations[CYCLE_STALE_FRESH_ACK].arm_switch,
                "fresh-command acknowledgement was not low");
  zexpect_true(g_motor_observations[CYCLE_STALE_RECOVERY].arm_switch,
               "fresh-command recovery was not high");
  expect_generated_inputs_forwarded(CYCLE_GENERATED_ERROR, true);
  expect_generated_inputs_forwarded(CYCLE_GENERATED_LATE_NAN, true);
  expect_generated_inputs_forwarded(CYCLE_ARM_RACE_LOW_FRESH, false);
  expect_generated_inputs_forwarded(CYCLE_ARM_RACE_HIGH_OLD_UPDATED, false);
  expect_generated_inputs_forwarded(CYCLE_ARM_RACE_HIGH_EQUAL_RETAINED, false);
  expect_generated_inputs_forwarded(CYCLE_ARM_RACE_LOW_FRESH_ACK, false);
  expect_generated_inputs_forwarded(CYCLE_ARM_RACE_HIGH_ANNOUNCE_FRESH, false);
  expect_generated_inputs_forwarded(CYCLE_ARM_RACE_HIGH_AUTHORIZED_FRESH, true);
  zexpect_false(g_motor_observations[CYCLE_ARM_RACE_HIGH_OLD_UPDATED].armed,
                "newly observed pre-edge command armed POSITION");
  zexpect_false(g_motor_observations[CYCLE_ARM_RACE_HIGH_EQUAL_RETAINED].armed,
                "retained edge-time command armed POSITION");
  zexpect_true(g_health_publish_count[CYCLE_ARM_RACE_LOW_FRESH] >= 1U);
  zexpect_false(g_health_publish_armed[CYCLE_ARM_RACE_LOW_FRESH]);
  zexpect_true(g_health_publish_count[CYCLE_ARM_RACE_LOW_FUTURE_PRELOAD] >= 1U);
  zexpect_false(g_health_publish_armed[CYCLE_ARM_RACE_LOW_FUTURE_PRELOAD]);
  zexpect_true(g_health_publish_count[CYCLE_ARM_RACE_LOW_FRESH_ACK] >= 1U);
  zexpect_true(g_health_publish_count[CYCLE_ARM_RACE_HIGH_ANNOUNCE_FRESH] >=
               1U);
  zexpect_false(g_health_publish_armed[CYCLE_ARM_RACE_HIGH_ANNOUNCE_FRESH]);
  zexpect_equal(
      g_generated_observations[CYCLE_GENERATED_ERROR].error_signal_status,
      UINT32_C(0x80), "non-bit0 generated error status was not exercised");
  for (size_t motor = 0U; motor < EFMU_MOTOR_COUNT; ++motor) {
    if (motor == 2U) {
      zexpect_true(
          isnan(
              g_generated_observations[CYCLE_GENERATED_LATE_NAN].motor[motor]),
          "third generated motor did not carry the injected NaN");
    } else {
      zexpect_true(
          isfinite(
              g_generated_observations[CYCLE_GENERATED_LATE_NAN].motor[motor]),
          "generated motor %zu must stay finite in late-element NaN test",
          motor);
    }
  }
}

ZTEST_SUITE(process_wrapper_fault_injection, NULL, NULL, NULL, NULL, NULL);
