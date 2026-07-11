/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_CONTROL_TYPES_H_
#define RDD2_CONTROL_TYPES_H_

#include <stdbool.h>
#include <stdint.h>

#include <synapse/types_reader.h>

/*
 * Private controller working types. These never cross a transport boundary;
 * all external I/O uses the generated synapse_fbs 0.6 fixed-layout structs.
 * Keeping these types private lets the numerically sensitive controller evolve
 * independently without creating an alternate wire schema.
 */
typedef synapse_types_Vec3f_t rdd2_vec3f_t;
typedef synapse_types_RateTriplet_t rdd2_rate_triplet_t;

typedef struct {
	float roll;
	float pitch;
	float yaw;
} rdd2_attitude_euler_t;

typedef struct {
	int32_t ch0;
	int32_t ch1;
	int32_t ch2;
	int32_t ch3;
	int32_t ch4;
	int32_t ch5;
	int32_t ch6;
	int32_t ch7;
	int32_t ch8;
	int32_t ch9;
	int32_t ch10;
	int32_t ch11;
	int32_t ch12;
	int32_t ch13;
	int32_t ch14;
	int32_t ch15;
} rdd2_rc_channels_t;

typedef struct {
	int64_t rc_stamp_ms;
	int32_t throttle_us;
	uint8_t rc_link_quality;
	uint8_t flight_mode;
	bool armed;
	bool rc_valid;
	bool rc_stale;
	bool imu_ok;
	bool arm_switch;
} rdd2_control_status_t;

typedef struct {
	float m0;
	float m1;
	float m2;
	float m3;
} rdd2_motor_values_t;

typedef struct {
	uint16_t m0;
	uint16_t m1;
	uint16_t m2;
	uint16_t m3;
} rdd2_motor_raw_t;

#endif
