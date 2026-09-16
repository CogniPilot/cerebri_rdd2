#!/usr/bin/env python3
"""Replay recorded GNSS and optical-flow samples to the RDD2 firmware as
Synapse wire frames over UDP/IPv6, the same path the vehicle uses in flight.

The flight controller receives these two sensor streams through the direct
Synapse wire receiver (subsys/synapse_wire/synapse_wire.c): a UDP/IPv6 socket
per stream carrying a fixed 44-byte big-endian header (synapse/wire.h v1)
followed by the bare fixed-layout payload struct. This tool reads the
gnss_fix and optical_flow_vel channels out of a flight log, re-wraps each
recorded payload in a fresh wire header (new session id, contiguous sequence
numbers), re-stamps the timestamps into a domain the receiver and the GPS
adapter accept, and sends the frames paced by the log accept times.

See README.md for the frame layout, the stamping rule, and how to point the
tool at a native_sim or QEMU target.
"""
import argparse
import collections
import os
import random
import socket
import struct
import sys
import time

# --- Synapse wire v1 header contract (synapse/wire.h, big-endian on the wire).
WIRE_MAGIC = 0x53594E57           # "SYNW"
WIRE_VERSION = 1
WIRE_HEADER_SIZE = 44
SCHEMA_SET_ID = 0x232721F0EE5B6C32  # SYNAPSE_SCHEMA_SET_WIRE_ID in synapse_wire.c
FLAG_CAPTURE_TIME_GPTP_SYNCED = 0x0001
_HEADER = struct.Struct(">IHHIIQQHHQ")  # 44 bytes, see pack_header()

# TimeStatus enum (types.fbs).
TIME_LOCAL_FREERUN = 0
TIME_GPTP_SYNCED = 1

# Per-stream deployment bindings. Defaults mirror boards/mr_vmu_tropic.conf and
# the subsys/synapse_wire/Kconfig defaults.
STREAMS = {
    "gnss": {
        "channel": "gnss_fix",
        "topic_id": 8,          # synapse_topic_TopicId_GnssFix
        "payload_size": 64,     # sizeof(synapse_topic_GnssFixData_t)
        "node_id": 11,          # CONFIG_RDD2_SYNAPSE_WIRE_GNSS_SOURCE_NODE_ID
        "default_port": 46008,  # CONFIG_RDD2_SYNAPSE_WIRE_GNSS_PORT
        "time_status_off": 56,  # GnssFixData.time_status byte offset
    },
    "flow": {
        "channel": "optical_flow_vel",
        "topic_id": 10,         # synapse_topic_TopicId_OpticalFlowVelocity
        "payload_size": 32,     # sizeof(synapse_topic_OpticalFlowVelocityData_t)
        "node_id": 12,          # CONFIG_RDD2_SYNAPSE_WIRE_OPTICAL_SOURCE_NODE_ID
        "default_port": 46010,  # CONFIG_RDD2_SYNAPSE_WIRE_OPTICAL_PORT
        "time_status_off": 30,  # OpticalFlowVelocityData.time_status byte offset
    },
}

# control_imu fixed-layout struct (InertialSampleData): ts u8, ax ay az f4,
# gx gy gz f4, temp f4, flags u1, time_status u1, id u1, pad u1 = 40 bytes.
_IMU = struct.Struct("<Q7fBBBB")


def read_channels(path, wanted):
    """Walk an uncompressed, index-less synapse/1 MCAP and return, per wanted
    channel name, a list of (log_time_ns, raw_payload_bytes). The payload is
    the bare fixed-layout struct pulled out of its one-field FlatBuffer root
    (struct bytes start at root+4), exactly the bytes the wire carried.
    """
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:8] != b"\x89MCAP0\r\n":
        raise SystemExit("not an MCAP file: %s" % path)
    channels = {}
    out = {name: [] for name in wanted}
    pos = 8
    end = len(data)
    while pos + 9 <= end:
        op = data[pos]
        length = struct.unpack_from("<Q", data, pos + 1)[0]
        body = data[pos + 9:pos + 9 + length]
        pos += 9 + length
        if op == 0x04:  # Channel
            cid = struct.unpack_from("<H", body, 0)[0]
            topic_len = struct.unpack_from("<I", body, 4)[0]
            channels[cid] = body[8:8 + topic_len].decode()
        elif op == 0x05:  # Message
            cid, _seq, log_time, _pub = struct.unpack_from("<HIQQ", body, 0)
            name = channels.get(cid)
            if name not in out:
                continue
            payload = body[22:]
            root = struct.unpack_from("<I", payload, 0)[0]
            vtable = root - struct.unpack_from("<i", payload, root)[0]
            size = struct.unpack_from("<H", payload, vtable + 2)[0]
            out[name].append((log_time, payload[root + 4:root + size]))
        elif op == 0x02:  # DataEnd
            break
    return out


