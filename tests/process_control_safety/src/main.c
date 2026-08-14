/* SPDX-License-Identifier: Apache-2.0 */

#include "control_safety.h"
#include "motor_override.h"

#include <math.h>
#include <zephyr/ztest.h>

ZTEST(process_control_safety, test_command_requires_an_observed_sample) {
  zexpect_false(rdd2_control_timestamp_is_fresh(
      false, 1000U, 1000U, RDD2_GUIDANCE_COMMAND_TIMEOUT_NS));
}

ZTEST(process_control_safety, test_command_accepts_exact_timeout_boundary) {
  zexpect_true(rdd2_control_timestamp_is_fresh(
      true, UINT64_C(25000000), UINT64_C(50000000),
      RDD2_GUIDANCE_COMMAND_TIMEOUT_NS));
}

ZTEST(process_control_safety, test_command_rejects_stale_payload_on_receipt) {
  const uint64_t now_ns = UINT64_C(50000000);

  zexpect_false(rdd2_control_timestamp_is_fresh(
      true, now_ns - RDD2_GUIDANCE_COMMAND_TIMEOUT_NS - 1U, now_ns,
      RDD2_GUIDANCE_COMMAND_TIMEOUT_NS));
}

ZTEST(process_control_safety, test_command_rejects_future_timestamps) {
  const uint64_t now_ns = UINT64_C(50000000);

  zexpect_false(rdd2_control_timestamp_is_fresh(
      true, now_ns + 1U, now_ns, RDD2_GUIDANCE_COMMAND_TIMEOUT_NS));
}

ZTEST(process_control_safety,
      test_generated_status_and_nonfinite_values_fail_closed) {
  const float finite_values[] = {-1.0f, 0.0f, 1.0f};
  const float nan_values[] = {0.0f, NAN};
  const float infinite_values[] = {INFINITY, 0.0f};

  zexpect_true(rdd2_generated_step_ok(0U));
  zexpect_false(rdd2_generated_step_ok(1U));
  zexpect_false(rdd2_generated_step_ok(UINT32_MAX));
  zexpect_true(rdd2_control_values_are_finite(finite_values, 3U));
  zexpect_false(rdd2_control_values_are_finite(nan_values, 2U));
  zexpect_false(rdd2_control_values_are_finite(infinite_values, 2U));
}

ZTEST(process_control_safety,
      test_fault_latch_requires_arm_switch_acknowledgement) {
  bool latched = false;

  latched = rdd2_control_fault_latch(latched, true, true, true);
  zexpect_true(latched);
  latched = rdd2_control_fault_latch(latched, true, true, false);
  zexpect_true(latched);
  latched = rdd2_control_fault_latch(latched, true, false, false);
  zexpect_false(latched);
}

ZTEST(process_control_safety,
      test_fault_while_switch_low_does_not_block_later_arm) {
  bool latched = rdd2_control_fault_latch(false, true, false, true);

  zexpect_false(latched);
  latched = rdd2_control_fault_latch(latched, true, true, false);
  zexpect_false(latched);
}

ZTEST(process_control_safety,
      test_invalid_low_switch_does_not_acknowledge_fault) {
  bool latched = rdd2_control_fault_latch(true, false, false, false);

  zexpect_true(latched);
  latched = rdd2_control_fault_latch(latched, true, false, false);
  zexpect_false(latched);
}

ZTEST(process_control_safety, test_guidance_requires_current_arm_switch) {
  zexpect_true(rdd2_guidance_arm_allowed(true, false, true, true));
  zexpect_false(rdd2_guidance_arm_allowed(true, false, true, false));
  zexpect_false(rdd2_guidance_arm_allowed(true, false, false, true));
  zexpect_false(rdd2_guidance_arm_allowed(true, true, true, true));
  zexpect_false(rdd2_guidance_arm_allowed(false, false, true, true));
}

ZTEST(process_control_safety,
      test_inactive_motor_override_preserves_flight_outputs) {
  float motors[] = {0.25f, 0.5f, 0.75f, 1.0f};
  const float override[] = {0.0f, 0.0f, 0.0f, 0.0f};
  uint16_t raw[] = {100U, 200U, 300U, 400U};
  const uint16_t raw_override[] = {0U, 0U, 0U, 0U};

  zexpect_false(rdd2_motor_override_copy_f32(false, motors, override, 4U));
  zexpect_false(
      rdd2_motor_override_copy_u16(false, raw, raw_override, 4U));
  for (size_t index = 0U; index < 4U; ++index) {
    zexpect_equal(motors[index], 0.25f * (float)(index + 1U));
    zexpect_equal(raw[index], (uint16_t)(100U * (index + 1U)));
  }
}

ZTEST(process_control_safety, test_active_motor_override_replaces_outputs) {
  float motors[] = {0.25f, 0.5f, 0.75f, 1.0f};
  const float override[] = {0.1f, 0.2f, 0.3f, 0.4f};
  uint16_t raw[] = {100U, 200U, 300U, 400U};
  const uint16_t raw_override[] = {1U, 2U, 3U, 4U};

  zexpect_true(rdd2_motor_override_copy_f32(true, motors, override, 4U));
  zexpect_true(rdd2_motor_override_copy_u16(true, raw, raw_override, 4U));
  for (size_t index = 0U; index < 4U; ++index) {
    zexpect_equal(motors[index], override[index]);
    zexpect_equal(raw[index], raw_override[index]);
  }
}

ZTEST_SUITE(process_control_safety, NULL, NULL, NULL, NULL, NULL);
