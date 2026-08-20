/* SPDX-License-Identifier: Apache-2.0 */

#include "flight_log.h"
#include "flight_log_fs.h"

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

static int cmd_log_status(const struct shell *sh, size_t argc, char **argv)
{
	struct rdd2_flight_log_status status;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rdd2_flight_log_status_get(&status);

	shell_print(sh, "logging=%s active=%s mounted=%s sync_ok=%s",
		    status.want_logging ? "on" : "off", status.active ? "yes" : "no",
		    status.mounted ? "yes" : "no", status.last_sync_ok ? "yes" : "no");
	shell_print(sh, "session=%s%04u%s bytes=%llu dropped=%u ring_high=%u flush_err=%u",
		    RDD2_FLIGHT_LOG_FILE_PREFIX, (unsigned int)(status.session_index % 10000U),
		    RDD2_FLIGHT_LOG_FILE_SUFFIX, (unsigned long long)status.bytes_written,
		    status.dropped_frames, status.ring_high_water, status.flush_errors);
	return 0;
}

static int cmd_log_start(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rdd2_flight_log_request_start();
	shell_print(sh, "logging start requested");
	return 0;
}

static int cmd_log_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rdd2_flight_log_request_stop();
	shell_print(sh, "logging stop requested");
	return 0;
}

static int cmd_log_rotate(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rdd2_flight_log_request_rotate();
	shell_print(sh, "session rotation requested");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_flightlog,
	SHELL_CMD(status, NULL, "Show logger state and counters.", cmd_log_status),
	SHELL_CMD(start, NULL, "Request logging to start (or resume).", cmd_log_start),
	SHELL_CMD(stop, NULL, "Request logging to stop and close the file.", cmd_log_stop),
	SHELL_CMD(rotate, NULL, "Close the current file and open the next.", cmd_log_rotate),
	SHELL_SUBCMD_SET_END);

/* Named flightlog: the Zephyr logging subsystem already owns the `log` command
 * when CONFIG_LOG_CMDS is set. */
SHELL_CMD_REGISTER(flightlog, &sub_flightlog, "SD flight logger control.", NULL);
