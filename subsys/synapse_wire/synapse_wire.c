/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Direct sensor receiver for the fixed Synapse wire header. The socket layer
 * validates the UDP/IPv6 binding, receive interface, destination address, and
 * hop limit before a payload is allowed onto the local ZROS bus.
 */

#include "synapse_wire.h"

#include "interfaces/synapse_time_status.h"
#include "interfaces/zros_topics.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#endif

#include <zros/zros_node.h>
#include <zros/zros_pub.h>

#include <synapse/optical_flow_reader.h>
#include <synapse/sensors_reader.h>
#include <synapse/topic_catalog.h>
#include <synapse/types_reader.h>
#include <synapse/wire.h>

LOG_MODULE_REGISTER(rdd2_synapse_wire, CONFIG_RDD2_SYNAPSE_WIRE_LOG_LEVEL);

#define SYNAPSE_SCHEMA_SET_WIRE_ID UINT64_C(0x232721f0ee5b6c32)
#define OPTICAL_FLAGS_MASK         UINT8_C(0x07)
#define GNSS_FLAGS_MASK            UINT8_C(0x0f)
#define GNSS_CANONICAL_SIZE        58U
#define GNSS_STABLE_SAMPLES        5U
#define GNSS_RECENT_MS             300U
#define GNSS_MAX_HACC_MM           10000U
#define GNSS_MAX_VACC_MM           15000U
#define GNSS_MAX_SACC_MM_S         5000U
#define SOCKET_RETRY_MS            1000
#define SOCKET_POLL_MS             1000
#define WIRE_BUFFER_SIZE (SYNAPSE_WIRE_V1_HEADER_SIZE + sizeof(synapse_topic_GnssFixData_t))

BUILD_ASSERT(sizeof(synapse_topic_OpticalFlowVelocityData_t) == 32U);
BUILD_ASSERT(sizeof(synapse_topic_GnssFixData_t) == 64U);
BUILD_ASSERT(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);

enum stream_kind {
	STREAM_OPTICAL = 0,
	STREAM_GNSS,
	STREAM_COUNT,
};

struct stream_stats {
	uint32_t received;
	uint32_t accepted;
	uint32_t publish_failed;
	uint32_t socket_errors;
	uint32_t session_changes;
	uint32_t sequence_gaps;
	uint32_t rejected[SYNAPSE_WIRE_REJECTION_PEER_COMPATIBILITY + 1U];
	uint64_t session_id;
	uint64_t last_capture_timestamp_ns;
	uint64_t last_receive_gptp_ns;
	uint32_t last_sequence;
	int64_t last_accepted_ms;
	uint16_t last_header_flags;
	uint8_t last_receiver_time_status;
};

struct stream_context {
	enum stream_kind kind;
	const char *name;
	const char *source_address_text;
	struct net_in6_addr source_address;
	uint16_t topic_id;
	uint16_t payload_size;
	uint16_t port;
	uint32_t source_node_id;
	uint64_t maximum_age_ns;
	uint64_t session_id;
	uint64_t last_accepted_monotonic_ns;
	synapse_wire_receiver_state_t receiver_state;
	struct stream_stats stats;
	int fd;
};

struct gnss_readiness {
	bool usable;
	uint8_t stable_samples;
	int64_t last_sample_ms;
};

struct receiver_context {
	struct zros_node node;
	struct zros_pub optical_pub;
	struct zros_pub gnss_pub;
	synapse_topic_OpticalFlowVelocityData_t optical;
	synapse_topic_GnssFixData_t gnss;
	struct stream_context streams[STREAM_COUNT];
	struct net_in6_addr local_address;
	struct net_if *iface;
	unsigned int ifindex;
	struct gnss_readiness gnss_readiness;
	struct k_spinlock lock;
	struct k_thread thread;
	bool time_ever_synced;
};

union received_payload {
	synapse_topic_OpticalFlowVelocityData_t optical;
	synapse_topic_GnssFixData_t gnss;
};

struct carrier_observation {
	struct sockaddr_in6 source;
	struct net_in6_addr destination;
	unsigned int ifindex;
	int hop_limit;
	bool packet_info_seen;
	bool hop_limit_seen;
	bool truncated;
};

static K_THREAD_STACK_DEFINE(g_stack, CONFIG_RDD2_SYNAPSE_WIRE_THREAD_STACK_SIZE);
static struct receiver_context g_receiver;

