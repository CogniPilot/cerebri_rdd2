/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lockstep_transport.h"
#include "lockstep_input.h"
#include "rc_input.h"
#include "synapse_messages.h"
#include "topic_bus.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

struct lockstep_input_store {
	uint8_t slots[2][RDD2_LOCKSTEP_INPUT_MAX_SIZE];
	uint16_t lengths[2];
	atomic_t generation;
};

static struct lockstep_input_store g_lockstep_input_store;
static K_SEM_DEFINE(g_lockstep_input_sem, 0, 1);

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

	if (manual == NULL) {
		return false;
	}
	memcpy(channels, csyn_rc_channels_data(&manual->rc), sizeof(rc));
	/* RDD2's existing controller assigns arm/mode to channels 4/5. */
	channels[4] = manual->rc.ch6;
	channels[5] = manual->rc.ch4;
	lockstep_report_rc_input(&rc, manual->valid ? 100U : 0U, manual->valid);
	return true;
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
