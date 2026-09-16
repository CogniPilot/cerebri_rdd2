/* SPDX-License-Identifier: Apache-2.0 */

#include "interfaces/data.h"
#include "lockstep_shared.h"
#include "lockstep_transport.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cerebri_lockstep/sequence.h>

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
  uint64_t coordinator_boot_ns = 0U;

  ARG_UNUSED(arg0);
  ARG_UNUSED(arg1);
  ARG_UNUSED(arg2);

  while (true) {
    int rc = cerebri_lockstep_sequence_wait(&g_lockstep);

    if (rc == -ECANCELED) {
      return;
    }
    if (rc != 0) {
      LOG_ERR("FastDyn lockstep wait failed: %d", rc);
      return;
    }

    switch (rdd2_lockstep_advance_frame(&g_lockstep,
                                        &rdd2_fastdyn_lockstep_shared,
                                        &coordinator_boot_ns, &flight_generation,
                                        &motor_generation)) {
    case RDD2_LOCKSTEP_FRAME_OK:
      cerebri_lockstep_sequence_respond(&g_lockstep);
      break;
    case RDD2_LOCKSTEP_FRAME_TERMINATED:
      return;
    case RDD2_LOCKSTEP_FRAME_INVALID:
      LOG_ERR("invalid FastDyn lockstep input for sequence %u",
              cerebri_lockstep_sequence_current(&g_lockstep));
      return;
    }
  }
}

static int fastdyn_init(void) {
  int rc;

  memset(&rdd2_fastdyn_lockstep_shared, 0,
         sizeof(rdd2_fastdyn_lockstep_shared));
  rc = rdd2_lockstep_gps_mission_init();
  if (rc != 0) {
    return rc;
  }
  rc = cerebri_lockstep_sequence_init(
      &g_lockstep, &rdd2_fastdyn_lockstep_shared.input_sequence,
      &rdd2_fastdyn_lockstep_shared.response_sequence,
      &rdd2_fastdyn_lockstep_shared.terminate, false);
  if (rc != 0) {
    return rc;
  }
  __atomic_store_n(&rdd2_fastdyn_lockstep_shared.magic, RDD2_LOCKSTEP_MAGIC,
                   __ATOMIC_RELEASE);

  k_thread_create(&g_fastdyn_thread, g_fastdyn_stack,
                  K_THREAD_STACK_SIZEOF(g_fastdyn_stack), fastdyn_thread, NULL,
                  NULL, NULL, CONFIG_RDD2_LOCKSTEP_THREAD_PRIORITY, 0,
                  K_NO_WAIT);
  k_thread_name_set(&g_fastdyn_thread, "rdd2_lockstep_fastdyn");
  LOG_INF("FastDyn shared-memory lockstep enabled");
  return 0;
}

SYS_INIT(fastdyn_init, APPLICATION, 1);
