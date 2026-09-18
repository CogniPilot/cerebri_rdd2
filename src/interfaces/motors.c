/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "drivers.h"

#include "synapse_time_status.h"
#include "zros_topics.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/misc/nxp_flexio_dshot/nxp_flexio_dshot.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/shell/shell.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <zros/private/zros_node_struct.h>
#include <zros/private/zros_pub_struct.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>

#if defined(CONFIG_PWM)
#define PWM_FREQ CONFIG_PWM_FREQUENCY
#endif

#define MOTOR_NODE DT_ALIAS(motors)

static atomic_t g_motor_test_active;
static rdd2_motor_values_t g_motor_test_values;
static atomic_t g_motor_raw_test_active;
static rdd2_motor_raw_t g_motor_raw_test_values;
static struct zros_node g_rdd2_motor_output_node;
static struct zros_pub g_rdd2_motor_output_pub;
static rdd2_topic_motor_output_blob_t g_rdd2_motor_output_blob;
static bool g_rdd2_motor_output_pub_ready;

static void motor_output_publish(const rdd2_motor_values_t *motors,
                                 const rdd2_motor_raw_t *raw, bool armed,
                                 bool test_mode) {
  ARG_UNUSED(raw);
  ARG_UNUSED(test_mode);

  if (!g_rdd2_motor_output_pub_ready) {
    return;
  }
  rdd2_topic_make_pwm_output(&g_rdd2_motor_output_blob, motors, armed);

  (void)zros_pub_update(&g_rdd2_motor_output_pub);
}

static float clampf(float value, float min_value, float max_value) {
  if (value < min_value) {
    return min_value;
  }
  if (value > max_value) {
    return max_value;
  }
  return value;
}

static uint16_t motor_to_dshot(float normalized, bool armed) {
  float min_output = armed ? RDD2_MOTOR_IDLE_THROTTLE : 0.0f;
  float clamped = clampf(normalized, min_output, 1.0f);
  float span = (float)(DSHOT_MAX - DSHOT_MIN);

  if (!armed || clamped <= 0.0f) {
    return DSHOT_DISARMED;
  }

  return (uint16_t)(DSHOT_MIN + (clamped * span) + 0.5f);
}

#if defined(CONFIG_RDD2_DSHOT) && !defined(CONFIG_RDD2_LOCKSTEP)
/*
 * Bidirectional DShot readback. The driver decodes the ESC responses captured
 * since the previous trigger, and runs its receive-baud training, only inside
 * sensor_sample_fetch, so it is called once per output cycle right before the
 * next trigger. The driver reports eRPM in hundreds, and degrees C, volts and
 * amps for the extended telemetry; the topics carry real eRPM, centivolt and
 * deciamp.
 *
 * eRPM is published every cycle: it is the fastest-moving value here and a
 * rotor-frequency filter needs it at the loop rate, and a publish is a copy of
 * 40 bytes into the topic's spare buffer. The extended telemetry replaces an
 * eRPM frame when the ESC sends it, so it is published only on the cycles that
 * carried one, which is a few hertz per quantity.
 */
static struct zros_pub g_esc_rpm_pub;
static struct zros_pub g_esc_telemetry_pub;
static rdd2_esc_rpm_t g_esc_rpm;
static rdd2_esc_telemetry_t g_esc_telemetry;
static bool g_esc_pub_ready;

