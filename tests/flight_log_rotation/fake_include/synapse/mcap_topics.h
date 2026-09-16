/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Fixed-layout topic payload types and MCAP topic descriptors for the
 * flight-log rotation suite. Every payload size matches the production
 * synapse/1 catalog so the capture ring frames and the streamed byte volume
 * reproduce the recorded flight offered load.
 */

#ifndef RDD2_TEST_FAKE_SYNAPSE_MCAP_TOPICS_H_
#define RDD2_TEST_FAKE_SYNAPSE_MCAP_TOPICS_H_

#include <stdint.h>

#include <synapse/mcap.h>

typedef enum {
	synapse_types_TimeStatus_LocalFreerun = 0,
	synapse_types_TimeStatus_External = 1,
	synapse_types_TimeStatus_Gnss = 2,
} synapse_types_TimeStatus_enum_t;

/* Leading uint64 publish time matches the production fixed-layout convention:
 * the writer reads publish_time from offset 0 of every payload. */
#define RDD2_FAKE_PAYLOAD(name, size)                                                              \
	typedef struct __attribute__((packed)) {                                                   \
		uint64_t publish_time_ns;                                                          \
		uint8_t _bytes[(size) - 8U];                                                        \
	} name

RDD2_FAKE_PAYLOAD(synapse_topic_InertialSampleData_t, 40U);
RDD2_FAKE_PAYLOAD(synapse_topic_PwmSignalOutputsData_t, 48U);
RDD2_FAKE_PAYLOAD(synapse_topic_OdometryEstimateData_t, 232U);
RDD2_FAKE_PAYLOAD(synapse_topic_AttitudeEstimateData_t, 40U);
RDD2_FAKE_PAYLOAD(synapse_topic_ControlLoopMetricsData_t, 24U);
RDD2_FAKE_PAYLOAD(synapse_topic_AttitudeCommandData_t, 48U);
RDD2_FAKE_PAYLOAD(synapse_topic_RateCommandData_t, 32U);
RDD2_FAKE_PAYLOAD(synapse_topic_ManualControlData_t, 40U);
RDD2_FAKE_PAYLOAD(synapse_topic_OpticalFlowVelocityData_t, 32U);
RDD2_FAKE_PAYLOAD(synapse_topic_GnssFixData_t, 64U);

typedef enum {
	synapse_topic_VehicleHealthFlags_Armed = 1U << 0,
} synapse_topic_VehicleHealthFlags_t;

typedef struct __attribute__((packed)) {
	uint64_t publish_time_ns;
	uint32_t flags;
	uint8_t _bytes[56U - 12U];
} synapse_topic_VehicleHealthData_t;

typedef struct __attribute__((packed)) {
	uint64_t timestamp_ns;
	uint64_t time_unix_ns;
	uint32_t time_status;
	uint8_t _bytes[40U - 20U];
} synapse_topic_TimeReferenceData_t;

#define RDD2_FAKE_TOPIC(id, name, size)                                                            \
	((synapse_mcap_topic_t){                                                                    \
		.topic_id = (id),                                                                   \
		.schema_name = name,                                                                \
		.schema_data = (const uint8_t *)name,                                               \
		.schema_size = sizeof(name),                                                        \
		.payload_size = (size),                                                             \
		.fixed_layout = 1U,                                                                 \
	})

#define SYNAPSE_MCAP_TOPIC_InertialSample RDD2_FAKE_TOPIC(1, "synapse.topic.InertialSample", 40U)
#define SYNAPSE_MCAP_TOPIC_PwmSignalOutputs                                                        \
	RDD2_FAKE_TOPIC(2, "synapse.topic.PwmSignalOutputs", 48U)
#define SYNAPSE_MCAP_TOPIC_OdometryEstimate                                                        \
	RDD2_FAKE_TOPIC(3, "synapse.topic.OdometryEstimate", 232U)
#define SYNAPSE_MCAP_TOPIC_AttitudeEstimate                                                        \
	RDD2_FAKE_TOPIC(4, "synapse.topic.AttitudeEstimate", 40U)
#define SYNAPSE_MCAP_TOPIC_VehicleHealth RDD2_FAKE_TOPIC(5, "synapse.topic.VehicleHealth", 56U)
#define SYNAPSE_MCAP_TOPIC_ControlLoopMetrics                                                      \
	RDD2_FAKE_TOPIC(6, "synapse.topic.ControlLoopMetrics", 24U)
#define SYNAPSE_MCAP_TOPIC_AttitudeCommand                                                         \
	RDD2_FAKE_TOPIC(7, "synapse.topic.AttitudeCommand", 48U)
#define SYNAPSE_MCAP_TOPIC_RateCommand RDD2_FAKE_TOPIC(8, "synapse.topic.RateCommand", 32U)
#define SYNAPSE_MCAP_TOPIC_ManualControlCommand                                                    \
	RDD2_FAKE_TOPIC(9, "synapse.topic.ManualControlCommand", 40U)
#define SYNAPSE_MCAP_TOPIC_OpticalFlowVelocity                                                     \
	RDD2_FAKE_TOPIC(10, "synapse.topic.OpticalFlowVelocity", 32U)
#define SYNAPSE_MCAP_TOPIC_GnssFix RDD2_FAKE_TOPIC(11, "synapse.topic.GnssFix", 64U)
#define SYNAPSE_MCAP_TOPIC_TimeReference RDD2_FAKE_TOPIC(12, "synapse.topic.TimeReference", 40U)

#endif /* RDD2_TEST_FAKE_SYNAPSE_MCAP_TOPICS_H_ */
