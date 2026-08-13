/* SPDX-License-Identifier: Apache-2.0 */

#include "processes.h"

#include "control_safety.h"
#include "hotpath_memory.h"
#include "interfaces/drivers.h"
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

#include "Vehicles_Rdd2_RateControlAllocator.h"

LOG_MODULE_DECLARE(rdd2, LOG_LEVEL_INF);

#define RC_STALE_TIMEOUT_MS 100
#define THROTTLE_ARM_MAX_US 1050
#define ARM_SWITCH_THRESHOLD_US 1500
#define THROTTLE_CHANNEL_INDEX 2U
#define ARM_CHANNEL_INDEX 4U

struct rate_control_allocator_process {
  RateControlAllocatorState efmu;
  rdd2_vec3f_t gyro;
  rdd2_vec3f_t accel;
  rdd2_rc_channels_t rc;
  rdd2_control_status_t status;
  rdd2_motor_values_t motors;
  rdd2_motor_raw_t raw_test;
  synapse_topic_RateCommandData_t rate_command;
  synapse_topic_AttitudeEstimateData_t navigation;
  synapse_topic_InertialSampleData_t imu_message;
  synapse_topic_VehicleHealthData_t health_message;
  synapse_topic_ControlLoopMetricsData_t metrics_message;
  struct zros_node node;
  struct zros_sub rate_command_sub;
  struct zros_sub navigation_sub;
  struct zros_pub imu_pub;
  struct zros_pub health_pub;
  struct zros_pub metrics_pub;
  bool rate_command_observed;
  bool control_fault_latched;
  bool previous_arm_switch;
  bool arm_request_announced;
  uint64_t arm_request_timestamp_ns;
  float dt;
  uint32_t imu_to_motor_latency_us;
};

static RDD2_HOTPATH_DTCM_BSS struct rate_control_allocator_process g_process;

enum {
  RDD2_MOTOR_COUNT = sizeof(((rdd2_motor_values_t *)0)->value) / sizeof(float),
  EFMU_MOTOR_COUNT =
      sizeof(((RateControlAllocatorState *)0)->motor) / sizeof(float),
};

_Static_assert(RDD2_MOTOR_COUNT == EFMU_MOTOR_COUNT,
               "motor driver and generated allocator capacities differ");

static bool loop_divider_expired(uint32_t *countdown, uint32_t divisor) {
  if (*countdown > 0U) {
    (*countdown)--;
    return false;
  }

  *countdown = divisor - 1U;
  return true;
}

static uint64_t sample_timestamp_ns(uint64_t interrupt_timestamp_ns) {
  if (interrupt_timestamp_ns != 0U) {
    return interrupt_timestamp_ns;
  }
  return (uint64_t)k_uptime_get() * 1000000ULL;
}

static void publish_imu(struct rate_control_allocator_process *process,
                        uint64_t interrupt_timestamp_ns) {
  process->imu_message = (synapse_topic_InertialSampleData_t){
      .timestamp_ns = sample_timestamp_ns(interrupt_timestamp_ns),
      .accel_flu_m_s2 = process->accel,
      .gyro_flu_rad_s = process->gyro,
      .flags = process->status.imu_ok
                   ? synapse_topic_InertialFieldFlags_Accel |
                         synapse_topic_InertialFieldFlags_Gyro
                   : 0U,
  };
  (void)zros_pub_update(&process->imu_pub);
}

