/* SPDX-License-Identifier: Apache-2.0 */

#include "lockstep_input.h"

#include <string.h>

#include <zephyr/sys/util.h>

BUILD_ASSERT(sizeof(synapse_topic_InertialSampleData_t) == 56U);
BUILD_ASSERT(sizeof(synapse_topic_RadioControlData_t) == 48U);
BUILD_ASSERT(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);

bool rdd2_lockstep_decode_inertial(const uint8_t *buf, size_t len, rdd2_vec3f_t *gyro,
				   rdd2_vec3f_t *accel, uint64_t *sample_time_ns)
{
	const synapse_topic_InertialSampleData_t *sample;

	if (buf == NULL || len != sizeof(synapse_topic_InertialSampleData_t)) {
		return false;
	}
	/* Canonical fixed-layout topics are bare generated structs, matching
	 * csyn_decode_manual_control(). Generated accessors handle endianness. */
	sample = (const synapse_topic_InertialSampleData_t *)buf;
	if ((synapse_topic_InertialSampleData_flags(sample) &
	     (synapse_topic_InertialFieldFlags_Accel | synapse_topic_InertialFieldFlags_Gyro)) !=
	    (synapse_topic_InertialFieldFlags_Accel | synapse_topic_InertialFieldFlags_Gyro)) {
		return false;
	}
	if (gyro != NULL) {
		*gyro = *synapse_topic_InertialSampleData_gyro_flu_rad_s(sample);
	}
	if (accel != NULL) {
		*accel = *synapse_topic_InertialSampleData_accel_flu_m_s2(sample);
	}
	if (sample_time_ns != NULL) {
		*sample_time_ns = synapse_topic_InertialSampleData_timestamp_us(sample) * 1000U;
	}
	return true;
}