static void esc_readback(const struct device *dshot_dev) {
  struct sensor_value val[4];
  uint8_t was_valid = g_esc_rpm.valid;
  uint8_t fresh = 0U;

  if (sensor_sample_fetch_chan(dshot_dev, SENSOR_CHAN_RPM) == 0) {
    g_esc_rpm.decoded++;
    g_esc_rpm.valid = 0x0FU;
    (void)sensor_channel_get(dshot_dev, SENSOR_CHAN_RPM, val);
    for (size_t i = 0; i < 4U; i++) {
      g_esc_rpm.erpm[i] = val[i].val1 * 100;
    }
  } else {
    g_esc_rpm.no_data++;
    g_esc_rpm.valid = 0U;
  }
  g_esc_rpm.timestamp_ns = synapse_time_boot_ns();

  /* The fetch does not report extended telemetry either way, so read it
   * unconditionally. Each get reports -ENODATA unless a channel decoded a
   * fresh value since the last read. */
  if (sensor_channel_get(dshot_dev, SENSOR_CHAN_DIE_TEMP, val) == 0) {
    fresh |= RDD2_ESC_TELEMETRY_TEMPERATURE;
    for (size_t i = 0; i < 4U; i++) {
      g_esc_telemetry.temperature_degc[i] = (int16_t)val[i].val1;
    }
  }
  if (sensor_channel_get(dshot_dev, SENSOR_CHAN_VOLTAGE, val) == 0) {
    fresh |= RDD2_ESC_TELEMETRY_VOLTAGE;
    for (size_t i = 0; i < 4U; i++) {
      g_esc_telemetry.voltage_cv[i] =
          (uint16_t)(val[i].val1 * 100 + val[i].val2 / 10000);
    }
  }
  if (sensor_channel_get(dshot_dev, SENSOR_CHAN_CURRENT, val) == 0) {
    fresh |= RDD2_ESC_TELEMETRY_CURRENT;
    for (size_t i = 0; i < 4U; i++) {
      g_esc_telemetry.current_da[i] = (int16_t)(val[i].val1 * 10);
    }
  }

  if (!g_esc_pub_ready) {
    return;
  }
  /* With nothing answering, publish the sample that goes invalid and then stay
   * quiet: a build without bidirectional ESCs feeds no empty frames to the log
   * or the transmitter. */
  if (g_esc_rpm.valid != 0U || was_valid != 0U) {
    (void)zros_pub_update(&g_esc_rpm_pub);
  }
  if (fresh != 0U) {
    g_esc_telemetry.fresh = fresh;
    g_esc_telemetry.timestamp_ns = g_esc_rpm.timestamp_ns;
    (void)zros_pub_update(&g_esc_telemetry_pub);
  }
}
#endif

static uint64_t motor_output_trigger_and_timestamp(void) {
#if defined(CONFIG_RDD2_DSHOT) && !defined(CONFIG_RDD2_LOCKSTEP)
  const struct device *const dshot_dev = DEVICE_DT_GET(MOTOR_NODE);

  esc_readback(dshot_dev);
  nxp_flexio_dshot_trigger(dshot_dev);
  return nxp_flexio_dshot_last_trigger_ns_get(dshot_dev);
#else
  return synapse_time_boot_ns();
#endif
}

int rdd2_motor_output_init(void) {
  int rc;

  if (!device_is_ready(DEVICE_DT_GET(MOTOR_NODE))) {
    return -ENODEV;
  }
#if defined(CONFIG_RDD2_DSHOT)
  if (nxp_flexio_dshot_channel_count(DEVICE_DT_GET(MOTOR_NODE)) != 4U) {
    return -EINVAL;
  }
#endif

  zros_node_init(&g_rdd2_motor_output_node, "rdd2_motor_output");
  rc = zros_pub_init(&g_rdd2_motor_output_pub, &g_rdd2_motor_output_node,
                     &topic_pwm_signal_outputs, &g_rdd2_motor_output_blob);
  g_rdd2_motor_output_pub_ready = (rc == 0);
#if defined(CONFIG_RDD2_DSHOT) && !defined(CONFIG_RDD2_LOCKSTEP)
  g_esc_pub_ready =
      zros_pub_init(&g_esc_rpm_pub, &g_rdd2_motor_output_node, &topic_esc_rpm,
                    &g_esc_rpm) == 0 &&
      zros_pub_init(&g_esc_telemetry_pub, &g_rdd2_motor_output_node,
                    &topic_esc_telemetry, &g_esc_telemetry) == 0;
#endif

  rdd2_motor_test_clear();
  rdd2_motor_raw_test_clear();
  motor_output_publish(&(rdd2_motor_values_t){0},
                       &(rdd2_motor_raw_t){
                           .value = {DSHOT_DISARMED, DSHOT_DISARMED,
                                     DSHOT_DISARMED, DSHOT_DISARMED},
                       },
                       false, false);
  return rc;
}

bool rdd2_motor_output_ready(void) {
  return device_is_ready(DEVICE_DT_GET(MOTOR_NODE));
}

