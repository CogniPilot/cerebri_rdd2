/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gnss_m10_protocol.h"

#include <errno.h>
#include <string.h>

#include <zephyr/modem/ubx/keys.h>

#define M10_CONFIG_STEP_COUNT 1U
#define UBX_CFG_LAYER_RAM 1U
#define M10_UART_STOP_BITS_ONE 1U
#define M10_UART_DATA_BITS_EIGHT 0U
#define M10_UART_PARITY_NONE 0U

BUILD_ASSERT(RDD2_GNSS_M10_ACK_TIMEOUT_MS > 1000U,
             "M10 ACK timeout needs the documented 1 s receiver bound plus "
             "scheduling margin");

static void checksum_bytes(const uint8_t *data, size_t length, uint8_t *ck_a,
                           uint8_t *ck_b) {
  for (size_t i = 0U; i < length; i++) {
    *ck_a = (uint8_t)(*ck_a + data[i]);
    *ck_b = (uint8_t)(*ck_b + *ck_a);
  }
}

void rdd2_gnss_m10_parser_init(struct rdd2_gnss_m10_parser *parser) {
  memset(parser, 0, sizeof(*parser));
  parser->state = RDD2_GNSS_M10_RX_SYNC1;
}

static enum rdd2_gnss_m10_rx_event
parser_complete(struct rdd2_gnss_m10_parser *parser,
                struct rdd2_gnss_m10_frame *frame) {
  uint8_t ck_a = 0U;
  uint8_t ck_b = 0U;

  checksum_bytes(parser->header, sizeof(parser->header), &ck_a, &ck_b);
  checksum_bytes(parser->payload, parser->payload_length, &ck_a, &ck_b);
  if (ck_a != parser->checksum[0] || ck_b != parser->checksum[1]) {
    return RDD2_GNSS_M10_RX_CHECKSUM_ERROR;
  }

  frame->class_id = parser->header[0];
  frame->message_id = parser->header[1];
  frame->payload_length = parser->payload_length;
  frame->payload = parser->payload;
  return RDD2_GNSS_M10_RX_FRAME;
}

enum rdd2_gnss_m10_rx_event
rdd2_gnss_m10_parser_feed(struct rdd2_gnss_m10_parser *parser, uint8_t byte,
                          struct rdd2_gnss_m10_frame *frame) {
  enum rdd2_gnss_m10_rx_event event = RDD2_GNSS_M10_RX_NONE;

  switch (parser->state) {
  case RDD2_GNSS_M10_RX_SYNC1:
    if (byte == UBX_PREAMBLE_SYNC_CHAR_1) {
      parser->state = RDD2_GNSS_M10_RX_SYNC2;
    }
    break;
  case RDD2_GNSS_M10_RX_SYNC2:
    if (byte == UBX_PREAMBLE_SYNC_CHAR_2) {
      parser->state = RDD2_GNSS_M10_RX_HEADER;
      parser->position = 0U;
    } else if (byte != UBX_PREAMBLE_SYNC_CHAR_1) {
      parser->state = RDD2_GNSS_M10_RX_SYNC1;
    }
    break;
  case RDD2_GNSS_M10_RX_HEADER:
    parser->header[parser->position++] = byte;
    if (parser->position < sizeof(parser->header)) {
      break;
    }
    parser->payload_length =
        (uint16_t)parser->header[2] | ((uint16_t)parser->header[3] << 8);
    if (parser->payload_length > sizeof(parser->payload)) {
      /* Consume the complete announced payload and checksum before looking
       * for sync again. Otherwise a syntactically valid ACK embedded in an
       * oversized frame could be accepted as a configuration response. The
       * 16-bit UBX length bounds this skip without allocating the payload. */
      parser->skip_remaining = (uint32_t)parser->payload_length + 2U;
      parser->state = RDD2_GNSS_M10_RX_SKIP;
      parser->position = 0U;
      event = RDD2_GNSS_M10_RX_OVERSIZE;
      break;
    }
    parser->position = 0U;
    parser->state = parser->payload_length == 0U ? RDD2_GNSS_M10_RX_CHECKSUM
                                                 : RDD2_GNSS_M10_RX_PAYLOAD;
    break;
  case RDD2_GNSS_M10_RX_PAYLOAD:
    parser->payload[parser->position++] = byte;
    if (parser->position >= parser->payload_length) {
      parser->position = 0U;
      parser->state = RDD2_GNSS_M10_RX_CHECKSUM;
    }
    break;
  case RDD2_GNSS_M10_RX_CHECKSUM:
  default:
    parser->checksum[parser->position++] = byte;
    if (parser->position < sizeof(parser->checksum)) {
      break;
    }
    event = parser_complete(parser, frame);
    parser->state = RDD2_GNSS_M10_RX_SYNC1;
    parser->position = 0U;
    break;
  case RDD2_GNSS_M10_RX_SKIP:
    if (--parser->skip_remaining == 0U) {
      parser->state = RDD2_GNSS_M10_RX_SYNC1;
      parser->position = 0U;
    }
    break;
  }

  return event;
}