static bool float_is_finite(float value)
{
	uint32_t bits;

	memcpy(&bits, &value, sizeof(bits));
	return (bits & UINT32_C(0x7f800000)) != UINT32_C(0x7f800000);
}

static bool time_status_valid(synapse_types_TimeStatus_enum_t status)
{
	return status == synapse_types_TimeStatus_LocalFreerun ||
	       status == synapse_types_TimeStatus_GptpSynced ||
	       status == synapse_types_TimeStatus_GptpHoldover;
}

static bool header_payload_time_valid(const synapse_wire_header_t *header,
				      uint64_t payload_timestamp_ns,
				      synapse_types_TimeStatus_enum_t status)
{
	const bool header_synchronized =
		(header->flags & SYNAPSE_WIRE_FLAG_CAPTURE_TIME_GPTP_SYNCED) != 0U;
	const bool payload_synchronized = status == synapse_types_TimeStatus_GptpSynced;

	return header_synchronized == payload_synchronized &&
	       (!header_synchronized || header->capture_timestamp_ns == payload_timestamp_ns);
}

static bool optical_payload_read(const synapse_wire_datagram_view_t *view,
				 synapse_topic_OpticalFlowVelocityData_t *sample,
				 bool *flag_semantics_valid)
{
	if (view->payload_size != sizeof(*sample)) {
		return false;
	}

	memcpy(sample, view->payload, sizeof(*sample));
	*flag_semantics_valid =
		header_payload_time_valid(&view->header, sample->timestamp_ns, sample->time_status);

	return sample->timestamp_ns != 0U && float_is_finite(sample->velocity_flu_m_s.x) &&
	       float_is_finite(sample->velocity_flu_m_s.y) && float_is_finite(sample->distance_m) &&
	       sample->distance_m >= 0.0f && float_is_finite(sample->roll_rad) &&
	       float_is_finite(sample->pitch_rad) && (sample->flags & ~OPTICAL_FLAGS_MASK) == 0U &&
	       time_status_valid(sample->time_status);
}

static bool gnss_payload_read(const synapse_wire_datagram_view_t *view,
			      synapse_topic_GnssFixData_t *sample, bool *flag_semantics_valid)
{
	const uint8_t *payload = view->payload;

	if (view->payload_size != sizeof(*sample)) {
		return false;
	}
	for (size_t index = GNSS_CANONICAL_SIZE; index < sizeof(*sample); ++index) {
		if (payload[index] != 0U) {
			return false;
		}
	}

	memcpy(sample, payload, sizeof(*sample));
	*flag_semantics_valid =
		header_payload_time_valid(&view->header, sample->timestamp_ns, sample->time_status);

	if (sample->timestamp_ns == 0U || sample->latitude_deg_e7 < -INT32_C(900000000) ||
	    sample->latitude_deg_e7 > INT32_C(900000000) ||
	    sample->longitude_deg_e7 < -INT32_C(1800000000) ||
	    sample->longitude_deg_e7 > INT32_C(1800000000) ||
	    (sample->flags & ~GNSS_FLAGS_MASK) != 0U ||
	    sample->fix_type > synapse_types_GnssFixType_DeadReckoning ||
	    !time_status_valid(sample->time_status)) {
		return false;
	}
	if ((sample->flags & synapse_topic_GnssFixFlags_TimeValid) != 0U &&
	    sample->time_unix_ns == 0U) {
		return false;
	}
	if ((sample->flags & synapse_topic_GnssFixFlags_CourseValid) != 0U &&
	    sample->course_over_ground_cdeg >= 36000U) {
		return false;
	}
	if ((sample->flags & synapse_topic_GnssFixFlags_YawValid) != 0U &&
	    sample->yaw_cdeg >= 36000U) {
		return false;
	}
	return true;
}

static bool gnss_fix_type_usable(uint8_t fix_type)
{
	return fix_type == synapse_types_GnssFixType_Fix3d ||
	       fix_type == synapse_types_GnssFixType_Dgnss ||
	       fix_type == synapse_types_GnssFixType_RtkFloat ||
	       fix_type == synapse_types_GnssFixType_RtkFixed;
}

static bool gnss_fix_usable(const synapse_topic_GnssFixData_t *fix)
{
	return gnss_fix_type_usable(fix->fix_type) && fix->altitude_msl_mm >= -INT32_C(1000000) &&
	       fix->altitude_msl_mm <= INT32_C(20000000) &&
	       fix->horizontal_accuracy_mm <= GNSS_MAX_HACC_MM &&
	       fix->vertical_accuracy_mm <= GNSS_MAX_VACC_MM &&
	       fix->velocity_accuracy_mm_s <= GNSS_MAX_SACC_MM_S;
}