uint64_t rdd2_motor_output_write_all(const rdd2_motor_values_t *motors,
                                     bool armed, bool test_mode) {
  float min_output = (armed && !test_mode) ? RDD2_MOTOR_IDLE_THROTTLE : 0.0f;
  rdd2_motor_values_t applied = {0};
  rdd2_motor_raw_t raw = {0};
  const float *motor_values = rdd2_topic_motor_values_data_const(motors);
  float *applied_values = rdd2_topic_motor_values_data(&applied);
  uint16_t *raw_values = rdd2_topic_motor_raw_data(&raw);

#if defined(CONFIG_RDD2_LOCKSTEP)
  /* Intermediate 800 Hz controller outputs do not cross the 200 Hz plant
   * boundary. Avoid four fake-device writes and a trigger for each one. */
  if (!rdd2_imu_stream_lockstep_at_target()) {
    return 0U;
  }
#endif

#if defined(CONFIG_RDD2_DSHOT) || defined(CONFIG_RDD2_LOCKSTEP)
  for (size_t i = 0; i < 4U; i++) {
    applied_values[i] =
        armed ? clampf(motor_values[i], min_output, 1.0f) : 0.0f;
    raw_values[i] =
        motor_to_dshot(applied_values[i], armed && applied_values[i] > 0.0f);
#if defined(CONFIG_RDD2_DSHOT) && !defined(CONFIG_RDD2_LOCKSTEP)
    nxp_flexio_dshot_data_set(DEVICE_DT_GET(MOTOR_NODE), i, raw_values[i],
                              false);
#endif
  }
#else
  for (size_t i = 0; i < 4U; i++) {
    applied_values[i] =
        armed ? clampf(motor_values[i], min_output, 1.0f) : 0.0f;
    pwm_set(DEVICE_DT_GET(MOTOR_NODE), i, PWM_HZ(PWM_FREQ),
            PWM_HZ(PWM_FREQ) / UINT16_MAX * applied_values[i],
            PWM_POLARITY_NORMAL);
  }
#endif

  motor_output_publish(&applied, &raw, armed, test_mode);
  return motor_output_trigger_and_timestamp();
}

uint64_t rdd2_motor_output_write_all_raw(const rdd2_motor_raw_t *raw,
                                         bool test_mode) {
  rdd2_motor_values_t applied = {0};
  rdd2_motor_raw_t clamped = {0};
  const uint16_t *raw_values = rdd2_topic_motor_raw_data_const(raw);
  float *applied_values = rdd2_topic_motor_values_data(&applied);
  uint16_t *clamped_values = rdd2_topic_motor_raw_data(&clamped);
  bool armed = false;

#if defined(CONFIG_RDD2_LOCKSTEP)
  if (!rdd2_imu_stream_lockstep_at_target()) {
    return 0U;
  }
#endif

#if defined(CONFIG_RDD2_DSHOT) || defined(CONFIG_RDD2_LOCKSTEP)
  for (size_t i = 0; i < 4U; i++) {
    uint16_t value = raw_values[i];

    if (value != DSHOT_DISARMED && value < DSHOT_MIN) {
      value = DSHOT_MIN;
    }
    if (value > DSHOT_MAX) {
      value = DSHOT_MAX;
    }

    clamped_values[i] = value;
    if (value == DSHOT_DISARMED) {
      applied_values[i] = 0.0f;
    } else {
      applied_values[i] =
          (float)(value - DSHOT_MIN) / (float)(DSHOT_MAX - DSHOT_MIN);
      armed = true;
    }
#if defined(CONFIG_RDD2_DSHOT) && !defined(CONFIG_RDD2_LOCKSTEP)
    nxp_flexio_dshot_data_set(DEVICE_DT_GET(MOTOR_NODE), i, value, false);
#endif
  }
#else
  for (size_t i = 0; i < 4U; i++) {
    applied_values[i] = armed ? clampf(raw_values[i], 0.0f, 1.0f) : 0.0f;
    pwm_set(DEVICE_DT_GET(MOTOR_NODE), i, PWM_HZ(PWM_FREQ),
            PWM_HZ(PWM_FREQ) * applied_values[i], PWM_POLARITY_NORMAL);
  }
#endif

  motor_output_publish(&applied, &clamped, armed, test_mode);
  return motor_output_trigger_and_timestamp();
}

