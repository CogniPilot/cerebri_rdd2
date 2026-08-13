/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gnss_m10_protocol.h"
#include "gnss_onboard.h"

#include <synapse/types_reader.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

/* The receiver hangs off the UART, so the port and rate worth reporting are
 * the parent's, not the gnss node's own. */
#define GNSS_UART DT_PARENT(DT_ALIAS(gnss))

static const char *config_status_name(uint8_t status) {
  switch (status) {
  case RDD2_GNSS_M10_CONFIG_UNCONFIGURED:
    return "sending";
  case RDD2_GNSS_M10_CONFIG_WAITING_ACK:
    return "wait-ack";
  case RDD2_GNSS_M10_CONFIG_CONFIGURED:
    return "configured";
  case RDD2_GNSS_M10_CONFIG_FAILED:
  default:
    return "FAILED";
  }
}

/* Reads stored counters only; the receiver thread is never disturbed. */
static int cmd_gnss_status(const struct shell *sh, size_t argc, char **argv) {
  struct rdd2_gnss_onboard_stats stats;

  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  rdd2_gnss_onboard_stats_get(&stats);

  shell_print(sh, "port=%s receiver=UART1 baud=%u",
              DT_NODE_FULL_NAME(GNSS_UART),
              (unsigned int)DT_PROP(GNSS_UART, current_speed));
  shell_print(
      sh, "cfg  state=%s step=%u/1 attempt=%u ack=%u nak=%u timeout=%u",
      config_status_name(stats.config_status), (unsigned int)stats.config_step,
      (unsigned int)stats.config_attempt, (unsigned int)stats.config_acks,
      (unsigned int)stats.config_naks, (unsigned int)stats.config_timeouts);
  shell_print(sh, "cfg  unexpected_ack=%u bad_ack_len=%u",
              (unsigned int)stats.unexpected_acks,
              (unsigned int)stats.bad_ack_length);
  shell_print(
      sh,
      "ubx  frames=%u other=%u csum_err=%u bad_len=%u oversize=%u overrun=%u",
      (unsigned int)stats.frames, (unsigned int)stats.other_frames,
      (unsigned int)stats.checksum_errors, (unsigned int)stats.bad_length,
      (unsigned int)stats.oversize, (unsigned int)stats.ring_overrun);
  shell_print(sh, "pvt  samples=%u published=%u failed=%u",
              (unsigned int)stats.samples, (unsigned int)stats.published,
              (unsigned int)stats.publish_failed);
  shell_print(
      sh, "gate configured=%s stream=%s fix=%s accuracy=%s READY=%s",
      stats.configured ? "yes" : "NO",
      stats.stable_samples >= RDD2_GNSS_M10_STABLE_SAMPLES ? "stable" : "NO",
      stats.fix_accepted ? "accepted" : "NO",
      stats.accuracy_accepted ? "accepted" : "NO", stats.ready ? "YES" : "NO");

  if (stats.config_status == RDD2_GNSS_M10_CONFIG_FAILED) {
    shell_error(
        sh,
        "M10 configuration failed closed; reboot after checking wiring/baud");
  }

  if (stats.frames == 0U) {
    shell_warn(sh,
               "no UBX frames yet: the receiver is silent or the baud above "
               "does not match it. A missing fix would still produce frames.");
    return 0;
  }

  if (stats.samples == 0U) {
    shell_warn(sh, "UBX arriving but no NAV-PVT: the module is not sending it");
    return 0;
  }

  shell_print(sh,
              "rate %u.%03u Hz gap=%u ms max_gap=%u ms stable=%u rate_err=%u "
              "age=%lld ms",
              (unsigned int)stats.measured_rate_millihz / 1000U,
              (unsigned int)stats.measured_rate_millihz % 1000U,
              (unsigned int)stats.last_gap_ms, (unsigned int)stats.max_gap_ms,
              (unsigned int)stats.stable_samples,
              (unsigned int)stats.rate_errors,
              k_uptime_get() - stats.last_sample_ms);
  shell_print(sh, "last fix=%s accepted=%s sats=%u pdop=%u.%02u",
              synapse_types_GnssFixType_name(stats.last_fix_type),
              stats.fix_accepted ? "yes" : "NO",
              (unsigned int)stats.last_satellites,
              (unsigned int)stats.last_hdop_centi / 100U,
              (unsigned int)stats.last_hdop_centi % 100U);
  shell_print(sh, "accuracy h=%u/%u mm v=%u/%u mm speed=%u/%u mm/s%s",
              (unsigned int)stats.last_hacc_mm, RDD2_GNSS_M10_MAX_HACC_MM,
              (unsigned int)stats.last_vacc_mm, RDD2_GNSS_M10_MAX_VACC_MM,
              (unsigned int)stats.last_sacc_mm_s, RDD2_GNSS_M10_MAX_SACC_MM_S,
              stats.accuracy_accepted ? " accepted" : " REJECTED");

  if (!stats.fix_accepted) {
    shell_warn(
        sh,
        "fix rejected: readiness requires 3D, DGNSS, RTK float, or RTK fixed");
  }
  if (!stats.ready) {
    shell_warn(sh, "GNSS is not ready for position flight");
  }

  return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_gnss,
                               SHELL_CMD(status, NULL,
                                         "onboard receiver counters",
                                         cmd_gnss_status),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(gnss, &sub_gnss, "onboard GNSS receiver diagnostics", NULL);
