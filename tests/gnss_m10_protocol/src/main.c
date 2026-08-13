/* SPDX-License-Identifier: Apache-2.0 */

#include "gnss_m10_protocol.h"
#include "gnss_m10_topic.h"

#include <string.h>

#include <zephyr/modem/ubx/keys.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

static size_t encode_frame(uint8_t class_id, uint8_t message_id,
                           const void *payload, uint16_t payload_length,
                           uint8_t *frame, size_t frame_size) {
  uint8_t ck_a = 0U;
  uint8_t ck_b = 0U;
  size_t length = UBX_FRAME_SZ(payload_length);

  zassert_true(frame_size >= length);
  frame[0] = UBX_PREAMBLE_SYNC_CHAR_1;
  frame[1] = UBX_PREAMBLE_SYNC_CHAR_2;
  frame[2] = class_id;
  frame[3] = message_id;
  frame[4] = (uint8_t)payload_length;
  frame[5] = (uint8_t)(payload_length >> 8);
  memcpy(&frame[UBX_FRAME_HEADER_SZ], payload, payload_length);
  for (size_t i = 2U; i < 6U + payload_length; i++) {
    ck_a = (uint8_t)(ck_a + frame[i]);
    ck_b = (uint8_t)(ck_b + ck_a);
  }
  frame[length - 2U] = ck_a;
  frame[length - 1U] = ck_b;
  return length;
}

static enum rdd2_gnss_m10_frame_kind
inject_frame(struct rdd2_gnss_m10_parser *parser,
             struct rdd2_gnss_m10_state *state, const uint8_t *encoded,
             size_t length, int64_t now_ms) {
  struct rdd2_gnss_m10_frame frame;
  enum rdd2_gnss_m10_rx_event event = RDD2_GNSS_M10_RX_NONE;

  for (size_t i = 0U; i < length; i++) {
    event = rdd2_gnss_m10_parser_feed(parser, encoded[i], &frame);
    if (i + 1U < length) {
      zassert_equal(event, RDD2_GNSS_M10_RX_NONE);
    }
  }
  zassert_equal(event, RDD2_GNSS_M10_RX_FRAME);
  return rdd2_gnss_m10_handle_frame(state, &frame, now_ms);
}

static enum rdd2_gnss_m10_frame_kind
inject_ack(struct rdd2_gnss_m10_parser *parser,
           struct rdd2_gnss_m10_state *state, bool accepted, uint8_t ack_class,
           uint8_t ack_id, int64_t now_ms) {
  struct ubx_ack ack = {.class = ack_class, .id = ack_id};
  uint8_t frame[UBX_FRAME_SZ(sizeof(ack))];
  size_t length =
      encode_frame(UBX_CLASS_ID_ACK, accepted ? UBX_MSG_ID_ACK : UBX_MSG_ID_NAK,
                   &ack, sizeof(ack), frame, sizeof(frame));

  return inject_frame(parser, state, frame, length, now_ms);
}

static enum rdd2_gnss_m10_frame_kind
inject_pvt(struct rdd2_gnss_m10_parser *parser,
           struct rdd2_gnss_m10_state *state, const struct ubx_nav_pvt *pvt,
           int64_t now_ms) {
  uint8_t frame[UBX_FRAME_SZ(sizeof(*pvt))];
  size_t length = encode_frame(UBX_CLASS_ID_NAV, UBX_MSG_ID_NAV_PVT, pvt,
                               sizeof(*pvt), frame, sizeof(frame));

  return inject_frame(parser, state, frame, length, now_ms);
}