static bool gnss_ready_from(const struct gnss_readiness *readiness, int64_t now_ms)
{
	return readiness->usable && readiness->stable_samples >= GNSS_STABLE_SAMPLES &&
	       readiness->last_sample_ms >= 0 && now_ms >= readiness->last_sample_ms &&
	       now_ms - readiness->last_sample_ms <= GNSS_RECENT_MS;
}

bool rdd2_synapse_wire_gnss_ready_get(void)
{
	struct gnss_readiness readiness;
	k_spinlock_key_t key = k_spin_lock(&g_receiver.lock);

	readiness = g_receiver.gnss_readiness;
	k_spin_unlock(&g_receiver.lock, key);
	return gnss_ready_from(&readiness, k_uptime_get());
}

static void rejection_record(struct stream_context *stream, synapse_wire_rejection_t rejection)
{
	k_spinlock_key_t key = k_spin_lock(&g_receiver.lock);

	if (rejection >= SYNAPSE_WIRE_REJECTION_NONE &&
	    rejection <= SYNAPSE_WIRE_REJECTION_PEER_COMPATIBILITY) {
		stream->stats.rejected[rejection]++;
	}
	k_spin_unlock(&g_receiver.lock, key);
}

static void socket_error_record(struct stream_context *stream)
{
	k_spinlock_key_t key = k_spin_lock(&g_receiver.lock);

	stream->stats.socket_errors++;
	k_spin_unlock(&g_receiver.lock, key);
}

static void received_record(struct stream_context *stream)
{
	k_spinlock_key_t key = k_spin_lock(&g_receiver.lock);

	stream->stats.received++;
	k_spin_unlock(&g_receiver.lock, key);
}

static void timing_record(struct stream_context *stream, const synapse_wire_header_t *header,
			  uint64_t receive_gptp_ns,
			  synapse_types_TimeStatus_enum_t receiver_time_status)
{
	k_spinlock_key_t key = k_spin_lock(&g_receiver.lock);

	stream->stats.last_capture_timestamp_ns = header->capture_timestamp_ns;
	stream->stats.last_receive_gptp_ns = receive_gptp_ns;
	stream->stats.last_header_flags = header->flags;
	stream->stats.last_receiver_time_status = (uint8_t)receiver_time_status;
	k_spin_unlock(&g_receiver.lock, key);
}

static void gnss_readiness_record(const synapse_topic_GnssFixData_t *fix, bool session_changed,
				  int64_t now_ms)
{
	const bool usable = gnss_fix_usable(fix);
	k_spinlock_key_t key = k_spin_lock(&g_receiver.lock);

	if (session_changed || !usable) {
		g_receiver.gnss_readiness.stable_samples = 0U;
	}
	if (usable && g_receiver.gnss_readiness.stable_samples < GNSS_STABLE_SAMPLES) {
		g_receiver.gnss_readiness.stable_samples++;
	}
	g_receiver.gnss_readiness.usable = usable;
	g_receiver.gnss_readiness.last_sample_ms = now_ms;
	k_spin_unlock(&g_receiver.lock, key);
}

static void accepted_record(struct stream_context *stream,
			    const synapse_wire_validation_result_t *result, bool session_changed,
			    uint64_t session_id, int64_t now_ms)
{
	k_spinlock_key_t key = k_spin_lock(&g_receiver.lock);

	stream->stats.accepted++;
	stream->stats.sequence_gaps += result->sequence_gap;
	stream->stats.session_id = session_id;
	stream->stats.last_sequence = result->datagram.header.sequence;
	stream->stats.last_accepted_ms = now_ms;
	if (session_changed) {
		stream->stats.session_changes++;
	}
	k_spin_unlock(&g_receiver.lock, key);
}

static void publish_failure_record(struct stream_context *stream)
{
	k_spinlock_key_t key = k_spin_lock(&g_receiver.lock);

	stream->stats.publish_failed++;
	k_spin_unlock(&g_receiver.lock, key);
}

static struct net_if *application_iface_get(void)
{
	struct net_if *base = net_if_get_default();
	struct net_if *vlan;
	int result;

