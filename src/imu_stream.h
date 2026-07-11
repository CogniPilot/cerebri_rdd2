#ifndef RDD2_IMU_STREAM_H_
#define RDD2_IMU_STREAM_H_

#include "synapse_messages.h"

#include <stdbool.h>

int rdd2_imu_stream_init(void);
bool rdd2_imu_stream_wait_next(rdd2_vec3f_t *gyro, rdd2_vec3f_t *accel, float *dt,
			       uint64_t *interrupt_timestamp_ns);
bool rdd2_imu_stream_lockstep_at_target(void);

#endif
