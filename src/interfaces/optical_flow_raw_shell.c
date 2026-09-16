/* SPDX-License-Identifier: Apache-2.0 */

#include "processes/navigation_optical_flow_raw.h"
#include "processes/processes.h"

#include <errno.h>
#include <stdint.h>

#include <zephyr/shell/shell.h>

static int cmd_optical_flow_raw_status(const struct shell *sh, size_t argc,
                                       char **argv) {
  struct rdd2_navigation_optical_flow_raw_diagnostics diagnostics;

  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  if (!rdd2_navigation_optical_flow_raw_diagnostics_get(&diagnostics)) {
    shell_error(sh, "optical-flow raw diagnostics unavailable");
    return -ENOTSUP;
  }

  shell_print(
      sh,
      "adapter=%s valid=%u fresh=%u source_generation=%u "
      "source_timestamp_ns=%llu age_ms=%llu",
      rdd2_navigation_optical_flow_raw_status_name(diagnostics.adapter_status),
      diagnostics.measurement_valid, diagnostics.measurement_fresh,
      diagnostics.source_generation,
      (unsigned long long)diagnostics.source_timestamp_ns,
      (unsigned long long)(diagnostics.accepted_age_ns / UINT64_C(1000000)));
  shell_print(sh,
              "accepted=%u rejected=%u los_rad={%.5f,%.5f} "
              "gyro_rad={%.5f,%.5f} dt_s=%.4f distance_m=%.3f quality=%.2f "
              "flags=0x%02x time_status=%u",
              diagnostics.accepted_count, diagnostics.rejected_count,
              (double)diagnostics.line_of_sight_rad[0],
              (double)diagnostics.line_of_sight_rad[1],
              (double)diagnostics.gyro_body_flu_rad[0],
              (double)diagnostics.gyro_body_flu_rad[1],
              (double)diagnostics.integration_time_s,
              (double)diagnostics.ground_distance_m,
              (double)diagnostics.quality, diagnostics.flags,
              diagnostics.time_status);
  shell_print(sh,
              "fused_now=%u fused_count=%u last_fused_control_ns=%llu "
              "estimator_rejections=%d outcome=%d source=%d recovery_stage=%d "
              "control_timestamp_ns=%llu",
              diagnostics.correction_accepted,
              diagnostics.fusion_accepted_count,
              (unsigned long long)diagnostics.last_fusion_control_timestamp_ns,
              diagnostics.consecutive_estimator_rejections,
              diagnostics.correction_outcome, diagnostics.correction_source,
              diagnostics.recovery_stage,
              (unsigned long long)diagnostics.control_timestamp_ns);
  return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    optical_flow_raw_commands,
    SHELL_CMD(status, NULL,
              "Show tightly coupled optical-flow transport and fusion status.",
              cmd_optical_flow_raw_status),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(optical_flow_raw, &optical_flow_raw_commands,
                   "Inspect the tightly coupled optical-flow fusion path.",
                   NULL);