static uint32_t read_le32(const uint8_t *bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
         ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static bool find_key(const struct rdd2_gnss_m10_frame *frame, uint32_t wanted,
                     uint32_t *value) {
  size_t position = sizeof(struct ubx_cfg_val_hdr);

  while (position + sizeof(uint32_t) <= frame->payload_length) {
    uint32_t key = read_le32(&frame->payload[position]);
    uint8_t storage = (uint8_t)(key >> 28);
    size_t value_size;

    position += sizeof(uint32_t);
    if (storage == 1U || storage == 2U) {
      value_size = 1U;
    } else if (storage == 3U) {
      value_size = 2U;
    } else if (storage == 4U) {
      value_size = 4U;
    } else {
      return false;
    }
    if (position + value_size > frame->payload_length) {
      return false;
    }
    if (key == wanted) {
      *value = 0U;
      for (size_t i = 0U; i < value_size; i++) {
        *value |= (uint32_t)frame->payload[position + i] << (8U * i);
      }
      return true;
    }
    position += value_size;
  }
  return false;
}

static struct rdd2_gnss_m10_frame
parse_outbound(const uint8_t *encoded, size_t length,
               struct rdd2_gnss_m10_parser *parser) {
  struct rdd2_gnss_m10_frame frame = {0};
  enum rdd2_gnss_m10_rx_event event = RDD2_GNSS_M10_RX_NONE;

  rdd2_gnss_m10_parser_init(parser);
  for (size_t i = 0U; i < length; i++) {
    event = rdd2_gnss_m10_parser_feed(parser, encoded[i], &frame);
  }
  zassert_equal(event, RDD2_GNSS_M10_RX_FRAME);
  zassert_equal(frame.class_id, UBX_CLASS_ID_CFG);
  zassert_equal(frame.message_id, UBX_MSG_ID_CFG_VAL_SET);
  zassert_equal(frame.payload[0], UBX_CFG_VAL_VER_SIMPLE);
  zassert_equal(frame.payload[1], 1U);
  return frame;
}

static void assert_key(const struct rdd2_gnss_m10_frame *frame, uint32_t key,
                       uint32_t expected) {
  uint32_t actual = UINT32_MAX;

  zassert_true(find_key(frame, key, &actual), "missing key 0x%08x", key);
  zassert_equal(actual, expected, "key 0x%08x", key);
}

static void complete_configuration(struct rdd2_gnss_m10_state *state,
                                   struct rdd2_gnss_m10_parser *parser) {
  uint8_t request[RDD2_GNSS_M10_FRAME_MAX];

  zassert_true(
      rdd2_gnss_m10_prepare_config(state, 0, request, sizeof(request)) > 0);
  zassert_equal(inject_ack(parser, state, true, UBX_CLASS_ID_CFG,
                           UBX_MSG_ID_CFG_VAL_SET, 0),
                RDD2_GNSS_M10_FRAME_ACK);
  zassert_equal(state->config_status, RDD2_GNSS_M10_CONFIG_CONFIGURED);
}

static void assert_output_unusable(const struct rdd2_gnss_m10_state *state) {
  struct rdd2_gnss_m10_output output;

  rdd2_gnss_m10_output_get(state, &output);
  zassert_false(output.usable);
  zassert_equal(output.fix, RDD2_GNSS_M10_FIX_NONE);
  zassert_equal(output.horizontal_accuracy_mm, RDD2_GNSS_M10_ACCURACY_UNUSABLE);
  zassert_equal(output.vertical_accuracy_mm, RDD2_GNSS_M10_ACCURACY_UNUSABLE);
  zassert_equal(output.velocity_accuracy_mm_s, RDD2_GNSS_M10_ACCURACY_UNUSABLE);
}

ZTEST(gnss_m10_protocol, test_valset_sequence_is_exact_and_ack_gated) {
  static const uint32_t nmea_keys[] = {
      UBX_KEY_MSG_OUT_NMEA_GGA_UART1, UBX_KEY_MSG_OUT_NMEA_RMC_UART1,
      UBX_KEY_MSG_OUT_NMEA_GSV_UART1, UBX_KEY_MSG_OUT_NMEA_DTM_UART1,
      UBX_KEY_MSG_OUT_NMEA_GBS_UART1, UBX_KEY_MSG_OUT_NMEA_GLL_UART1,
      UBX_KEY_MSG_OUT_NMEA_GNS_UART1, UBX_KEY_MSG_OUT_NMEA_GRS_UART1,
      UBX_KEY_MSG_OUT_NMEA_GSA_UART1, UBX_KEY_MSG_OUT_NMEA_GST_UART1,
      UBX_KEY_MSG_OUT_NMEA_VTG_UART1, UBX_KEY_MSG_OUT_NMEA_VLW_UART1,
      UBX_KEY_MSG_OUT_NMEA_ZDA_UART1,
  };
  struct rdd2_gnss_m10_state state;
  struct rdd2_gnss_m10_parser rx_parser;
  struct rdd2_gnss_m10_parser inspect_parser;
  struct rdd2_gnss_m10_frame request_frame;
  uint8_t request[RDD2_GNSS_M10_FRAME_MAX];
  int length;

  rdd2_gnss_m10_state_init(&state);
  rdd2_gnss_m10_parser_init(&rx_parser);

  length = rdd2_gnss_m10_prepare_config(&state, 0, request, sizeof(request));
  zassert_true(length > 0);
  request_frame = parse_outbound(request, (size_t)length, &inspect_parser);
  zassert_equal(request_frame.payload_length, 144U);
  assert_key(&request_frame, UBX_KEY_CFG_UART1_ENABLED, 1U);
  assert_key(&request_frame, UBX_KEY_CFG_UART1_BAUDRATE, 115200U);
  assert_key(&request_frame, UBX_KEY_CFG_UART1_STOPBITS, 1U);
  assert_key(&request_frame, UBX_KEY_CFG_UART1_DATABITS, 0U);
  assert_key(&request_frame, UBX_KEY_CFG_UART1_PARITY, 0U);
  assert_key(&request_frame, UBX_KEY_UART1_PROTO_IN_UBX, 1U);
  assert_key(&request_frame, UBX_KEY_UART1_PROTO_IN_NMEA, 0U);
  {
    uint32_t unused;

    zassert_false(
        find_key(&request_frame, UBX_KEY_UART1_PROTO_IN_RTCM3X, &unused));
    zassert_false(
        find_key(&request_frame, UBX_KEY_UART1_PROTO_OUT_RTCM3X, &unused));
  }
  assert_key(&request_frame, UBX_KEY_UART1_PROTO_OUT_UBX, 1U);
  assert_key(&request_frame, UBX_KEY_UART1_PROTO_OUT_NMEA, 0U);
  assert_key(&request_frame, UBX_KEY_MSG_OUT_UBX_NAV_PVT_UART1, 1U);
  for (size_t i = 0U; i < ARRAY_SIZE(nmea_keys); i++) {
    assert_key(&request_frame, nmea_keys[i], 0U);
  }
  assert_key(&request_frame, UBX_KEY_RATE_MEAS, RDD2_GNSS_M10_TARGET_PERIOD_MS);
  assert_key(&request_frame, UBX_KEY_RATE_NAV, 1U);
  assert_key(&request_frame, UBX_KEY_NAV_CFG_FIX_MODE, UBX_FIX_MODE_AUTO);
  assert_key(&request_frame, UBX_KEY_NAV_CFG_DYN_MODEL,
             UBX_DYN_MODEL_AIRBORNE_2G);

  /* A valid ACK frame for a different command cannot advance the step. */
  zassert_equal(inject_ack(&rx_parser, &state, true, UBX_CLASS_ID_CFG,
                           UBX_MSG_ID_CFG_RATE, 1),
                RDD2_GNSS_M10_FRAME_ACK);
  zassert_equal(state.config_step, 0U);
  zassert_equal(state.unexpected_acks, 1U);
  zassert_equal(state.config_status, RDD2_GNSS_M10_CONFIG_WAITING_ACK);
  zassert_equal(inject_ack(&rx_parser, &state, true, UBX_CLASS_ID_CFG,
                           UBX_MSG_ID_CFG_VAL_SET, 2),
                RDD2_GNSS_M10_FRAME_ACK);
  zassert_equal(state.config_step, 1U);
  zassert_equal(state.config_status, RDD2_GNSS_M10_CONFIG_CONFIGURED);
  zassert_equal(state.config_acks, 1U);
}

ZTEST(gnss_m10_protocol,
      test_delayed_duplicate_ack_cannot_advance_another_step) {
  struct rdd2_gnss_m10_state state;
  struct rdd2_gnss_m10_parser parser;
  uint8_t first_request[RDD2_GNSS_M10_FRAME_MAX];
  uint8_t retry_request[RDD2_GNSS_M10_FRAME_MAX];
  int first_length;
  int retry_length;

  rdd2_gnss_m10_state_init(&state);
  rdd2_gnss_m10_parser_init(&parser);
  first_length = rdd2_gnss_m10_prepare_config(&state, 0, first_request,
                                              sizeof(first_request));
  zassert_true(first_length > 0);
  rdd2_gnss_m10_tick(&state, RDD2_GNSS_M10_ACK_TIMEOUT_MS);
  retry_length =
      rdd2_gnss_m10_prepare_config(&state, RDD2_GNSS_M10_ACK_TIMEOUT_MS,
                                   retry_request, sizeof(retry_request));
  zassert_equal(retry_length, first_length);
  zassert_mem_equal(retry_request, first_request, (size_t)first_length);

  /* Either request's ACK proves the same complete configuration. */
  zassert_equal(inject_ack(&parser, &state, true, UBX_CLASS_ID_CFG,
                           UBX_MSG_ID_CFG_VAL_SET,
                           RDD2_GNSS_M10_ACK_TIMEOUT_MS + 1U),
                RDD2_GNSS_M10_FRAME_ACK);
  zassert_equal(state.config_status, RDD2_GNSS_M10_CONFIG_CONFIGURED);
  zassert_equal(state.config_step, 1U);
  /* The other delayed ACK is diagnostic-only and cannot change state. */
  zassert_equal(inject_ack(&parser, &state, true, UBX_CLASS_ID_CFG,
                           UBX_MSG_ID_CFG_VAL_SET,
                           RDD2_GNSS_M10_ACK_TIMEOUT_MS + 2U),
                RDD2_GNSS_M10_FRAME_ACK);
  zassert_equal(state.config_status, RDD2_GNSS_M10_CONFIG_CONFIGURED);
  zassert_equal(state.config_step, 1U);
  zassert_equal(state.unexpected_acks, 1U);
}

ZTEST(gnss_m10_protocol, test_nak_retries_then_fails_closed) {
  struct rdd2_gnss_m10_state state;
  struct rdd2_gnss_m10_parser parser;
  uint8_t request[RDD2_GNSS_M10_FRAME_MAX];

  rdd2_gnss_m10_state_init(&state);
  rdd2_gnss_m10_parser_init(&parser);
  for (uint8_t attempt = 0U; attempt < RDD2_GNSS_M10_CONFIG_ATTEMPTS;
       attempt++) {
    zassert_true(rdd2_gnss_m10_prepare_config(&state, attempt, request,
                                              sizeof(request)) > 0);
    zassert_equal(inject_ack(&parser, &state, false, UBX_CLASS_ID_CFG,
                             UBX_MSG_ID_CFG_VAL_SET, attempt),
                  RDD2_GNSS_M10_FRAME_NAK);
  }
  zassert_equal(state.config_naks, RDD2_GNSS_M10_CONFIG_ATTEMPTS);
  zassert_equal(state.config_status, RDD2_GNSS_M10_CONFIG_FAILED);
  zassert_false(state.ready);
  zassert_equal(
      rdd2_gnss_m10_prepare_config(&state, 1000, request, sizeof(request)), 0);
}

ZTEST(gnss_m10_protocol, test_timeout_retries_then_fails_closed) {
  struct rdd2_gnss_m10_state state;
  uint8_t request[RDD2_GNSS_M10_FRAME_MAX];
  int64_t now_ms = 0;

  rdd2_gnss_m10_state_init(&state);
  zassert_true(RDD2_GNSS_M10_ACK_TIMEOUT_MS > 1000U);
  for (uint8_t attempt = 0U; attempt < RDD2_GNSS_M10_CONFIG_ATTEMPTS;
       attempt++) {
    zassert_true(rdd2_gnss_m10_prepare_config(&state, now_ms, request,
                                              sizeof(request)) > 0);
    rdd2_gnss_m10_tick(&state, now_ms + RDD2_GNSS_M10_ACK_TIMEOUT_MS - 1U);
    zassert_equal(state.config_status, RDD2_GNSS_M10_CONFIG_WAITING_ACK);
    now_ms += RDD2_GNSS_M10_ACK_TIMEOUT_MS;
    rdd2_gnss_m10_tick(&state, now_ms);
  }
  zassert_equal(state.config_timeouts, RDD2_GNSS_M10_CONFIG_ATTEMPTS);
  zassert_equal(state.config_status, RDD2_GNSS_M10_CONFIG_FAILED);
  zassert_false(state.ready);
}

ZTEST(gnss_m10_protocol, test_ack_deadline_boundary_is_fail_closed) {
  struct rdd2_gnss_m10_state state;
  struct rdd2_gnss_m10_parser parser;
  uint8_t request[RDD2_GNSS_M10_FRAME_MAX];

  rdd2_gnss_m10_state_init(&state);
  rdd2_gnss_m10_parser_init(&parser);
  zassert_true(
      rdd2_gnss_m10_prepare_config(&state, 0, request, sizeof(request)) > 0);
  zassert_equal(inject_ack(&parser, &state, true, UBX_CLASS_ID_CFG,
                           UBX_MSG_ID_CFG_VAL_SET,
                           RDD2_GNSS_M10_ACK_TIMEOUT_MS - 1U),
                RDD2_GNSS_M10_FRAME_ACK);
  zassert_equal(state.config_status, RDD2_GNSS_M10_CONFIG_CONFIGURED);

  rdd2_gnss_m10_state_init(&state);
  rdd2_gnss_m10_parser_init(&parser);
  zassert_true(
      rdd2_gnss_m10_prepare_config(&state, 0, request, sizeof(request)) > 0);
  zassert_equal(inject_ack(&parser, &state, true, UBX_CLASS_ID_CFG,
                           UBX_MSG_ID_CFG_VAL_SET,
                           RDD2_GNSS_M10_ACK_TIMEOUT_MS),
                RDD2_GNSS_M10_FRAME_ACK);
  zassert_equal(state.config_status, RDD2_GNSS_M10_CONFIG_UNCONFIGURED);
  zassert_equal(state.config_timeouts, 1U);
  zassert_equal(state.config_acks, 0U);
  zassert_equal(state.unexpected_acks, 1U);
}

ZTEST(gnss_m10_protocol, test_checksum_error_does_not_fake_ack) {
  struct rdd2_gnss_m10_state state;
  struct rdd2_gnss_m10_parser parser;
  struct rdd2_gnss_m10_frame parsed;
  struct ubx_ack ack = {.class = UBX_CLASS_ID_CFG,
                        .id = UBX_MSG_ID_CFG_VAL_SET};
  uint8_t request[RDD2_GNSS_M10_FRAME_MAX];
  uint8_t encoded[UBX_FRAME_SZ(sizeof(ack))];
  size_t length;
  enum rdd2_gnss_m10_rx_event event = RDD2_GNSS_M10_RX_NONE;

  rdd2_gnss_m10_state_init(&state);
  rdd2_gnss_m10_parser_init(&parser);
  zassert_true(
      rdd2_gnss_m10_prepare_config(&state, 0, request, sizeof(request)) > 0);
  length = encode_frame(UBX_CLASS_ID_ACK, UBX_MSG_ID_ACK, &ack, sizeof(ack),
                        encoded, sizeof(encoded));
  encoded[length - 1U] ^= 0x01U;
  for (size_t i = 0U; i < length; i++) {
    event = rdd2_gnss_m10_parser_feed(&parser, encoded[i], &parsed);
  }
  zassert_equal(event, RDD2_GNSS_M10_RX_CHECKSUM_ERROR);
  zassert_equal(state.config_step, 0U);
  zassert_equal(state.config_status, RDD2_GNSS_M10_CONFIG_WAITING_ACK);
}

static void feed_oversize_with_embedded_frame(
    struct rdd2_gnss_m10_parser *parser, uint16_t payload_length,
    const uint8_t *embedded, size_t embedded_length) {
  uint8_t header[] = {
      UBX_PREAMBLE_SYNC_CHAR_1, UBX_PREAMBLE_SYNC_CHAR_2,      0x99U, 0x01U,
      (uint8_t)payload_length,  (uint8_t)(payload_length >> 8)};
  size_t embedded_position = payload_length / 2U;
  struct rdd2_gnss_m10_frame frame;
  enum rdd2_gnss_m10_rx_event event;

  for (size_t i = 0U; i < ARRAY_SIZE(header); i++) {
    event = rdd2_gnss_m10_parser_feed(parser, header[i], &frame);
    zassert_equal(event, i + 1U == ARRAY_SIZE(header)
                             ? RDD2_GNSS_M10_RX_OVERSIZE
                             : RDD2_GNSS_M10_RX_NONE);
  }
  for (size_t i = 0U; i < payload_length; i++) {
    uint8_t byte = 0xA5U;

    if (i >= embedded_position && i - embedded_position < embedded_length) {
      byte = embedded[i - embedded_position];
    }
    zassert_equal(rdd2_gnss_m10_parser_feed(parser, byte, &frame),
                  RDD2_GNSS_M10_RX_NONE);
  }
  /* The skipped checksum is deliberately arbitrary: it must not be parsed. */
  zassert_equal(
      rdd2_gnss_m10_parser_feed(parser, UBX_PREAMBLE_SYNC_CHAR_1, &frame),
      RDD2_GNSS_M10_RX_NONE);
  zassert_equal(
      rdd2_gnss_m10_parser_feed(parser, UBX_PREAMBLE_SYNC_CHAR_2, &frame),
      RDD2_GNSS_M10_RX_NONE);
  zassert_equal(parser->state, RDD2_GNSS_M10_RX_SYNC1);
}

ZTEST(gnss_m10_protocol,
      test_oversize_payload_cannot_smuggle_configuration_ack) {
  struct rdd2_gnss_m10_state state;
  struct rdd2_gnss_m10_parser parser;
  struct ubx_ack ack = {.class = UBX_CLASS_ID_CFG,
                        .id = UBX_MSG_ID_CFG_VAL_SET};
  uint8_t request[RDD2_GNSS_M10_FRAME_MAX];
  uint8_t encoded_ack[UBX_FRAME_SZ(sizeof(ack))];
  size_t ack_length;

  rdd2_gnss_m10_state_init(&state);
  rdd2_gnss_m10_parser_init(&parser);
  zassert_true(
      rdd2_gnss_m10_prepare_config(&state, 0, request, sizeof(request)) > 0);
  ack_length = encode_frame(UBX_CLASS_ID_ACK, UBX_MSG_ID_ACK, &ack, sizeof(ack),
                            encoded_ack, sizeof(encoded_ack));

  /* Exercise the first byte above the buffer and a length above the M10's
   * documented 512-byte protocol payload. Both remain bounded skips. */
  feed_oversize_with_embedded_frame(&parser, RDD2_GNSS_M10_MAX_PAYLOAD + 1U,
                                    encoded_ack, ack_length);
  feed_oversize_with_embedded_frame(&parser, 513U, encoded_ack, ack_length);
  zassert_equal(state.config_status, RDD2_GNSS_M10_CONFIG_WAITING_ACK);
  zassert_equal(state.config_acks, 0U);

  /* Parsing resumes only after the full oversized frame and its checksum. */
  zassert_equal(inject_ack(&parser, &state, true, UBX_CLASS_ID_CFG,
                           UBX_MSG_ID_CFG_VAL_SET, 1),
                RDD2_GNSS_M10_FRAME_ACK);
  zassert_equal(state.config_status, RDD2_GNSS_M10_CONFIG_CONFIGURED);
}

ZTEST(gnss_m10_protocol, test_pvt_rate_fix_accuracy_and_age_gate_readiness) {
  struct rdd2_gnss_m10_state state;
  struct rdd2_gnss_m10_parser parser;
  struct ubx_nav_pvt pvt = {
      .fix_type = UBX_NAV_FIX_TYPE_3D,
      .flags = UBX_NAV_PVT_FLAGS_GNSS_FIX_OK,
      .nav =
          {
              .horiz_acc = 1000U,
              .vert_acc = 1500U,
              .speed_acc = 200U,
          },
  };

  rdd2_gnss_m10_state_init(&state);
  rdd2_gnss_m10_parser_init(&parser);
  complete_configuration(&state, &parser);

  for (int64_t now_ms = 100; now_ms <= 400; now_ms += 100) {
    zassert_equal(inject_pvt(&parser, &state, &pvt, now_ms),
                  RDD2_GNSS_M10_FRAME_PVT);
    zassert_false(state.ready);
    assert_output_unusable(&state);
  }
  zassert_equal(inject_pvt(&parser, &state, &pvt, 500),
                RDD2_GNSS_M10_FRAME_PVT);
  zassert_equal(state.measured_rate_millihz, 10000U);
  zassert_equal(state.last_gap_ms, 100U);
  zassert_equal(state.stable_samples, RDD2_GNSS_M10_STABLE_SAMPLES);
  zassert_equal(state.fix, RDD2_GNSS_M10_FIX_3D);
  zassert_true(state.ready);
  {
    struct rdd2_gnss_m10_output output;

    rdd2_gnss_m10_output_get(&state, &output);
    zassert_true(output.usable);
    zassert_equal(output.fix, RDD2_GNSS_M10_FIX_3D);
    zassert_equal(output.horizontal_accuracy_mm, 1000U);
    zassert_equal(output.vertical_accuracy_mm, 1500U);
    zassert_equal(output.velocity_accuracy_mm_s, 200U);
  }

  rdd2_gnss_m10_tick(&state, 500 + RDD2_GNSS_M10_RECENT_MS + 1U);
  zassert_false(state.ready);
  assert_output_unusable(&state);

  pvt.fix_type = UBX_NAV_FIX_TYPE_2D;
  zassert_equal(inject_pvt(&parser, &state, &pvt, 900),
                RDD2_GNSS_M10_FRAME_PVT);
  zassert_equal(state.fix, RDD2_GNSS_M10_FIX_2D);
  zassert_false(state.fix_accepted);
  zassert_false(state.ready);
  assert_output_unusable(&state);
  zassert_equal(state.stable_samples, 0U);
  zassert_equal(state.rate_errors, 1U);

  pvt.fix_type = UBX_NAV_FIX_TYPE_3D;
  pvt.flags |= RDD2_UBX_NAV_PVT_FLAGS_DIFF_SOLN;
  zassert_equal(inject_pvt(&parser, &state, &pvt, 1000),
                RDD2_GNSS_M10_FRAME_PVT);
  zassert_equal(state.fix, RDD2_GNSS_M10_FIX_DGNSS);
  zassert_true(state.fix_accepted);
  pvt.nav.horiz_acc = RDD2_GNSS_M10_MAX_HACC_MM + 1U;
  zassert_equal(inject_pvt(&parser, &state, &pvt, 1100),
                RDD2_GNSS_M10_FRAME_PVT);
  zassert_false(state.accuracy_accepted);
  zassert_false(state.ready);
  assert_output_unusable(&state);

  pvt.nav.horiz_acc = 1000U;
  pvt.nav.flags3 = UBX_NAV_PVT_FLAGS3_INVALID_LLH;
  zassert_equal(inject_pvt(&parser, &state, &pvt, 1200),
                RDD2_GNSS_M10_FRAME_PVT);
  zassert_equal(state.fix, RDD2_GNSS_M10_FIX_NONE);
  zassert_false(state.fix_accepted);
}

ZTEST(gnss_m10_protocol, test_five_hz_and_zero_accuracy_never_report_ready) {
  struct rdd2_gnss_m10_state state;
  struct rdd2_gnss_m10_parser parser;
  struct ubx_nav_pvt pvt = {
      .fix_type = UBX_NAV_FIX_TYPE_3D,
      .flags = UBX_NAV_PVT_FLAGS_GNSS_FIX_OK,
      .nav =
          {
              .horiz_acc = 1000U,
              .vert_acc = 1500U,
              .speed_acc = 200U,
          },
  };

  rdd2_gnss_m10_state_init(&state);
  rdd2_gnss_m10_parser_init(&parser);
  complete_configuration(&state, &parser);
  for (int64_t now_ms = 200; now_ms <= 1000; now_ms += 200) {
    zassert_equal(inject_pvt(&parser, &state, &pvt, now_ms),
                  RDD2_GNSS_M10_FRAME_PVT);
  }
  zassert_equal(state.measured_rate_millihz, 5000U);
  zassert_equal(state.stable_samples, 0U);
  zassert_false(state.ready);
  assert_output_unusable(&state);
  {
    synapse_topic_GnssFixData_t fix;

    rdd2_gnss_m10_topic_build(&pvt, &state, 1000, &fix);
    zassert_equal(fix.fix_type, synapse_types_GnssFixType_NoFix);
    zassert_equal(fix.horizontal_accuracy_mm, RDD2_GNSS_M10_ACCURACY_UNUSABLE);
  }

  /* Re-establish 10 Hz, but reset/default accuracy fields remain rejected. */
  for (int64_t now_ms = 1100; now_ms <= 1500; now_ms += 100) {
    zassert_equal(inject_pvt(&parser, &state, &pvt, now_ms),
                  RDD2_GNSS_M10_FRAME_PVT);
  }
  zassert_true(state.ready);
  zassert_equal(rdd2_gnss_m10_fix_for_output(&state), RDD2_GNSS_M10_FIX_3D);
  pvt.nav.speed_acc = 0U;
  zassert_equal(inject_pvt(&parser, &state, &pvt, 1600),
                RDD2_GNSS_M10_FRAME_PVT);
  zassert_false(state.accuracy_accepted);
  zassert_false(state.ready);
  zassert_equal(state.stable_samples, 0U);
  assert_output_unusable(&state);
  {
    synapse_topic_GnssFixData_t fix;

    rdd2_gnss_m10_topic_build(&pvt, &state, 1600, &fix);
    zassert_equal(fix.fix_type, synapse_types_GnssFixType_NoFix);
    zassert_equal(fix.velocity_accuracy_mm_s, RDD2_GNSS_M10_ACCURACY_UNUSABLE);
  }
}

ZTEST(gnss_m10_protocol, test_pre_ack_samples_never_qualify_post_ack_output) {
  struct rdd2_gnss_m10_state state;
  struct rdd2_gnss_m10_parser parser;
  struct ubx_nav_pvt pvt = {
      .fix_type = UBX_NAV_FIX_TYPE_3D,
      .flags = UBX_NAV_PVT_FLAGS_GNSS_FIX_OK,
      .nav =
          {
              .horiz_acc = 1000U,
              .vert_acc = 1500U,
              .speed_acc = 200U,
          },
  };
  uint8_t request[RDD2_GNSS_M10_FRAME_MAX];

  rdd2_gnss_m10_state_init(&state);
  rdd2_gnss_m10_parser_init(&parser);
  for (int64_t now_ms = 100; now_ms <= 500; now_ms += 100) {
    zassert_equal(inject_pvt(&parser, &state, &pvt, now_ms),
                  RDD2_GNSS_M10_FRAME_PVT);
    assert_output_unusable(&state);
  }
  zassert_equal(state.stable_samples, RDD2_GNSS_M10_STABLE_SAMPLES);
  zassert_true(
      rdd2_gnss_m10_prepare_config(&state, 501, request, sizeof(request)) > 0);
  zassert_equal(inject_ack(&parser, &state, true, UBX_CLASS_ID_CFG,
                           UBX_MSG_ID_CFG_VAL_SET, 502),
                RDD2_GNSS_M10_FRAME_ACK);
  zassert_equal(state.stable_samples, 0U);
  zassert_false(state.ready);
  assert_output_unusable(&state);
}

ZTEST(gnss_m10_protocol, test_rtk_requires_a_real_3d_fix) {
  struct ubx_nav_pvt pvt = {
      .fix_type = UBX_NAV_FIX_TYPE_2D,
      .flags = UBX_NAV_PVT_FLAGS_GNSS_FIX_OK |
               UBX_NAV_PVT_FLAGS_GNSS_CARR_SOLN_FIXED,
  };

  zassert_equal(rdd2_gnss_m10_fix_from_pvt(&pvt), RDD2_GNSS_M10_FIX_2D);
  pvt.fix_type = UBX_NAV_FIX_TYPE_3D;
  zassert_equal(rdd2_gnss_m10_fix_from_pvt(&pvt), RDD2_GNSS_M10_FIX_RTK_FIXED);
}

ZTEST(gnss_m10_protocol,
      test_topic_boundary_initial_stability_drop_and_self_aging) {
  struct rdd2_gnss_m10_publication_state publication = {0};
  struct rdd2_gnss_m10_state state;
  struct rdd2_gnss_m10_parser parser;
  struct ubx_nav_pvt pvt = {
      .fix_type = UBX_NAV_FIX_TYPE_3D,
      .flags = UBX_NAV_PVT_FLAGS_GNSS_FIX_OK,
      .nav =
          {
              .horiz_acc = 1000U,
              .vert_acc = 1500U,
              .speed_acc = 200U,
              .latitude = 400000000,
              .longitude = -860000000,
          },
  };
  synapse_topic_GnssFixData_t fix;
  bool usable;

  rdd2_gnss_m10_state_init(&state);
  rdd2_gnss_m10_parser_init(&parser);
  rdd2_gnss_m10_topic_invalidate(0, &fix);
  zassert_equal(fix.fix_type, synapse_types_GnssFixType_NoFix);
  zassert_equal(fix.horizontal_accuracy_mm, RDD2_GNSS_M10_ACCURACY_UNUSABLE);
  zassert_equal(fix.vertical_accuracy_mm, RDD2_GNSS_M10_ACCURACY_UNUSABLE);
  zassert_equal(fix.velocity_accuracy_mm_s, RDD2_GNSS_M10_ACCURACY_UNUSABLE);

  /* A good receiver fix before configuration still crosses as NoFix. */
  zassert_equal(inject_pvt(&parser, &state, &pvt, 50), RDD2_GNSS_M10_FRAME_PVT);
  rdd2_gnss_m10_topic_build(&pvt, &state, 50, &fix);
  zassert_equal(fix.fix_type, synapse_types_GnssFixType_NoFix);

  complete_configuration(&state, &parser);
  for (int64_t now_ms = 100; now_ms <= 400; now_ms += 100) {
    zassert_equal(inject_pvt(&parser, &state, &pvt, now_ms),
                  RDD2_GNSS_M10_FRAME_PVT);
    rdd2_gnss_m10_topic_build(&pvt, &state, now_ms, &fix);
    zassert_equal(fix.fix_type, synapse_types_GnssFixType_NoFix);
    usable = rdd2_gnss_m10_publication_barrier(&state, &fix);
    zassert_false(usable);
    rdd2_gnss_m10_publication_complete(&publication, &state, usable, true);
  }
  zassert_equal(inject_pvt(&parser, &state, &pvt, 500),
                RDD2_GNSS_M10_FRAME_PVT);
  rdd2_gnss_m10_topic_build(&pvt, &state, 500, &fix);
  zassert_equal(fix.fix_type, synapse_types_GnssFixType_Fix3d);
  zassert_equal(fix.latitude_deg_e7, pvt.nav.latitude);

  /* The concurrent gate closes before publication and opens only after a
   * successful usable update. */
  usable = rdd2_gnss_m10_publication_barrier(&state, &fix);
  zassert_true(usable);
  zassert_false(state.ready);
  zassert_false(rdd2_gnss_m10_ready_at(&state, 500));
  rdd2_gnss_m10_publication_complete(&publication, &state, usable, true);
  zassert_true(rdd2_gnss_m10_ready_at(&state, 500));
  zassert_true(rdd2_gnss_m10_ready_at(&state, 500 + RDD2_GNSS_M10_RECENT_MS));
  /* A higher-priority reader self-ages even if the GNSS thread is starved
   * and the cached ready bit has not yet been refreshed. */
  zassert_false(
      rdd2_gnss_m10_ready_at(&state, 500 + RDD2_GNSS_M10_RECENT_MS + 1U));

  pvt.fix_type = UBX_NAV_FIX_TYPE_2D;
  zassert_equal(inject_pvt(&parser, &state, &pvt, 600),
                RDD2_GNSS_M10_FRAME_PVT);
  rdd2_gnss_m10_topic_build(&pvt, &state, 600, &fix);
  zassert_equal(fix.fix_type, synapse_types_GnssFixType_NoFix);
  zassert_false(rdd2_gnss_m10_publication_barrier(&state, &fix));
  zassert_false(state.ready);
}

ZTEST(gnss_m10_protocol, test_age_invalidation_and_publication_failure_retry) {
  struct rdd2_gnss_m10_publication_state publication = {0};
  struct rdd2_gnss_m10_state state;
  synapse_topic_GnssFixData_t fix;

  rdd2_gnss_m10_state_init(&state);
  state.config_status = RDD2_GNSS_M10_CONFIG_CONFIGURED;
  state.stable_samples = RDD2_GNSS_M10_STABLE_SAMPLES;
  state.fix = RDD2_GNSS_M10_FIX_3D;
  state.fix_accepted = true;
  state.accuracy_accepted = true;
  state.last_pvt_ms = 1000;
  state.ready = true;

  rdd2_gnss_m10_tick(&state, 1000 + RDD2_GNSS_M10_RECENT_MS + 1U);
  zassert_false(state.ready);
  zassert_true(rdd2_gnss_m10_invalidation_due(&publication, true, &state));
  rdd2_gnss_m10_topic_invalidate(1301, &fix);
  zassert_equal(fix.fix_type, synapse_types_GnssFixType_NoFix);
  zassert_equal(fix.hdop_centi, RDD2_GNSS_M10_ACCURACY_UNUSABLE);

  /* A failed NoFix update remains pending across every scheduling pass. */
  rdd2_gnss_m10_publication_result(&publication, false);
  zassert_true(rdd2_gnss_m10_invalidation_due(&publication, false, &state));
  rdd2_gnss_m10_publication_result(&publication, false);
  zassert_true(publication.invalidation_pending);
  rdd2_gnss_m10_publication_result(&publication, true);
  zassert_false(rdd2_gnss_m10_invalidation_due(&publication, false, &state));
}

ZTEST(gnss_m10_protocol, test_velocity_up_conversion_handles_int32_extremes) {
  zassert_equal(rdd2_gnss_m10_velocity_up_cm_s(INT32_MIN), INT16_MAX);
  zassert_equal(rdd2_gnss_m10_velocity_up_cm_s(INT32_MAX), INT16_MIN);
  zassert_equal(rdd2_gnss_m10_velocity_up_cm_s(1234), -123);
  zassert_equal(rdd2_gnss_m10_velocity_up_cm_s(-1234), 123);
}

ZTEST(gnss_m10_protocol, test_utc_time_preserves_signed_nanoseconds) {
  struct rdd2_gnss_m10_state state = {
      .ready = true,
      .fix = RDD2_GNSS_M10_FIX_3D,
      .horizontal_accuracy_mm = 1000U,
      .vertical_accuracy_mm = 1500U,
      .velocity_accuracy_mm_s = 200U,
  };
  struct ubx_nav_pvt pvt = {
      .time =
          {
              .year = 1970U,
              .month = 1U,
              .day = 1U,
              .hour = 0U,
              .minute = 0U,
              .second = 1U,
              .valid = UBX_NAV_PVT_VALID_DATE | UBX_NAV_PVT_VALID_TIME,
              .nano = 123456789,
          },
      .fix_type = UBX_NAV_FIX_TYPE_3D,
      .flags = UBX_NAV_PVT_FLAGS_GNSS_FIX_OK,
  };
  synapse_topic_GnssFixData_t fix;

  rdd2_gnss_m10_topic_build(&pvt, &state, 1, &fix);
  zexpect_true((fix.flags & synapse_topic_GnssFixFlags_TimeValid) != 0U);
  zexpect_equal(fix.time_unix_ns, UINT64_C(1123456789));

  pvt.time.nano = -250000000;
  rdd2_gnss_m10_topic_build(&pvt, &state, 2, &fix);
  zexpect_true((fix.flags & synapse_topic_GnssFixFlags_TimeValid) != 0U);
  zexpect_equal(fix.time_unix_ns, UINT64_C(750000000));

  pvt.time.nano = 1000000000;
  rdd2_gnss_m10_topic_build(&pvt, &state, 3, &fix);
  zexpect_false((fix.flags & synapse_topic_GnssFixFlags_TimeValid) != 0U);
  zexpect_equal(fix.time_unix_ns, 0U);

  pvt.time.nano = 0;
  pvt.time.month = 2U;
  pvt.time.day = 30U;
  rdd2_gnss_m10_topic_build(&pvt, &state, 4, &fix);
  zexpect_false((fix.flags & synapse_topic_GnssFixFlags_TimeValid) != 0U);
}

ZTEST_SUITE(gnss_m10_protocol, NULL, NULL, NULL, NULL, NULL);