static bool append_u8(uint8_t *payload, size_t size, size_t *position,
                      uint32_t key, uint8_t value) {
  if (*position + sizeof(key) + sizeof(value) > size) {
    return false;
  }
  payload[(*position)++] = (uint8_t)key;
  payload[(*position)++] = (uint8_t)(key >> 8);
  payload[(*position)++] = (uint8_t)(key >> 16);
  payload[(*position)++] = (uint8_t)(key >> 24);
  payload[(*position)++] = value;
  return true;
}

static bool append_u16(uint8_t *payload, size_t size, size_t *position,
                       uint32_t key, uint16_t value) {
  if (*position + sizeof(key) + sizeof(value) > size) {
    return false;
  }
  payload[(*position)++] = (uint8_t)key;
  payload[(*position)++] = (uint8_t)(key >> 8);
  payload[(*position)++] = (uint8_t)(key >> 16);
  payload[(*position)++] = (uint8_t)(key >> 24);
  payload[(*position)++] = (uint8_t)value;
  payload[(*position)++] = (uint8_t)(value >> 8);
  return true;
}

static bool append_u32(uint8_t *payload, size_t size, size_t *position,
                       uint32_t key, uint32_t value) {
  if (*position + sizeof(key) + sizeof(value) > size) {
    return false;
  }
  payload[(*position)++] = (uint8_t)key;
  payload[(*position)++] = (uint8_t)(key >> 8);
  payload[(*position)++] = (uint8_t)(key >> 16);
  payload[(*position)++] = (uint8_t)(key >> 24);
  payload[(*position)++] = (uint8_t)value;
  payload[(*position)++] = (uint8_t)(value >> 8);
  payload[(*position)++] = (uint8_t)(value >> 16);
  payload[(*position)++] = (uint8_t)(value >> 24);
  return true;
}

static int encode_frame(uint8_t class_id, uint8_t message_id,
                        const uint8_t *payload, size_t payload_length,
                        uint8_t *frame, size_t frame_size) {
  uint8_t ck_a = 0U;
  uint8_t ck_b = 0U;
  size_t length = payload_length + UBX_FRAME_SZ_WITHOUT_PAYLOAD;

  if (payload_length > UINT16_MAX || frame_size < length) {
    return -ENOSPC;
  }
  frame[0] = UBX_PREAMBLE_SYNC_CHAR_1;
  frame[1] = UBX_PREAMBLE_SYNC_CHAR_2;
  frame[2] = class_id;
  frame[3] = message_id;
  frame[4] = (uint8_t)payload_length;
  frame[5] = (uint8_t)(payload_length >> 8);
  memcpy(&frame[UBX_FRAME_HEADER_SZ], payload, payload_length);
  checksum_bytes(&frame[UBX_FRAME_MSG_CLASS_IDX], 4U + payload_length, &ck_a,
                 &ck_b);
  frame[length - 2U] = ck_a;
  frame[length - 1U] = ck_b;
  return (int)length;
}

