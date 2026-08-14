/* SPDX-License-Identifier: Apache-2.0 */

#include "drivers.h"
#include "synapse_time_status.h"
#include "zros_topics.h"

#include <string.h>

#include <zephyr/kernel.h>

#include <zros/private/zros_node_struct.h>
#include <zros/private/zros_pub_struct.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>

static struct zros_node g_node;
static struct zros_pub g_pub;
static rdd2_topic_motor_output_blob_t g_output;
static bool g_ready;
static bool g_time_ever_synced;

static uint64_t publish_zero_output(void)
{
	int64_t offset_ns = 0;
	synapse_types_TimeStatus_enum_t time_status =
		synapse_time_status_resolve(&g_time_ever_synced, &offset_ns);
	uint64_t boot_ns = synapse_time_boot_ns();

	memset(&g_output, 0, sizeof(g_output));
	g_output.timestamp_ns = synapse_time_apply_offset(boot_ns, offset_ns);
	g_output.time_status = time_status;
	if (g_ready) {
		(void)zros_pub_update(&g_pub);
	}
	return boot_ns;
}

int rdd2_motor_output_init(void)
{
	int rc;

	zros_node_init(&g_node, "rdd2_stub_motor_inhibit");
	rc = zros_pub_init(&g_pub, &g_node, &topic_pwm_signal_outputs, &g_output);
	g_ready = (rc == 0);
	if (g_ready) {
		(void)publish_zero_output();
	}
	return rc;
}

bool rdd2_motor_output_ready(void)
{
	return false;
}

uint64_t rdd2_motor_output_write_all(const rdd2_motor_values_t *motors,
				     bool armed, bool test_mode)
{
	ARG_UNUSED(motors);
	ARG_UNUSED(armed);
	ARG_UNUSED(test_mode);
	return publish_zero_output();
}

uint64_t rdd2_motor_output_write_all_raw(const rdd2_motor_raw_t *raw,
					 bool test_mode)
{
	ARG_UNUSED(raw);
	ARG_UNUSED(test_mode);
	return publish_zero_output();
}

bool rdd2_motor_test_get(rdd2_motor_values_t *motors)
{
	if (motors != NULL) {
		*motors = (rdd2_motor_values_t){0};
	}
	return false;
}

void rdd2_motor_test_set(size_t index, float value)
{
	ARG_UNUSED(index);
	ARG_UNUSED(value);
}

void rdd2_motor_test_clear(void)
{
}

bool rdd2_motor_raw_test_get(rdd2_motor_raw_t *raw)
{
	if (raw != NULL) {
		*raw = (rdd2_motor_raw_t){0};
	}
	return false;
}

void rdd2_motor_raw_test_set(size_t index, uint16_t value)
{
	ARG_UNUSED(index);
	ARG_UNUSED(value);
}

void rdd2_motor_raw_test_set_all(uint16_t value)
{
	ARG_UNUSED(value);
}

void rdd2_motor_raw_test_clear(void)
{
}
