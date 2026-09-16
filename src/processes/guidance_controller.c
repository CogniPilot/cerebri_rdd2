/* SPDX-License-Identifier: Apache-2.0 */

#include "processes.h"

#include "control_safety.h"
#include "hotpath_memory.h"
#include "interfaces/zros_topics.h"
#include "scheduling.h"

#include "gnss_source.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zros/private/zros_node_struct.h>
#include <zros/private/zros_pub_struct.h>
#include <zros/private/zros_sub_struct.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>
#include <zros/zros_sub.h>

#include "Vehicles_Rdd2_GuidanceController.h"

LOG_MODULE_DECLARE(rdd2, LOG_LEVEL_INF);

#define GUIDANCE_STACK_SIZE 4096
#define GUIDANCE_REFERENCE_TIMEOUT_NS UINT64_C(100000000)

#define GUIDANCE_REFERENCE_TOPIC topic_trajectory_reference

struct guidance_controller_process {
  GuidanceControllerState efmu;
  synapse_topic_ManualControlData_t manual;
  synapse_topic_VehicleHealthData_t health;
  synapse_topic_AttitudeEstimateData_t attitude;
  synapse_topic_InertialSampleData_t control_clock;
  synapse_topic_OdometryEstimateData_t odometry;
  synapse_topic_LocalPositionCommandData_t reference;
  synapse_topic_RateCommandData_t rate_command;
  synapse_topic_AttitudeCommandData_t attitude_command;
  struct zros_node node;
  struct zros_sub manual_sub;
  struct zros_sub health_sub;
  struct zros_sub attitude_sub;
  struct zros_sub control_clock_sub;
  struct zros_sub odometry_sub;
  struct zros_sub reference_sub;
  struct zros_pub rate_command_pub;
  struct zros_pub attitude_command_pub;
  struct rdd2_release_scheduler release_scheduler;
  bool have_reference;
  bool control_fault_latched;
  bool position_fallback_latched;
  bool awaiting_disarmed_health;
};

static RDD2_HOTPATH_DTCM_BSS struct guidance_controller_process g_process;
static struct k_thread g_thread;
K_THREAD_STACK_DEFINE(g_guidance_stack, GUIDANCE_STACK_SIZE);

static void
copy_manual_inputs_to_efmu(GuidanceControllerState *efmu,
                           const synapse_topic_ManualControlData_t *manual,
                           const synapse_topic_VehicleHealthData_t *health) {
  const uint8_t manual_required = synapse_topic_ManualControlFlags_Valid |
                                  synapse_topic_ManualControlFlags_Active;
  bool health_armed =
      (health->flags & synapse_topic_VehicleHealthFlags_Armed) != 0U;
  bool health_failsafe =
      (health->flags & synapse_topic_VehicleHealthFlags_Failsafe) != 0U;
  bool manual_valid =
      (manual->flags & manual_required) == manual_required &&
      (manual->flags & synapse_topic_ManualControlFlags_KillSwitch) == 0U;
  bool arm_switch =
      manual_valid &&
      (manual->flags & synapse_topic_ManualControlFlags_ArmSwitch) != 0U;

  efmu->mode = manual->flight_mode <= 2U ? manual->flight_mode : 0;
  efmu->armed = rdd2_guidance_arm_allowed(health_armed, health_failsafe,
                                          manual_valid, arm_switch);
  efmu->stick[0] = 0.001f * (float)manual->roll_milli;
  efmu->stick[1] = 0.001f * (float)manual->pitch_milli;
  efmu->stick[2] = 0.001f * (float)manual->yaw_milli;
  efmu->throttle = 0.001f * (float)manual->throttle_milli;
}

/*
 * Guidance no longer runs on the estimator's clock, so nothing else would
 * notice a stalled estimator: it would keep steering on the last estimate for
 * as long as the estimator stayed down. The age check is what the old
 * coupling provided implicitly, made explicit and bounded. The deadline is
 * five estimator periods, long enough that a single missed release is
 * invisible and short enough that dead reckoning on a frozen estimate cannot
 * accumulate.
 */
#define GUIDANCE_NAVIGATION_MAX_AGE_NS                                         \
  ((UINT64_C(5) * UINT64_C(1000000000)) / RDD2_NAVIGATION_ESTIMATOR_RATE_HZ)

static bool navigation_estimate_is_fresh(
    const synapse_topic_AttitudeEstimateData_t *attitude,
    uint64_t control_now_ns) {
  return attitude->timestamp_ns != 0U && control_now_ns >= attitude->timestamp_ns &&
         (control_now_ns - attitude->timestamp_ns) <=
             GUIDANCE_NAVIGATION_MAX_AGE_NS;
}

