#ifndef RDD2_LOCKSTEP_TRANSPORT_H_
#define RDD2_LOCKSTEP_TRANSPORT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "interfaces/data.h"

#include <zephyr/kernel.h>

#include <synapse/control_reader.h>
#include <synapse/sensors_reader.h>
#include <synapse/state_reader.h>

#define RDD2_LOCKSTEP_INPUT_MAX_SIZE 56U

bool rdd2_lockstep_latest_input_get(uint8_t *buf, size_t buf_size, size_t *len,
                                    uint32_t *generation);
bool rdd2_lockstep_input_wait_next(uint32_t *last_generation,
                                   k_timeout_t timeout);
bool rdd2_lockstep_handle_input_blob(const uint8_t *buf, size_t len);
bool rdd2_lockstep_handle_manual_control(
    const synapse_topic_ManualControlData_t *manual);
int rdd2_lockstep_gps_mission_init(void);
bool rdd2_lockstep_handle_gps_mission(const synapse_topic_GnssFixData_t *fix,
                                      const rdd2_waypoint_plan_t *plan,
                                      uint64_t control_now_ns);
void rdd2_lockstep_gps_mission_status_get(
    struct rdd2_lockstep_gps_mission_status *status);
bool rdd2_lockstep_flight_state_blob_if_updated(uint32_t *last_generation,
                                                uint8_t *buf, size_t buf_size,
                                                size_t *len);
bool rdd2_lockstep_motor_output_blob_if_updated(uint32_t *last_generation,
                                                uint8_t *buf, size_t buf_size,
                                                size_t *len);

#endif
