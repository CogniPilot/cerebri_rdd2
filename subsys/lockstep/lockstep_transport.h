#ifndef RDD2_LOCKSTEP_TRANSPORT_H_
#define RDD2_LOCKSTEP_TRANSPORT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "interfaces/data.h"
#include "lockstep_shared.h"

#include <zephyr/kernel.h>

#include <cerebri_lockstep/sequence.h>
#include <synapse/control_reader.h>
#include <synapse/sensors_reader.h>
#include <synapse/state_reader.h>

#define RDD2_LOCKSTEP_INPUT_MAX_SIZE 56U

/* Outcome of advancing the firmware across one plant macro-step. */
enum rdd2_lockstep_frame_result {
  RDD2_LOCKSTEP_FRAME_OK = 0,
  RDD2_LOCKSTEP_FRAME_TERMINATED,
  RDD2_LOCKSTEP_FRAME_INVALID,
};

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

/*
 * Advance the firmware across one plant macro-step.
 *
 * The host presents a single inertial sample per plant frame, timestamped at
 * the end of the frame. The firmware control loop runs at 800 Hz, so a frame
 * spans several controller ticks. This feeds the firmware one control-period
 * sample at a time, in ascending timestamp order up to the frame boundary in
 * *coordinator_boot_ns, and blocks on each tick's motor output. Blocking per
 * tick lets the lower-priority estimator, planner, and guidance threads run
 * once per controller tick rather than once per plant frame, so every process
 * releases at its configured divisor of the 800 Hz base. Per-frame inputs
 * (manual control and the GNSS/mission plan) are applied once at frame start.
 * On success the aggregated outputs are copied into *shared and the caller
 * should respond to the host.
 */
enum rdd2_lockstep_frame_result rdd2_lockstep_advance_frame(
    struct cerebri_lockstep_sequence *sequence,
    struct rdd2_lockstep_shared *shared, uint64_t *coordinator_boot_ns,
    uint32_t *flight_generation, uint32_t *motor_generation);

#endif