def pack_header(topic_id, node_id, sequence, capture_ts_ns, payload_len, flags,
                session_id):
    return _HEADER.pack(WIRE_MAGIC, WIRE_VERSION, topic_id, node_id, sequence,
                        capture_ts_ns, SCHEMA_SET_ID, payload_len, flags,
                        session_id)


def restamp_payload(raw, time_status_off, payload_ts_ns, time_status):
    """Return the payload with timestamp_ns (offset 0) and the time_status
    byte overwritten; every other recorded field is left untouched."""
    buf = bytearray(raw)
    struct.pack_into("<Q", buf, 0, payload_ts_ns)
    buf[time_status_off] = time_status
    return bytes(buf)


def build_schedule(channels, streams, rate, start_s, end_s):
    """Merge the selected streams into one send schedule sorted by log accept
    time, rebased so the earliest selected frame is t=0. Returns a list of
    dicts and the rebase origin in log-time ns."""
    events = []
    for key, cfg in streams.items():
        for log_time, raw in channels[cfg["channel"]]:
            if len(raw) != cfg["payload_size"]:
                raise SystemExit(
                    "%s: payload %d bytes, expected %d"
                    % (cfg["channel"], len(raw), cfg["payload_size"]))
            events.append([log_time, key, cfg, raw])
    if not events:
        raise SystemExit("no gnss_fix or optical_flow_vel frames in log")
    events.sort(key=lambda item: item[0])
    origin = events[0][0]
    schedule = []
    for log_time, key, cfg, raw in events:
        rel_s = (log_time - origin) / 1e9
        if start_s is not None and rel_s < start_s:
            continue
        if end_s is not None and rel_s > end_s:
            continue
        schedule.append({
            "rel_ns": log_time - origin,
            "send_at_s": rel_s / rate,
            "key": key,
            "cfg": cfg,
            "raw": raw,
        })
    return schedule, origin


def make_frames(schedule, time_base, freerun_offset_ns=0):
    """Assign a session id and per-stream sequence numbers, re-stamp each
    payload and return (metadata, datagram bytes) in send order."""
    session_id = random.getrandbits(64) or 1
    sequence = {key: 0 for key in STREAMS}
    frames = []
    for item in schedule:
        cfg = item["cfg"]
        sequence[item["key"]] += 1
        seq = sequence[item["key"]]
        if time_base == "verbatim":
            # Keep the recorded per-frame time_status so the freerun -> gPTP
            # transition is reproduced at the logged instant, and carry the
            # producer clock's offset from the receiver boot clock so the GPS
            # adapter future gate is exercised the way it was in flight.
            recorded_status = item["raw"][cfg["time_status_off"]]
            if recorded_status == TIME_GPTP_SYNCED:
                # No gPTP master is emulated on the BIL wire, so align the
                # synced-phase stamp to the receiver boot clock (offset 0).
                payload_ts = item["rel_ns"] or 1
                time_status = TIME_GPTP_SYNCED
                capture_ts = payload_ts
                flags = FLAG_CAPTURE_TIME_GPTP_SYNCED
            else:
                # Producer freerun clock ran ahead of the receiver boot clock;
                # replay that lead so freerun fixes land in the future gate.
                payload_ts = (item["rel_ns"] + freerun_offset_ns) or 1
                time_status = TIME_LOCAL_FREERUN
                capture_ts = 0
                flags = 0
        elif time_base == "synced":
            payload_ts = time.time_ns()
            time_status = TIME_GPTP_SYNCED
            capture_ts = payload_ts
            flags = FLAG_CAPTURE_TIME_GPTP_SYNCED
        else:  # freerun
            # Boot-relative monotonic stamp; the receiver freerun path requires
            # capture_timestamp_ns == 0 with the flag clear, and the payload
            # timestamp_ns must be non-zero.
            payload_ts = item["rel_ns"] or 1
            time_status = TIME_LOCAL_FREERUN
            capture_ts = 0
            flags = 0
        payload = restamp_payload(item["raw"], cfg["time_status_off"],
                                  payload_ts, time_status)
        header = pack_header(cfg["topic_id"], cfg["node_id"], seq, capture_ts,
                             cfg["payload_size"], flags, session_id)
        frames.append({
            "key": item["key"],
            "port": cfg["port"],
            "seq": seq,
            "payload_ts": payload_ts,
            "send_at_s": item["send_at_s"],
            "datagram": header + payload,
        })
    return session_id, frames


