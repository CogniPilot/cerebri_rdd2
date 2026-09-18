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
 * next trigger. Values stay in the driver's units: eRPM in hundreds, degrees
 * C, volts, amps.
 */
static struct {
  struct sensor_value rpm[4];
  struct sensor_value temperature[4];
  struct sensor_value voltage[4];
  struct sensor_value current[4];
  uint32_t decoded;
  uint32_t no_data;
} g_esc;

static void esc_readback(const struct device *dshot_dev) {
  if (sensor_sample_fetch_chan(dshot_dev, SENSOR_CHAN_RPM) != 0) {
    g_esc.no_data++;
    return;
  }
  g_esc.decoded++;
  (void)sensor_channel_get(dshot_dev, SENSOR_CHAN_RPM, g_esc.rpm);
  (void)sensor_channel_get(dshot_dev, SENSOR_CHAN_DIE_TEMP, g_esc.temperature);
  (void)sensor_channel_get(dshot_dev, SENSOR_CHAN_VOLTAGE, g_esc.voltage);
  (void)sensor_channel_get(dshot_dev, SENSOR_CHAN_CURRENT, g_esc.current);
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
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  shell_print(sh, "decoded=%u no_data=%u", g_esc.decoded, g_esc.no_data);
  for (int i = 0; i < 4; i++) {
    shell_print(sh, "esc%d erpm=%d temp=%dC volt=%d.%02uV curr=%dA", i,
                g_esc.rpm[i].val1 * 100, g_esc.temperature[i].val1, g_esc.voltage[i].val1,
                (unsigned)(g_esc.voltage[i].val2 / 10000), g_esc.current[i].val1);
  }
  return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(motors_cmds,
                               SHELL_CMD(esc, NULL, "Bidirectional DShot readback per ESC.",
                                         cmd_motors_esc),
                               SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(motors, &motors_cmds, "Motor output diagnostics.", NULL);
#endif
