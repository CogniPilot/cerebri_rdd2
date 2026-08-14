#ifndef RDD2_TOPIC_BUS_H_
#define RDD2_TOPIC_BUS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include <zros/zros_topic.h>

#include "data.h"

BUILD_ASSERT(sizeof(rdd2_topic_motor_output_blob_t) == 48U);

/*
 * RDD2's internal bus, declared here because zros_topics.c defines it.
 * CSyn's bridge references these weakly to mirror them onto Ethernet, and
 * csyn_zros.h declares them too for that purpose, but the declarations a
 * subsystem of this application compiles against are these: reading the bus
 * must not require a transport's header.
 */
ZROS_TOPIC_DECLARE(manual_input, synapse_topic_ManualControlData_t);
/* Driver-decoded IMU sample handed from the rate process to the estimator. */
ZROS_TOPIC_DECLARE(control_imu, synapse_topic_InertialSampleData_t);
ZROS_TOPIC_DECLARE(waypoint_plan, rdd2_waypoint_plan_t);
ZROS_TOPIC_DECLARE(trajectory_reference, synapse_topic_LocalPositionCommandData_t);
ZROS_TOPIC_DECLARE(navigation_odometry, synapse_topic_OdometryEstimateData_t);
ZROS_TOPIC_DECLARE(rate_command, synapse_topic_RateCommandData_t);
ZROS_TOPIC_DECLARE(vehicle_health, synapse_topic_VehicleHealthData_t);
ZROS_TOPIC_DECLARE(attitude_estimate, synapse_topic_AttitudeEstimateData_t);
ZROS_TOPIC_DECLARE(attitude_command, synapse_topic_AttitudeCommandData_t);
ZROS_TOPIC_DECLARE(control_loop_metrics, synapse_topic_ControlLoopMetricsData_t);
ZROS_TOPIC_DECLARE(pwm_signal_outputs, synapse_topic_PwmSignalOutputsData_t);
ZROS_TOPIC_DECLARE(inertial_sample, synapse_topic_InertialSampleData_t);
ZROS_TOPIC_DECLARE(external_odometry, synapse_topic_ExternalOdometryData_t);
ZROS_TOPIC_DECLARE(local_position_command, synapse_topic_LocalPositionCommandData_t);
/* The fix lives on the internal bus whatever produced it: the onboard reader
 * in subsys/gnss_source, the deterministic lockstep source, the serial
 * transport when a ground station injects it, or the mesh backend that
 * identity-copies an externally received fix off the CSyn "gnss" topic.
 * Exactly one of those is compiled in, so the single-publisher backend holds
 * and the producer owns the publisher. */
ZROS_TOPIC_DECLARE(gnss_fix, synapse_topic_GnssFixData_t);

uint32_t rdd2_topic_generation(const struct zros_topic *topic);
bool rdd2_topic_has_sample(const struct zros_topic *topic);
bool rdd2_topic_copy_blob(const struct zros_topic *topic, uint8_t *buf, size_t buf_size,
			  size_t *len);
uint32_t rdd2_topic_flight_state_generation(void);
bool rdd2_topic_flight_state_copy_blob(uint8_t *buf, size_t buf_size, size_t *len);
uint32_t rdd2_topic_motor_output_generation(void);
bool rdd2_topic_motor_output_copy_blob(uint8_t *buf, size_t buf_size, size_t *len);
/* Latest GNSS fix, whatever filled it. Returns false until a fix arrives. */
bool rdd2_topic_gnss_copy(synapse_topic_GnssFixData_t *fix, uint32_t *generation);
void rdd2_topic_shell_formatters_init(void);
#endif