static bool navigation_inputs_are_valid(
    const synapse_topic_AttitudeEstimateData_t *attitude,
    const synapse_topic_OdometryEstimateData_t *odometry) {
  const float values[] = {
      attitude->attitude.w,         attitude->attitude.x,
      attitude->attitude.y,         attitude->attitude.z,
      odometry->position_enu_m.x,   odometry->position_enu_m.y,
      odometry->position_enu_m.z,   odometry->velocity_enu_m_s.x,
      odometry->velocity_enu_m_s.y, odometry->velocity_enu_m_s.z,
  };
  const uint8_t required = synapse_topic_AttitudeEstimateFlags_AttitudeValid;

  return (attitude->flags & required) == required &&
         odometry->quality_pct > 0 &&
         rdd2_control_values_are_finite(values, ARRAY_SIZE(values));
}

static void copy_navigation_inputs_to_efmu(
    GuidanceControllerState *efmu,
    const synapse_topic_AttitudeEstimateData_t *attitude,
    const synapse_topic_OdometryEstimateData_t *odometry, bool inputs_valid) {
  efmu->positionWorldEnu_m[0] =
      inputs_valid ? odometry->position_enu_m.x : 0.0f;
  efmu->positionWorldEnu_m[1] =
      inputs_valid ? odometry->position_enu_m.y : 0.0f;
  efmu->positionWorldEnu_m[2] =
      inputs_valid ? odometry->position_enu_m.z : 0.0f;
  efmu->velocityWorldEnu_m_s[0] =
      inputs_valid ? odometry->velocity_enu_m_s.x : 0.0f;
  efmu->velocityWorldEnu_m_s[1] =
      inputs_valid ? odometry->velocity_enu_m_s.y : 0.0f;
  efmu->velocityWorldEnu_m_s[2] =
      inputs_valid ? odometry->velocity_enu_m_s.z : 0.0f;
  efmu->quaternionWorldBody[0] = inputs_valid ? attitude->attitude.w : 1.0f;
  efmu->quaternionWorldBody[1] = inputs_valid ? attitude->attitude.x : 0.0f;
  efmu->quaternionWorldBody[2] = inputs_valid ? attitude->attitude.y : 0.0f;
  efmu->quaternionWorldBody[3] = inputs_valid ? attitude->attitude.z : 0.0f;
}

static bool reference_inputs_are_valid(
    const synapse_topic_LocalPositionCommandData_t *reference, bool observed,
    uint64_t control_now_ns) {
  const float values[] = {
      reference->position_enu_m.x,
      reference->position_enu_m.y,
      reference->position_enu_m.z,
      reference->velocity_enu_m_s.x,
      reference->velocity_enu_m_s.y,
      reference->velocity_enu_m_s.z,
      reference->acceleration_or_force_enu.x,
      reference->acceleration_or_force_enu.y,
      reference->acceleration_or_force_enu.z,
      reference->yaw_rad,
      reference->yaw_rate_rad_s,
  };

  return rdd2_control_timestamp_is_fresh(observed, reference->timestamp_ns,
                                         control_now_ns,
                                         GUIDANCE_REFERENCE_TIMEOUT_NS) &&
         reference->coordinate_frame == synapse_types_LocalFrame_LocalEnu &&
         reference->type_mask == 0U &&
         rdd2_control_values_are_finite(values, ARRAY_SIZE(values));
}

static void copy_reference_inputs_to_efmu(
    GuidanceControllerState *efmu,
    const synapse_topic_LocalPositionCommandData_t *reference,
    bool inputs_valid) {
  efmu->positionWorld_m[0] = inputs_valid ? reference->position_enu_m.x : 0.0f;
  efmu->positionWorld_m[1] = inputs_valid ? reference->position_enu_m.y : 0.0f;
  efmu->positionWorld_m[2] = inputs_valid ? reference->position_enu_m.z : 0.0f;
  efmu->velocityWorld_m_s[0] =
      inputs_valid ? reference->velocity_enu_m_s.x : 0.0f;
  efmu->velocityWorld_m_s[1] =
      inputs_valid ? reference->velocity_enu_m_s.y : 0.0f;
  efmu->velocityWorld_m_s[2] =
      inputs_valid ? reference->velocity_enu_m_s.z : 0.0f;
  efmu->accelerationWorld_m_s2[0] =
      inputs_valid ? reference->acceleration_or_force_enu.x : 0.0f;
  efmu->accelerationWorld_m_s2[1] =
      inputs_valid ? reference->acceleration_or_force_enu.y : 0.0f;
  efmu->accelerationWorld_m_s2[2] =
      inputs_valid ? reference->acceleration_or_force_enu.z : 0.0f;
  efmu->yaw_rad = inputs_valid ? reference->yaw_rad : 0.0f;
}

