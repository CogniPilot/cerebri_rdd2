/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/ztest.h>

#include "../../../src/processes/scheduling.h"

#define rdd2_topic_make_control_loop_metrics data_test_make_control_loop_metrics
#define rdd2_topic_make_pwm_output data_test_make_pwm_output
#define rdd2_topic_make_vehicle_health data_test_make_vehicle_health

#include "../../../src/interfaces/data.c"

#undef rdd2_topic_make_vehicle_health
#undef rdd2_topic_make_pwm_output
#undef rdd2_topic_make_control_loop_metrics

ZTEST(process_wrapper_fault_injection,
      test_vehicle_health_reports_control_fault_latch) {
  rdd2_control_status_t status = {0};
  synapse_topic_VehicleHealthData_t health;

  data_test_make_vehicle_health(&health, &status);
  zexpect_equal(health.flags, 0U);

  status.failsafe = true;
  data_test_make_vehicle_health(&health, &status);
  zexpect_equal(health.flags, synapse_topic_VehicleHealthFlags_Failsafe);

  status.armed = true;
  data_test_make_vehicle_health(&health, &status);
  zexpect_equal(health.flags, synapse_topic_VehicleHealthFlags_Armed |
                                  synapse_topic_VehicleHealthFlags_Failsafe);

  status.failsafe = false;
  data_test_make_vehicle_health(&health, &status);
  zexpect_equal(health.flags, synapse_topic_VehicleHealthFlags_Armed);
}

ZTEST(process_wrapper_fault_injection,
      test_release_schedulers_preserve_exact_rates) {
  struct rdd2_release_scheduler navigation = {0};
  struct rdd2_release_scheduler guidance = {0};
  struct rdd2_release_scheduler planning = {0};
  uint32_t navigation_releases = 0U;
  uint32_t guidance_releases = 0U;
  uint32_t planning_releases = 0U;

  for (uint32_t sample = 0U; sample < RDD2_CONTROL_RATE_HZ; ++sample) {
    navigation_releases += rdd2_release_due(&navigation, RDD2_CONTROL_RATE_HZ,
                                            RDD2_NAVIGATION_ESTIMATOR_RATE_HZ)
                               ? 1U
                               : 0U;
  }
  /* Guidance and planning divide the CONTROL tick, not the estimator
   * publication: their rates must not move when the estimator rate does. */
  for (uint32_t sample = 0U; sample < RDD2_CONTROL_RATE_HZ; ++sample) {
    guidance_releases +=
        rdd2_release_due(&guidance, RDD2_CONTROL_RATE_HZ, RDD2_GUIDANCE_RATE_HZ)
            ? 1U
            : 0U;
    planning_releases +=
        rdd2_release_due(&planning, RDD2_CONTROL_RATE_HZ, RDD2_PLANNING_RATE_HZ)
            ? 1U
            : 0U;
  }

  zexpect_equal(navigation_releases, RDD2_NAVIGATION_ESTIMATOR_RATE_HZ);
  zexpect_equal(guidance_releases, RDD2_GUIDANCE_RATE_HZ);
  zexpect_equal(planning_releases, RDD2_PLANNING_RATE_HZ);
  zexpect_true(navigation.phase < RDD2_CONTROL_RATE_HZ);
  zexpect_true(guidance.phase < RDD2_CONTROL_RATE_HZ);
  zexpect_true(planning.phase < RDD2_CONTROL_RATE_HZ);
}
