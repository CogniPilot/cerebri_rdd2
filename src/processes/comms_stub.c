/* SPDX-License-Identifier: Apache-2.0 */

#include "processes.h"

#include "interfaces/zros_topics.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

LOG_MODULE_DECLARE(rdd2, LOG_LEVEL_INF);

int rdd2_comms_stub_process_run(void)
{
	LOG_INF("passive communications endpoint ready");
	k_sleep(K_FOREVER);
	return 0;
}

#if defined(CONFIG_SHELL)
static int cmd_stub_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh,
		    "image=PASSIVE-COMMS controller=absent outputs=absent periodic_stub_work=off");
	shell_print(sh, "received gnss=%u optical_flow=%u",
		    rdd2_topic_generation(&topic_gnss_fix),
		    rdd2_topic_generation(&topic_optical_flow_vel));
	return 0;
}

SHELL_CMD_REGISTER(stub, NULL, "Show passive communications endpoint status.",
		   cmd_stub_status);
#endif
