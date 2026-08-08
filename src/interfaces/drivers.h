/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_INTERFACES_DRIVERS_H_
#define RDD2_INTERFACES_DRIVERS_H_

#include "data.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RDD2_RC_INPUT_EVENT_LINK_QUALITY 0x1000
#define RDD2_RC_INPUT_EVENT_VALID        0x1001
#define RDD2_MOTOR_IDLE_THROTTLE         0.0f

#define RDD2_FLIGHT_MODE_CHANNEL_INDEX   5U
#define RDD2_FLIGHT_MODE_ACRO_MAX_US     1333
#define RDD2_FLIGHT_MODE_POSITION_MIN_US 1667

int rdd2_imu_stream_init(void);
bool rdd2_imu_stream_wait_next(rdd2_vec3f_t *gyro, rdd2_vec3f_t *accel, float *dt,
			       uint64_t *interrupt_timestamp_ns);
bool rdd2_imu_stream_lockstep_at_target(void);

int rdd2_rc_input_init(void);
void rdd2_rc_input_latest_get(rdd2_rc_channels_t *rc, int64_t *stamp_ms, bool *valid);
uint8_t rdd2_rc_input_link_quality_get(void);
uint8_t rdd2_rc_flight_mode(const rdd2_rc_channels_t *rc);

int rdd2_motor_output_init(void);
bool rdd2_motor_output_ready(void);
uint64_t rdd2_motor_output_write_all(const rdd2_motor_values_t *motors, bool armed, bool test_mode);
uint64_t rdd2_motor_output_write_all_raw(const rdd2_motor_raw_t *raw, bool test_mode);
bool rdd2_motor_test_get(rdd2_motor_values_t *motors);
void rdd2_motor_test_set(size_t index, float value);
void rdd2_motor_test_clear(void);
bool rdd2_motor_raw_test_get(rdd2_motor_raw_t *raw);
void rdd2_motor_raw_test_set(size_t index, uint16_t value);
void rdd2_motor_raw_test_set_all(uint16_t value);
void rdd2_motor_raw_test_clear(void);

#endif /* RDD2_INTERFACES_DRIVERS_H_ */
