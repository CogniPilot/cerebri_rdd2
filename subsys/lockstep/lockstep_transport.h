#ifndef RDD2_LOCKSTEP_TRANSPORT_H_
#define RDD2_LOCKSTEP_TRANSPORT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include <csyn/csyn_types.h>

#define RDD2_LOCKSTEP_INPUT_MAX_SIZE 56U

bool rdd2_lockstep_latest_input_get(uint8_t *buf, size_t buf_size, size_t *len,
				    uint32_t *generation);
bool rdd2_lockstep_input_wait_next(uint32_t *last_generation, k_timeout_t timeout);
bool rdd2_lockstep_handle_input_blob(const uint8_t *buf, size_t len);
bool rdd2_lockstep_handle_manual_control(const struct csyn_manual_control *manual);
bool rdd2_lockstep_flight_state_blob_if_updated(uint32_t *last_generation, uint8_t *buf,
						size_t buf_size, size_t *len);
bool rdd2_lockstep_motor_output_blob_if_updated(uint32_t *last_generation, uint8_t *buf,
						size_t buf_size, size_t *len);

#endif
