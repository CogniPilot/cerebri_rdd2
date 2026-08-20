/* SPDX-License-Identifier: Apache-2.0 */

#include "data.h"

#include "synapse_time_status.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

BUILD_ASSERT(sizeof(synapse_topic_InertialSampleData_t) == 40U);
BUILD_ASSERT(sizeof(synapse_topic_ManualControlData_t) == 40U);
BUILD_ASSERT(sizeof(synapse_topic_PwmSignalOutputsData_t) == 48U);
BUILD_ASSERT(sizeof(synapse_topic_VehicleHealthData_t) == 56U);
BUILD_ASSERT(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);

static uint64_t timestamp_ns(void) {
  return synapse_time_boot_ns();
}

void rdd2_topic_make_vehicle_health(synapse_topic_VehicleHealthData_t *output,
                                    const rdd2_control_status_t *status) {
  uint64_t now_ns = timestamp_ns();
  uint32_t sensors = synapse_topic_SensorComponentFlags_Gyro |
                     synapse_topic_SensorComponentFlags_Accel |
                     synapse_topic_SensorComponentFlags_RadioControl |
                     synapse_topic_SensorComponentFlags_MotorOutputs |
                     synapse_topic_SensorComponentFlags_Estimator;
  uint32_t healthy = synapse_topic_SensorComponentFlags_MotorOutputs |
                     synapse_topic_SensorComponentFlags_Estimator;
  uint8_t flags = 0U;

  if (status->imu_ok) {
    healthy |= synapse_topic_SensorComponentFlags_Gyro |
               synapse_topic_SensorComponentFlags_Accel;
  }
  if (status->rc_valid && !status->rc_stale) {
    healthy |= synapse_topic_SensorComponentFlags_RadioControl;
  }
  if (status->armed) {
    flags |= synapse_topic_VehicleHealthFlags_Armed;
  }
  if (status->failsafe) {
    flags |= synapse_topic_VehicleHealthFlags_Failsafe;
  }

  *output = (synapse_topic_VehicleHealthData_t){
      .timestamp_ns = now_ns,
      .sensors_present = sensors,
      .sensors_enabled = sensors,
      .sensors_health = healthy,
      .flight_mode = status->flight_mode,
      .link_quality_pct = status->rc_link_quality,
      .flags = flags,
  };
}

void rdd2_topic_make_control_loop_metrics(
    synapse_topic_ControlLoopMetricsData_t *output,
    uint32_t main_loop_latency_us) {
  *output = (synapse_topic_ControlLoopMetricsData_t){
      .timestamp_ns = timestamp_ns(),
      .period_us = 625U,
      .latency_us = main_loop_latency_us,
  };
}

void rdd2_topic_make_pwm_output(rdd2_topic_motor_output_blob_t *output,
                                const rdd2_motor_values_t *motors, bool armed) {
  const float *values = motors->value;
  uint16_t *pwm = &output->output0_us;

  memset(output, 0, sizeof(*output));
  output->timestamp_ns = timestamp_ns();
  output->active_mask = 0x0fU;
  for (size_t i = 0; i < 4U; ++i) {
    float value = armed ? CLAMP(values[i], 0.0f, 1.0f) : 0.0f;
    pwm[i] = (uint16_t)(1000.0f + value * 1000.0f + 0.5f);
  }
}
