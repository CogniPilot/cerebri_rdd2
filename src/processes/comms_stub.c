/* SPDX-License-Identifier: Apache-2.0 */

#include "processes.h"

#include "interfaces/drivers.h"
#include "interfaces/zros_topics.h"
#include "scheduling.h"

#if defined(CONFIG_RDD2_GNSS_SOURCE_ONBOARD)
#include "gnss_onboard.h"
#endif

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include <zros/private/zros_node_struct.h>
#include <zros/private/zros_pub_struct.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>
#include <zros/zros_topic.h>

LOG_MODULE_DECLARE(rdd2, LOG_LEVEL_INF);

#define STUB_NAV_PUBLISH_DIV 8U
#define STUB_STATUS_PUBLISH_DIV 32U

struct comms_stub_process {
	rdd2_vec3f_t gyro;
	rdd2_vec3f_t accel;
	rdd2_rc_channels_t rc;
	synapse_topic_InertialSampleData_t imu;
	synapse_topic_AttitudeEstimateData_t attitude;
	synapse_topic_OdometryEstimateData_t odometry;
	synapse_topic_VehicleHealthData_t health;
	synapse_topic_ControlLoopMetricsData_t metrics;
	struct zros_node node;
	struct zros_pub imu_pub;
	struct zros_pub attitude_pub;
	struct zros_pub odometry_pub;
	struct zros_pub health_pub;
	struct zros_pub metrics_pub;
	bool imu_stream_ready;
	bool rc_ready;
	uint32_t nav_countdown;
	uint32_t status_countdown;
};

static struct comms_stub_process g_process;

static uint64_t now_ns(void)
{
	return (uint64_t)k_uptime_get() * UINT64_C(1000000);
}

static bool divider_expired(uint32_t *countdown, uint32_t divisor)
{
	if (*countdown > 0U) {
		(*countdown)--;
		return false;
	}
	*countdown = divisor - 1U;
	return true;
}

static bool gnss_ready(void)
{
#if defined(CONFIG_RDD2_GNSS_SOURCE_ONBOARD)
	return rdd2_gnss_onboard_ready_get();
#else
	return false;
#endif
}

static int publishers_init(struct comms_stub_process *process)
{
	int rc;

	zros_node_init(&process->node, "rdd2_stub_nav");
	rc = zros_pub_init(&process->imu_pub, &process->node, &topic_control_imu,
			   &process->imu);
	if (rc == 0) {
		rc = zros_pub_init(&process->attitude_pub, &process->node,
				   &topic_attitude_estimate, &process->attitude);
	}
	if (rc == 0) {
		rc = zros_pub_init(&process->odometry_pub, &process->node,
				   &topic_navigation_odometry, &process->odometry);
	}
	if (rc == 0) {
		rc = zros_pub_init(&process->health_pub, &process->node,
				   &topic_vehicle_health, &process->health);
	}
	if (rc == 0) {
		rc = zros_pub_init(&process->metrics_pub, &process->node,
				   &topic_control_loop_metrics, &process->metrics);
	}
	return rc;
}

static void publish_imu(struct comms_stub_process *process, bool sample_valid,
			uint64_t sample_ns)
{
	process->imu = (synapse_topic_InertialSampleData_t){
		.timestamp_ns = sample_ns,
		.accel_flu_m_s2 = process->accel,
		.gyro_flu_rad_s = process->gyro,
		.flags = sample_valid ? synapse_topic_InertialFieldFlags_Accel |
					      synapse_topic_InertialFieldFlags_Gyro
				      : 0U,
		.time_status = synapse_types_TimeStatus_LocalFreerun,
	};
	(void)zros_pub_update(&process->imu_pub);
}

static void publish_invalid_navigation(struct comms_stub_process *process,
				       uint64_t sample_ns)
{
	process->attitude = (synapse_topic_AttitudeEstimateData_t){
		.timestamp_ns = sample_ns,
		.attitude = {.w = 1.0f},
		.flags = 0U,
		.time_status = synapse_types_TimeStatus_LocalFreerun,
	};
	process->odometry = (synapse_topic_OdometryEstimateData_t){
		.timestamp_ns = sample_ns,
		.attitude = {.w = 1.0f},
		.quality_pct = 0,
		.time_status = synapse_types_TimeStatus_LocalFreerun,
	};
	(void)zros_pub_update(&process->attitude_pub);
	(void)zros_pub_update(&process->odometry_pub);
}

