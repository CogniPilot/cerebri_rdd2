/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lockstep_transport.h"
#include "lockstep_input.h"
#include "interfaces/data.h"
#include "interfaces/drivers.h"
#include "interfaces/zros_topics.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <zros/private/zros_node_struct.h>
#include <zros/private/zros_pub_struct.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>

struct lockstep_input_store {
	uint8_t slots[2][RDD2_LOCKSTEP_INPUT_MAX_SIZE];
	uint16_t lengths[2];
	atomic_t generation;
};

static struct lockstep_input_store g_lockstep_input_store;
static K_SEM_DEFINE(g_lockstep_input_sem, 0, 1);
static struct zros_node g_lockstep_navigation_node;
static struct zros_pub g_lockstep_odometry_pub;
static struct zros_pub g_lockstep_reference_pub;
static synapse_topic_ExternalOdometryData_t g_lockstep_odometry;
static synapse_topic_LocalPositionCommandData_t g_lockstep_reference;
static bool g_lockstep_navigation_ready;

static void lockstep_input_store_publish(const uint8_t *buf, size_t len)
{
	uint32_t next_generation = (uint32_t)atomic_get(&g_lockstep_input_store.generation) + 1U;
	uint32_t slot = next_generation & 1U;

	memcpy(g_lockstep_input_store.slots[slot], buf, len);
	g_lockstep_input_store.lengths[slot] = (uint16_t)len;
	atomic_set(&g_lockstep_input_store.generation, (atomic_val_t)next_generation);
	k_sem_give(&g_lockstep_input_sem);
}

bool rdd2_lockstep_latest_input_get(uint8_t *buf, size_t buf_size, size_t *len,
				    uint32_t *generation)
{
	uint32_t generation_start;
	uint32_t generation_end;
	uint32_t slot;
	uint16_t length;

	if (buf == NULL || len == NULL || generation == NULL) {
		return false;
	}

	do {
		generation_start = (uint32_t)atomic_get(&g_lockstep_input_store.generation);
		if (generation_start == 0U) {
			return false;
		}

		slot = generation_start & 1U;
		length = g_lockstep_input_store.lengths[slot];
		if (length == 0U || length > buf_size) {
			return false;
		}

		memcpy(buf, g_lockstep_input_store.slots[slot], length);
		generation_end = (uint32_t)atomic_get(&g_lockstep_input_store.generation);
	} while (generation_start != generation_end);

	*len = length;
	*generation = generation_start;
	return true;
}

bool rdd2_lockstep_input_wait_next(uint32_t *last_generation, k_timeout_t timeout)
{
	uint32_t generation;

	if (last_generation == NULL) {
		return false;
	}

	generation = (uint32_t)atomic_get(&g_lockstep_input_store.generation);
	if (generation != 0U && generation != *last_generation) {
		*last_generation = generation;
		return true;
	}

	while (k_sem_take(&g_lockstep_input_sem, timeout) == 0) {
		generation = (uint32_t)atomic_get(&g_lockstep_input_store.generation);
		if (generation != 0U && generation != *last_generation) {
			*last_generation = generation;
			return true;
		}
	}

	return false;
}

static void lockstep_report_rc_input(const rdd2_rc_channels_t *rc, uint8_t rc_link_quality,
				     bool rc_valid)
{
	const struct device *const rc_dev = DEVICE_DT_GET(DT_ALIAS(rc));
	const int32_t *channels = rdd2_topic_rc_channels_data_const(rc);

	if (!device_is_ready(rc_dev)) {
		return;
	}

	for (size_t i = 0; i < 16U; i++) {
		(void)input_report_abs(rc_dev, (uint16_t)(i + 1U), channels[i], false, K_FOREVER);
	}

	(void)input_report(rc_dev, INPUT_EV_MSC, RDD2_RC_INPUT_EVENT_LINK_QUALITY, rc_link_quality,
			   false, K_FOREVER);
	(void)input_report(rc_dev, INPUT_EV_MSC, RDD2_RC_INPUT_EVENT_VALID, rc_valid ? 1 : 0, true,
			   K_FOREVER);
}

bool rdd2_lockstep_handle_input_blob(const uint8_t *buf, size_t len)
{
	if (buf == NULL || len == 0U || len > RDD2_LOCKSTEP_INPUT_MAX_SIZE) {
		return false;
	}
	if (!rdd2_lockstep_decode_inertial(buf, len, NULL, NULL, NULL)) {
		return false;
	}

	lockstep_input_store_publish(buf, len);
	return true;
}

bool rdd2_lockstep_handle_manual_control(const struct csyn_manual_control *manual)
{
	rdd2_rc_channels_t rc = {0};
	int32_t *channels = rdd2_topic_rc_channels_data(&rc);
	const int32_t *manual_channels;

	if (manual == NULL) {
		return false;
	}
	manual_channels = csyn_rc_channels_data(&manual->rc);
	for (size_t channel = 0U; channel < RDD2_RC_CHANNEL_COUNT; ++channel) {
		channels[channel] = manual_channels[channel];
	}
	/* RDD2's existing controller assigns arm/mode to channels 4/5. */
	channels[4] = manual->rc.ch6;
	channels[5] = manual->rc.ch4;
	lockstep_report_rc_input(&rc, manual->valid ? 100U : 0U, manual->valid);
	return true;
}

int rdd2_lockstep_navigation_init(void)
{
	int rc;

	zros_node_init(&g_lockstep_navigation_node, "rdd2_lockstep_navigation");
	rc = zros_pub_init(&g_lockstep_odometry_pub, &g_lockstep_navigation_node,
			   &topic_external_odometry, &g_lockstep_odometry);
	if (rc == 0) {
		rc = zros_pub_init(&g_lockstep_reference_pub,
				   &g_lockstep_navigation_node,
				   &topic_local_position_command,
				   &g_lockstep_reference);
	}
	g_lockstep_navigation_ready = rc == 0;
	return rc;
}

bool rdd2_lockstep_handle_navigation(
	const synapse_topic_ExternalOdometryData_t *odometry,
	const synapse_topic_LocalPositionCommandData_t *command)
{
	if (!g_lockstep_navigation_ready || odometry == NULL || command == NULL) {
		return false;
	}

	g_lockstep_odometry = *odometry;
	g_lockstep_reference = *command;
	return zros_pub_update(&g_lockstep_odometry_pub) == 0 &&
	       zros_pub_update(&g_lockstep_reference_pub) == 0;
}

bool rdd2_lockstep_flight_state_blob_if_updated(uint32_t *last_generation, uint8_t *buf,
						size_t buf_size, size_t *len)
{
	uint32_t generation = rdd2_topic_flight_state_generation();

	if (last_generation == NULL || generation == 0U || generation == *last_generation) {
		return false;
	}

	if (!rdd2_topic_flight_state_copy_blob(buf, buf_size, len)) {
		return false;
	}

	*last_generation = generation;
	return true;
}

bool rdd2_lockstep_motor_output_blob_if_updated(uint32_t *last_generation, uint8_t *buf,
						size_t buf_size, size_t *len)
{
	uint32_t generation = rdd2_topic_motor_output_generation();

	if (last_generation == NULL || generation == 0U || generation == *last_generation) {
		return false;
	}

	if (!rdd2_topic_motor_output_copy_blob(buf, buf_size, len)) {
		return false;
	}

	*last_generation = generation;
	return true;
}
