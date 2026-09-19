/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lockstep_transport.h"
#include "gnss_lockstep.h"
#include "gnss_source.h"
#include "interfaces/data.h"
#include "interfaces/drivers.h"
#include "interfaces/zros_topics.h"
#include "lockstep_input.h"
#include "processes/processes.h"
#include "processes/scheduling.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <zros/private/zros_node_struct.h>
#include <zros/private/zros_pub_struct.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>

struct lockstep_input_store {
  uint8_t slots[2][RDD2_LOCKSTEP_INPUT_MAX_SIZE];
  uint16_t lengths[2];
  atomic_t generation;
};

static struct lockstep_input_store g_lockstep_input_store;
static K_SEM_DEFINE(g_lockstep_input_sem, 0, 1);
static struct zros_node g_lockstep_mission_node;
static struct zros_pub g_lockstep_plan_pub;
static rdd2_waypoint_plan_t g_lockstep_plan;
static uint64_t g_control_now_ns;
static int32_t g_last_plan_sequence;
static bool g_plan_sequence_observed;
static bool g_lockstep_mission_ready;
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_LOCKSTEP_RAW)
static struct zros_pub g_lockstep_flow_pub;
static synapse_topic_OpticalFlowData_t g_lockstep_flow;
static uint64_t g_lockstep_flow_last_ns;
#endif

static void lockstep_input_store_publish(const uint8_t *buf, size_t len) {
  uint32_t next_generation =
      (uint32_t)atomic_get(&g_lockstep_input_store.generation) + 1U;
  uint32_t slot = next_generation & 1U;

  memcpy(g_lockstep_input_store.slots[slot], buf, len);
  g_lockstep_input_store.lengths[slot] = (uint16_t)len;
  atomic_set(&g_lockstep_input_store.generation, (atomic_val_t)next_generation);
  k_sem_give(&g_lockstep_input_sem);
}

bool rdd2_lockstep_latest_input_get(uint8_t *buf, size_t buf_size, size_t *len,
                                    uint32_t *generation) {
  uint32_t generation_start;
  uint32_t generation_end;
  uint32_t slot;
  uint16_t length;

  if (buf == NULL || len == NULL || generation == NULL) {
    return false;
  }

  do {
    generation_start = (uint32_t)atomic_get(&g_lockstep_input_store.generation);
    if (generation_start == 0U) {
      return false;
    }

    slot = generation_start & 1U;
    length = g_lockstep_input_store.lengths[slot];
    if (length == 0U || length > buf_size) {
      return false;
    }

    memcpy(buf, g_lockstep_input_store.slots[slot], length);
    generation_end = (uint32_t)atomic_get(&g_lockstep_input_store.generation);
  } while (generation_start != generation_end);

  *len = length;
  *generation = generation_start;
  return true;
}

bool rdd2_lockstep_input_wait_next(uint32_t *last_generation,
                                   k_timeout_t timeout) {
  uint32_t generation;

  if (last_generation == NULL) {
    return false;
  }

  generation = (uint32_t)atomic_get(&g_lockstep_input_store.generation);
  if (generation != 0U && generation != *last_generation) {
    *last_generation = generation;
    return true;
  }

  while (k_sem_take(&g_lockstep_input_sem, timeout) == 0) {
    generation = (uint32_t)atomic_get(&g_lockstep_input_store.generation);
    if (generation != 0U && generation != *last_generation) {
      *last_generation = generation;
      return true;
    }
  }

  return false;
}

static void lockstep_report_rc_input(const rdd2_rc_channels_t *rc,
                                     uint8_t rc_link_quality, bool rc_valid) {
  const struct device *const rc_dev = DEVICE_DT_GET(DT_ALIAS(rc));
  const int32_t *channels = rdd2_topic_rc_channels_data_const(rc);

  if (!device_is_ready(rc_dev)) {
    return;
  }

  for (size_t i = 0; i < 16U; i++) {
    (void)input_report_abs(rc_dev, (uint16_t)(i + 1U), channels[i], false,
                           K_FOREVER);
  }

  (void)input_report(rc_dev, INPUT_EV_MSC, RDD2_RC_INPUT_EVENT_LINK_QUALITY,
                     rc_link_quality, false, K_FOREVER);
  (void)input_report(rc_dev, INPUT_EV_MSC, RDD2_RC_INPUT_EVENT_VALID,
                     rc_valid ? 1 : 0, true, K_FOREVER);
}

