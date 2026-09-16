/* SPDX-License-Identifier: Apache-2.0 */

#include "interfaces/data.h"
#include "lockstep_shared.h"
#include "lockstep_transport.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <nsi_host_trampolines.h>
#include <nsi_main.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cerebri_lockstep/sequence.h>

LOG_MODULE_REGISTER(rdd2_lockstep_direct, LOG_LEVEL_INF);

void *rdd2_lockstep_host_map(const char *path, unsigned long size);
void rdd2_lockstep_host_unmap(void *mapping, unsigned long size);

static K_THREAD_STACK_DEFINE(g_direct_stack,
                             CONFIG_RDD2_LOCKSTEP_THREAD_STACK_SIZE);
static struct k_thread g_direct_thread;
static struct rdd2_lockstep_shared *g_shared;
static struct cerebri_lockstep_sequence g_lockstep;

static void direct_thread(void *arg0, void *arg1, void *arg2) {
  uint32_t flight_generation = 0U;
  uint32_t motor_generation = 0U;
  uint64_t coordinator_boot_ns = 0U;

  ARG_UNUSED(arg0);
  ARG_UNUSED(arg1);
  ARG_UNUSED(arg2);

  while (true) {
    int rc = cerebri_lockstep_sequence_wait(&g_lockstep);

    if (rc == -ECANCELED) {
      nsi_exit(0);
    }
    if (rc != 0) {
      LOG_ERR("direct SIL lockstep wait failed: %d", rc);
      return;
    }

    /* PwmSignalOutputs is emitted every controller tick; the frame advance
     * blocks on it once per tick, which is the deterministic completion
     * barrier for each of the frame's controller substeps. */
    switch (rdd2_lockstep_advance_frame(&g_lockstep, g_shared,
                                        &coordinator_boot_ns, &flight_generation,
                                        &motor_generation)) {
    case RDD2_LOCKSTEP_FRAME_OK:
      cerebri_lockstep_sequence_respond(&g_lockstep);
      break;
    case RDD2_LOCKSTEP_FRAME_TERMINATED:
      nsi_exit(0);
      break;
    case RDD2_LOCKSTEP_FRAME_INVALID:
      LOG_ERR("invalid direct SIL input for sequence %u",
              cerebri_lockstep_sequence_current(&g_lockstep));
      return;
    }
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
  if (__atomic_load_n(&g_shared->magic, __ATOMIC_ACQUIRE) !=
      RDD2_LOCKSTEP_MAGIC) {
    rdd2_lockstep_host_unmap(g_shared, sizeof(*g_shared));
    g_shared = NULL;
    LOG_ERR("direct lockstep transport has invalid magic");
    return -EIO;
  }
  rc = rdd2_lockstep_gps_mission_init();
  if (rc != 0) {
    rdd2_lockstep_host_unmap(g_shared, sizeof(*g_shared));
    g_shared = NULL;
    return rc;
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