static void update_arming_state(struct rate_control_allocator_process *process,
                                uint64_t control_now_ns,
                                bool rate_command_updated, bool inputs_usable) {
  const int32_t *channels = rdd2_topic_rc_channels_data_const(&process->rc);
  bool announced_this_cycle = false;
  bool health_announced_before_edge = process->arm_request_announced;
  bool stale;

#if defined(CONFIG_RDD2_LOCKSTEP)
  stale = !process->status.rc_valid;
#else
  stale =
      !process->status.rc_valid ||
      ((k_uptime_get() - process->status.rc_stamp_ms) > RC_STALE_TIMEOUT_MS);
#endif

  process->status.flight_mode = rdd2_rc_flight_mode(&process->rc);
  process->status.arm_switch =
      channels[ARM_CHANNEL_INDEX] >= ARM_SWITCH_THRESHOLD_US;
  process->status.throttle_us = channels[THROTTLE_CHANNEL_INDEX];
  process->status.rc_stale = stale;

  if (!process->status.arm_switch) {
    if (process->previous_arm_switch) {
      process->arm_request_announced = false;
    }
    process->arm_request_timestamp_ns = 0U;
    if (!process->arm_request_announced) {
      process->status.armed = false;
      rdd2_topic_make_vehicle_health(&process->health_message,
                                     &process->status);
      process->arm_request_announced =
          zros_pub_update(&process->health_pub) == 0;
    }
  } else if (!process->previous_arm_switch) {
    process->arm_request_timestamp_ns = control_now_ns;
    health_announced_before_edge = process->arm_request_announced;
  }
  process->previous_arm_switch = process->status.arm_switch;

  process->control_fault_latched =
      rdd2_control_fault_latch(process->control_fault_latched, !stale,
                               process->status.arm_switch, false);
  process->status.failsafe = process->control_fault_latched;
  if (process->status.arm_switch && !process->arm_request_announced) {
    process->status.armed = false;
    rdd2_topic_make_vehicle_health(&process->health_message, &process->status);
    process->arm_request_announced = zros_pub_update(&process->health_pub) == 0;
    announced_this_cycle =
        !health_announced_before_edge && process->arm_request_announced;
  }
  if (!process->status.arm_switch) {
    process->status.armed = false;
  } else if (!process->status.imu_ok || stale ||
             process->control_fault_latched) {
    process->status.armed = false;
  } else if (!process->status.armed &&
             process->status.throttle_us <= THROTTLE_ARM_MAX_US &&
             process->arm_request_announced && !announced_this_cycle &&
             rate_command_updated && inputs_usable &&
             process->rate_command.timestamp_ns >=
                 process->arm_request_timestamp_ns) {
    process->status.armed = true;
  }
}

static bool
rate_command_values_are_finite(const synapse_topic_RateCommandData_t *command) {
  const float values[] = {
      command->body_rate_flu_rad_s.roll,
      command->body_rate_flu_rad_s.pitch,
      command->body_rate_flu_rad_s.yaw,
      command->thrust,
  };

  return rdd2_control_values_are_finite(values, ARRAY_SIZE(values));
}

static bool navigation_rates_are_valid(
    const synapse_topic_AttitudeEstimateData_t *navigation) {
  const float rates[] = {
      navigation->angular_velocity_flu_rad_s.roll,
      navigation->angular_velocity_flu_rad_s.pitch,
      navigation->angular_velocity_flu_rad_s.yaw,
  };

  return (navigation->flags & synapse_topic_AttitudeEstimateFlags_RatesValid) !=
             0U &&
         rdd2_control_values_are_finite(rates, ARRAY_SIZE(rates));
}

static bool
control_inputs_are_usable(struct rate_control_allocator_process *process,
                          uint64_t control_now_ns, bool *rate_command_updated) {
  *rate_command_updated = zros_sub_update(&process->rate_command_sub) == 0;
  if (*rate_command_updated) {
    process->rate_command_observed = true;
  }
  (void)zros_sub_update(&process->navigation_sub);

  return rdd2_control_timestamp_is_fresh(
             process->rate_command_observed, process->rate_command.timestamp_ns,
             control_now_ns, RDD2_GUIDANCE_COMMAND_TIMEOUT_NS) &&
         process->rate_command.type_mask == 0U &&
         rate_command_values_are_finite(&process->rate_command) &&
         navigation_rates_are_valid(&process->navigation);
}

