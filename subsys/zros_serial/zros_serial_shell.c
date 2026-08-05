/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zros_serial.h"

#include <stdio.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

/* Reads stored counters only; the transport thread is never disturbed. */
static int cmd_zros_serial_status(const struct shell *sh, size_t argc, char **argv)
{
	struct rdd2_zros_serial_stats stats;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rdd2_zros_serial_stats_get(&stats);

	shell_print(sh, "port=%s baud=%u ready=%s init_rc=%d", rdd2_zros_serial_port_name(),
		    (unsigned int)rdd2_zros_serial_baud(),
		    rdd2_zros_serial_ready() ? "yes" : "no", rdd2_zros_serial_init_result());
	shell_print(sh, "rx  frames=%u crc_err=%u bad_len=%u unknown=%u wrong_dir=%u",
		    (unsigned int)stats.rx_frames, (unsigned int)stats.rx_crc_errors,
		    (unsigned int)stats.rx_bad_length, (unsigned int)stats.rx_unknown_topic,
		    (unsigned int)stats.rx_wrong_direction);
	shell_print(sh, "rx  seq_gaps=%u ring_overrun=%u", (unsigned int)stats.rx_seq_gaps,
		    (unsigned int)stats.rx_ring_overrun);
	shell_print(sh, "tx  frames=%u dropped=%u", (unsigned int)stats.tx_frames,
		    (unsigned int)stats.tx_dropped);

	return 0;
}

/*
 * Which UART the radio is actually plugged into is a wiring question the
 * firmware cannot infer, and rebuilding once per candidate is slow. The scan
 * listens on every port named by a zros-scanN alias at once and reports where
 * bytes appear, so one flash answers it.
 */
struct scan_port {
	const struct device *dev;
	const char *name;
};

