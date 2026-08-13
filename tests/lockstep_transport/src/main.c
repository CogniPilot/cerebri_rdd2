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

ZTEST_SUITE(lockstep_transport, NULL, NULL, NULL, NULL, NULL);