bool rdd2_lockstep_handle_input_blob(const uint8_t *buf, size_t len) {
  if (buf == NULL || len == 0U || len > RDD2_LOCKSTEP_INPUT_MAX_SIZE) {
    return false;
  }
  if (!rdd2_lockstep_decode_inertial(buf, len, NULL, NULL, NULL)) {
    return false;
  }

  lockstep_input_store_publish(buf, len);
  return true;
}

static int32_t centered_milli_to_pwm(int32_t value) {
  return 1500 + CLAMP(value, -1000, 1000) / 2;
}

bool rdd2_lockstep_handle_manual_control(
    const synapse_topic_ManualControlData_t *manual) {
  rdd2_rc_channels_t rc = {0};
  int32_t *channels = rdd2_topic_rc_channels_data(&rc);
  uint8_t required_flags;
  uint16_t required_axes;
  bool valid;

  if (manual == NULL || manual->flight_mode > 2U) {
    return false;
  }

  for (size_t channel = 0U; channel < RDD2_RC_CHANNEL_COUNT; ++channel) {
    channels[channel] = 1500;
  }
  channels[0] = centered_milli_to_pwm(manual->roll_milli);
  channels[1] = centered_milli_to_pwm(manual->pitch_milli);
  channels[2] = 1000 + CLAMP((int32_t)manual->throttle_milli, 0, 1000);
  channels[3] = centered_milli_to_pwm(-(int32_t)manual->yaw_milli);
  channels[4] =
      (manual->flags & synapse_topic_ManualControlFlags_ArmSwitch) != 0U ? 2000
                                                                         : 1000;
  channels[5] = manual->flight_mode == 0U   ? 1000
                : manual->flight_mode == 1U ? 1500
                                            : 2000;

  required_flags = synapse_topic_ManualControlFlags_Valid |
                   synapse_topic_ManualControlFlags_Active;
  required_axes = synapse_topic_ManualControlAxes_Roll |
                  synapse_topic_ManualControlAxes_Pitch |
                  synapse_topic_ManualControlAxes_Throttle |
                  synapse_topic_ManualControlAxes_Yaw;
  valid = (manual->flags & required_flags) == required_flags &&
          (manual->flags & synapse_topic_ManualControlFlags_KillSwitch) == 0U &&
          (manual->active_axes & required_axes) == required_axes;
  if (!valid) {
    channels[4] = 1000;
  }
  lockstep_report_rc_input(&rc, valid ? 100U : 0U, valid);
  return true;
}

int rdd2_lockstep_gps_mission_init(void) {
  int rc;

  g_lockstep_plan = (rdd2_waypoint_plan_t){0};
  g_control_now_ns = 0U;
  g_last_plan_sequence = 0;
  g_plan_sequence_observed = false;
  rc = rdd2_gnss_lockstep_init();
  if (rc == 0) {
    zros_node_init(&g_lockstep_mission_node, "rdd2_lockstep_mission");
    rc = zros_pub_init(&g_lockstep_plan_pub, &g_lockstep_mission_node,
                       &topic_waypoint_plan, &g_lockstep_plan);
  }
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_LOCKSTEP_RAW)
  if (rc == 0) {
    g_lockstep_flow = (synapse_topic_OpticalFlowData_t){0};
    g_lockstep_flow_last_ns = 0U;
    rc = zros_pub_init(&g_lockstep_flow_pub, &g_lockstep_mission_node,
                       &topic_optical_flow, &g_lockstep_flow);
  }
#endif
  g_lockstep_mission_ready = rc == 0;
  return rc;
}