def open_socket(dest, port_of_stream):
    """One UDP/IPv6 socket, hop limit pinned to 1 (the receiver's carrier
    policy rejects anything else). For a non-loopback destination the source
    port is bound to the stream port so the receiver's binding check passes;
    that bind is skipped on ::1 so the loopback self-test capturer can hold
    the port."""
    family_dest = dest.split("%", 1)[0]
    loopback = family_dest in ("::1",)
    sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_UNICAST_HOPS, 1)
    if not loopback:
        try:
            sock.bind(("::", port_of_stream))
        except OSError as exc:
            print("note: could not bind source port %d (%s); using ephemeral"
                  % (port_of_stream, exc), file=sys.stderr)
    return sock


def resolve_dest(dest, port):
    infos = socket.getaddrinfo(dest, port, socket.AF_INET6, socket.SOCK_DGRAM)
    return infos[0][4]


def run_send(frames, dest, rate):
    socks = {}
    dests = {}
    for key, cfg in STREAMS.items():
        socks[key] = open_socket(dest, cfg["port"])
    counts = collections.Counter()
    t0 = time.monotonic()
    for frame in frames:
        target = frame["send_at_s"]
        while True:
            slack = target - (time.monotonic() - t0)
            if slack <= 0:
                break
            time.sleep(min(slack, 0.05))
        key = frame["key"]
        if key not in dests:
            dests[key] = resolve_dest(dest, frame["port"])
        socks[key].sendto(frame["datagram"], dests[key])
        counts[key] += 1
    for sock in socks.values():
        sock.close()
    return counts


def run_dry_run(frames, session_id, out_path):
    """Write every datagram, length-prefixed (u16 LE length + bytes), to a
    file and print a per-frame summary. No socket, no pacing."""
    counts = collections.Counter()
    with open(out_path, "wb") as handle:
        for frame in frames:
            dg = frame["datagram"]
            handle.write(struct.pack("<H", len(dg)))
            handle.write(dg)
            counts[frame["key"]] += 1
    print("session_id 0x%016x" % session_id)
    print("wrote %d frames to %s" % (sum(counts.values()), out_path))
    for key, cfg in STREAMS.items():
        if counts[key]:
            print("  %-4s topic=%d port=%d frames=%d"
                  % (key, cfg["topic_id"], cfg["port"], counts[key]))
    # First frame of each stream, header byte dump for inspection.
    seen = set()
    for frame in frames:
        if frame["key"] in seen:
            continue
        seen.add(frame["key"])
        head = frame["datagram"][:WIRE_HEADER_SIZE]
        print("  %-4s first header: %s" % (frame["key"], head.hex()))
    return counts


