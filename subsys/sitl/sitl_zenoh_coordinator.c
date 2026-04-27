/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sitl_transport.h"
#include "topic_flatbuffer.h"

#include <stdatomic.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zenoh-pico.h>

LOG_MODULE_REGISTER(rdd2_sitl_zenoh, LOG_LEVEL_INF);

static K_THREAD_STACK_DEFINE(g_sitl_zenoh_thread_stack, CONFIG_RDD2_SITL_THREAD_STACK_SIZE);
static struct k_thread g_sitl_zenoh_thread;

struct sitl_zenoh_rx_store {
	uint8_t slots[2][RDD2_SITL_INPUT_MAX_SIZE];
	uint16_t lengths[2];
	atomic_uint generation;
};

static struct sitl_zenoh_rx_store g_zenoh_rx_store;

static void sitl_zenoh_rx_store_publish(const uint8_t *buf, size_t len)
{
	unsigned int next_generation =
		atomic_load_explicit(&g_zenoh_rx_store.generation, memory_order_relaxed) + 1U;
	unsigned int slot = next_generation & 1U;

	memcpy(g_zenoh_rx_store.slots[slot], buf, len);
	g_zenoh_rx_store.lengths[slot] = (uint16_t)len;
	atomic_store_explicit(&g_zenoh_rx_store.generation, next_generation, memory_order_release);
}

static bool sitl_zenoh_rx_store_get_if_updated(unsigned int *last_generation, uint8_t *buf,
					       size_t buf_size, size_t *len)
{
	unsigned int generation_start;
	unsigned int generation_end;
	unsigned int slot;
	uint16_t length;

	if (last_generation == NULL || buf == NULL || len == NULL) {
		return false;
	}

	do {
		generation_start =
			atomic_load_explicit(&g_zenoh_rx_store.generation, memory_order_acquire);
		if (generation_start == 0U || generation_start == *last_generation) {
			return false;
		}

		slot = generation_start & 1U;
		length = g_zenoh_rx_store.lengths[slot];
		if (length == 0U || length > buf_size) {
			return false;
		}

		memcpy(buf, g_zenoh_rx_store.slots[slot], length);
		generation_end =
			atomic_load_explicit(&g_zenoh_rx_store.generation, memory_order_acquire);
	} while (generation_start != generation_end);

	*len = length;
	*last_generation = generation_start;
	return true;
}

static bool sitl_zenoh_keyexpr_matches(z_loaned_sample_t *sample, const char *expected)
{
	z_view_string_t keystr;
	size_t key_len;
	size_t expected_len;

	if (expected == NULL || z_keyexpr_as_view_string(z_sample_keyexpr(sample), &keystr) < 0) {
		return false;
	}

	key_len = z_string_len(z_loan(keystr));
	expected_len = strlen(expected);

	return key_len == expected_len &&
	       memcmp(z_string_data(z_loan(keystr)), expected, expected_len) == 0;
}

static void sitl_zenoh_input_handler(z_loaned_sample_t *sample, void *arg)
{
	z_bytes_reader_t reader;
	size_t payload_len;
	uint8_t buf[RDD2_SITL_INPUT_MAX_SIZE];

	ARG_UNUSED(arg);

	if (!sitl_zenoh_keyexpr_matches(sample, CONFIG_RDD2_SITL_ZENOH_INPUT_KEYEXPR)) {
		return;
	}

	payload_len = z_bytes_len(z_sample_payload(sample));
	if (payload_len > sizeof(buf)) {
		LOG_WRN("sitl zenoh input too large: %zu", payload_len);
		return;
	}

	reader = z_bytes_get_reader(z_sample_payload(sample));
	if (z_bytes_reader_read(&reader, buf, payload_len) != payload_len) {
		LOG_WRN("sitl zenoh input read failed");
		return;
	}

	sitl_zenoh_rx_store_publish(buf, payload_len);
}

static int sitl_zenoh_config_init(z_owned_config_t *config)
{
	bool is_client = strcmp(CONFIG_RDD2_SITL_ZENOH_MODE, "client") == 0;
	uint8_t locator_key = is_client ? Z_CONFIG_CONNECT_KEY : Z_CONFIG_LISTEN_KEY;
	int rc;

	rc = z_config_default(config);
	if (rc < 0) {
		return rc;
	}

	rc = zp_config_insert(z_loan_mut(*config), Z_CONFIG_MODE_KEY, CONFIG_RDD2_SITL_ZENOH_MODE);
	if (rc < 0) {
		return rc;
	}

	if (CONFIG_RDD2_SITL_ZENOH_LOCATOR[0] != '\0') {
		rc = zp_config_insert(z_loan_mut(*config), locator_key,
				      CONFIG_RDD2_SITL_ZENOH_LOCATOR);
		if (rc < 0) {
			return rc;
		}
	}

	return 0;
}