static void publish_efmu_outputs(struct guidance_controller_process *process,
                                uint64_t control_now_ns) {
  GuidanceControllerState *efmu = &process->efmu;
  /* Stamped with the control clock, not with the estimate it consumed: the
   * rate loop times the command out against the same clock, so a stalled
   * estimator now expires the command instead of leaving the rate loop
   * tracking a command whose timestamp never advances. */
  uint64_t timestamp_ns = control_now_ns;

  process->rate_command = (synapse_topic_RateCommandData_t){
      .timestamp_ns = timestamp_ns,
      .body_rate_flu_rad_s =
          {
              .roll = efmu->angularVelocityCommandFlu_rad_s[0],
              .pitch = efmu->angularVelocityCommandFlu_rad_s[1],
              .yaw = efmu->angularVelocityCommandFlu_rad_s[2],
          },
      .thrust = efmu->thrust_N,
  };
  process->attitude_command = (synapse_topic_AttitudeCommandData_t){
      .timestamp_ns = timestamp_ns,
      .attitude = process->attitude.attitude,
      .body_rate_flu_rad_s = process->rate_command.body_rate_flu_rad_s,
      .thrust = process->rate_command.thrust,
  };
  (void)zros_pub_update(&process->rate_command_pub);
  (void)zros_pub_update(&process->attitude_command_pub);
}

static bool efmu_outputs_are_finite(const GuidanceControllerState *efmu) {
  const float values[] = {
      efmu->angularVelocityCommandFlu_rad_s[0],
      efmu->angularVelocityCommandFlu_rad_s[1],
      efmu->angularVelocityCommandFlu_rad_s[2],
      efmu->thrust_N,
  };

  return rdd2_control_values_are_finite(values, ARRAY_SIZE(values));
}

static void guidance_controller_thread(void *arg1, void *arg2, void *arg3) {
  struct guidance_controller_process *process = arg1;

  ARG_UNUSED(arg2);
  ARG_UNUSED(arg3);

  while (true) {
    bool arm_switch;
    bool arm_switch_ack_valid;
    bool current_fault;
    bool health_armed;
    bool health_update_ok;
    bool manual_update_ok;
    bool manual_valid;
    bool navigation_valid;
    bool outputs_finite;
    bool position_arm_blocked;
    bool position_capable;
    bool position_requested;
    bool reference_valid;
    bool step_ok;
    uint64_t control_now_ns;

    /*
     * Released by dividing the control tick, not by the estimator's
     * publication. Clocking guidance off the estimator made a stalled
     * estimator stop guidance dead, silently and with no age check anywhere,
     * and it made the guidance rate a hostage of the estimator rate. The
     * model states these as independent sample() clocks; this brings the
     * firmware's process graph into agreement with it.
     */
    if (zros_sub_wait(&process->control_clock_sub, K_FOREVER) != 0 ||
        zros_sub_update(&process->control_clock_sub) != 0) {
      continue;
    }
    if (!rdd2_release_due(&process->release_scheduler, RDD2_CONTROL_RATE_HZ,
                          RDD2_GUIDANCE_RATE_HZ)) {
      continue;
    }
    control_now_ns = process->control_clock.timestamp_ns;
    (void)zros_sub_update(&process->attitude_sub);

    manual_update_ok = zros_sub_update(&process->manual_sub) == 0;
    health_update_ok = zros_sub_update(&process->health_sub) == 0;
    (void)zros_sub_update(&process->odometry_sub);
    if (zros_sub_update(&process->reference_sub) == 0) {
      process->have_reference = true;
    }

    navigation_valid =
        navigation_inputs_are_valid(&process->attitude, &process->odometry) &&
        navigation_estimate_is_fresh(&process->attitude, control_now_ns);
    manual_valid =
        (process->manual.flags & (synapse_topic_ManualControlFlags_Valid |
                                  synapse_topic_ManualControlFlags_Active)) ==
            (synapse_topic_ManualControlFlags_Valid |
             synapse_topic_ManualControlFlags_Active) &&
        (process->manual.flags & synapse_topic_ManualControlFlags_KillSwitch) ==
            0U;
    arm_switch =
        manual_valid && (process->manual.flags &
                         synapse_topic_ManualControlFlags_ArmSwitch) != 0U;
    arm_switch_ack_valid = manual_update_ok && manual_valid;
    if (manual_update_ok && manual_valid && !arm_switch) {
      process->awaiting_disarmed_health = true;
    }
    if (health_update_ok && (process->health.flags &
                             synapse_topic_VehicleHealthFlags_Armed) == 0U) {
      process->awaiting_disarmed_health = false;
    }
    health_armed =
        !process->awaiting_disarmed_health &&
        (process->health.flags & synapse_topic_VehicleHealthFlags_Armed) != 0U;
    process->control_fault_latched =
        rdd2_control_fault_latch(process->control_fault_latched,
                                 arm_switch_ack_valid, arm_switch, false);
    {
      synapse_topic_VehicleHealthData_t effective_health = process->health;

      if (!health_armed) {
        effective_health.flags &= ~synapse_topic_VehicleHealthFlags_Armed;
      }
      copy_manual_inputs_to_efmu(&process->efmu, &process->manual,
                                 &effective_health);
    }
    position_requested = process->efmu.mode == 2;
    reference_valid =
        reference_inputs_are_valid(&process->reference, process->have_reference,
                                   control_now_ns);
    position_capable = rdd2_position_source_ready_get() && reference_valid &&
                       rdd2_navigation_position_quality_is_usable(
                           process->odometry.quality_pct);
    position_arm_blocked =
        position_requested && !position_capable && !health_armed && arm_switch;
    if (manual_update_ok && manual_valid && process->manual.flight_mode <= 1U) {
      process->position_fallback_latched = false;
    } else if (position_requested && health_armed && !position_capable) {
      process->position_fallback_latched = true;
    }
    if (position_requested && health_armed &&
        (process->position_fallback_latched || !position_capable)) {
      process->efmu.mode = 1;
    }
    copy_navigation_inputs_to_efmu(&process->efmu, &process->attitude,
                                   &process->odometry, navigation_valid);
    copy_reference_inputs_to_efmu(&process->efmu, &process->reference,
                                  reference_valid);
    if (!navigation_valid || position_arm_blocked ||
        process->control_fault_latched) {
      process->efmu.armed = false;
    }
    GuidanceController_dostep(&process->efmu);
    step_ok =
        rdd2_generated_step_ok(process->efmu.rumoca_galec_error_signal_status);
    outputs_finite = efmu_outputs_are_finite(&process->efmu);
    current_fault = !navigation_valid || position_arm_blocked || !step_ok ||
                    !outputs_finite;
    if (current_fault && arm_switch && !process->control_fault_latched) {
      LOG_ERR("Guidance fault: navigation=%d position_arm_blocked=%d "
              "generated_status=%u outputs_finite=%d",
              navigation_valid, position_arm_blocked,
              process->efmu.rumoca_galec_error_signal_status, outputs_finite);
    }
    process->control_fault_latched = rdd2_control_fault_latch(
        process->control_fault_latched, arm_switch_ack_valid, arm_switch,
        current_fault);
    if (!current_fault && !process->control_fault_latched) {
      publish_efmu_outputs(process, control_now_ns);
    }
  }
}