static bool build_uart_config(uint8_t *payload, size_t size, size_t *position) {
  return append_u8(payload, size, position, UBX_KEY_CFG_UART1_ENABLED, 1U) &&
         append_u32(payload, size, position, UBX_KEY_CFG_UART1_BAUDRATE,
                    115200U) &&
         append_u8(payload, size, position, UBX_KEY_CFG_UART1_STOPBITS,
                   M10_UART_STOP_BITS_ONE) &&
         append_u8(payload, size, position, UBX_KEY_CFG_UART1_DATABITS,
                   M10_UART_DATA_BITS_EIGHT) &&
         append_u8(payload, size, position, UBX_KEY_CFG_UART1_PARITY,
                   M10_UART_PARITY_NONE) &&
         append_u8(payload, size, position, UBX_KEY_UART1_PROTO_IN_UBX, 1U) &&
         append_u8(payload, size, position, UBX_KEY_UART1_PROTO_IN_NMEA, 0U) &&
         append_u8(payload, size, position, UBX_KEY_UART1_PROTO_OUT_UBX, 1U) &&
         append_u8(payload, size, position, UBX_KEY_UART1_PROTO_OUT_NMEA, 0U);
}

static bool build_message_config(uint8_t *payload, size_t size,
                                 size_t *position) {
  static const uint32_t nmea_keys[] = {
      UBX_KEY_MSG_OUT_NMEA_GGA_UART1, UBX_KEY_MSG_OUT_NMEA_RMC_UART1,
      UBX_KEY_MSG_OUT_NMEA_GSV_UART1, UBX_KEY_MSG_OUT_NMEA_DTM_UART1,
      UBX_KEY_MSG_OUT_NMEA_GBS_UART1, UBX_KEY_MSG_OUT_NMEA_GLL_UART1,
      UBX_KEY_MSG_OUT_NMEA_GNS_UART1, UBX_KEY_MSG_OUT_NMEA_GRS_UART1,
      UBX_KEY_MSG_OUT_NMEA_GSA_UART1, UBX_KEY_MSG_OUT_NMEA_GST_UART1,
      UBX_KEY_MSG_OUT_NMEA_VTG_UART1, UBX_KEY_MSG_OUT_NMEA_VLW_UART1,
      UBX_KEY_MSG_OUT_NMEA_ZDA_UART1,
  };

  if (!append_u8(payload, size, position, UBX_KEY_MSG_OUT_UBX_NAV_PVT_UART1,
                 1U)) {
    return false;
  }
  for (size_t i = 0U; i < sizeof(nmea_keys) / sizeof(nmea_keys[0]); i++) {
    if (!append_u8(payload, size, position, nmea_keys[i], 0U)) {
      return false;
    }
  }
  return true;
}

static bool build_rate_config(uint8_t *payload, size_t size, size_t *position) {
  return append_u16(payload, size, position, UBX_KEY_RATE_MEAS,
                    RDD2_GNSS_M10_TARGET_PERIOD_MS) &&
         append_u16(payload, size, position, UBX_KEY_RATE_NAV, 1U) &&
         append_u8(payload, size, position, UBX_KEY_NAV_CFG_FIX_MODE,
                   UBX_FIX_MODE_AUTO) &&
         append_u8(payload, size, position, UBX_KEY_NAV_CFG_DYN_MODEL,
                   UBX_DYN_MODEL_AIRBORNE_2G);
}

static int build_config_step(uint8_t step, uint8_t *frame, size_t frame_size) {
  uint8_t payload[RDD2_GNSS_M10_MAX_PAYLOAD] = {
      UBX_CFG_VAL_VER_SIMPLE,
      UBX_CFG_LAYER_RAM,
      0U,
      0U,
  };
  size_t position = sizeof(struct ubx_cfg_val_hdr);
  bool complete;

  if (step != 0U) {
    return -EINVAL;
  }
  /* One VALSET is deliberate: ACK-ACK identifies only CFG-VALSET, not
   * individual key sets. Keeping the complete configuration in one request
   * makes a delayed ACK after a timeout safe because every retry is
   * semantically identical. */
  complete = build_uart_config(payload, sizeof(payload), &position) &&
             build_message_config(payload, sizeof(payload), &position) &&
             build_rate_config(payload, sizeof(payload), &position);
  if (!complete) {
    return -ENOSPC;
  }
  return encode_frame(UBX_CLASS_ID_CFG, UBX_MSG_ID_CFG_VAL_SET, payload,
                      position, frame, frame_size);
}

void rdd2_gnss_m10_state_init(struct rdd2_gnss_m10_state *state) {
  memset(state, 0, sizeof(*state));
  state->config_status = RDD2_GNSS_M10_CONFIG_UNCONFIGURED;
  state->last_pvt_ms = -1;
}

