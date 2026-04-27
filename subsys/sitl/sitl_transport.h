#ifndef RDD2_SITL_TRANSPORT_H_
#define RDD2_SITL_TRANSPORT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#define RDD2_SITL_INPUT_MAX_SIZE 256U

bool rdd2_sitl_latest_input_get(uint8_t *buf, size_t buf_size, size_t *len,
				uint32_t *generation);
bool rdd2_sitl_input_wait_next(uint32_t *last_generation, k_timeout_t timeout);
bool rdd2_sitl_handle_input_blob(const uint8_t *buf, size_t len);
bool rdd2_sitl_flight_state_blob_if_updated(uint32_t *last_generation, uint8_t *buf,
					     size_t buf_size, size_t *len);
bool rdd2_sitl_motor_output_blob_if_updated(uint32_t *last_generation, uint8_t *buf,
					    size_t buf_size, size_t *len);

#endif
