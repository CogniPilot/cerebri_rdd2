/* SPDX-License-Identifier: Apache-2.0 */
/* Onboard magnetometer: sampled at a fixed rate through the Zephyr sensor
 * API, rotated from the sensor axes into the FLU body frame, and published as
 * the magnetic_field topic for the navigation estimator's heading correction
 * and initial alignment. */
#include "drivers.h"
#include "synapse_time_status.h"
#include "zros_topics.h"

#include <math.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zros/private/zros_node_struct.h>
#include <zros/private/zros_pub_struct.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>

LOG_MODULE_REGISTER(rdd2_magnetometer, LOG_LEVEL_INF);

#if defined(CONFIG_RDD2_MAGNETOMETER_BMM350)
#define MAG_NODE DT_NODELABEL(bmm350)
#elif defined(CONFIG_RDD2_MAGNETOMETER_IST8310)
#define MAG_NODE DT_NODELABEL(ist8310)
#else
#error "select a magnetometer device"
#endif

/* A sample older than this no longer counts the sensor as healthy. */
#define MAG_STALE_NS (500ULL * 1000ULL * 1000ULL)
#define GAUSS_TO_TESLA 1.0e-4f

static const struct device *const g_mag_dev = DEVICE_DT_GET(MAG_NODE);
static struct zros_node g_mag_node;
static struct zros_pub g_mag_pub;
static synapse_topic_MagneticFieldData_t g_mag_msg;
static atomic_t g_mag_last_ok_ns_high;
static atomic_t g_mag_last_ok_ns_low;
static K_THREAD_STACK_DEFINE(g_mag_stack, 2048);
static struct k_thread g_mag_thread;

static void mag_last_ok_set(uint64_t now_ns)
{
	atomic_set(&g_mag_last_ok_ns_high, (atomic_val_t)(now_ns >> 32));
	atomic_set(&g_mag_last_ok_ns_low, (atomic_val_t)(now_ns & 0xffffffffULL));
}

static uint64_t mag_last_ok_get(void)
{
	uint64_t high = (uint64_t)(uint32_t)atomic_get(&g_mag_last_ok_ns_high);
	uint64_t low = (uint64_t)(uint32_t)atomic_get(&g_mag_last_ok_ns_low);
	return (high << 32) | low;
}

/*
 * Sensor axes to FLU body axes. The default is the same mapping the IMU
 * interface applies to the ICM45686 on this board: body x (forward) is sensor
 * y, body y (left) is -sensor x, body z (up) is sensor z. The mount yaw
 * selects that rotation about z; the z flag inverts the vertical axis for a
 * package mounted upside down. Verify on the bench with the aircraft pointed
 * at a known heading before trusting the estimator's magnetic heading.
 */
static void mag_sensor_axes_to_body(float sx, float sy, float sz, float out[3])
{
	float bx;
	float by;

#if defined(CONFIG_RDD2_MAGNETOMETER_MOUNT_YAW_0)
	bx = sx;
	by = sy;
#elif defined(CONFIG_RDD2_MAGNETOMETER_MOUNT_YAW_90)
	bx = sy;
	by = -sx;
#elif defined(CONFIG_RDD2_MAGNETOMETER_MOUNT_YAW_180)
	bx = -sx;
	by = -sy;
#else
	bx = -sy;
	by = sx;
#endif
	out[0] = bx;
	out[1] = by;
	out[2] = IS_ENABLED(CONFIG_RDD2_MAGNETOMETER_Z_INVERTED) ? -sz : sz;
}

static bool mag_read(float field_flu_tesla[3], float *temperature_c)
{
	struct sensor_value values[3];
	struct sensor_value temperature;
	float sensor_axes[3];

	if (sensor_sample_fetch(g_mag_dev) != 0 ||
	    sensor_channel_get(g_mag_dev, SENSOR_CHAN_MAGN_XYZ, values) != 0) {
		return false;
	}
	for (size_t axis = 0U; axis < 3U; ++axis) {
		sensor_axes[axis] = sensor_value_to_float(&values[axis]) * GAUSS_TO_TESLA;
		if (!isfinite(sensor_axes[axis])) {
			return false;
		}
	}
	mag_sensor_axes_to_body(sensor_axes[0], sensor_axes[1], sensor_axes[2], field_flu_tesla);
	if (sensor_channel_get(g_mag_dev, SENSOR_CHAN_DIE_TEMP, &temperature) == 0) {
		*temperature_c = sensor_value_to_float(&temperature);
	} else {
		*temperature_c = 0.0f;
	}
	return true;
}

static void mag_thread(void *p1, void *p2, void *p3)
{
	const k_timeout_t period = K_USEC(1000000U / CONFIG_RDD2_MAGNETOMETER_RATE_HZ);
	uint32_t failures = 0U;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		float field[3];
		float temperature_c;
		uint64_t now_ns = synapse_time_boot_ns();

		if (mag_read(field, &temperature_c)) {
			g_mag_msg.timestamp_ns = now_ns;
			g_mag_msg.mag_flu_tesla.x = field[0];
			g_mag_msg.mag_flu_tesla.y = field[1];
			g_mag_msg.mag_flu_tesla.z = field[2];
			g_mag_msg.temperature_c = temperature_c;
			g_mag_msg.flags = 0U;
			g_mag_msg.time_status = synapse_types_TimeStatus_LocalFreerun;
			g_mag_msg.id = 0U;
			(void)zros_pub_update(&g_mag_pub);
			mag_last_ok_set(now_ns);
			failures = 0U;
		} else {
			failures++;
			if (failures == 10U) {
				LOG_WRN("magnetometer read failing");
			}
		}
		k_sleep(period);
	}
}

int rdd2_magnetometer_init(void)
{
	int rc;

	if (!device_is_ready(g_mag_dev)) {
		LOG_WRN("magnetometer %s not ready; heading aiding unavailable", g_mag_dev->name);
		return 0;
	}
	zros_node_init(&g_mag_node, "rdd2_magnetometer");
	rc = zros_pub_init(&g_mag_pub, &g_mag_node, &topic_magnetic_field, &g_mag_msg);
	if (rc != 0) {
		return rc;
	}
	k_thread_create(&g_mag_thread, g_mag_stack, K_THREAD_STACK_SIZEOF(g_mag_stack), mag_thread,
			NULL, NULL, NULL, K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
	k_thread_name_set(&g_mag_thread, "rdd2_mag");
	return 0;
}

bool rdd2_magnetometer_healthy(void)
{
	uint64_t last = mag_last_ok_get();
	uint64_t now = synapse_time_boot_ns();

	return last != 0U && now >= last && now - last <= MAG_STALE_NS;
}
