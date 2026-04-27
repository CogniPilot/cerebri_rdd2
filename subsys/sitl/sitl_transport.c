/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rc_input.h"
#include "sitl_flatbuffer.h"
#include "sitl_transport.h"
#include "topic_bus.h"
#include "topic_flatbuffer.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

struct sitl_input_store {
	uint8_t slots[2][RDD2_SITL_INPUT_MAX_SIZE];
	uint16_t lengths[2];
	atomic_t generation;
};

static struct sitl_input_store g_sitl_input_store;
static K_SEM_DEFINE(g_sitl_input_sem, 0, 1);

static void sitl_input_store_publish(const uint8_t *buf, size_t len)
{
	uint32_t next_generation = (uint32_t)atomic_get(&g_sitl_input_store.generation) + 1U;
	uint32_t slot = next_generation & 1U;

	memcpy(g_sitl_input_store.slots[slot], buf, len);
	g_sitl_input_store.lengths[slot] = (uint16_t)len;
	atomic_set(&g_sitl_input_store.generation, (atomic_val_t)next_generation);
	k_sem_give(&g_sitl_input_sem);
}

bool rdd2_sitl_latest_input_get(uint8_t *buf, size_t buf_size, size_t *len,
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
		generation_start = (uint32_t)atomic_get(&g_sitl_input_store.generation);
		if (generation_start == 0U) {
			return false;
		}

		slot = generation_start & 1U;
		length = g_sitl_input_store.lengths[slot];
		if (length == 0U || length > buf_size) {
			return false;
		}

		memcpy(buf, g_sitl_input_store.slots[slot], length);
		generation_end = (uint32_t)atomic_get(&g_sitl_input_store.generation);
	} while (generation_start != generation_end);

	*len = length;
	*generation = generation_start;
	return true;
}

bool rdd2_sitl_input_wait_next(uint32_t *last_generation, k_timeout_t timeout)
{
	uint32_t generation;

	if (last_generation == NULL) {
		return false;
	}

	generation = (uint32_t)atomic_get(&g_sitl_input_store.generation);
	if (generation != 0U && generation != *last_generation) {
		*last_generation = generation;
		return true;
	}

	while (k_sem_take(&g_sitl_input_sem, timeout) == 0) {
		generation = (uint32_t)atomic_get(&g_sitl_input_store.generation);
		if (generation != 0U && generation != *last_generation) {
			*last_generation = generation;
			return true;
		}
	}

	return false;
}

static void sitl_report_rc_input(const synapse_topic_RcChannels16_t *rc, uint8_t rc_link_quality,
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

bool rdd2_sitl_handle_input_blob(const uint8_t *buf, size_t len)
{
	synapse_topic_RcChannels16_t rc;
	uint8_t rc_link_quality;
	bool rc_valid;

	if (buf == NULL || len == 0U || len > RDD2_SITL_INPUT_MAX_SIZE) {
		return false;
	}

	if (!rdd2_sitl_fb_unpack_input(buf, len, NULL, NULL, &rc, &rc_link_quality, &rc_valid,
				       NULL, NULL)) {
		return false;
	}

	sitl_input_store_publish(buf, len);
	sitl_report_rc_input(&rc, rc_link_quality, rc_valid);
	return true;
}

bool rdd2_sitl_flight_state_blob_if_updated(uint32_t *last_generation, uint8_t *buf,
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

bool rdd2_sitl_motor_output_blob_if_updated(uint32_t *last_generation, uint8_t *buf,
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