int rdd2_guidance_controller_process_start(void) {
  struct guidance_controller_process *process = &g_process;
  int rc;

  *process = (struct guidance_controller_process){0};
  process->attitude.attitude.w = 1.0f;
  GuidanceController_startup(&process->efmu);
  GuidanceController_recalibrate(&process->efmu);
  zros_node_init(&process->node, "efmu_guidance");

  rc = zros_sub_init(&process->manual_sub, &process->node, &topic_manual_input,
                     &process->manual, 0.0);
  if (rc != 0) {
    return rc;
  }
  rc = zros_sub_init(&process->health_sub, &process->node,
                     &topic_vehicle_health, &process->health, 0.0);
  if (rc != 0) {
    return rc;
  }
  rc = zros_sub_init(&process->attitude_sub, &process->node,
                     &topic_attitude_estimate, &process->attitude, 0.0);
  if (rc != 0) {
    return rc;
  }
  rc = zros_sub_init(&process->control_clock_sub, &process->node,
                     &topic_control_imu, &process->control_clock, 0.0);
  if (rc != 0) {
    return rc;
  }
  rc = zros_sub_init(&process->odometry_sub, &process->node,
                     &topic_navigation_odometry, &process->odometry, 0.0);
  if (rc != 0) {
    return rc;
  }
  rc = zros_sub_init(&process->reference_sub, &process->node,
                     &GUIDANCE_REFERENCE_TOPIC, &process->reference, 0.0);
  if (rc != 0) {
    return rc;
  }

  rc = zros_pub_init(&process->rate_command_pub, &process->node,
                     &topic_rate_command, &process->rate_command);
  if (rc == 0) {
    rc = zros_pub_init(&process->attitude_command_pub, &process->node,
                       &topic_attitude_command, &process->attitude_command);
  }
  if (rc != 0) {
    return rc;
  }

  k_thread_create(&g_thread, g_guidance_stack,
                  K_THREAD_STACK_SIZEOF(g_guidance_stack),
                  guidance_controller_thread, process, NULL, NULL,
                  RDD2_GUIDANCE_PRIORITY, 0, K_NO_WAIT);
  k_thread_name_set(&g_thread, "efmu_guidance");
  return 0;
}
