/* SPDX-License-Identifier: Apache-2.0 */

#include "lockstep_shared.h"
#include "lockstep_transport.h"
#include "synapse_messages.h"

#include <errno.h>
#include <stdbool.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cerebri_lockstep/sequence.h>

#include <csyn/csyn_codec.h>

LOG_MODULE_REGISTER(rdd2_lockstep_fastdyn, LOG_LEVEL_INF);

/* This symbol is deliberately global. The host resolves it from the firmware
 * ELF and maps the corresponding bytes in FastDyn's file-backed QEMU RAM, so
 * neither side hardcodes a firmware address or payload offset. */
struct rdd2_lockstep_shared rdd2_fastdyn_lockstep_shared;

static K_THREAD_STACK_DEFINE(g_fastdyn_stack,
                             CONFIG_RDD2_LOCKSTEP_THREAD_STACK_SIZE);
static struct k_thread g_fastdyn_thread;
static struct cerebri_lockstep_sequence g_lockstep;

static void fastdyn_thread(void *arg0, void *arg1, void *arg2) {
  uint32_t flight_generation = 0U;
  uint32_t motor_generation = 0U;

  ARG_UNUSED(arg0);
  ARG_UNUSED(arg1);
  ARG_UNUSED(arg2);

  while (true) {
    uint32_t sequence;
    rdd2_topic_flight_state_blob_t flight;
    rdd2_topic_motor_output_blob_t motor;
    size_t flight_len = 0U;
    size_t motor_len = 0U;
    struct csyn_manual_control manual = {0};
    int rc = cerebri_lockstep_sequence_wait(&g_lockstep);

    if (rc == -ECANCELED) {
      return;
    }
    if (rc != 0) {
      LOG_ERR("FastDyn lockstep wait failed: %d", rc);
      return;
    }
    sequence = cerebri_lockstep_sequence_current(&g_lockstep);
    if (!csyn_decode_manual_control(
            &rdd2_fastdyn_lockstep_shared.manual_control,
            sizeof(rdd2_fastdyn_lockstep_shared.manual_control), &manual.rc,
            &manual.valid) ||
        !rdd2_lockstep_handle_manual_control(&manual) ||
        !rdd2_lockstep_handle_input_blob(
            (const uint8_t *)&rdd2_fastdyn_lockstep_shared.inertial_sample,
            sizeof(rdd2_fastdyn_lockstep_shared.inertial_sample))) {
      LOG_ERR("invalid FastDyn lockstep input for sequence %u", sequence);
      return;
    }

    while (!rdd2_lockstep_motor_output_blob_if_updated(
        &motor_generation, (uint8_t *)&motor, sizeof(motor), &motor_len)) {
      if (cerebri_lockstep_sequence_terminated(&g_lockstep)) {
        return;
      }
      k_yield();
    }
    (void)rdd2_lockstep_flight_state_blob_if_updated(
        &flight_generation, (uint8_t *)&flight, sizeof(flight), &flight_len);
    rdd2_fastdyn_lockstep_shared.pwm_signal_outputs = motor;
    if (flight_len == sizeof(flight)) {
      rdd2_fastdyn_lockstep_shared.vehicle_health = flight.vehicle_health;
      rdd2_fastdyn_lockstep_shared.attitude_estimate = flight.attitude_estimate;
      rdd2_fastdyn_lockstep_shared.attitude_command = flight.attitude_command;
      rdd2_fastdyn_lockstep_shared.control_loop_metrics =
          flight.control_loop_metrics;
    }
    cerebri_lockstep_sequence_respond(&g_lockstep);
  }
}

static int fastdyn_init(void) {
  int rc;

  rdd2_fastdyn_lockstep_shared = (struct rdd2_lockstep_shared){
      .magic = RDD2_LOCKSTEP_MAGIC,
  };
  rc = cerebri_lockstep_sequence_init(
      &g_lockstep, &rdd2_fastdyn_lockstep_shared.input_sequence,
      &rdd2_fastdyn_lockstep_shared.response_sequence,
      &rdd2_fastdyn_lockstep_shared.terminate, IS_ENABLED(CONFIG_CSYN_ZENOH));
  if (rc != 0) {
    return rc;
  }

  k_thread_create(&g_fastdyn_thread, g_fastdyn_stack,
                  K_THREAD_STACK_SIZEOF(g_fastdyn_stack), fastdyn_thread, NULL,
                  NULL, NULL, CONFIG_RDD2_LOCKSTEP_THREAD_PRIORITY, 0,
                  K_NO_WAIT);
  k_thread_name_set(&g_fastdyn_thread, "rdd2_lockstep_fastdyn");
  LOG_INF("FastDyn shared-memory lockstep enabled");
  return 0;
}

SYS_INIT(fastdyn_init, APPLICATION, 1);