static int sitl_zenoh_open(z_owned_session_t *session)
{
	z_owned_config_t config;
	z_owned_closure_sample_t callback;
	z_view_keyexpr_t keyexpr;
	int rc;

	z_internal_null(&config);
	z_internal_null(&callback);
	z_internal_null(session);

	rc = sitl_zenoh_config_init(&config);
	if (rc < 0) {
		return rc;
	}

	rc = z_view_keyexpr_from_str(&keyexpr, CONFIG_RDD2_SITL_ZENOH_INPUT_KEYEXPR);
	if (rc < 0) {
		z_drop(z_move(config));
		return rc;
	}

	z_closure(&callback, sitl_zenoh_input_handler, NULL, NULL);
	rc = z_open(session, z_move(config), NULL);
	if (rc < 0) {
		z_drop(z_move(callback));
		return rc;
	}

	rc = z_declare_background_subscriber(z_loan(*session), z_loan(keyexpr), z_move(callback),
					    NULL);
	if (rc < 0) {
		z_drop(z_move(callback));
		z_drop(z_move(*session));
		return rc;
	}

	return 0;
}

static int sitl_zenoh_put(const z_loaned_session_t *session, const char *keyexpr,
			  const uint8_t *payload, size_t payload_len)
{
	z_owned_bytes_t bytes;
	z_view_keyexpr_t view;
	int rc;

	z_internal_null(&bytes);

	rc = z_view_keyexpr_from_str(&view, keyexpr);
	if (rc < 0) {
		return rc;
	}

	rc = z_bytes_copy_from_buf(&bytes, payload, payload_len);
	if (rc < 0) {
		return rc;
	}

	rc = z_put(session, z_loan(view), z_move(bytes), NULL);

	return rc;
}

static void sitl_zenoh_send_flight_state_if_updated(const z_loaned_session_t *session)
{
	static uint32_t last_generation;
	uint8_t buf[RDD2_TOPIC_FB_FLIGHT_STATE_SIZE];
	size_t len;
	int rc;

	if (!rdd2_sitl_flight_state_blob_if_updated(&last_generation, buf, sizeof(buf), &len)) {
		return;
	}

	rc = sitl_zenoh_put(session, CONFIG_RDD2_SITL_ZENOH_FLIGHT_STATE_KEYEXPR, buf, len);
	if (rc < 0) {
		LOG_WRN("sitl zenoh flight_state put failed: %d", rc);
	}
}

static void sitl_zenoh_send_motor_output_if_updated(const z_loaned_session_t *session)
{
	static uint32_t last_generation;
	uint8_t buf[RDD2_TOPIC_FB_MOTOR_OUTPUT_SIZE];
	size_t len;
	int rc;

	if (!rdd2_sitl_motor_output_blob_if_updated(&last_generation, buf, sizeof(buf), &len)) {
		return;
	}

	rc = sitl_zenoh_put(session, CONFIG_RDD2_SITL_ZENOH_MOTOR_OUTPUT_KEYEXPR, buf, len);
	if (rc < 0) {
		LOG_WRN("sitl zenoh motor_output put failed: %d", rc);
	}
}

static void sitl_zenoh_rx_drain(void)
{
	static unsigned int last_generation;
	uint8_t buf[RDD2_SITL_INPUT_MAX_SIZE];
	size_t len;

	while (sitl_zenoh_rx_store_get_if_updated(&last_generation, buf, sizeof(buf), &len)) {
		(void)rdd2_sitl_handle_input_blob(buf, len);
	}
}

static void sitl_zenoh_thread(void *arg0, void *arg1, void *arg2)
{
	ARG_UNUSED(arg0);
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);

	while (true) {
		z_owned_session_t session;
		int rc;

		rc = sitl_zenoh_open(&session);
		if (rc < 0) {
			LOG_WRN("sitl zenoh open failed: %d", rc);
			k_sleep(K_MSEC(CONFIG_RDD2_SITL_ZENOH_RETRY_MS));
			continue;
		}

		LOG_INF("sitl zenoh %s %s input=%s flight=%s motor=%s",
			CONFIG_RDD2_SITL_ZENOH_MODE, CONFIG_RDD2_SITL_ZENOH_LOCATOR,
			CONFIG_RDD2_SITL_ZENOH_INPUT_KEYEXPR,
			CONFIG_RDD2_SITL_ZENOH_FLIGHT_STATE_KEYEXPR,
			CONFIG_RDD2_SITL_ZENOH_MOTOR_OUTPUT_KEYEXPR);

		while (!z_session_is_closed(z_loan(session))) {
			sitl_zenoh_rx_drain();
			sitl_zenoh_send_flight_state_if_updated(z_loan(session));
			sitl_zenoh_send_motor_output_if_updated(z_loan(session));
			k_sleep(K_MSEC(1));
		}

		LOG_WRN("sitl zenoh session closed, retrying");
		z_drop(z_move(session));
		k_sleep(K_MSEC(CONFIG_RDD2_SITL_ZENOH_RETRY_MS));
	}
}

static int rdd2_sitl_zenoh_init(void)
{
	k_thread_create(&g_sitl_zenoh_thread, g_sitl_zenoh_thread_stack,
			K_THREAD_STACK_SIZEOF(g_sitl_zenoh_thread_stack), sitl_zenoh_thread,
			NULL, NULL, NULL, CONFIG_RDD2_SITL_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&g_sitl_zenoh_thread, "rdd2_sitl_zenoh");

	return 0;
}

SYS_INIT(rdd2_sitl_zenoh_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