	if (base == NULL || !net_if_is_up(base)) {
		return NULL;
	}
	result = net_eth_vlan_enable(base, CONFIG_RDD2_SYNAPSE_WIRE_VLAN_ID);
	if (result != 0 && result != -EALREADY) {
		LOG_ERR("VLAN %d enable failed: %d", CONFIG_RDD2_SYNAPSE_WIRE_VLAN_ID, result);
		return NULL;
	}
	vlan = net_eth_get_vlan_iface(base, CONFIG_RDD2_SYNAPSE_WIRE_VLAN_ID);
	if (vlan == NULL) {
		vlan = net_eth_get_vlan_iface(NULL, CONFIG_RDD2_SYNAPSE_WIRE_VLAN_ID);
	}
	if (vlan != NULL && !net_if_is_up(vlan) && net_if_up(vlan) != 0) {
		return NULL;
	}
	if (vlan == NULL ||
	    (net_if_ipv6_addr_lookup_by_iface(vlan, &g_receiver.local_address) == NULL &&
	     net_if_ipv6_addr_add(vlan, &g_receiver.local_address, NET_ADDR_MANUAL, 0) == NULL)) {
		LOG_ERR("VLAN %d IPv6 address setup failed", CONFIG_RDD2_SYNAPSE_WIRE_VLAN_ID);
		return NULL;
	}
	return vlan;
}

static int stream_socket_open(struct stream_context *stream)
{
	struct sockaddr_in6 local = {0};
	int enabled = 1;
	int fd;

	g_receiver.iface = application_iface_get();
	if (g_receiver.iface == NULL) {
		return -ENETDOWN;
	}
	g_receiver.ifindex = (unsigned int)net_if_get_by_iface(g_receiver.iface);

	fd = zsock_socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if (fd < 0) {
		return -errno;
	}
	if (zsock_setsockopt(fd, NET_IPPROTO_IPV6, ZSOCK_IPV6_RECVPKTINFO, &enabled,
			     sizeof(enabled)) != 0 ||
	    zsock_setsockopt(fd, NET_IPPROTO_IPV6, ZSOCK_IPV6_RECVHOPLIMIT, &enabled,
			     sizeof(enabled)) != 0) {
		int error = -errno;

		(void)zsock_close(fd);
		return error;
	}

	local.sin6_family = AF_INET6;
	local.sin6_port = htons(stream->port);
	local.sin6_addr = g_receiver.local_address;
	local.sin6_scope_id = g_receiver.ifindex;
	if (zsock_bind(fd, (const struct sockaddr *)&local, sizeof(local)) != 0) {
		int error = -errno;

		(void)zsock_close(fd);
		return error;
	}

	LOG_INF("RX %s VLAN %d iface %u [%s]:%u", stream->name, CONFIG_RDD2_SYNAPSE_WIRE_VLAN_ID,
		g_receiver.ifindex, CONFIG_RDD2_SYNAPSE_WIRE_LOCAL_ADDRESS, stream->port);
	return fd;
}

static bool carrier_observe(struct net_msghdr *message, struct carrier_observation *observation)
{
	struct net_cmsghdr *control;
	struct net_cmsghdr *previous = NULL;

	observation->truncated = (message->msg_flags & (ZSOCK_MSG_TRUNC | ZSOCK_MSG_CTRUNC)) != 0;
	for (control = NET_CMSG_FIRSTHDR(message); control != NULL && control != previous;
	     previous = control, control = NET_CMSG_NXTHDR(message, control)) {
		if (control->cmsg_level != NET_IPPROTO_IPV6) {
			continue;
		}
		if (control->cmsg_type == ZSOCK_IPV6_PKTINFO) {
			const struct net_in6_pktinfo *info =
				(const struct net_in6_pktinfo *)NET_CMSG_DATA(control);

			observation->destination = info->ipi6_addr;
			observation->ifindex = info->ipi6_ifindex;
			observation->packet_info_seen = true;
		} else if (control->cmsg_type == ZSOCK_IPV6_HOPLIMIT) {
			observation->hop_limit = *(const int *)NET_CMSG_DATA(control);
			observation->hop_limit_seen = true;
		}
	}

	return !observation->truncated && observation->packet_info_seen &&
	       observation->hop_limit_seen && observation->hop_limit == 1;
}

static bool binding_valid(const struct stream_context *stream,
			  const struct carrier_observation *observation)
{
	return observation->source.sin6_family == AF_INET6 &&
	       observation->source.sin6_port == htons(stream->port) &&
	       net_ipv6_addr_cmp(&observation->source.sin6_addr, &stream->source_address) &&
	       observation->ifindex == g_receiver.ifindex &&
	       net_ipv6_addr_cmp(&observation->destination, &g_receiver.local_address);
}

