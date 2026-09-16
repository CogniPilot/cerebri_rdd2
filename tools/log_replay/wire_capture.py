#!/usr/bin/env python3
"""Bind the GNSS and optical-flow wire ports and print decoded Synapse wire
frames. Companion to wire_replay.py for a loopback self-test: run this on ::1,
send with `wire_replay.py LOG.mcap --dest ::1`, and compare the frame counts
and sequence gaps.

This decodes and reports only. It does not run the firmware's carrier, binding,
freshness or payload validation; those live on the flight controller.
"""
import argparse
import collections
import socket
import struct
import sys
import time

WIRE_MAGIC = 0x53594E57
WIRE_VERSION = 1
WIRE_HEADER_SIZE = 44
_HEADER = struct.Struct(">IHHIIQQHHQ")

TOPIC_NAMES = {8: "gnss_fix", 10: "optical_flow_vel"}


def decode_header(datagram):
    if len(datagram) < WIRE_HEADER_SIZE:
        return None
    (magic, version, topic_id, node_id, sequence, capture_ts, schema_set,
     payload_len, flags, session_id) = _HEADER.unpack_from(datagram, 0)
    return {
        "magic": magic,
        "version": version,
        "topic_id": topic_id,
        "node_id": node_id,
        "sequence": sequence,
        "capture_ts": capture_ts,
        "schema_set": schema_set,
        "payload_len": payload_len,
        "flags": flags,
        "session_id": session_id,
        "payload": datagram[WIRE_HEADER_SIZE:],
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bind", default="::1", help="local IPv6 address (default ::1)")
    ap.add_argument("--gnss-port", type=int, default=46008)
    ap.add_argument("--flow-port", type=int, default=46010)
    ap.add_argument("--count", type=int, default=0,
                    help="stop after this many frames (0 = until interrupted)")
    ap.add_argument("--timeout", type=float, default=0.0,
                    help="stop after this many seconds of silence (0 = never)")
    ap.add_argument("--quiet", action="store_true",
                    help="only print the closing summary")
    args = ap.parse_args()

    ports = {args.gnss_port: "gnss", args.flow_port: "flow"}
    socks = {}
    for port in ports:
        sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((args.bind, port))
        sock.setblocking(False)
        socks[sock] = port

    received = collections.Counter()
    accepted = collections.Counter()
    gaps = collections.Counter()
    sessions = {}
    last_seq = {}
    print("listening on [%s] gnss:%d flow:%d" % (args.bind, args.gnss_port,
                                                 args.flow_port),
          file=sys.stderr)
    import select
    last_rx = time.monotonic()
    total = 0
    try:
        while True:
            if args.count and total >= args.count:
                break
            if args.timeout and (time.monotonic() - last_rx) > args.timeout:
                break
            readable, _, _ = select.select(list(socks), [], [], 0.25)
            for sock in readable:
                port = socks[sock]
                datagram, _peer = sock.recvfrom(65535)
                last_rx = time.monotonic()
                stream = ports[port]
                received[stream] += 1
                total += 1
                header = decode_header(datagram)
                if header is None or header["magic"] != WIRE_MAGIC or \
                        header["version"] != WIRE_VERSION:
                    if not args.quiet:
                        print("%-4s bad frame (%d bytes)" % (stream, len(datagram)))
                    continue
                if len(header["payload"]) != header["payload_len"]:
                    if not args.quiet:
                        print("%-4s length mismatch payload=%d hdr=%d"
                              % (stream, len(header["payload"]),
                                 header["payload_len"]))
                    continue
                accepted[stream] += 1
                sess = header["session_id"]
                sessions[stream] = sess
                key = (stream, sess)
                if key in last_seq:
                    delta = (header["sequence"] - last_seq[key]) & 0xFFFFFFFF
                    if delta > 1:
                        gaps[stream] += delta - 1
                last_seq[key] = header["sequence"]
                if not args.quiet:
                    payload_ts = struct.unpack_from("<Q", header["payload"], 0)[0]
                    print("%-4s topic=%d node=%d seq=%u sess=0x%016x "
                          "flags=0x%04x capture=%d payload_ts=%d len=%d"
                          % (stream, header["topic_id"], header["node_id"],
                             header["sequence"], sess, header["flags"],
                             header["capture_ts"], payload_ts,
                             header["payload_len"]))
    except KeyboardInterrupt:
        pass
    finally:
        for sock in socks:
            sock.close()

    print("--- summary ---")
    for stream in ("gnss", "flow"):
        print("%-4s received=%d accepted=%d gaps=%d session=%s"
              % (stream, received[stream], accepted[stream], gaps[stream],
                 ("0x%016x" % sessions[stream]) if stream in sessions else "-"))


if __name__ == "__main__":
    main()
