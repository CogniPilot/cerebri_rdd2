#ifndef RDD2_CSYN_SERIAL_H_
#define RDD2_CSYN_SERIAL_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

/*
 * Compact synapse serial framing, the encoding fbs/transport.fbs points
 * constrained byte-stream links at instead of the multiplexed Frame table:
 *
 *   off  size  field
 *    0     2   sync      0x53 0x59 ('S','Y')
 *    2     2   len       u16 LE, payload byte count
 *    4     2   topic_id  u16 LE, synapse catalog TopicId
 *    6     1   seq       u8, wraps, for loss detection
 *    7     1   flags     u8, reserved, zero
 *    8     N   payload   bare fixed-layout struct, exactly as stored by CSyn
 *   8+N    2   crc16     CRC-16/CCITT-FALSE over bytes [2, 8+N)
 *
 * The payload is the same byte image the Zenoh transport publishes, so a
 * frame carries no schema of its own: topic_id resolves through the pinned
 * synapse_fbs catalog and the payload size is checked against it.
 */
#define RDD2_CSYN_SERIAL_SYNC0        0x53U
#define RDD2_CSYN_SERIAL_SYNC1        0x59U
#define RDD2_CSYN_SERIAL_HEADER_SIZE  8U
#define RDD2_CSYN_SERIAL_TRAILER_SIZE 2U
#define RDD2_CSYN_SERIAL_CRC_SEED     0xffffU

struct rdd2_csyn_serial_stats {
	uint32_t rx_frames;
	uint32_t rx_crc_errors;
	uint32_t rx_unknown_topic;
	uint32_t rx_wrong_direction;
	uint32_t rx_bad_length;
	uint32_t rx_ring_overrun;
	uint32_t rx_seq_gaps;
	uint32_t tx_frames;
	uint32_t tx_dropped;
	uint32_t tx_oversize;
};

bool rdd2_csyn_serial_ready(void);
/* Zero when up, otherwise the negative errno that stopped initialisation. */
int rdd2_csyn_serial_init_result(void);
/* True when the transport holds this device with RX interrupts enabled. A
 * port scan must take it back for the duration of the scan and hand it over
 * again afterwards. */
bool rdd2_csyn_serial_owns(const struct device *dev);
/* Stop the transport touching its UART so a port scan can reconfigure and
 * poll it safely. Drops any queued outbound frames. */
void rdd2_csyn_serial_pause(bool pause);
void rdd2_csyn_serial_stats_get(struct rdd2_csyn_serial_stats *stats);
const char *rdd2_csyn_serial_port_name(void);
uint32_t rdd2_csyn_serial_baud(void);

#endif