def write_imu(path, channels):
    """Undo the FLU body mapping from src/interfaces/imu.c so the samples read
    back in ICM45686 sensor axes. See README.md for the record layout."""
    rows = channels.get("control_imu")
    if not rows:
        raise SystemExit("no control_imu channel in log for --imu-out")
    rows = sorted(rows, key=lambda item: item[0])
    t0 = struct.unpack_from("<Q", rows[0][1], 0)[0]
    as_csv = path.endswith(".csv")
    record = struct.Struct("<Q7f")  # t_ns, ax ay az, gx gy gz (sensor), temp
    with open(path, "w" if as_csv else "wb") as handle:
        if as_csv:
            handle.write("t_ns,ax_m_s2,ay_m_s2,az_m_s2,"
                         "gx_rad_s,gy_rad_s,gz_rad_s,temp_c\n")
        for _log_time, raw in rows:
            ts, ax, ay, az, gx, gy, gz, temp, _flags, _ts_status, _id, _pad = \
                _IMU.unpack_from(raw, 0)
            # Body FLU -> sensor axes: body.x = sensor.y, body.y = -sensor.x,
            # body.z = sensor.z, so sensor.x = -body.y, sensor.y = body.x.
            sax, say, saz = -ay, ax, az
            sgx, sgy, sgz = -gy, gx, gz
            t_ns = ts - t0
            if as_csv:
                handle.write("%d,%.7f,%.7f,%.7f,%.7f,%.7f,%.7f,%.4f\n"
                             % (t_ns, sax, say, saz, sgx, sgy, sgz, temp))
            else:
                handle.write(record.pack(t_ns, sax, say, saz, sgx, sgy, sgz,
                                         temp))
    print("wrote %d IMU samples to %s (%s, sensor axes)"
          % (len(rows), path, "csv" if as_csv else "binary"))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", help="flight log (synapse/1 MCAP)")
    ap.add_argument("--dest", help="receiver IPv6 address, e.g. ::1 or "
                    "fe80::4:9fff:fe00:150%%eth0 (required unless --dry-run)")
    ap.add_argument("--gnss-port", type=int,
                    default=STREAMS["gnss"]["default_port"],
                    help="GNSS UDP port (default %d)"
                    % STREAMS["gnss"]["default_port"])
    ap.add_argument("--flow-port", type=int,
                    default=STREAMS["flow"]["default_port"],
                    help="optical-flow UDP port (default %d)"
                    % STREAMS["flow"]["default_port"])
    ap.add_argument("--rate", type=float, default=1.0,
                    help="playback speed multiplier (default 1.0)")
    ap.add_argument("--start", type=float, default=None,
                    help="skip frames before T0 seconds from the first frame")
    ap.add_argument("--end", type=float, default=None,
                    help="skip frames after T1 seconds from the first frame")
    ap.add_argument("--time-base", choices=["freerun", "synced", "verbatim"],
                    default="freerun",
                    help="freerun: boot-relative monotonic stamp, header "
                    "unsynced (default). synced: unix-epoch stamp with the "
                    "gPTP flag set, for a gPTP-disciplined target. verbatim: "
                    "keep each frame's recorded time_status (reproducing the "
                    "freerun->gPTP transition) and apply --freerun-offset-ns "
                    "to freerun stamps so the producer clock lead is replayed.")
    ap.add_argument("--freerun-offset-ns", type=int, default=356_000_000,
                    help="verbatim time-base only: nanoseconds the producer "
                    "freerun clock led the receiver boot clock in the log "
                    "(default 356e6, the flight0114 measurement); drives the "
                    "GPS adapter future gate.")
    ap.add_argument("--no-gnss", action="store_true", help="skip the GNSS stream")
    ap.add_argument("--no-flow", action="store_true",
                    help="skip the optical-flow stream")
    ap.add_argument("--dry-run", action="store_true",
                    help="write frames to a file instead of sending")
    ap.add_argument("--imu-out", metavar="FILE",
                    help="also write the 800 Hz IMU samples in sensor axes "
                    "(binary, or CSV if FILE ends in .csv)")
    args = ap.parse_args()

    if args.rate <= 0:
        raise SystemExit("--rate must be positive")
    if not args.dry_run and not args.dest:
        raise SystemExit("--dest is required unless --dry-run")

    STREAMS["gnss"]["port"] = args.gnss_port
    STREAMS["flow"]["port"] = args.flow_port
    active = {}
    if not args.no_gnss:
        active["gnss"] = STREAMS["gnss"]
    if not args.no_flow:
        active["flow"] = STREAMS["flow"]
    if not active:
        raise SystemExit("nothing to send: both streams disabled")

    wanted = [cfg["channel"] for cfg in active.values()]
    if args.imu_out:
        wanted.append("control_imu")
    channels = read_channels(args.log, wanted)

    if args.imu_out:
        write_imu(args.imu_out, channels)

    schedule, _origin = build_schedule(channels, active, args.rate,
                                       args.start, args.end)
    session_id, frames = make_frames(schedule, args.time_base,
                                     args.freerun_offset_ns)

    if args.dry_run:
        base = os.path.splitext(os.path.basename(args.log))[0]
        out_path = base + ".wireframes"
        run_dry_run(frames, session_id, out_path)
        return

    print("session_id 0x%016x time-base %s rate %g -> %s"
          % (session_id, args.time_base, args.rate, args.dest))
    counts = run_send(frames, args.dest, args.rate)
    print("sent %d frames (%s)"
          % (sum(counts.values()),
             ", ".join("%s=%d" % (k, counts[k]) for k in active)))


if __name__ == "__main__":
    main()
