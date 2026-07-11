/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_LOCKSTEP_INPUT_H_
#define RDD2_LOCKSTEP_INPUT_H_

#include "synapse_messages.h"

#include <stddef.h>
#include <stdint.h>

bool rdd2_lockstep_decode_inertial(const uint8_t *buf, size_t len, rdd2_vec3f_t *gyro,
				   rdd2_vec3f_t *accel, uint64_t *sample_time_ns);

#endif
