/* SPDX-License-Identifier: Apache-2.0 */

#include "lockstep_shared.h"
#include "lockstep_transport.h"
#include "synapse_messages.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <nsi_host_trampolines.h>
#include <nsi_main.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cerebri_lockstep/sequence.h>

#include <csyn/csyn_codec.h>

LOG_MODULE_REGISTER(rdd2_lockstep_direct, LOG_LEVEL_INF);

void *rdd2_lockstep_host_map(const char *path, unsigned long size);
void rdd2_lockstep_host_unmap(void *mapping, unsigned long size);

static K_THREAD_STACK_DEFINE(g_direct_stack,
                             CONFIG_RDD2_LOCKSTEP_THREAD_STACK_SIZE);
static struct k_thread g_direct_thread;
static struct rdd2_lockstep_shared *g_shared;
static struct cerebri_lockstep_sequence g_lockstep;

static void exit_if_terminated(void) {
  if (cerebri_lockstep_sequence_terminated(&g_lockstep)) {
    nsi_exit(0);
  }
}

static void direct_thread(void *arg0, void *arg1, void *arg2) {
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
    int rc = cerebri_lockstep_sequence_wait(&g_lockstep);
    struct csyn_manual_control manual = {0};

    if (rc == -ECANCELED) {
      nsi_exit(0);
    }
    if (rc != 0) {
      LOG_ERR("direct SIL lockstep wait failed: %d", rc);
      return;
    }
    sequence = cerebri_lockstep_sequence_current(&g_lockstep);
    if (!csyn_decode_manual_control(&g_shared->manual_control,
                                    sizeof(g_shared->manual_control),
                                    &manual.rc, &manual.valid) ||
        !rdd2_lockstep_handle_manual_control(&manual) ||
        !rdd2_lockstep_handle_input_blob(
            (const uint8_t *)&g_shared->inertial_sample,
            sizeof(g_shared->inertial_sample))) {
      LOG_ERR("invalid direct SIL input for sequence %u", sequence);
      return;
    }
    /* PwmSignalOutputs is emitted every controller tick. Waiting for its next
     * generation is the deterministic completion barrier for this input.
     */
    while (!rdd2_lockstep_motor_output_blob_if_updated(
        &motor_generation, (uint8_t *)&motor, sizeof(motor), &motor_len)) {
      exit_if_terminated();
      k_yield();
    }
    (void)rdd2_lockstep_flight_state_blob_if_updated(
        &flight_generation, (uint8_t *)&flight, sizeof(flight), &flight_len);
    g_shared->pwm_signal_outputs = motor;
    if (flight_len == sizeof(flight)) {
      g_shared->vehicle_health = flight.vehicle_health;
      g_shared->attitude_estimate = flight.attitude_estimate;
      g_shared->attitude_command = flight.attitude_command;
      g_shared->control_loop_metrics = flight.control_loop_metrics;
    }
    cerebri_lockstep_sequence_respond(&g_lockstep);
  }
}

static int direct_init(void) {
  char *path = nsi_host_getenv("RDD2_LOCKSTEP_SHM");
  int rc;

  if (path == NULL || path[0] == '\0') {
    LOG_ERR("RDD2_LOCKSTEP_SHM is required for direct lockstep");
    return -EINVAL;
  }
  g_shared = rdd2_lockstep_host_map(path, sizeof(*g_shared));
  if (g_shared == NULL) {
    LOG_ERR("cannot map direct lockstep transport");
    return -EIO;
  }
  if (g_shared->magic != RDD2_LOCKSTEP_MAGIC) {
    rdd2_lockstep_host_unmap(g_shared, sizeof(*g_shared));
    g_shared = NULL;
    LOG_ERR("direct lockstep transport has invalid magic");
    return -EIO;
  }
  rc = cerebri_lockstep_sequence_init(
      &g_lockstep, &g_shared->input_sequence, &g_shared->response_sequence,
      &g_shared->terminate,
      nsi_host_getenv("RDD2_LOCKSTEP_COOPERATIVE") != NULL);
  if (rc != 0) {
    rdd2_lockstep_host_unmap(g_shared, sizeof(*g_shared));
    g_shared = NULL;
    return rc;
  }

  k_thread_create(&g_direct_thread, g_direct_stack,
                  K_THREAD_STACK_SIZEOF(g_direct_stack), direct_thread, NULL,
                  NULL, NULL, CONFIG_RDD2_LOCKSTEP_THREAD_PRIORITY, 0,
                  K_NO_WAIT);
  k_thread_name_set(&g_direct_thread, "rdd2_sil_direct");
  LOG_INF("direct shared-memory lockstep enabled");
  return 0;
}

SYS_INIT(direct_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
