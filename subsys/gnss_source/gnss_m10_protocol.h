/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure UBX parser and u-blox M10 boot/readiness state machine.
 */

#ifndef RDD2_GNSS_M10_PROTOCOL_H_
#define RDD2_GNSS_M10_PROTOCOL_H_

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/toolchain.h>

#include <zephyr/modem/ubx/protocol.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_RDD2_GNSS_UBX_MAX_PAYLOAD)
#define RDD2_GNSS_M10_MAX_PAYLOAD CONFIG_RDD2_GNSS_UBX_MAX_PAYLOAD
#else
#define RDD2_GNSS_M10_MAX_PAYLOAD 160U
#endif
#define RDD2_GNSS_M10_FRAME_MAX UBX_FRAME_SZ(RDD2_GNSS_M10_MAX_PAYLOAD)
#define RDD2_GNSS_M10_TARGET_PERIOD_MS 100U
#define RDD2_GNSS_M10_TARGET_RATE_HZ 10U
#define RDD2_GNSS_M10_ACK_TIMEOUT_MS 1100U
#define RDD2_GNSS_M10_CONFIG_ATTEMPTS 3U
#define RDD2_GNSS_M10_STABLE_SAMPLES 5U
#define RDD2_GNSS_M10_MIN_GAP_MS 75U
#define RDD2_GNSS_M10_MAX_GAP_MS 125U
#define RDD2_GNSS_M10_RECENT_MS 300U
#define RDD2_GNSS_M10_MAX_HACC_MM 10000U
#define RDD2_GNSS_M10_MAX_VACC_MM 15000U
#define RDD2_GNSS_M10_MAX_SACC_MM_S 5000U
#define RDD2_GNSS_M10_ACCURACY_UNUSABLE UINT16_MAX

/* NAV-PVT flags.diffSoln is bit 1 in the u-blox interface contract. */
#define RDD2_UBX_NAV_PVT_FLAGS_DIFF_SOLN (1U << 1)

enum rdd2_gnss_m10_rx_state {
  RDD2_GNSS_M10_RX_SYNC1 = 0,
  RDD2_GNSS_M10_RX_SYNC2,
  RDD2_GNSS_M10_RX_HEADER,
  RDD2_GNSS_M10_RX_PAYLOAD,
  RDD2_GNSS_M10_RX_CHECKSUM,
  RDD2_GNSS_M10_RX_SKIP,
};

enum rdd2_gnss_m10_rx_event {
  RDD2_GNSS_M10_RX_NONE = 0,
  RDD2_GNSS_M10_RX_FRAME,
  RDD2_GNSS_M10_RX_CHECKSUM_ERROR,
  RDD2_GNSS_M10_RX_OVERSIZE,
};

struct rdd2_gnss_m10_frame {
  uint8_t class_id;
  uint8_t message_id;
  uint16_t payload_length;
  const uint8_t *payload;
};

struct rdd2_gnss_m10_parser {
  enum rdd2_gnss_m10_rx_state state;
  uint8_t header[4];
  uint8_t payload[RDD2_GNSS_M10_MAX_PAYLOAD];
  uint8_t checksum[2];
  uint16_t position;
  uint16_t payload_length;
  uint32_t skip_remaining;
};

enum rdd2_gnss_m10_config_status {
  RDD2_GNSS_M10_CONFIG_UNCONFIGURED = 0,
  RDD2_GNSS_M10_CONFIG_WAITING_ACK,
  RDD2_GNSS_M10_CONFIG_CONFIGURED,
  RDD2_GNSS_M10_CONFIG_FAILED,
};

enum rdd2_gnss_m10_fix {
  RDD2_GNSS_M10_FIX_NONE = 0,
  RDD2_GNSS_M10_FIX_DEAD_RECKONING,
  RDD2_GNSS_M10_FIX_2D,
  RDD2_GNSS_M10_FIX_3D,
  RDD2_GNSS_M10_FIX_DGNSS,
  RDD2_GNSS_M10_FIX_RTK_FLOAT,
  RDD2_GNSS_M10_FIX_RTK_FIXED,
  RDD2_GNSS_M10_FIX_TIME_ONLY,
};