#define SCAN_ENTRY(idx)                                                                            \
	{DEVICE_DT_GET(DT_ALIAS(csyn_scan##idx)), DT_NODE_FULL_NAME(DT_ALIAS(csyn_scan##idx))},

/* NULL-terminated, so the table is never a zero-length array. */
static const struct scan_port g_scan_ports[] = {
#if DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(csyn_scan0))
	SCAN_ENTRY(0)
#endif
#if DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(csyn_scan1))
	SCAN_ENTRY(1)
#endif
#if DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(csyn_scan2))
	SCAN_ENTRY(2)
#endif
#if DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(csyn_scan3))
	SCAN_ENTRY(3)
#endif
	{NULL, NULL},
};

/*
 * Minimum marker hits before a port is called a protocol rather than noise.
 * A receiver at its true rate produces tens of markers in a few seconds, so a
 * handful means partial framing on a near-miss rate, not a decode. Set well
 * above the chance rate (~1 per 64 kB) so a marginal count reads as unknown
 * rather than as a confident answer.
 */
#define SCAN_MIN_HITS 12U

struct scan_state {
	uint32_t bytes;
	uint32_t syncs;
	uint32_t nmea;
	uint32_t ubx;
	uint8_t prev;
	bool open;
	bool reclaimed;
	bool saved_ok;
	struct uart_config saved;
};

static void scan_port_open(const struct scan_port *port, struct scan_state *st, uint32_t baud)
{
	struct uart_config cfg;

	/* Taking RX interrupts back keeps the transport from eating the very
	 * bytes the scan is looking for on its own port. */
	if (rdd2_zros_serial_owns(port->dev)) {
		uart_irq_rx_disable(port->dev);
		st->reclaimed = true;
	}

	st->saved_ok = uart_config_get(port->dev, &st->saved) == 0;
	if (st->saved_ok) {
		cfg = st->saved;
		cfg.baudrate = baud;
		(void)uart_configure(port->dev, &cfg);
	}

	st->open = true;
}

static void scan_port_close(const struct scan_port *port, struct scan_state *st)
{
	if (!st->open) {
		return;
	}

	if (st->saved_ok) {
		(void)uart_configure(port->dev, &st->saved);
	}

	/* Only ever re-enable a port the transport had open: enabling RX
	 * interrupts on a device with no callback installed invites an
	 * unhandled interrupt. */
	if (st->reclaimed) {
		uart_irq_rx_enable(port->dev);
	}
}

static int cmd_zros_serial_scan(const struct shell *sh, size_t argc, char **argv)
{
	struct scan_state state[ARRAY_SIZE(g_scan_ports)] = {0};
	size_t count = ARRAY_SIZE(g_scan_ports) - 1U;
	uint32_t seconds = argc > 1U ? (uint32_t)strtoul(argv[1], NULL, 10) : 5U;
	uint32_t baud = argc > 2U ? (uint32_t)strtoul(argv[2], NULL, 10) : 57600U;
	int64_t deadline;
	uint32_t total = 0U;

	if (count == 0U) {
		shell_warn(sh, "no zros-scanN devicetree aliases defined");
		return 0;
	}

	if (seconds == 0U || seconds > 60U) {
		shell_error(sh, "seconds must be 1..60");
		return -EINVAL;
	}

	/* uart_configure() divides by the baud, so zero reaches the LPUART
	 * clock calculation as a divide by zero. */
	if (baud < 1200U || baud > 3000000U) {
		shell_error(sh, "baud must be 1200..3000000");
		return -EINVAL;
	}

	/* Must happen before any port is reconfigured: the transport and this
	 * command otherwise race on the same peripheral register. */
	rdd2_zros_serial_pause(true);

	for (size_t i = 0U; i < count; i++) {
		if (!device_is_ready(g_scan_ports[i].dev)) {
			shell_warn(sh, "%s not ready, skipping", g_scan_ports[i].name);
			continue;
		}
		scan_port_open(&g_scan_ports[i], &state[i], baud);
	}

	shell_print(sh, "listening on %u ports for %u s at %u baud; send traffic now",
		    (unsigned int)count, (unsigned int)seconds, (unsigned int)baud);

	deadline = k_uptime_get() + (int64_t)seconds * 1000;
	while (k_uptime_get() < deadline) {
		for (size_t i = 0U; i < count; i++) {
			uint8_t c;

			if (!state[i].open) {
				continue;
			}

			while (uart_poll_in(g_scan_ports[i].dev, &c) == 0) {
				state[i].bytes++;
				if (state[i].prev == RDD2_ZROS_SERIAL_SYNC0 &&
				    c == RDD2_ZROS_SERIAL_SYNC1) {
					state[i].syncs++;
				}
				/*
				 * A GNSS receiver at the right baud produces a
				 * steady stream of "$Gx" / "$Px" sentence
				 * starts. At the wrong baud the same wire is
				 * still busy, so byte count alone proves
				 * nothing - this is what separates a rate that
				 * decodes from one that only receives noise.
				 */
				if (state[i].prev == '$' && (c == 'G' || c == 'P')) {
					state[i].nmea++;
				}
				/*
				 * u-blox binary framing. A receiver talking UBX
				 * at the right baud looks identical to noise by
				 * byte count alone, and the NMEA driver cannot
				 * parse any of it, so it is worth telling apart
				 * from a wrong rate.
				 */
				if (state[i].prev == 0xb5U && c == 0x62U) {
					state[i].ubx++;
				}
				state[i].prev = c;
			}
		}
		k_sleep(K_MSEC(1));
	}

	for (size_t i = 0U; i < count; i++) {
		scan_port_close(&g_scan_ports[i], &state[i]);
	}

	rdd2_zros_serial_pause(false);

	shell_print(sh, "%-10s %-20s %8s %6s %6s %6s  %s", "alias", "node", "bytes", "sync", "nmea",
		    "ubx", "verdict");
	for (size_t i = 0U; i < count; i++) {
		/*
		 * Both markers are two-byte sequences, so noise on a wire read
		 * at the wrong baud hits them by chance roughly once per 64 kB.
		 * Requiring several, and reporting whichever is stronger, keeps
		 * a single accidental match from naming the wrong protocol.
		 */
		const uint32_t sync_hits = state[i].syncs;
		const uint32_t nmea_hits = state[i].nmea;
		const uint32_t ubx_hits = state[i].ubx;
		const char *verdict = "silent";

		if (!state[i].open) {
			verdict = "not scanned (device not ready)";
		} else if (sync_hits >= SCAN_MIN_HITS && sync_hits >= nmea_hits &&
			   sync_hits >= ubx_hits) {
			verdict = "SYNAPSE FRAMES -- point zros-serial here";
		} else if (nmea_hits >= SCAN_MIN_HITS && nmea_hits >= ubx_hits) {
			verdict = "NMEA -- point the gnss node here at this baud";
		} else if (ubx_hits >= SCAN_MIN_HITS) {
			verdict = "UBX binary -- right baud, but the NMEA driver cannot read it";
		} else if (nmea_hits > 0U || ubx_hits > 0U) {
			verdict = "a few markers only -- near miss, try neighbouring rates";
		} else if (state[i].bytes > 0U) {
			verdict = "bytes but nothing decodes -- wrong baud";
		}

		total += state[i].bytes;
		shell_print(sh, "zros-scan%-3u %-20s %8u %6u %6u %6u  %s", (unsigned int)i,
			    g_scan_ports[i].name, (unsigned int)state[i].bytes,
			    (unsigned int)state[i].syncs, (unsigned int)state[i].nmea,
			    (unsigned int)state[i].ubx, verdict);
	}

	if (total == 0U) {
		shell_print(sh, "nothing on any port: check wiring, radio pairing, and that "
				"the sender is running");
	}

	return 0;
}

/*
 * Marker counting can only recognise protocols it was taught. When a port is
 * clearly busy but nothing decodes, the bytes themselves are the answer: ASCII
 * means a misframed text protocol, b5 62 means UBX, d3 means RTCM, and evenly
 * distributed high bits mean the rate is simply wrong.
 */
static int cmd_zros_serial_dump(const struct shell *sh, size_t argc, char **argv)
{
	static uint8_t buf[256];
	struct scan_state state = {0};
	size_t idx = (size_t)strtoul(argv[1], NULL, 10);
	uint32_t baud = (uint32_t)strtoul(argv[2], NULL, 10);
	size_t want = argc > 3U ? (size_t)strtoul(argv[3], NULL, 10) : 128U;
	size_t got = 0U;
	int64_t deadline;

	if (idx >= ARRAY_SIZE(g_scan_ports) - 1U) {
		shell_error(sh, "port index must be 0..%u",
			    (unsigned int)(ARRAY_SIZE(g_scan_ports) - 2U));
		return -EINVAL;
	}
	if (baud < 1200U || baud > 3000000U) {
		shell_error(sh, "baud must be 1200..3000000");
		return -EINVAL;
	}
	want = MIN(want, sizeof(buf));

	if (!device_is_ready(g_scan_ports[idx].dev)) {
		shell_error(sh, "%s not ready", g_scan_ports[idx].name);
		return -ENODEV;
	}

	rdd2_zros_serial_pause(true);
	scan_port_open(&g_scan_ports[idx], &state, baud);

	deadline = k_uptime_get() + 5000;
	while (got < want && k_uptime_get() < deadline) {
		uint8_t c;

		while (got < want && uart_poll_in(g_scan_ports[idx].dev, &c) == 0) {
			buf[got++] = c;
		}
		k_sleep(K_MSEC(1));
	}

	scan_port_close(&g_scan_ports[idx], &state);
	rdd2_zros_serial_pause(false);

	shell_print(sh, "%s @ %u baud: %u bytes", g_scan_ports[idx].name, (unsigned int)baud,
		    (unsigned int)got);
	for (size_t off = 0U; off < got; off += 16U) {
		char hex[16 * 3 + 1];
		char txt[17];
		size_t n = MIN((size_t)16U, got - off);

		for (size_t i = 0U; i < n; i++) {
			uint8_t c = buf[off + i];

			(void)snprintf(&hex[i * 3], 4, "%02x ", c);
			txt[i] = (c >= 0x20U && c < 0x7fU) ? (char)c : '.';
		}
		hex[n * 3] = '\0';
		txt[n] = '\0';
		shell_print(sh, "%04x  %-48s |%s|", (unsigned int)off, hex, txt);
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_zros_serial,
			       SHELL_CMD(status, NULL, "serial link counters",
					 cmd_zros_serial_status),
			       SHELL_CMD_ARG(scan, NULL,
					     "find the radio's port: scan [seconds] [baud]",
					     cmd_zros_serial_scan, 1, 2),
			       SHELL_CMD_ARG(dump, NULL,
					     "hex dump a port: dump <scanN> <baud> [bytes]",
					     cmd_zros_serial_dump, 3, 1),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(zros_serial, &sub_zros_serial, "zros serial transport diagnostics", NULL);