static bool session_select(const struct stream_context *stream, const synapse_wire_header_t *header,
			   uint64_t receive_monotonic_ns, uint64_t *session_id,
			   bool *session_changed)
{
	const uint64_t silence_ns =
		(uint64_t)CONFIG_RDD2_SYNAPSE_WIRE_SESSION_ROLLOVER_SILENCE_MS * UINT64_C(1000000);

	if (header->source_session_id == 0U) {
		return false;
	}
	if (stream->session_id == 0U || header->source_session_id == stream->session_id) {
		*session_id = header->source_session_id;
		*session_changed = stream->session_id == 0U;
		return true;
	}
	if (receive_monotonic_ns >= stream->last_accepted_monotonic_ns &&
	    receive_monotonic_ns - stream->last_accepted_monotonic_ns >= silence_ns) {
		*session_id = header->source_session_id;
		*session_changed = true;
		return true;
	}

	*session_id = stream->session_id;
	*session_changed = false;
	return true;
}

static bool payload_read(const struct stream_context *stream,
			 const synapse_wire_datagram_view_t *view, union received_payload *payload,
			 bool *flag_semantics_valid)
{
	if (stream->kind == STREAM_OPTICAL) {
		return optical_payload_read(view, &payload->optical, flag_semantics_valid);
	}
	return gnss_payload_read(view, &payload->gnss, flag_semantics_valid);
}

static bool payload_publish(const struct stream_context *stream,
			    const union received_payload *payload, bool session_changed,
			    int64_t now_ms)
{
	if (stream->kind == STREAM_OPTICAL) {
		g_receiver.optical = payload->optical;
		return zros_pub_update(&g_receiver.optical_pub) == 0;
	}

	g_receiver.gnss = payload->gnss;
	if (zros_pub_update(&g_receiver.gnss_pub) != 0) {
		return false;
	}
	gnss_readiness_record(&payload->gnss, session_changed, now_ms);
	return true;
}

static void datagram_process(struct stream_context *stream, const uint8_t *datagram,
			     size_t datagram_size, const struct carrier_observation *carrier)
{
	synapse_wire_datagram_view_t view;
	synapse_wire_rejection_t framing_rejection;
	synapse_wire_validation_result_t result;
	synapse_wire_receiver_state_t initial_state = {0};
	const synapse_wire_receiver_state_t *validation_state;
	synapse_wire_validation_policy_t policy;
	synapse_wire_observations_t observations;
	union received_payload payload;
	uint64_t receive_monotonic_ns = synapse_time_boot_ns();
	uint64_t receive_gptp_ns = 0U;
	uint64_t session_id;
	int64_t offset_ns;
	int64_t now_ms = k_uptime_get();
	bool payload_flags_valid = false;
	bool session_changed;
	bool payload_valid;
	bool carrier_valid;
	bool published;
	synapse_types_TimeStatus_enum_t receiver_time_status;

	received_record(stream);
	if (synapse_wire_decode(datagram, datagram_size, &view, &framing_rejection) !=
	    SYNAPSE_WIRE_STATUS_OK) {
		return;
	}
	if (framing_rejection != SYNAPSE_WIRE_REJECTION_NONE) {
		rejection_record(stream, framing_rejection);
		return;
	}
	if (!session_select(stream, &view.header, receive_monotonic_ns, &session_id,
			    &session_changed)) {
		rejection_record(stream, SYNAPSE_WIRE_REJECTION_SESSION);
		return;
	}

	payload_valid = payload_read(stream, &view, &payload, &payload_flags_valid);
	carrier_valid = !carrier->truncated && carrier->packet_info_seen &&
			carrier->hop_limit_seen && carrier->hop_limit == 1;
	receiver_time_status =
		synapse_time_status_resolve(&g_receiver.time_ever_synced, &offset_ns);
	if (receiver_time_status == synapse_types_TimeStatus_GptpSynced) {
		receive_gptp_ns = synapse_time_apply_offset(receive_monotonic_ns, offset_ns);
	}
	timing_record(stream, &view.header, receive_gptp_ns, receiver_time_status);

	policy = (synapse_wire_validation_policy_t){
		.topic_id = stream->topic_id,
		.schema_set_id = SYNAPSE_SCHEMA_SET_WIRE_ID,
		.source_node_id = stream->source_node_id,
		.source_session_id = session_id,
		.payload_size = stream->payload_size,
		.allowed_flags_mask = SYNAPSE_WIRE_FLAG_CAPTURE_TIME_GPTP_SYNCED,
		.sequence_window = CONFIG_RDD2_SYNAPSE_WIRE_SEQUENCE_WINDOW,
		.resync_run_length = CONFIG_RDD2_SYNAPSE_WIRE_RESYNC_RUN_LENGTH,
		.maximum_sample_age_ns = stream->maximum_age_ns,
		.future_skew_allowance_ns =
			(uint64_t)CONFIG_RDD2_SYNAPSE_WIRE_FUTURE_SKEW_MS * UINT64_C(1000000),
		.freshness_enabled = true,
	};
	observations = (synapse_wire_observations_t){
		.carrier_policy_valid = carrier_valid,
		.binding_valid = binding_valid(stream, carrier),
		.header_flag_semantics_valid = true,
		.payload_structure_valid = payload_valid,
		.payload_flag_semantics_valid = payload_flags_valid,
		.receiver_gptp_synchronized =
			receiver_time_status == synapse_types_TimeStatus_GptpSynced,
		.receive_gptp_ns = receive_gptp_ns,
		.receive_monotonic_ns = receive_monotonic_ns,
	};
	validation_state = session_changed ? &initial_state : &stream->receiver_state;
	if (synapse_wire_validate(datagram, datagram_size, &policy, &observations, validation_state,
				  &result) != SYNAPSE_WIRE_STATUS_OK) {
		return;
	}

	if (!result.deliver) {
		if (!session_changed && result.state_transition_valid) {
			(void)synapse_wire_commit(&stream->receiver_state, &result);
		}
		rejection_record(stream, result.rejection);
		return;
	}

	published = payload_publish(stream, &payload, session_changed, now_ms);
	if (!published) {
		publish_failure_record(stream);
		return;
	}
	if (session_changed) {
		stream->receiver_state = initial_state;
		stream->session_id = session_id;
	}
	if (synapse_wire_commit(&stream->receiver_state, &result) != SYNAPSE_WIRE_STATUS_OK) {
		return;
	}
	stream->last_accepted_monotonic_ns = receive_monotonic_ns;
	accepted_record(stream, &result, session_changed, session_id, now_ms);
}