static void publish_fail_closed_status(struct comms_stub_process *process,
				       bool imu_valid, bool rc_valid,
				       uint8_t link_quality,
				       uint32_t latency_us)
{
	const uint32_t present = synapse_topic_SensorComponentFlags_Gyro |
				 synapse_topic_SensorComponentFlags_Accel |
				 synapse_topic_SensorComponentFlags_Gnss |
				 synapse_topic_SensorComponentFlags_RadioControl |
				 synapse_topic_SensorComponentFlags_MotorOutputs |
				 synapse_topic_SensorComponentFlags_Estimator;
	uint32_t healthy = synapse_topic_SensorComponentFlags_MotorOutputs;

	if (imu_valid) {
		healthy |= synapse_topic_SensorComponentFlags_Gyro |
			   synapse_topic_SensorComponentFlags_Accel;
	}
	if (rc_valid) {
		healthy |= synapse_topic_SensorComponentFlags_RadioControl;
	}
	if (gnss_ready()) {
		healthy |= synapse_topic_SensorComponentFlags_Gnss;
	}
	process->health = (synapse_topic_VehicleHealthData_t){
		.timestamp_ns = now_ns(),
		.sensors_present = present,
		.sensors_enabled = present,
		.sensors_health = healthy,
		.flight_mode = rdd2_rc_flight_mode(&process->rc),
		.link_quality_pct = link_quality,
		.flags = synapse_topic_VehicleHealthFlags_Failsafe,
		.time_status = synapse_types_TimeStatus_LocalFreerun,
	};
	process->metrics = (synapse_topic_ControlLoopMetricsData_t){
		.timestamp_ns = process->health.timestamp_ns,
		.period_us = 625U,
		.latency_us = latency_us,
		.time_status = synapse_types_TimeStatus_LocalFreerun,
	};
	(void)zros_pub_update(&process->health_pub);
	(void)zros_pub_update(&process->metrics_pub);
}

static uint32_t sample_latency_us(uint64_t sample_ns, uint64_t completed_ns)
{
	uint64_t latency_ns;

	if (sample_ns == 0U || completed_ns <= sample_ns) {
		return 0U;
	}
	latency_ns = completed_ns - sample_ns;
	return (uint32_t)MIN(latency_ns / UINT64_C(1000), UINT32_MAX);
}

int rdd2_navigation_estimator_process_start(void)
{
	return 0;
}

int rdd2_waypoint_trajectory_planner_process_start(void)
{
	return 0;
}

int rdd2_guidance_controller_process_start(void)
{
	return 0;
}

int rdd2_rate_control_allocator_process_run(void)
{
	struct comms_stub_process *process = &g_process;
	int rc;

	*process = (struct comms_stub_process){0};
	rc = publishers_init(process);
	if (rc != 0) {
		return rc;
	}
	process->rc_ready = rdd2_rc_input_init() == 0;
	rc = rdd2_motor_output_init();
	if (rc != 0) {
		return rc;
	}
	process->imu_stream_ready = rdd2_imu_stream_init() == 0;
	LOG_WRN("STUB-NAV running: navigation invalid, failsafe latched, arm denied");

	while (true) {
		uint64_t sample_ns = 0U;
		uint64_t completed_ns;
		int64_t rc_stamp_ms = 0;
		uint8_t link_quality = 0U;
		bool imu_valid;
		bool rc_valid = false;
		float dt = RDD2_CONTROL_DT_S;

		if (process->imu_stream_ready) {
			imu_valid = rdd2_imu_stream_wait_next(
				&process->gyro, &process->accel, &dt, &sample_ns);
		} else {
			k_usleep(625U);
			process->gyro = (rdd2_vec3f_t){0};
			process->accel = (rdd2_vec3f_t){0};
			imu_valid = false;
		}
		if (sample_ns == 0U) {
			sample_ns = now_ns();
		}
		if (process->rc_ready) {
			rdd2_rc_input_latest_get(&process->rc, &rc_stamp_ms,
						 &rc_valid);
			link_quality = rdd2_rc_input_link_quality_get();
		}
		ARG_UNUSED(rc_stamp_ms);
		publish_imu(process, imu_valid, sample_ns);
		completed_ns = rdd2_motor_output_write_all(
			&(rdd2_motor_values_t){0}, false, false);
		if (divider_expired(&process->nav_countdown,
				    STUB_NAV_PUBLISH_DIV)) {
			publish_invalid_navigation(process, sample_ns);
		}
		if (divider_expired(&process->status_countdown,
				    STUB_STATUS_PUBLISH_DIV)) {
			publish_fail_closed_status(
				process, imu_valid, rc_valid, link_quality,
				sample_latency_us(sample_ns, completed_ns));
		}
	}
}

#if defined(CONFIG_SHELL)
static int cmd_stub_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_warn(sh, "image=STUB-NAV non_flyable=yes arm=denied motors=zero");
	shell_print(sh,
		    "generation gnss=%u imu=%u attitude=%u odometry=%u health=%u pwm=%u loop=%u waypoint=%u",
		    rdd2_topic_generation(&topic_gnss_fix),
		    rdd2_topic_generation(&topic_control_imu),
		    rdd2_topic_generation(&topic_attitude_estimate),
		    rdd2_topic_generation(&topic_navigation_odometry),
		    rdd2_topic_generation(&topic_vehicle_health),
		    rdd2_topic_generation(&topic_pwm_signal_outputs),
		    rdd2_topic_generation(&topic_control_loop_metrics),
		    rdd2_topic_generation(&topic_waypoint_plan));
	return 0;
}

SHELL_CMD_REGISTER(stub, NULL, "Show non-flyable communications image status.",
		   cmd_stub_status);
#endif
