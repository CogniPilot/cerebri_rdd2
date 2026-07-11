#ifndef RDD2_CONTROL_IO_H_
#define RDD2_CONTROL_IO_H_

#include "synapse_messages.h"

int rdd2_control_io_init(void);
void rdd2_control_input_wait(rdd2_vec3f_t *gyro, rdd2_vec3f_t *accel,
			     rdd2_rc_channels_t *rc,
			     rdd2_control_status_t *status, float *dt,
			     uint64_t *imu_interrupt_timestamp_ns);

#endif