static void config_attempt_failed(struct rdd2_gnss_m10_state *state) {
  if (state->attempts_this_step >= RDD2_GNSS_M10_CONFIG_ATTEMPTS) {
    state->config_status = RDD2_GNSS_M10_CONFIG_FAILED;
  } else {
    state->config_status = RDD2_GNSS_M10_CONFIG_UNCONFIGURED;
  }
  state->ready = false;
}

int rdd2_gnss_m10_prepare_config(struct rdd2_gnss_m10_state *state,
                                 int64_t now_ms, uint8_t *frame,
                                 size_t frame_size) {
  int length;

  if (state == NULL || frame == NULL) {
    return -EINVAL;
  }
  if (state->config_status != RDD2_GNSS_M10_CONFIG_UNCONFIGURED) {
    return 0;
  }
  if (state->config_step >= M10_CONFIG_STEP_COUNT) {
    state->config_status = RDD2_GNSS_M10_CONFIG_FAILED;
    return -EINVAL;
  }

  length = build_config_step(state->config_step, frame, frame_size);
  if (length < 0) {
    state->config_status = RDD2_GNSS_M10_CONFIG_FAILED;
    return length;
  }
  state->attempts_this_step++;
  state->ack_deadline_ms = now_ms + RDD2_GNSS_M10_ACK_TIMEOUT_MS;
  state->config_status = RDD2_GNSS_M10_CONFIG_WAITING_ACK;
  return length;
}

enum rdd2_gnss_m10_fix
rdd2_gnss_m10_fix_from_pvt(const struct ubx_nav_pvt *pvt) {
  if ((pvt->flags & UBX_NAV_PVT_FLAGS_GNSS_FIX_OK) == 0U ||
      (pvt->nav.flags3 & UBX_NAV_PVT_FLAGS3_INVALID_LLH) != 0U) {
    return RDD2_GNSS_M10_FIX_NONE;
  }
  if (pvt->fix_type == UBX_NAV_FIX_TYPE_3D) {
    if ((pvt->flags & UBX_NAV_PVT_FLAGS_GNSS_CARR_SOLN_FIXED) != 0U) {
      return RDD2_GNSS_M10_FIX_RTK_FIXED;
    }
    if ((pvt->flags & UBX_NAV_PVT_FLAGS_GNSS_CARR_SOLN_FLOATING) != 0U) {
      return RDD2_GNSS_M10_FIX_RTK_FLOAT;
    }
    if ((pvt->flags & RDD2_UBX_NAV_PVT_FLAGS_DIFF_SOLN) != 0U) {
      return RDD2_GNSS_M10_FIX_DGNSS;
    }
  }

  switch (pvt->fix_type) {
  case UBX_NAV_FIX_TYPE_DR:
  case UBX_NAV_FIX_TYPE_GNSS_DR_COMBINED:
    return RDD2_GNSS_M10_FIX_DEAD_RECKONING;
  case UBX_NAV_FIX_TYPE_2D:
    return RDD2_GNSS_M10_FIX_2D;
  case UBX_NAV_FIX_TYPE_3D:
    return RDD2_GNSS_M10_FIX_3D;
  case UBX_NAV_FIX_TYPE_TIME_ONLY:
    return RDD2_GNSS_M10_FIX_TIME_ONLY;
  case UBX_NAV_FIX_TYPE_NO_FIX:
  default:
    return RDD2_GNSS_M10_FIX_NONE;
  }
}

static bool fix_is_accepted(enum rdd2_gnss_m10_fix fix) {
  return fix == RDD2_GNSS_M10_FIX_3D || fix == RDD2_GNSS_M10_FIX_DGNSS ||
         fix == RDD2_GNSS_M10_FIX_RTK_FLOAT ||
         fix == RDD2_GNSS_M10_FIX_RTK_FIXED;
}

static bool readiness_gates_at(const struct rdd2_gnss_m10_state *state,
                               int64_t now_ms) {
  bool recent = state->last_pvt_ms >= 0 && now_ms >= state->last_pvt_ms &&
                now_ms - state->last_pvt_ms <= RDD2_GNSS_M10_RECENT_MS;

  return state->config_status == RDD2_GNSS_M10_CONFIG_CONFIGURED && recent &&
         state->stable_samples >= RDD2_GNSS_M10_STABLE_SAMPLES &&
         state->fix_accepted && state->accuracy_accepted;
}