enum rdd2_gnss_m10_frame_kind {
  RDD2_GNSS_M10_FRAME_OTHER = 0,
  RDD2_GNSS_M10_FRAME_ACK,
  RDD2_GNSS_M10_FRAME_NAK,
  RDD2_GNSS_M10_FRAME_PVT,
  RDD2_GNSS_M10_FRAME_BAD_LENGTH,
};

struct rdd2_gnss_m10_state {
  enum rdd2_gnss_m10_config_status config_status;
  uint8_t config_step;
  uint8_t attempts_this_step;
  uint8_t stable_samples;
  int64_t ack_deadline_ms;
  int64_t last_pvt_ms;
  uint32_t config_acks;
  uint32_t config_naks;
  uint32_t config_timeouts;
  uint32_t unexpected_acks;
  uint32_t bad_ack_length;
  uint32_t rate_errors;
  uint32_t last_gap_ms;
  uint32_t max_gap_ms;
  uint32_t measured_rate_millihz;
  uint32_t horizontal_accuracy_mm;
  uint32_t vertical_accuracy_mm;
  uint32_t velocity_accuracy_mm_s;
  enum rdd2_gnss_m10_fix fix;
  bool fix_accepted;
  bool accuracy_accepted;
  bool ready;
};

struct rdd2_gnss_m10_output {
  enum rdd2_gnss_m10_fix fix;
  uint16_t horizontal_accuracy_mm;
  uint16_t vertical_accuracy_mm;
  uint16_t velocity_accuracy_mm_s;
  bool usable;
};

void rdd2_gnss_m10_parser_init(struct rdd2_gnss_m10_parser *parser);

enum rdd2_gnss_m10_rx_event
rdd2_gnss_m10_parser_feed(struct rdd2_gnss_m10_parser *parser, uint8_t byte,
                          struct rdd2_gnss_m10_frame *frame);

void rdd2_gnss_m10_state_init(struct rdd2_gnss_m10_state *state);

/* Returns one encoded frame length, zero when no transmission is due, or a
 * negative errno on an invalid buffer/state. Only one request is outstanding.
 */
int rdd2_gnss_m10_prepare_config(struct rdd2_gnss_m10_state *state,
                                 int64_t now_ms, uint8_t *frame,
                                 size_t frame_size);

enum rdd2_gnss_m10_frame_kind
rdd2_gnss_m10_handle_frame(struct rdd2_gnss_m10_state *state,
                           const struct rdd2_gnss_m10_frame *frame,
                           int64_t now_ms);

/* Advances ACK timeout and fix-age readiness without performing any I/O. */
void rdd2_gnss_m10_tick(struct rdd2_gnss_m10_state *state, int64_t now_ms);

/* Computes readiness at the caller's time without trusting the cached age.
 * This is the flight-gating form used across scheduling-priority boundaries. */
bool rdd2_gnss_m10_ready_at(const struct rdd2_gnss_m10_state *state,
                            int64_t now_ms);

enum rdd2_gnss_m10_fix
rdd2_gnss_m10_fix_from_pvt(const struct ubx_nav_pvt *pvt);

/* The only fix classification allowed to cross the onboard topic boundary. */
enum rdd2_gnss_m10_fix
rdd2_gnss_m10_fix_for_output(const struct rdd2_gnss_m10_state *state);

void rdd2_gnss_m10_output_get(const struct rdd2_gnss_m10_state *state,
                              struct rdd2_gnss_m10_output *output);

/* NED down is positive; the flight topic uses up-positive centimetres/second.
 */
int16_t rdd2_gnss_m10_velocity_up_cm_s(int32_t velocity_down_mm_s);

#endif /* RDD2_GNSS_M10_PROTOCOL_H_ */