static int stream_receive(struct stream_context *stream)
{
	uint8_t datagram[WIRE_BUFFER_SIZE];
	union {
		struct net_cmsghdr alignment;
		uint8_t bytes[NET_CMSG_SPACE(sizeof(struct net_in6_pktinfo)) +
			      NET_CMSG_SPACE(sizeof(int))];
	} control;
	struct net_iovec vector = {
		.iov_base = datagram,
		.iov_len = sizeof(datagram),
	};
	struct carrier_observation carrier = {0};
	struct net_msghdr message = {
		.msg_name = &carrier.source,
		.msg_namelen = sizeof(carrier.source),
		.msg_iov = &vector,
		.msg_iovlen = 1,
		.msg_control = control.bytes,
		.msg_controllen = sizeof(control.bytes),
	};
	ssize_t received;

	memset(&control, 0, sizeof(control));
	received = zsock_recvmsg(stream->fd, &message, 0);
	if (received < 0) {
		return -errno;
	}
	(void)carrier_observe(&message, &carrier);
	datagram_process(stream, datagram, (size_t)received, &carrier);
	return 0;
}

static void receiver_run(void *first, void *second, void *third)
{
	struct receiver_context *receiver = first;
	struct zsock_pollfd poll_fds[STREAM_COUNT];

	ARG_UNUSED(second);
	ARG_UNUSED(third);

	for (;;) {
		bool any_open = false;

		for (size_t index = 0U; index < STREAM_COUNT; ++index) {
			struct stream_context *stream = &receiver->streams[index];

			if (stream->fd < 0) {
				stream->fd = stream_socket_open(stream);
				if (stream->fd < 0) {
					socket_error_record(stream);
				}
			}
			poll_fds[index] = (struct zsock_pollfd){
				.fd = stream->fd,
				.events = ZSOCK_POLLIN,
			};
			any_open = any_open || stream->fd >= 0;
		}
		if (!any_open) {
			k_sleep(K_MSEC(SOCKET_RETRY_MS));
			continue;
		}

		int result = zsock_poll(poll_fds, STREAM_COUNT, SOCKET_POLL_MS);
		if (result < 0) {
			k_sleep(K_MSEC(SOCKET_RETRY_MS));
			continue;
		}
		for (size_t index = 0U; index < STREAM_COUNT; ++index) {
			struct stream_context *stream = &receiver->streams[index];

			if (stream->fd < 0) {
				continue;
			}
			if ((poll_fds[index].revents & ZSOCK_POLLIN) != 0) {
				if (stream_receive(stream) != 0) {
					socket_error_record(stream);
					(void)zsock_close(stream->fd);
					stream->fd = -1;
				}
			} else if ((poll_fds[index].revents &
				    (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL)) != 0) {
				socket_error_record(stream);
				(void)zsock_close(stream->fd);
				stream->fd = -1;
			}
		}
	}
}