bool rdd2_gnss_m10_ready_at(const struct rdd2_gnss_m10_state *state,
                            int64_t now_ms) {
  /* ready is also a publication barrier: the onboard boundary clears it
   * before publishing a newly rejected sample and only sets it after a
   * usable topic update succeeds. */
  return state->ready && readiness_gates_at(state, now_ms);
}

static void refresh_ready(struct rdd2_gnss_m10_state *state, int64_t now_ms) {
  state->ready = readiness_gates_at(state, now_ms);
}

static void note_pvt(struct rdd2_gnss_m10_state *state,
                     const struct ubx_nav_pvt *pvt, int64_t now_ms) {
  bool gap_accepted = state->last_pvt_ms < 0;

  if (state->last_pvt_ms >= 0 && now_ms >= state->last_pvt_ms) {
    uint64_t gap = (uint64_t)(now_ms - state->last_pvt_ms);

    state->last_gap_ms = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
    if (state->last_gap_ms > state->max_gap_ms) {
      state->max_gap_ms = state->last_gap_ms;
    }
    state->measured_rate_millihz =
        state->last_gap_ms == 0U ? 0U : 1000000U / state->last_gap_ms;
    if (state->last_gap_ms >= RDD2_GNSS_M10_MIN_GAP_MS &&
        state->last_gap_ms <= RDD2_GNSS_M10_MAX_GAP_MS) {
      gap_accepted = true;
    } else {
      state->rate_errors++;
    }
  } else {
    if (state->last_pvt_ms >= 0) {
      state->rate_errors++;
    }
    state->last_gap_ms = 0U;
    state->measured_rate_millihz = 0U;
  }

  state->last_pvt_ms = now_ms;
  state->horizontal_accuracy_mm = pvt->nav.horiz_acc;
  state->vertical_accuracy_mm = pvt->nav.vert_acc;
  state->velocity_accuracy_mm_s = pvt->nav.speed_acc;
  state->fix = rdd2_gnss_m10_fix_from_pvt(pvt);
  state->fix_accepted = fix_is_accepted(state->fix);
  /* Raw diagnostics preserve the receiver's estimates. The flight-facing
   * output requires positive estimates so a reset/default zero cannot read
   * as perfect accuracy. */
  state->accuracy_accepted = pvt->nav.horiz_acc > 0U &&
                             pvt->nav.vert_acc > 0U &&
                             pvt->nav.speed_acc > 0U &&
                             pvt->nav.horiz_acc <= RDD2_GNSS_M10_MAX_HACC_MM &&
                             pvt->nav.vert_acc <= RDD2_GNSS_M10_MAX_VACC_MM &&
                             pvt->nav.speed_acc <= RDD2_GNSS_M10_MAX_SACC_MM_S;
  if (gap_accepted && state->fix_accepted && state->accuracy_accepted) {
    if (state->stable_samples < UINT8_MAX) {
      state->stable_samples++;
    }
  } else {
    state->stable_samples = 0U;
  }
  refresh_ready(state, now_ms);
}

static enum rdd2_gnss_m10_frame_kind
handle_ack(struct rdd2_gnss_m10_state *state,
           const struct rdd2_gnss_m10_frame *frame) {
  const struct ubx_ack *ack;
  bool is_ack = frame->message_id == UBX_MSG_ID_ACK;

  if (frame->payload_length != sizeof(struct ubx_ack)) {
    state->bad_ack_length++;
    return RDD2_GNSS_M10_FRAME_BAD_LENGTH;
  }
  ack = (const struct ubx_ack *)frame->payload;
  if (state->config_status != RDD2_GNSS_M10_CONFIG_WAITING_ACK ||
      ack->class != UBX_CLASS_ID_CFG || ack->id != UBX_MSG_ID_CFG_VAL_SET) {
    state->unexpected_acks++;
    return is_ack ? RDD2_GNSS_M10_FRAME_ACK : RDD2_GNSS_M10_FRAME_NAK;
  }

  if (!is_ack) {
    state->config_naks++;
    config_attempt_failed(state);
    return RDD2_GNSS_M10_FRAME_NAK;
  }

  state->config_acks++;
  state->config_step++;
  state->attempts_this_step = 0U;
  if (state->config_step == M10_CONFIG_STEP_COUNT) {
    /* Only post-configuration samples qualify the configured stream. */
    state->stable_samples = 0U;
    state->last_pvt_ms = -1;
    state->last_gap_ms = 0U;
    state->measured_rate_millihz = 0U;
    state->fix = RDD2_GNSS_M10_FIX_NONE;
    state->fix_accepted = false;
    state->accuracy_accepted = false;
    state->ready = false;
    state->config_status = RDD2_GNSS_M10_CONFIG_CONFIGURED;
  } else {
    state->config_status = RDD2_GNSS_M10_CONFIG_UNCONFIGURED;
  }
  return RDD2_GNSS_M10_FRAME_ACK;
}

