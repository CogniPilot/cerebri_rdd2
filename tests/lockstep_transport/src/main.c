/* SPDX-License-Identifier: Apache-2.0 */

#include "interfaces/drivers.h"
#include "lockstep_input.h"
#include "lockstep_transport.h"

#include <string.h>

#include <synapse/sensors_builder.h>

#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <zros/private/zros_topic_struct.h>
#include <zros/zros_topic.h>

ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(gnss_fix, synapse_topic_GnssFixData_t);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(waypoint_plan, rdd2_waypoint_plan_t);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(trajectory_reference,
                                   synapse_topic_LocalPositionCommandData_t);

uint32_t rdd2_topic_generation(const struct zros_topic *topic) {
  return (uint32_t)atomic_get((atomic_t *)&topic->_lockless_generation);
}

bool rdd2_navigation_origin_valid_get(void) { return false; }

uint8_t rdd2_waypoint_mission_state_get(void) {
  return RDD2_WAYPOINT_MISSION_PENDING;
}

struct rc_capture {
  int32_t channels[RDD2_RC_CHANNEL_COUNT];
  bool channel_seen[RDD2_RC_CHANNEL_COUNT];
  int32_t link_quality;
  int32_t valid;
  size_t event_count;
};

static struct rc_capture capture;

static void capture_rc_event(struct input_event *event, void *user_data) {
  ARG_UNUSED(user_data);

  if (event->dev != DEVICE_DT_GET(DT_ALIAS(rc))) {
    return;
  }
  capture.event_count++;
  if (event->type == INPUT_EV_ABS && event->code >= 1U &&
      event->code <= RDD2_RC_CHANNEL_COUNT) {
    capture.channels[event->code - 1U] = event->value;
    capture.channel_seen[event->code - 1U] = true;
  } else if (event->type == INPUT_EV_MSC &&
             event->code == RDD2_RC_INPUT_EVENT_LINK_QUALITY) {
    capture.link_quality = event->value;
  } else if (event->type == INPUT_EV_MSC &&
             event->code == RDD2_RC_INPUT_EVENT_VALID) {
    capture.valid = event->value;
  }
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_ALIAS(rc)), capture_rc_event, NULL);

static void reset_capture(void) { memset(&capture, 0, sizeof(capture)); }

static uint16_t required_axes(void) {
  return synapse_topic_ManualControlAxes_Roll |
         synapse_topic_ManualControlAxes_Pitch |
         synapse_topic_ManualControlAxes_Throttle |
         synapse_topic_ManualControlAxes_Yaw;
}

static synapse_topic_ManualControlData_t valid_armed_manual(uint8_t mode) {
  synapse_topic_ManualControlData_t manual = {0};

  manual.active_axes = required_axes();
  manual.flight_mode = mode;
  manual.flags = synapse_topic_ManualControlFlags_Valid |
                 synapse_topic_ManualControlFlags_Active |
                 synapse_topic_ManualControlFlags_ArmSwitch;
  return manual;
}

static void assert_complete_capture(void) {
  zassert_equal(capture.event_count, RDD2_RC_CHANNEL_COUNT + 2U);
  for (size_t channel = 0U; channel < RDD2_RC_CHANNEL_COUNT; ++channel) {
    zassert_true(capture.channel_seen[channel], "missing RC channel %zu",
                 channel);
  }
}

ZTEST(lockstep_transport, test_misaligned_inertial_payload_decodes_exactly) {
  static const uint64_t timestamp_ns = UINT64_C(0x1122334455667788);
  synapse_topic_InertialSampleData_t sample = {0};
  rdd2_vec3f_t expected_accel;
  rdd2_vec3f_t expected_gyro;
  rdd2_vec3f_t decoded_accel = {0};
  rdd2_vec3f_t decoded_gyro = {0};
  uint64_t decoded_timestamp_ns = 0U;
  uint8_t storage[sizeof(sample) + 8U] __aligned(8);
  uint8_t *misaligned = &storage[1];

  synapse_types_Vec3f_assign(&expected_accel, 1.25f, -2.5f, 9.75f);
  synapse_types_Vec3f_assign(&expected_gyro, -0.125f, 0.5f, 2.0f);
  synapse_topic_InertialSampleData_assign(
      &sample, timestamp_ns, 1.25f, -2.5f, 9.75f, -0.125f, 0.5f, 2.0f, 24.0f,
      synapse_topic_InertialFieldFlags_Accel |
          synapse_topic_InertialFieldFlags_Gyro,
      synapse_types_TimeStatus_LocalFreerun, 7U);
  memcpy(misaligned, &sample, sizeof(sample));

  zassert_not_equal((uintptr_t)misaligned % _Alignof(uint64_t), 0U);
  zassert_true(rdd2_lockstep_decode_inertial(misaligned, sizeof(sample),
                                             &decoded_gyro, &decoded_accel,
                                             &decoded_timestamp_ns));
  zassert_equal(decoded_timestamp_ns, timestamp_ns);
  zassert_mem_equal(&decoded_accel, &expected_accel, sizeof(expected_accel));
  zassert_mem_equal(&decoded_gyro, &expected_gyro, sizeof(expected_gyro));
}