static int stream_configure(struct stream_context *stream, enum stream_kind kind, const char *name,
			    const char *source_address, uint16_t topic_id, uint16_t payload_size,
			    uint16_t port, uint32_t source_node_id, uint32_t maximum_age_ms)
{
	*stream = (struct stream_context){
		.kind = kind,
		.name = name,
		.source_address_text = source_address,
		.topic_id = topic_id,
		.payload_size = payload_size,
		.port = port,
		.source_node_id = source_node_id,
		.maximum_age_ns = (uint64_t)maximum_age_ms * UINT64_C(1000000),
		.fd = -1,
		.stats = {.last_accepted_ms = -1},
	};
	return net_addr_pton(AF_INET6, source_address, &stream->source_address) == 0 ? 0 : -EINVAL;
}

static int receiver_init(void)
{
	int result;

	g_receiver.gnss_readiness.last_sample_ms = -1;
	if (net_addr_pton(AF_INET6, CONFIG_RDD2_SYNAPSE_WIRE_LOCAL_ADDRESS,
			  &g_receiver.local_address) != 0) {
		return -EINVAL;
	}
	result = stream_configure(&g_receiver.streams[STREAM_OPTICAL], STREAM_OPTICAL,
				  "optical_flow", CONFIG_RDD2_SYNAPSE_WIRE_OPTICAL_SOURCE_ADDRESS,
				  synapse_topic_TopicId_OpticalFlowVelocity,
				  (uint16_t)sizeof(synapse_topic_OpticalFlowVelocityData_t),
				  CONFIG_RDD2_SYNAPSE_WIRE_OPTICAL_PORT,
				  CONFIG_RDD2_SYNAPSE_WIRE_OPTICAL_SOURCE_NODE_ID,
				  CONFIG_RDD2_SYNAPSE_WIRE_OPTICAL_MAX_AGE_MS);
	if (result != 0) {
		return result;
	}
	result = stream_configure(
		&g_receiver.streams[STREAM_GNSS], STREAM_GNSS, "gnss_fix",
		CONFIG_RDD2_SYNAPSE_WIRE_GNSS_SOURCE_ADDRESS, synapse_topic_TopicId_GnssFix,
		(uint16_t)sizeof(synapse_topic_GnssFixData_t), CONFIG_RDD2_SYNAPSE_WIRE_GNSS_PORT,
		CONFIG_RDD2_SYNAPSE_WIRE_GNSS_SOURCE_NODE_ID,
		CONFIG_RDD2_SYNAPSE_WIRE_GNSS_MAX_AGE_MS);
	if (result != 0) {
		return result;
	}

	zros_node_init(&g_receiver.node, "synapse_wire_rx");
	result = zros_pub_init(&g_receiver.optical_pub, &g_receiver.node, &topic_optical_flow_vel,
			       &g_receiver.optical);
	if (result != 0) {
		return result;
	}

	memset(&g_receiver.gnss, 0, sizeof(g_receiver.gnss));
	g_receiver.gnss.fix_type = synapse_types_GnssFixType_NoFix;
	g_receiver.gnss.time_status = synapse_types_TimeStatus_LocalFreerun;
	g_receiver.gnss.horizontal_accuracy_mm = UINT16_MAX;
	g_receiver.gnss.vertical_accuracy_mm = UINT16_MAX;
	g_receiver.gnss.velocity_accuracy_mm_s = UINT16_MAX;
	g_receiver.gnss.yaw_accuracy_cdeg = UINT16_MAX;
	result = zros_pub_init(&g_receiver.gnss_pub, &g_receiver.node, &topic_gnss_fix,
			       &g_receiver.gnss);
	if (result != 0 || zros_pub_update(&g_receiver.gnss_pub) != 0) {
		return result != 0 ? result : -EIO;
	}

	k_tid_t thread = k_thread_create(
		&g_receiver.thread, g_stack, K_THREAD_STACK_SIZEOF(g_stack), receiver_run,
		&g_receiver, NULL, NULL, CONFIG_RDD2_SYNAPSE_WIRE_THREAD_PRIORITY, 0, K_FOREVER);

	k_thread_name_set(thread, "synapse_wire_rx");
	k_thread_start(thread);
	LOG_INF("direct optical-flow and GNSS receiver enabled");
	return 0;
}

