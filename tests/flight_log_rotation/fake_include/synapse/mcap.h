/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Minimal constant-memory MCAP writer surface for the flight-log rotation
 * suite. It mirrors the field names and call signatures the flight logger uses
 * from the production writer, then accounts written bytes through the sink so
 * the byte-triggered rotation logic runs against a controllable byte volume.
 */

#ifndef RDD2_TEST_FAKE_SYNAPSE_MCAP_H_
#define RDD2_TEST_FAKE_SYNAPSE_MCAP_H_

#include <stddef.h>
#include <stdint.h>

#define SYNAPSE_MCAP_OK 0
#define SYNAPSE_MCAP_TIME_CORRELATED 2

typedef struct {
	int (*write)(void *context, const uint8_t *data, size_t size);
	int (*flush)(void *context);
	void *context;
} synapse_mcap_sink_t;

typedef struct {
	uint16_t topic_id;
	const char *schema_name;
	const uint8_t *schema_data;
	size_t schema_size;
	size_t payload_size;
	uint8_t fixed_layout;
} synapse_mcap_topic_t;

typedef struct {
	uint16_t topic_id;
	size_t payload_size;
	uint8_t fixed_layout;
} synapse_mcap_channel_t;

typedef struct {
	synapse_mcap_sink_t sink;
	int sticky_error;
	int open;
} synapse_mcap_writer_t;

int synapse_mcap_open(synapse_mcap_writer_t *writer, synapse_mcap_sink_t sink,
		      uint8_t *output_buffer, size_t output_buffer_size,
		      const char *library, const char *session_id, const char *source,
		      int time_mode);
int synapse_mcap_add_topic(synapse_mcap_writer_t *writer, const synapse_mcap_topic_t *topic,
			   const char *key, synapse_mcap_channel_t *channel);
int synapse_mcap_write_fixed(synapse_mcap_writer_t *writer, synapse_mcap_channel_t *channel,
			     uint64_t log_time_ns, uint64_t publish_time_ns, const void *payload,
			     size_t payload_size);
int synapse_mcap_flush(synapse_mcap_writer_t *writer);
int synapse_mcap_close(synapse_mcap_writer_t *writer);
int synapse_mcap_error(const synapse_mcap_writer_t *writer);

#endif /* RDD2_TEST_FAKE_SYNAPSE_MCAP_H_ */