static void expire_ack_deadline(struct rdd2_gnss_m10_state *state,
                                int64_t now_ms) {
  if (state->config_status == RDD2_GNSS_M10_CONFIG_WAITING_ACK &&
      now_ms >= state->ack_deadline_ms) {
    state->config_timeouts++;
    config_attempt_failed(state);
  }
}

enum rdd2_gnss_m10_frame_kind
rdd2_gnss_m10_handle_frame(struct rdd2_gnss_m10_state *state,
                           const struct rdd2_gnss_m10_frame *frame,
                           int64_t now_ms) {
  if (frame->class_id == UBX_CLASS_ID_ACK &&
      (frame->message_id == UBX_MSG_ID_ACK ||
       frame->message_id == UBX_MSG_ID_NAK)) {
    /* At the exact deadline the attempt has timed out; an ACK decoded
     * afterward cannot revive it even if the periodic tick was delayed. */
    expire_ack_deadline(state, now_ms);
    return handle_ack(state, frame);
  }
  if (frame->class_id != UBX_CLASS_ID_NAV ||
      frame->message_id != UBX_MSG_ID_NAV_PVT) {
    return RDD2_GNSS_M10_FRAME_OTHER;
  }
  if (frame->payload_length != sizeof(struct ubx_nav_pvt)) {
    return RDD2_GNSS_M10_FRAME_BAD_LENGTH;
  }

  note_pvt(state, (const struct ubx_nav_pvt *)frame->payload, now_ms);
  return RDD2_GNSS_M10_FRAME_PVT;
}

void rdd2_gnss_m10_tick(struct rdd2_gnss_m10_state *state, int64_t now_ms) {
  expire_ack_deadline(state, now_ms);
  refresh_ready(state, now_ms);
}

enum rdd2_gnss_m10_fix
rdd2_gnss_m10_fix_for_output(const struct rdd2_gnss_m10_state *state) {
  return state->ready ? state->fix : RDD2_GNSS_M10_FIX_NONE;
}

void rdd2_gnss_m10_output_get(const struct rdd2_gnss_m10_state *state,
                              struct rdd2_gnss_m10_output *output) {
  output->fix = rdd2_gnss_m10_fix_for_output(state);
  output->usable = output->fix != RDD2_GNSS_M10_FIX_NONE;
  if (!output->usable) {
    output->horizontal_accuracy_mm = RDD2_GNSS_M10_ACCURACY_UNUSABLE;
    output->vertical_accuracy_mm = RDD2_GNSS_M10_ACCURACY_UNUSABLE;
    output->velocity_accuracy_mm_s = RDD2_GNSS_M10_ACCURACY_UNUSABLE;
    return;
  }

  output->horizontal_accuracy_mm = (uint16_t)state->horizontal_accuracy_mm;
  output->vertical_accuracy_mm = (uint16_t)state->vertical_accuracy_mm;
  output->velocity_accuracy_mm_s = (uint16_t)state->velocity_accuracy_mm_s;
}

int16_t rdd2_gnss_m10_velocity_up_cm_s(int32_t velocity_down_mm_s) {
  int64_t velocity_up_cm_s = -(int64_t)velocity_down_mm_s / 10;

  return (int16_t)CLAMP(velocity_up_cm_s, INT16_MIN, INT16_MAX);
}