bool rdd2_motor_test_get(rdd2_motor_values_t *motors) {
  const float *test_values =
      rdd2_topic_motor_values_data_const(&g_motor_test_values);
  float *motor_values = rdd2_topic_motor_values_data(motors);
  bool active;
  unsigned int key = irq_lock();

  active = atomic_get(&g_motor_test_active) != 0;
  /* Only overwrite the caller's motor buffer while a manual motor test is
   * active. Copying the (idle) test values unconditionally would discard the
   * live allocator output that the caller uses when no test is running. */
  if (active) {
    for (size_t i = 0; i < 4U; i++) {
      motor_values[i] = test_values[i];
    }
  }

  irq_unlock(key);

  return active;
}

void rdd2_motor_test_set(size_t index, float value) {
  float *test_values = rdd2_topic_motor_values_data(&g_motor_test_values);
  unsigned int key = irq_lock();

  test_values[index] = clampf(value, 0.0f, 1.0f);
  atomic_set(&g_motor_test_active, 1);

  irq_unlock(key);
}

void rdd2_motor_test_clear(void) {
  float *test_values = rdd2_topic_motor_values_data(&g_motor_test_values);
  unsigned int key = irq_lock();

  for (size_t i = 0; i < 4U; i++) {
    test_values[i] = 0.0f;
  }
  atomic_set(&g_motor_test_active, 0);

  irq_unlock(key);
}

bool rdd2_motor_raw_test_get(rdd2_motor_raw_t *raw) {
  const uint16_t *test_values =
      rdd2_topic_motor_raw_data_const(&g_motor_raw_test_values);
  uint16_t *raw_values = rdd2_topic_motor_raw_data(raw);
  bool active;
  unsigned int key = irq_lock();

  for (size_t i = 0; i < 4U; i++) {
    raw_values[i] = test_values[i];
  }
  active = atomic_get(&g_motor_raw_test_active) != 0;

  irq_unlock(key);

  return active;
}

void rdd2_motor_raw_test_set(size_t index, uint16_t value) {
  uint16_t *test_values = rdd2_topic_motor_raw_data(&g_motor_raw_test_values);
  unsigned int key = irq_lock();

  test_values[index] = value;
  atomic_set(&g_motor_raw_test_active, 1);

  irq_unlock(key);
}

void rdd2_motor_raw_test_set_all(uint16_t value) {
  uint16_t *test_values = rdd2_topic_motor_raw_data(&g_motor_raw_test_values);
  unsigned int key = irq_lock();

  for (size_t i = 0; i < 4U; i++) {
    test_values[i] = value;
  }
  atomic_set(&g_motor_raw_test_active, 1);

  irq_unlock(key);
}

void rdd2_motor_raw_test_clear(void) {
  uint16_t *test_values = rdd2_topic_motor_raw_data(&g_motor_raw_test_values);
  unsigned int key = irq_lock();

  for (size_t i = 0; i < 4U; i++) {
    test_values[i] = DSHOT_DISARMED;
  }
  atomic_set(&g_motor_raw_test_active, 0);

  irq_unlock(key);
}

#if defined(CONFIG_RDD2_DSHOT) && !defined(CONFIG_RDD2_LOCKSTEP) && defined(CONFIG_SHELL)
static int cmd_motors_esc(const struct shell *sh, size_t argc, char **argv) {
  /* Read the published samples rather than the rate thread's own buffers: the
   * shell runs at a lower priority and would otherwise print a torn one. */
  rdd2_esc_rpm_t rpm = {0};
  rdd2_esc_telemetry_t edt = {0};

  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  (void)zros_topic_read(&topic_esc_rpm, &rpm);
  (void)zros_topic_read(&topic_esc_telemetry, &edt);
  shell_print(sh, "decoded=%u no_data=%u valid=0x%x edt_fresh=0x%x", rpm.decoded,
              rpm.no_data, rpm.valid, edt.fresh);
  for (int i = 0; i < 4; i++) {
    shell_print(sh, "esc%d erpm=%d temp=%dC volt=%u.%02uV curr=%d.%dA", i,
                rpm.erpm[i], edt.temperature_degc[i], edt.voltage_cv[i] / 100U,
                edt.voltage_cv[i] % 100U, edt.current_da[i] / 10,
                edt.current_da[i] % 10);
  }
  return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(motors_cmds,
                               SHELL_CMD(esc, NULL, "Bidirectional DShot readback per ESC.",
                                         cmd_motors_esc),
                               SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(motors, &motors_cmds, "Motor output diagnostics.", NULL);
#endif
