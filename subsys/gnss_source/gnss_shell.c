/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gnss_onboard.h"

#include <synapse/types_reader.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

/* The receiver hangs off the UART, so the port and rate worth reporting are
 * the parent's, not the gnss node's own. */
#define GNSS_UART DT_PARENT(DT_ALIAS(gnss))

/* Reads stored counters only; the receiver's workqueue is never disturbed. */
static int cmd_gnss_status(const struct shell *sh, size_t argc, char **argv)
{
	struct rdd2_gnss_onboard_stats stats;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rdd2_gnss_onboard_stats_get(&stats);

	shell_print(sh, "port=%s baud=%u", DT_NODE_FULL_NAME(GNSS_UART),
		    (unsigned int)DT_PROP(GNSS_UART, current_speed));
	shell_print(sh, "ubx  frames=%u other=%u csum_err=%u bad_len=%u oversize=%u overrun=%u",
		    (unsigned int)stats.frames, (unsigned int)stats.other_frames,
		    (unsigned int)stats.checksum_errors, (unsigned int)stats.bad_length,
		    (unsigned int)stats.oversize, (unsigned int)stats.ring_overrun);
	shell_print(sh, "pvt  samples=%u published=%u failed=%u", (unsigned int)stats.samples,
		    (unsigned int)stats.published, (unsigned int)stats.publish_failed);

	if (stats.frames == 0U) {
		shell_warn(sh, "no UBX frames yet: the receiver is silent or the baud above "
			       "does not match it. A missing fix would still produce frames.");
		return 0;
	}

	if (stats.samples == 0U) {
		shell_warn(sh, "UBX arriving but no NAV-PVT: the module is not sending it");
		return 0;
	}

	shell_print(sh, "last sample %lld ms ago", k_uptime_get() - stats.last_sample_ms);
	shell_print(sh, "last fix=%s sats=%u pdop=%u.%02u hacc=%u mm",
		    synapse_types_GnssFixType_name(stats.last_fix_type),
		    (unsigned int)stats.last_satellites,
		    (unsigned int)stats.last_hdop_centi / 100U,
		    (unsigned int)stats.last_hdop_centi % 100U,
		    (unsigned int)stats.last_hacc_mm);

	if (stats.last_fix_type < synapse_types_GnssFixType_Fix2d) {
		shell_warn(sh, "receiver is parsing but has no position lock yet");
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_gnss,
			       SHELL_CMD(status, NULL, "onboard receiver counters",
					 cmd_gnss_status),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(gnss, &sub_gnss, "onboard GNSS receiver diagnostics", NULL);
