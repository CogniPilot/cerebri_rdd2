/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sitl_transport.h"
#include "topic_flatbuffer.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(rdd2_sitl_udp, LOG_LEVEL_INF);

static int g_sitl_rx_sock = -1;
static int g_sitl_tx_sock = -1;
static struct sockaddr_in g_sitl_tx_flight_addr;
static struct sockaddr_in g_sitl_tx_motor_addr;
static K_THREAD_STACK_DEFINE(g_sitl_thread_stack, CONFIG_RDD2_SITL_THREAD_STACK_SIZE);
static struct k_thread g_sitl_thread;

static int sitl_socket_set_nonblocking(int sock)
{
	int flags = fcntl(sock, F_GETFL, 0);

	if (flags < 0) {
		return -errno;
	}

	if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
		return -errno;
	}

	return 0;
}

static int sitl_socket_init(int *sock, uint16_t bind_port)
{
	struct sockaddr_in addr = {0};
	int rc;

	*sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (*sock < 0) {
		return -errno;
	}

	rc = sitl_socket_set_nonblocking(*sock);
	if (rc != 0) {
		close(*sock);
		*sock = -1;
		return rc;
	}

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(bind_port);

	if (bind_port != 0U && bind(*sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		rc = -errno;
		close(*sock);
		*sock = -1;
		return rc;
	}

	return 0;
}

static int sitl_destination_init(struct sockaddr_in *addr, uint16_t port)
{
	memset(addr, 0, sizeof(*addr));
	addr->sin_family = AF_INET;
	addr->sin_port = htons(port);

	if (inet_pton(AF_INET, CONFIG_RDD2_SITL_HOST, &addr->sin_addr) != 1) {
		return -EINVAL;
	}

	return 0;
}

static void sitl_rx_drain(void)
{
	struct sockaddr_in source_addr;
	socklen_t source_addr_len = sizeof(source_addr);
	uint8_t buf[RDD2_SITL_INPUT_MAX_SIZE];

	while (true) {
		ssize_t len = recvfrom(g_sitl_rx_sock, buf, sizeof(buf), 0,
				       (struct sockaddr *)&source_addr, &source_addr_len);

		if (len < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				LOG_WRN("sitl rx failed: %d", errno);
			}
			break;
		}

		(void)rdd2_sitl_handle_input_blob(buf, (size_t)len);
		source_addr_len = sizeof(source_addr);
	}
}

static void sitl_send_flight_state_if_updated(void)
{
	static uint32_t last_generation;
	uint8_t buf[RDD2_TOPIC_FB_FLIGHT_STATE_SIZE];
	size_t len;

	if (!rdd2_sitl_flight_state_blob_if_updated(&last_generation, buf, sizeof(buf), &len)) {
		return;
	}

	(void)sendto(g_sitl_tx_sock, buf, len, 0, (struct sockaddr *)&g_sitl_tx_flight_addr,
		     sizeof(g_sitl_tx_flight_addr));
}

static void sitl_send_motor_output_if_updated(void)
{
	static uint32_t last_generation;
	uint8_t buf[RDD2_TOPIC_FB_MOTOR_OUTPUT_SIZE];
	size_t len;

	if (!rdd2_sitl_motor_output_blob_if_updated(&last_generation, buf, sizeof(buf), &len)) {
		return;
	}

	(void)sendto(g_sitl_tx_sock, buf, len, 0, (struct sockaddr *)&g_sitl_tx_motor_addr,
		     sizeof(g_sitl_tx_motor_addr));
}

static void sitl_transport_thread(void *arg0, void *arg1, void *arg2)
{
	ARG_UNUSED(arg0);
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);

	while (true) {
		sitl_rx_drain();
		sitl_send_flight_state_if_updated();
		sitl_send_motor_output_if_updated();
		k_sleep(K_MSEC(1));
	}
}

static int rdd2_sitl_init(void)
{
	int rc;

	rc = sitl_socket_init(&g_sitl_rx_sock, CONFIG_RDD2_SITL_RX_PORT);
	if (rc != 0) {
		LOG_ERR("sitl rx socket init failed: %d", -rc);
		return rc;
	}

	rc = sitl_socket_init(&g_sitl_tx_sock, 0U);
	if (rc != 0) {
		LOG_ERR("sitl tx socket init failed: %d", -rc);
		close(g_sitl_rx_sock);
		g_sitl_rx_sock = -1;
		return rc;
	}

	rc = sitl_destination_init(&g_sitl_tx_flight_addr,
				   CONFIG_RDD2_SITL_TX_FLIGHT_SNAPSHOT_PORT);
	if (rc != 0) {
		LOG_ERR("sitl flight destination init failed");
		return rc;
	}

	rc = sitl_destination_init(&g_sitl_tx_motor_addr, CONFIG_RDD2_SITL_TX_MOTOR_OUTPUT_PORT);
	if (rc != 0) {
		LOG_ERR("sitl motor destination init failed");
		return rc;
	}

	k_thread_create(&g_sitl_thread, g_sitl_thread_stack,
			K_THREAD_STACK_SIZEOF(g_sitl_thread_stack), sitl_transport_thread, NULL,
			NULL, NULL, CONFIG_RDD2_SITL_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&g_sitl_thread, "rdd2_sitl");

	LOG_INF("sitl udp rx=%d tx_flight=%d tx_motor=%d host=%s", CONFIG_RDD2_SITL_RX_PORT,
		CONFIG_RDD2_SITL_TX_FLIGHT_SNAPSHOT_PORT, CONFIG_RDD2_SITL_TX_MOTOR_OUTPUT_PORT,
		CONFIG_RDD2_SITL_HOST);

	return 0;
}

SYS_INIT(rdd2_sitl_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
