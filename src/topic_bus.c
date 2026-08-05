/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "topic_bus.h"

#include <csyn/csyn.h>

#include <string.h>

#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include <zros/private/zros_topic_struct.h>
#include <zros/zros_shell.h>

#include <synapse/topic_print.h>
#include <synapse/types_reader.h>

ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(rc, rdd2_rc_channels_t);

/* RDD2 owns its synapse_fbs 0.7 topic contract. CSyn resolves these compact
 * keys through the generated catalog and rejects mismatched payload sizes at
 * initialization. The lockstep transport remains direct shared memory; these
 * registrations support independent realtime Ethernet communication. */
CSYN_TOPIC_DEFINE(manual, "manual", CSYN_DIR_RX,
                  sizeof(synapse_topic_ManualControlData_t));
CSYN_TOPIC_DEFINE(imu, "imu", CSYN_DIR_RX,
                  sizeof(synapse_topic_InertialSampleData_t));
CSYN_TOPIC_DEFINE(pwm, "pwm", CSYN_DIR_TX,
                  sizeof(synapse_topic_PwmSignalOutputsData_t));
CSYN_TOPIC_DEFINE(health, "health", CSYN_DIR_TX,
                  sizeof(synapse_topic_VehicleHealthData_t));
CSYN_TOPIC_DEFINE(att, "att", CSYN_DIR_TX,
                  sizeof(synapse_topic_AttitudeEstimateData_t));
CSYN_TOPIC_DEFINE(att_sp, "att_sp", CSYN_DIR_TX,
                  sizeof(synapse_topic_AttitudeCommandData_t));
CSYN_TOPIC_DEFINE(loop, "loop", CSYN_DIR_TX,
                  sizeof(synapse_topic_ControlLoopMetricsData_t));
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(manual_control, struct csyn_manual_control);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(inertial_sample,
                                   synapse_topic_InertialSampleData_t);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(pwm_signal_outputs,
                                   synapse_topic_PwmSignalOutputsData_t);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(vehicle_health,
                                   synapse_topic_VehicleHealthData_t);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(attitude_estimate,
                                   synapse_topic_AttitudeEstimateData_t);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(attitude_command,
                                   synapse_topic_AttitudeCommandData_t);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(control_loop_metrics,
                                   synapse_topic_ControlLoopMetricsData_t);

#if defined(CONFIG_ZROS_SHELL)
static char g_topic_shell_field[128];
static char g_topic_shell_row[256];

#define TOPIC_SHELL_CELL_WIDTH 38U
#define TOPIC_SHELL_CELL_GAP   2U
#define TOPIC_SHELL_MAX_COLUMNS 3U

static uint16_t topic_synapse_id(const struct zros_topic *topic) {
  if (topic == &topic_inertial_sample) return synapse_topic_TopicId_InertialSample;
  if (topic == &topic_pwm_signal_outputs) return synapse_topic_TopicId_PwmSignalOutputs;
  if (topic == &topic_vehicle_health) return synapse_topic_TopicId_VehicleHealth;
  if (topic == &topic_attitude_estimate) return synapse_topic_TopicId_AttitudeEstimate;
  if (topic == &topic_attitude_command) return synapse_topic_TopicId_AttitudeCommand;
  if (topic == &topic_control_loop_metrics) return synapse_topic_TopicId_ControlLoopMetrics;
  return synapse_topic_TopicId_Unknown;
}

static void format_synapse_topic(const struct shell *sh,
                                 const struct zros_topic *topic,
                                 const void *msg, size_t msg_size) {
  uint16_t topic_id = topic_synapse_id(topic);
  const synapse_topic_fields_t *fields = synapse_topic_fields_by_id(topic_id);
  uint16_t terminal_width = sh->ctx->vt100_ctx.cons.terminal_wid;
  size_t columns;
  size_t row_columns = 0U;
  size_t row_len = 0U;

  if (topic_id == synapse_topic_TopicId_Unknown || fields == NULL ||
      fields->payload_size != msg_size) {
    shell_error(sh, "%s: unable to decode Synapse payload", topic->_name);
    return;
  }

  if (terminal_width == 0U) {
    terminal_width = CONFIG_SHELL_DEFAULT_TERMINAL_WIDTH;
  }
  columns = MAX(1U, terminal_width /
                            (TOPIC_SHELL_CELL_WIDTH + TOPIC_SHELL_CELL_GAP));
  columns = MIN(columns, TOPIC_SHELL_MAX_COLUMNS);

  shell_print(sh, "%s:", topic->_name);
  for (uint16_t i = 0; i < fields->field_count; ++i) {
    size_t field_len;

    if (synapse_field_snprint(g_topic_shell_field,
                              sizeof(g_topic_shell_field), &fields->fields[i],
                              msg) < 0) {
      shell_error(sh, "  %s: unable to decode field", fields->fields[i].name);
      continue;
    }

    field_len = strlen(g_topic_shell_field);
    if (field_len > TOPIC_SHELL_CELL_WIDTH) {
      if (row_columns > 0U) {
        shell_print(sh, "  %s", g_topic_shell_row);
        row_columns = 0U;
        row_len = 0U;
      }
      shell_print(sh, "  %s", g_topic_shell_field);
      continue;
    }

    row_len += (size_t)snprintk(
        &g_topic_shell_row[row_len], sizeof(g_topic_shell_row) - row_len,
        "%-*s%s", TOPIC_SHELL_CELL_WIDTH, g_topic_shell_field,
        row_columns + 1U < columns ? "  " : "");
    row_columns++;

    if (row_columns == columns) {
      shell_print(sh, "  %s", g_topic_shell_row);
      row_columns = 0U;
      row_len = 0U;
    }
  }

  if (row_columns > 0U) {
    shell_print(sh, "  %s", g_topic_shell_row);
  }
}