bool rdd2_lockstep_handle_gps_mission(const synapse_topic_GnssFixData_t *fix,
                                      const rdd2_waypoint_plan_t *plan,
                                      uint64_t control_now_ns) {
  if (!g_lockstep_mission_ready || fix == NULL || plan == NULL) {
    return false;
  }
  /*
   * A GNSS fix that the receiver adapter rejects is a normal runtime event,
   * not a corrupt transport frame: the future gate drops a fix stamped ahead
   * of the control clock (a producer whose freerun clock leads the boot clock
   * before time sync), the ordering gate drops an out-of-order or duplicate
   * fix, and the accuracy gate drops a degraded early fix. In every case the
   * estimator simply does not adopt the fix. The lockstep exchange must still
   * complete this step and respond, so the submit result is advisory here;
   * tearing the thread down on a legitimately gated fix would stall the whole
   * co-simulation. Only a structurally invalid mission input below is fatal.
   */
  (void)rdd2_gnss_lockstep_submit(fix, control_now_ns);
  g_control_now_ns = control_now_ns;
  if (plan->sequence < 0 ||
      (g_plan_sequence_observed && plan->sequence < g_last_plan_sequence)) {
    return false;
  }
  if (g_plan_sequence_observed && plan->sequence == g_last_plan_sequence) {
    return memcmp(plan, &g_lockstep_plan, sizeof(*plan)) == 0;
  }
  if (plan->sequence != 0) {
    g_lockstep_plan = *plan;
    if (zros_pub_update(&g_lockstep_plan_pub) != 0) {
      return false;
    }
    g_last_plan_sequence = plan->sequence;
    g_plan_sequence_observed = true;
  }
  return true;
}

bool rdd2_lockstep_handle_optical_flow(
    const synapse_topic_OpticalFlowData_t *sample) {
#if defined(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_LOCKSTEP_RAW)
  /* The coordinator repeats the newest sample in every frame; only a new
   * producer timestamp is a new sample. A zero timestamp is no sample at all. */
  if (!g_lockstep_mission_ready || sample == NULL ||
      sample->timestamp_ns == 0U ||
      sample->timestamp_ns == g_lockstep_flow_last_ns) {
    return true;
  }
  g_lockstep_flow = *sample;
  g_lockstep_flow_last_ns = sample->timestamp_ns;
  return zros_pub_update(&g_lockstep_flow_pub) == 0;
#else
  (void)sample;
  return true;
#endif
}

void rdd2_lockstep_gps_mission_status_get(
    struct rdd2_lockstep_gps_mission_status *status) {
  uint8_t state;

  if (status == NULL) {
    return;
  }
  state = rdd2_waypoint_mission_state_get();
  *status = (struct rdd2_lockstep_gps_mission_status){
      .timestamp_ns = g_control_now_ns,
      .plan_sequence = g_plan_sequence_observed ? g_last_plan_sequence : 0,
      .gnss_generation = rdd2_topic_generation(&topic_gnss_fix),
      .plan_generation = rdd2_topic_generation(&topic_waypoint_plan),
      .reference_generation =
          rdd2_topic_generation(&topic_trajectory_reference),
      .mission_state = state,
  };
  if (rdd2_position_source_ready_get()) {
    status->flags |= RDD2_LOCKSTEP_SOURCE_READY;
  }
  if (rdd2_navigation_origin_valid_get()) {
    status->flags |= RDD2_LOCKSTEP_ORIGIN_VALID;
  }
  {
    uint8_t flow_status = 0U;
    uint16_t flow_accepted = 0U;
    uint16_t flow_fused = 0U;

    rdd2_navigation_optical_flow_lockstep_get(&flow_status, &flow_accepted,
                                              &flow_fused);
    status->optical_flow_status = flow_status;
    status->optical_flow_accepted[0] = (uint8_t)(flow_accepted & 0xFFU);
    status->optical_flow_accepted[1] = (uint8_t)(flow_accepted >> 8);
    status->optical_flow_fused[0] = (uint8_t)(flow_fused & 0xFFU);
    status->optical_flow_fused[1] = (uint8_t)(flow_fused >> 8);
  }
  if (state == RDD2_WAYPOINT_MISSION_PENDING ||
      state == RDD2_WAYPOINT_MISSION_RUNNING) {
    status->flags |= RDD2_LOCKSTEP_PLAN_ACCEPTED;
  }
}