ZTEST(lockstep_transport, test_manual_modes_preserve_three_rc_positions) {
  static const int32_t expected_mode_us[] = {1000, 1500, 2000};

  for (uint8_t mode = 0U; mode < ARRAY_SIZE(expected_mode_us); ++mode) {
    synapse_topic_ManualControlData_t manual = valid_armed_manual(mode);

    reset_capture();
    zassert_true(rdd2_lockstep_handle_manual_control(&manual));
    assert_complete_capture();
    zassert_equal(capture.channels[4], 2000);
    zassert_equal(capture.channels[RDD2_FLIGHT_MODE_CHANNEL_INDEX],
                  expected_mode_us[mode]);
    zassert_equal(capture.link_quality, 100);
    zassert_equal(capture.valid, 1);
  }
}

ZTEST(lockstep_transport, test_kill_switch_forces_invalid_and_arm_low) {
  synapse_topic_ManualControlData_t manual = valid_armed_manual(2U);

  manual.flags |= synapse_topic_ManualControlFlags_KillSwitch;
  reset_capture();
  zassert_true(rdd2_lockstep_handle_manual_control(&manual));
  assert_complete_capture();
  zassert_equal(capture.channels[4], 1000);
  zassert_equal(capture.channels[RDD2_FLIGHT_MODE_CHANNEL_INDEX], 2000);
  zassert_equal(capture.link_quality, 0);
  zassert_equal(capture.valid, 0);
}

static synapse_topic_GnssFixData_t usable_fix(uint64_t timestamp_ns) {
  return (synapse_topic_GnssFixData_t){
      .timestamp_ns = timestamp_ns,
      .horizontal_accuracy_mm = 400U,
      .vertical_accuracy_mm = 700U,
      .velocity_accuracy_mm_s = 150U,
      .fix_type = synapse_types_GnssFixType_Fix3d,
      .time_status = synapse_types_TimeStatus_LocalFreerun,
  };
}

ZTEST(lockstep_transport, test_plan_publishes_once_and_replay_is_exact) {
  synapse_topic_GnssFixData_t fix = usable_fix(UINT64_C(100000000));
  rdd2_waypoint_plan_t plan = {
      .sequence = 1,
      .waypoint_count = 5,
      .nominal_speed = 0.3f,
      .min_segment_duration = 2.0f,
      .valid = true,
  };
  uint32_t generation;

  zassert_ok(rdd2_lockstep_gps_mission_init());
  zassert_true(rdd2_lockstep_handle_gps_mission(&fix, &plan, fix.timestamp_ns));
  generation = rdd2_topic_generation(&topic_waypoint_plan);
  zassert_equal(generation, 1U);

  zassert_true(rdd2_lockstep_handle_gps_mission(
      &fix, &plan, fix.timestamp_ns + UINT64_C(5000000)));
  zassert_equal(rdd2_topic_generation(&topic_waypoint_plan), generation,
                "retained lockstep input must not republish the plan");

  plan.nominal_speed = 0.4f;
  zassert_false(rdd2_lockstep_handle_gps_mission(
                    &fix, &plan, fix.timestamp_ns + UINT64_C(10000000)),
                "same sequence with mutated payload must fail closed");
  plan.sequence = -1;
  zassert_false(rdd2_lockstep_handle_gps_mission(
                    &fix, &plan, fix.timestamp_ns + UINT64_C(15000000)),
                "sequence rollback must fail closed");
  zassert_equal(rdd2_topic_generation(&topic_waypoint_plan), generation);
}

ZTEST_SUITE(lockstep_transport, NULL, NULL, NULL, NULL, NULL);