static void format_rc_channels(const struct shell *sh,
                               const struct zros_topic *topic,
                               const void *msg, size_t msg_size) {
  const rdd2_rc_channels_t *rc = msg;
  const int32_t *ch;

  ARG_UNUSED(topic);
  if (msg_size != sizeof(*rc)) {
    shell_error(sh, "invalid RC payload size: %u", (unsigned int)msg_size);
    return;
  }
  ch = rdd2_topic_rc_channels_data_const(rc);
  shell_print(sh,
              "ch1=%d ch2=%d ch3=%d ch4=%d ch5=%d ch6=%d ch7=%d ch8=%d "
              "ch9=%d ch10=%d ch11=%d ch12=%d ch13=%d ch14=%d ch15=%d ch16=%d",
              ch[0], ch[1], ch[2], ch[3], ch[4], ch[5], ch[6], ch[7],
              ch[8], ch[9], ch[10], ch[11], ch[12], ch[13], ch[14], ch[15]);
}

static void format_manual_control(const struct shell *sh,
                                  const struct zros_topic *topic,
                                  const void *msg, size_t msg_size) {
  const struct csyn_manual_control *manual = msg;

  ARG_UNUSED(topic);
  if (msg_size != sizeof(*manual)) {
    shell_error(sh, "invalid manual-control payload size: %u",
                (unsigned int)msg_size);
    return;
  }
  shell_print(sh, "valid=%s stamp_ms=%lld", manual->valid ? "true" : "false",
              (long long)manual->stamp_ms);
  format_rc_channels(sh, &topic_rc, &manual->rc, sizeof(manual->rc));
}

static struct zros_shell_topic_formatter g_topic_shell_formatters[] = {
    {.topic = &topic_rc, .format = format_rc_channels},
    {.topic = &topic_manual_control, .format = format_manual_control},
    {.topic = &topic_inertial_sample, .format = format_synapse_topic},
    {.topic = &topic_pwm_signal_outputs, .format = format_synapse_topic},
    {.topic = &topic_vehicle_health, .format = format_synapse_topic},
    {.topic = &topic_attitude_estimate, .format = format_synapse_topic},
    {.topic = &topic_attitude_command, .format = format_synapse_topic},
    {.topic = &topic_control_loop_metrics, .format = format_synapse_topic},
};
#endif

void rdd2_topic_shell_formatters_init(void) {
#if defined(CONFIG_ZROS_SHELL)
  for (size_t i = 0; i < ARRAY_SIZE(g_topic_shell_formatters); ++i) {
    (void)zros_shell_topic_formatter_register(&g_topic_shell_formatters[i]);
  }
#endif
}

uint32_t rdd2_topic_generation(const struct zros_topic *topic) {
  return (uint32_t)atomic_get((atomic_t *)&topic->_lockless_generation);
}

bool rdd2_topic_has_sample(const struct zros_topic *topic) {
  return rdd2_topic_generation(topic) != 0U;
}

bool rdd2_topic_copy_blob(const struct zros_topic *topic, uint8_t *buf,
                          size_t buf_size, size_t *len) {
  if (topic == NULL || buf == NULL || len == NULL) {
    return false;
  }

  if (!rdd2_topic_has_sample(topic) || (size_t)topic->_size > buf_size) {
    return false;
  }

  if (zros_topic_read((struct zros_topic *)topic, buf) != 0) {
    return false;
  }

  *len = (size_t)topic->_size;
  return true;
}

uint32_t rdd2_topic_flight_state_generation(void) {
  return rdd2_topic_generation(&topic_vehicle_health);
}

bool rdd2_topic_flight_state_copy_blob(uint8_t *buf, size_t buf_size,
                                       size_t *len) {
  rdd2_topic_flight_state_blob_t *state = (rdd2_topic_flight_state_blob_t *)buf;

  if (buf == NULL || len == NULL || buf_size < sizeof(*state) ||
      zros_topic_read(&topic_vehicle_health, &state->vehicle_health) != 0 ||
      zros_topic_read(&topic_attitude_estimate, &state->attitude_estimate) !=
          0 ||
      zros_topic_read(&topic_attitude_command, &state->attitude_command) != 0 ||
      zros_topic_read(&topic_control_loop_metrics,
                      &state->control_loop_metrics) != 0) {
    return false;
  }
  *len = sizeof(*state);
  return true;
}

uint32_t rdd2_topic_motor_output_generation(void) {
  return rdd2_topic_generation(&topic_pwm_signal_outputs);
}

bool rdd2_topic_motor_output_copy_blob(uint8_t *buf, size_t buf_size,
                                       size_t *len) {
  return rdd2_topic_copy_blob(&topic_pwm_signal_outputs, buf, buf_size, len);
}