bool rdd2_lockstep_flight_state_blob_if_updated(uint32_t *last_generation,
                                                uint8_t *buf, size_t buf_size,
                                                size_t *len) {
  uint32_t generation = rdd2_topic_flight_state_generation();

  if (last_generation == NULL || generation == 0U ||
      generation == *last_generation) {
    return false;
  }

  if (!rdd2_topic_flight_state_copy_blob(buf, buf_size, len)) {
    return false;
  }

  *last_generation = generation;
  return true;
}

bool rdd2_lockstep_motor_output_blob_if_updated(uint32_t *last_generation,
                                                uint8_t *buf, size_t buf_size,
                                                size_t *len) {
  uint32_t generation = rdd2_topic_motor_output_generation();

  if (last_generation == NULL || generation == 0U ||
      generation == *last_generation) {
    return false;
  }

  if (!rdd2_topic_motor_output_copy_blob(buf, buf_size, len)) {
    return false;
  }

  *last_generation = generation;
  return true;
}

enum rdd2_lockstep_frame_result rdd2_lockstep_advance_frame(
    struct cerebri_lockstep_sequence *sequence,
    struct rdd2_lockstep_shared *shared, uint64_t *coordinator_boot_ns,
    uint32_t *flight_generation, uint32_t *motor_generation) {
  rdd2_topic_flight_state_blob_t flight;
  rdd2_topic_motor_output_blob_t motor = {0};
  size_t flight_len = 0U;
  size_t motor_len = 0U;
  uint64_t base = *coordinator_boot_ns;
  uint64_t target = shared->inertial_sample.timestamp_ns;
  uint64_t tick_ns;

  if (!rdd2_lockstep_handle_manual_control(&shared->manual_control) ||
      !rdd2_lockstep_handle_gps_mission(
          &shared->gnss_fix, &shared->waypoint_plan,
          shared->inertial_sample.timestamp_ns)) {
    return RDD2_LOCKSTEP_FRAME_INVALID;
  }

  /* A plant macro-step ends at shared->inertial_sample.timestamp_ns. Replay the
   * frame's single inertial reading once per 800 Hz controller period up to
   * that boundary; a frame with no forward progress still advances one tick so
   * the exchange always completes. Each tick blocks on its motor output, which
   * yields the CPU long enough for the estimator, planner, and guidance threads
   * to run before the next tick is fed. */
  if (target <= base) {
    target = base + RDD2_CONTROL_PERIOD_NS;
  }
  for (tick_ns = base; tick_ns < target;) {
    synapse_topic_InertialSampleData_t sample = shared->inertial_sample;

    tick_ns += RDD2_CONTROL_PERIOD_NS;
    if (tick_ns > target) {
      tick_ns = target;
    }
    sample.timestamp_ns = tick_ns;
    if (!rdd2_lockstep_handle_input_blob((const uint8_t *)&sample,
                                         sizeof(sample))) {
      return RDD2_LOCKSTEP_FRAME_INVALID;
    }
    while (!rdd2_lockstep_motor_output_blob_if_updated(
        motor_generation, (uint8_t *)&motor, sizeof(motor), &motor_len)) {
      if (cerebri_lockstep_sequence_terminated(sequence)) {
        return RDD2_LOCKSTEP_FRAME_TERMINATED;
      }
      k_yield();
    }
  }
  *coordinator_boot_ns = target;
  /* The flow sample integrates the interval that just ran, so it is handed
   * over once that interval is complete and reaches the estimator with the
   * next tick, one controller period behind the motion it describes. */
  (void)rdd2_lockstep_handle_optical_flow(&shared->optical_flow);

  (void)rdd2_lockstep_flight_state_blob_if_updated(
      flight_generation, (uint8_t *)&flight, sizeof(flight), &flight_len);
  shared->pwm_signal_outputs = motor;
  if (flight_len == sizeof(flight)) {
    shared->vehicle_health = flight.vehicle_health;
    shared->attitude_estimate = flight.attitude_estimate;
    shared->attitude_command = flight.attitude_command;
    shared->control_loop_metrics = flight.control_loop_metrics;
    shared->odometry_estimate = flight.odometry_estimate;
    shared->planner_reference = flight.planner_reference;
  }
  rdd2_lockstep_gps_mission_status_get(&shared->mission_status);
  return RDD2_LOCKSTEP_FRAME_OK;
}
