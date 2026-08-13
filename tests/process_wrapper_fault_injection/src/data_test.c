/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/ztest.h>

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