static void
copy_topic_inputs_to_efmu(struct rate_control_allocator_process *process,
                          bool inputs_usable) {
  RateControlAllocatorState *efmu = &process->efmu;

  efmu->armed = process->status.armed && inputs_usable;
  efmu->thrust_N = inputs_usable ? process->rate_command.thrust : 0.0f;
  efmu->angularVelocityCommandFlu_rad_s[0] =
      inputs_usable ? process->rate_command.body_rate_flu_rad_s.roll : 0.0f;
  efmu->angularVelocityCommandFlu_rad_s[1] =
      inputs_usable ? process->rate_command.body_rate_flu_rad_s.pitch : 0.0f;
  efmu->angularVelocityCommandFlu_rad_s[2] =
      inputs_usable ? process->rate_command.body_rate_flu_rad_s.yaw : 0.0f;
  efmu->angularVelocityMeasuredFlu_rad_s[0] =
      inputs_usable ? process->navigation.angular_velocity_flu_rad_s.roll
                    : 0.0f;
  efmu->angularVelocityMeasuredFlu_rad_s[1] =
      inputs_usable ? process->navigation.angular_velocity_flu_rad_s.pitch
                    : 0.0f;
  efmu->angularVelocityMeasuredFlu_rad_s[2] =
      inputs_usable ? process->navigation.angular_velocity_flu_rad_s.yaw : 0.0f;
}

static void copy_efmu_outputs_to_motor_buffer(
    struct rate_control_allocator_process *process) {
  for (size_t motor = 0U; motor < RDD2_MOTOR_COUNT; ++motor) {
    process->motors.value[motor] = process->efmu.motor[motor];
  }
}

static void step_efmu(struct rate_control_allocator_process *process,
                      uint64_t control_now_ns) {
  bool rate_command_updated;
  bool inputs_usable =
      control_inputs_are_usable(process, control_now_ns, &rate_command_updated);
  bool outputs_finite;
  bool step_ok;

  update_arming_state(process, control_now_ns, rate_command_updated,
                      inputs_usable);
  copy_topic_inputs_to_efmu(process, inputs_usable);
  RateControlAllocator_dostep(&process->efmu);
  step_ok =
      rdd2_generated_step_ok(process->efmu.rumoca_galec_error_signal_status);
  outputs_finite =
      rdd2_control_values_are_finite(process->efmu.motor, EFMU_MOTOR_COUNT);
  if (!inputs_usable || !step_ok || !outputs_finite) {
    process->control_fault_latched = rdd2_control_fault_latch(
        process->control_fault_latched, !process->status.rc_stale,
        process->status.arm_switch, true);
    process->status.failsafe = process->control_fault_latched;
    process->status.armed = false;
    process->motors = (rdd2_motor_values_t){0};
    return;
  }
  copy_efmu_outputs_to_motor_buffer(process);
}

static uint32_t imu_to_motor_latency_us(uint64_t imu_timestamp_ns,
                                        uint64_t motor_timestamp_ns) {
  uint64_t latency_ns;

#if defined(CONFIG_RDD2_LOCKSTEP)
  ARG_UNUSED(imu_timestamp_ns);
  ARG_UNUSED(motor_timestamp_ns);
  return 0U;
#endif

  if (imu_timestamp_ns == 0U || motor_timestamp_ns == 0U ||
      motor_timestamp_ns <= imu_timestamp_ns) {
    return 0U;
  }

  latency_ns = motor_timestamp_ns - imu_timestamp_ns;
  if (latency_ns >= ((uint64_t)UINT32_MAX * 1000U)) {
    return UINT32_MAX;
  }
  return (uint32_t)(latency_ns / 1000U);
}