SYS_INIT(receiver_init, APPLICATION, 92);

static void snapshot_stream(struct rdd2_synapse_wire_stream_stats *out,
			    const struct stream_stats *stats)
{
	out->received = stats->received;
	out->accepted = stats->accepted;
	out->publish_failed = stats->publish_failed;
	out->socket_errors = stats->socket_errors;
	out->sequence_gaps = stats->sequence_gaps;
	out->session_changes = stats->session_changes;
	out->last_sequence = stats->last_sequence;
	out->session_id = stats->session_id;
	out->last_header_flags = stats->last_header_flags;
	out->last_receiver_time_status = stats->last_receiver_time_status;
}

void rdd2_synapse_wire_stats_snapshot(struct rdd2_synapse_wire_snapshot *out)
{
	k_spinlock_key_t key;

	if (out == NULL) {
		return;
	}

	key = k_spin_lock(&g_receiver.lock);
	snapshot_stream(&out->optical, &g_receiver.streams[STREAM_OPTICAL].stats);
	snapshot_stream(&out->gnss, &g_receiver.streams[STREAM_GNSS].stats);
	k_spin_unlock(&g_receiver.lock, key);
}

#if defined(CONFIG_SHELL)
static int status_print(const struct shell *shell, const struct stream_context *stream)
{
	struct stream_stats stats;
	k_spinlock_key_t key = k_spin_lock(&g_receiver.lock);

	stats = stream->stats;
	k_spin_unlock(&g_receiver.lock, key);
	shell_print(shell, "%s [%s]:%u rx=%u accepted=%u pub_fail=%u socket_err=%u", stream->name,
		    stream->source_address_text, stream->port, stats.received, stats.accepted,
		    stats.publish_failed, stats.socket_errors);
	shell_print(shell, "  session=%016llx changes=%u sequence=%u gaps=%u age=%lld ms",
		    (unsigned long long)stats.session_id, stats.session_changes,
		    stats.last_sequence, stats.sequence_gaps,
		    stats.last_accepted_ms < 0
			    ? -1LL
			    : (long long)(k_uptime_get() - stats.last_accepted_ms));
	shell_print(shell,
		    "  time flags=0x%04x receiver_status=%u capture=%llu receive=%llu delta=%lld ns",
		    stats.last_header_flags, stats.last_receiver_time_status,
		    (unsigned long long)stats.last_capture_timestamp_ns,
		    (unsigned long long)stats.last_receive_gptp_ns,
		    (long long)((int64_t)stats.last_receive_gptp_ns -
				(int64_t)stats.last_capture_timestamp_ns));
	for (size_t index = 1U; index < ARRAY_SIZE(stats.rejected); ++index) {
		if (stats.rejected[index] != 0U) {
			shell_print(shell, "  reject %s=%u",
				    synapse_wire_rejection_name((synapse_wire_rejection_t)index),
				    stats.rejected[index]);
		}
	}
	return 0;
}

static int cmd_wire_status(const struct shell *shell, size_t argc, char **argv)
{
	struct gnss_readiness readiness;
	k_spinlock_key_t key;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	for (size_t index = 0U; index < STREAM_COUNT; ++index) {
		(void)status_print(shell, &g_receiver.streams[index]);
	}
	key = k_spin_lock(&g_receiver.lock);
	readiness = g_receiver.gnss_readiness;
	k_spin_unlock(&g_receiver.lock, key);
	shell_print(shell, "gnss usable=%s stable=%u/%u ready=%s", readiness.usable ? "yes" : "no",
		    readiness.stable_samples, GNSS_STABLE_SAMPLES,
		    gnss_ready_from(&readiness, k_uptime_get()) ? "yes" : "no");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_wire,
			       SHELL_CMD(status, NULL, "show direct sensor receiver state",
					 cmd_wire_status),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(wire, &sub_wire, "direct Synapse wire receiver", NULL);
#endif