static void publish_status(struct rate_control_allocator_process *process) {
  static uint32_t publish_countdown;

  process->status.failsafe = process->control_fault_latched;

#if defined(CONFIG_RDD2_LOCKSTEP)
  if (!rdd2_imu_stream_lockstep_at_target()) {
    return;
  }
#endif
  if (!loop_divider_expired(&publish_countdown,
                            RDD2_FLIGHT_STATE_PUBLISH_DIV)) {
    return;
  }

  rdd2_topic_make_vehicle_health(&process->health_message, &process->status);
  rdd2_topic_make_control_loop_metrics(&process->metrics_message,
                                       process->imu_to_motor_latency_us);
  (void)zros_pub_update(&process->health_pub);
  (void)zros_pub_update(&process->metrics_pub);
}

static int process_zros_init(struct rate_control_allocator_process *process) {
  int rc;

  zros_node_init(&process->node, "efmu_rate");
  rc = zros_sub_init(&process->rate_command_sub, &process->node,
                     &topic_rate_command, &process->rate_command, 0.0);
  if (rc == 0) {
    rc = zros_sub_init(&process->navigation_sub, &process->node,
                       &topic_attitude_estimate, &process->navigation, 0.0);
  }
  if (rc == 0) {
    rc = zros_pub_init(&process->imu_pub, &process->node, &topic_control_imu,
                       &process->imu_message);
  }
  if (rc == 0) {
    rc = zros_pub_init(&process->health_pub, &process->node,
                       &topic_vehicle_health, &process->health_message);
  }
  if (rc == 0) {
    rc = zros_pub_init(&process->metrics_pub, &process->node,
                       &topic_control_loop_metrics, &process->metrics_message);
  }
  return rc;
}

static int process_drivers_init(void) {
  int rc;

  rc = rdd2_rc_input_init();
  if (rc == 0) {
    rc = rdd2_motor_output_init();
  }
  if (rc == 0) {
    rc = rdd2_imu_stream_init();
  }
  return rc;
}

int rdd2_rate_control_allocator_process_run(void) {
  struct rate_control_allocator_process *process = &g_process;
  int rc;

  *process = (struct rate_control_allocator_process){0};
  RateControlAllocator_startup(&process->efmu);
  RateControlAllocator_recalibrate(&process->efmu);

  rc = process_zros_init(process);
  if (rc != 0) {
    return rc;
  }
  rc = process_drivers_init();
  if (rc != 0) {
    return rc;
  }

  LOG_INF("RateControlAllocator eFMU process running");
  while (true) {
    uint64_t imu_timestamp_ns = 0U;
    uint64_t motor_timestamp_ns;

    process->status.imu_ok = rdd2_imu_stream_wait_next(
        &process->gyro, &process->accel, &process->dt, &imu_timestamp_ns);
    process->status.rc_link_quality = rdd2_rc_input_link_quality_get();
    rdd2_rc_input_latest_get(&process->rc, &process->status.rc_stamp_ms,
                             &process->status.rc_valid);
    publish_imu(process, imu_timestamp_ns);

    process->motors = (rdd2_motor_values_t){0};
    step_efmu(process, process->imu_message.timestamp_ns);
    if (rdd2_motor_test_get(&process->motors)) {
      bool test_values_valid = rdd2_control_values_are_finite(
          process->motors.value, RDD2_MOTOR_COUNT);

      if (!test_values_valid) {
        process->motors = (rdd2_motor_values_t){0};
      }
      motor_timestamp_ns = rdd2_motor_output_write_all(&process->motors,
                                                       test_values_valid, true);
    } else if (rdd2_motor_raw_test_get(&process->raw_test)) {
      motor_timestamp_ns =
          rdd2_motor_output_write_all_raw(&process->raw_test, true);
    } else {
      motor_timestamp_ns = rdd2_motor_output_write_all(
          &process->motors, process->status.armed, false);
    }

    process->imu_to_motor_latency_us =
        imu_to_motor_latency_us(imu_timestamp_ns, motor_timestamp_ns);
    publish_status(process);
  }
}
