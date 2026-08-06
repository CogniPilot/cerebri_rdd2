#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Ground-side peer for the RDD2 synapse serial transport.

Speaks the compact synapse serial framing that ``fbs/transport.fbs`` points
constrained byte-stream links at, so it can inject a GNSS fix into the vehicle
over a SiK telemetry radio and print whatever the vehicle streams back.

    ./synapse_serial.py selftest
    ./synapse_serial.py monitor            /dev/ttyUSB0
    ./synapse_serial.py send-gnss          /dev/ttyUSB0 --lat 37.7749 --lon -122.4194 --alt 12.0

Frame layout, little-endian throughout:

    off  size  field
     0     2   sync      0x53 0x59 ('S','Y')
     2     2   len       payload byte count
     4     2   topic_id  synapse catalog TopicId
     6     1   seq       wrapping counter
     7     1   flags     reserved, zero
     8     N   payload   bare fixed-layout struct
    8+N    2   crc16     CRC-16/CCITT-FALSE over bytes [2, 8+N)
"""

from __future__ import annotations

import argparse
import struct
import sys
import time
from dataclasses import dataclass

SYNC = b"\x53\x59"
HEADER_SIZE = 8
TRAILER_SIZE = 2
CRC_SEED = 0xFFFF

# Must match CONFIG_RDD2_ZROS_SERIAL_MAX_PAYLOAD (subsys/zros_serial/Kconfig).
# Accepting a larger length here than the firmware does is not permissive, it
# is wrong: the firmware rejects an over-long length immediately and recovers
# the frame behind it, while a decoder that waits for the bogus payload
# swallows that frame instead.
MAX_PAYLOAD = 128

# synapse_fbs 0.7.0 catalog ids.
TOPIC_GNSS_FIX = 8
TOPIC_NAMES = {
    1: "health",
    4: "manual",
    5: "imu",
    8: "gnss",
    11: "att",
    19: "att_sp",
    25: "pwm",
    26: "loop",
}

# synapse.topic.GnssFixData, 64 bytes with 7 trailing pad bytes.
GNSS_FIX_STRUCT = struct.Struct("<QQiiiiHHHHHHHHHhBBBBB7x")
assert GNSS_FIX_STRUCT.size == 64

# synapse.types.GnssFixType
FIX_TYPES = {
    0: "NoFix",
    1: "TimeOnly",
    2: "Fix2d",
    3: "Fix3d",
    4: "Dgnss",
    5: "RtkFloat",
    6: "RtkFixed",
    7: "DeadReckoning",
}

# synapse.topic.GnssFixFlags
FLAG_TIME_VALID = 1
FLAG_COURSE_VALID = 2
FLAG_YAW_VALID = 4
FLAG_VELOCITY_UP_VALID = 8

UINT16_MAX = 0xFFFF


def crc16_ccitt_false(data: bytes, seed: int = CRC_SEED) -> int:
    """Matches Zephyr's crc16_itu_t(0xffff, ...); check("123456789") == 0x29b1."""
    crc = seed
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def saturate_u16(value: float) -> int:
    """The schema requires producers to saturate accuracy fields, never truncate."""
    if value < 0:
        return 0
    return min(int(round(value)), UINT16_MAX)


@dataclass
class GnssFix:
    timestamp_us: int = 0
    time_unix_us: int = 0
    latitude_deg_e7: int = 0
    longitude_deg_e7: int = 0
    altitude_msl_mm: int = 0
    altitude_ellipsoid_mm: int = 0
    horizontal_accuracy_mm: int = 0
    vertical_accuracy_mm: int = 0
    velocity_accuracy_mm_s: int = 0
    yaw_accuracy_cdeg: int = 0
    hdop_centi: int = 0
    vdop_centi: int = 0
    ground_speed_cm_s: int = 0
    course_over_ground_cdeg: int = 0
    yaw_cdeg: int = 0
    velocity_up_cm_s: int = 0
    flags: int = 0
    fix_type: int = 3
    satellites_used: int = 0
    satellites_visible: int = 0
    id: int = 0

    def pack(self) -> bytes:
        return GNSS_FIX_STRUCT.pack(
            self.timestamp_us,
            self.time_unix_us,
            self.latitude_deg_e7,
            self.longitude_deg_e7,
            self.altitude_msl_mm,
            self.altitude_ellipsoid_mm,
            self.horizontal_accuracy_mm,
            self.vertical_accuracy_mm,
            self.velocity_accuracy_mm_s,
            self.yaw_accuracy_cdeg,
            self.hdop_centi,
            self.vdop_centi,
            self.ground_speed_cm_s,
            self.course_over_ground_cdeg,
            self.yaw_cdeg,
            self.velocity_up_cm_s,
            self.flags,
            self.fix_type,
            self.satellites_used,
            self.satellites_visible,
            self.id,
        )

    @classmethod
    def unpack(cls, payload: bytes) -> "GnssFix":
        return cls(*GNSS_FIX_STRUCT.unpack(payload))

    def describe(self) -> str:
        return (
            f"fix={FIX_TYPES.get(self.fix_type, self.fix_type)} "
            f"lat={self.latitude_deg_e7 / 1e7:.7f} lon={self.longitude_deg_e7 / 1e7:.7f} "
            f"alt={self.altitude_msl_mm / 1000.0:.2f}m "
            f"sats={self.satellites_used}/{self.satellites_visible} "
            f"hacc={self.horizontal_accuracy_mm / 1000.0:.2f}m "
            f"speed={self.ground_speed_cm_s / 100.0:.2f}m/s"
        )


def encode_frame(topic_id: int, payload: bytes, seq: int) -> bytes:
    body = struct.pack("<HHBB", len(payload), topic_id, seq & 0xFF, 0) + payload
    return SYNC + body + struct.pack("<H", crc16_ccitt_false(body))


class FrameDecoder:
    """Incremental decoder; feed it arbitrary byte chunks."""

    def __init__(self, max_payload: int = MAX_PAYLOAD) -> None:
        self.max_payload = max_payload
        self.buf = bytearray()
        self.crc_errors = 0

    def feed(self, chunk: bytes) -> list:
        """Consume a chunk and return the frames it completed.

        Deliberately eager rather than a generator: as a generator the buffer
        append would not happen until the caller iterated, so ignoring the
        result — or breaking out of the loop early — would silently discard
        bytes that can never be recovered.
        """
        self.buf.extend(chunk)
        frames = []
        while True:
            start = self.buf.find(SYNC)
            if start < 0:
                # Keep one byte in case a sync word straddles two chunks.
                del self.buf[: max(0, len(self.buf) - 1)]
                return frames
            del self.buf[:start]

            if len(self.buf) < HEADER_SIZE:
                return frames

            length, topic_id, seq, _flags = struct.unpack("<HHBB", self.buf[2:HEADER_SIZE])
            if length == 0 or length > self.max_payload:
                del self.buf[:2]
                continue

            total = HEADER_SIZE + length + TRAILER_SIZE
            if len(self.buf) < total:
                return frames

            body = bytes(self.buf[2 : HEADER_SIZE + length])
            (want,) = struct.unpack("<H", self.buf[HEADER_SIZE + length : total])
            if crc16_ccitt_false(body) != want:
                self.crc_errors += 1
                del self.buf[:2]
                continue

            payload = bytes(self.buf[HEADER_SIZE : HEADER_SIZE + length])
            del self.buf[:total]
            frames.append((topic_id, seq, payload))


def open_port(device: str, baud: int):
    try:
        import serial  # type: ignore
    except ImportError:
        sys.exit("pyserial is required for serial commands: pip install pyserial")
    return serial.Serial(device, baud, timeout=0.1)


def build_fix(args: argparse.Namespace, boot_us: int) -> GnssFix:
    flags = FLAG_TIME_VALID
    if args.course is not None:
        flags |= FLAG_COURSE_VALID

    return GnssFix(
        timestamp_us=boot_us,
        time_unix_us=int(time.time() * 1e6),
        latitude_deg_e7=int(round(args.lat * 1e7)),
        longitude_deg_e7=int(round(args.lon * 1e7)),
        altitude_msl_mm=int(round(args.alt * 1000.0)),
        altitude_ellipsoid_mm=int(round(args.alt * 1000.0)),
        horizontal_accuracy_mm=saturate_u16(args.hacc * 1000.0),
        vertical_accuracy_mm=saturate_u16(args.vacc * 1000.0),
        velocity_accuracy_mm_s=saturate_u16(0.5 * 1000.0),
        hdop_centi=saturate_u16(args.hdop * 100.0),
        vdop_centi=saturate_u16(args.hdop * 100.0),
        ground_speed_cm_s=saturate_u16(args.speed * 100.0),
        course_over_ground_cdeg=saturate_u16((args.course or 0.0) * 100.0),
        flags=flags,
        fix_type=args.fix_type,
        satellites_used=args.sats,
        satellites_visible=args.sats,
        id=args.instance,
    )


def cmd_send_gnss(args: argparse.Namespace) -> int:
    port = open_port(args.device, args.baud)
    period = 1.0 / args.rate
    start = time.monotonic()
    sent = 0

    print(f"sending gnss on {args.device} at {args.rate} Hz, ctrl-c to stop")
    try:
        while True:
            boot_us = int((time.monotonic() - start) * 1e6)
            # seq is a wrapping byte on the wire; count frames separately.
            frame = encode_frame(TOPIC_GNSS_FIX, build_fix(args, boot_us).pack(), sent & 0xFF)
            port.write(frame)
            sent += 1
            if args.count and sent >= args.count:
                break
            time.sleep(period)
    except KeyboardInterrupt:
        print()
    return 0


def cmd_monitor(args: argparse.Namespace) -> int:
    port = open_port(args.device, args.baud)
    decoder = FrameDecoder()
    counts: dict[int, int] = {}

    print(f"monitoring {args.device} at {args.baud} baud, ctrl-c to stop")
    try:
        while True:
            chunk = port.read(256)
            if not chunk:
                continue
            for topic_id, seq, payload in decoder.feed(chunk):
                counts[topic_id] = counts.get(topic_id, 0) + 1
                name = TOPIC_NAMES.get(topic_id, f"id{topic_id}")
                if topic_id == TOPIC_GNSS_FIX and len(payload) == GNSS_FIX_STRUCT.size:
                    print(f"[{seq:3d}] {name:8} {GnssFix.unpack(payload).describe()}")
                else:
                    print(f"[{seq:3d}] {name:8} {len(payload):3d} B  n={counts[topic_id]}")
    except KeyboardInterrupt:
        print(f"\ncrc errors: {decoder.crc_errors}")
    return 0


def cmd_selftest(args: argparse.Namespace) -> int:
    del args
    failures = 0

    # Zephyr asserts this exact value for crc16_itu_t(0xffff, ...) in
    # tests/unit/crc/main.c, which is what the firmware calls.
    check = crc16_ccitt_false(b"123456789")
    ok = check == 0x29B1
    failures += not ok
    print(f"{'ok  ' if ok else 'FAIL'} crc16 check value: 0x{check:04x} (want 0x29b1)")

    ok = GNSS_FIX_STRUCT.size == 64
    failures += not ok
    print(f"{'ok  ' if ok else 'FAIL'} GnssFixData size: {GNSS_FIX_STRUCT.size} (want 64)")

    fix = GnssFix(
        latitude_deg_e7=377749000,
        longitude_deg_e7=-1224194000,
        altitude_msl_mm=12000,
        fix_type=3,
        satellites_used=11,
    )
    frame = encode_frame(TOPIC_GNSS_FIX, fix.pack(), 42)
    ok = len(frame) == 74
    failures += not ok
    print(f"{'ok  ' if ok else 'FAIL'} framed gnss size: {len(frame)} (want 74)")

    # Round trip through the decoder, byte at a time, behind leading garbage.
    decoder = FrameDecoder()
    decoded = []
    for chunk in (b"\x00\xffnoise", *[bytes([b]) for b in frame]):
        decoded.extend(decoder.feed(chunk))
    ok = len(decoded) == 1 and decoded[0][0] == TOPIC_GNSS_FIX and decoded[0][1] == 42
    failures += not ok
    print(f"{'ok  ' if ok else 'FAIL'} decode after garbage: {len(decoded)} frame(s)")

    if decoded:
        back = GnssFix.unpack(decoded[0][2])
        ok = back == fix
        failures += not ok
        print(f"{'ok  ' if ok else 'FAIL'} round trip: {back.describe()}")

    # A single flipped bit must be rejected.
    corrupt = bytearray(frame)
    corrupt[20] ^= 0x01
    decoder = FrameDecoder()
    ok = len(list(decoder.feed(bytes(corrupt)))) == 0 and decoder.crc_errors == 1
    failures += not ok
    print(f"{'ok  ' if ok else 'FAIL'} corrupt frame rejected")

    print("FAILED" if failures else "all checks passed")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    def add_port_args(p: argparse.ArgumentParser) -> None:
        p.add_argument("device", help="serial device, e.g. /dev/ttyUSB0")
        p.add_argument("--baud", type=int, default=57600, help="default: 57600 (SiK default)")

    send = sub.add_parser("send-gnss", help="inject a GNSS fix into the vehicle")
    add_port_args(send)
    send.add_argument("--lat", type=float, required=True, help="degrees")
    send.add_argument("--lon", type=float, required=True, help="degrees")
    send.add_argument("--alt", type=float, default=0.0, help="metres above MSL")
    send.add_argument("--speed", type=float, default=0.0, help="ground speed m/s")
    send.add_argument("--course", type=float, default=None, help="course over ground degrees")
    send.add_argument("--hacc", type=float, default=1.5, help="horizontal accuracy metres")
    send.add_argument("--vacc", type=float, default=2.5, help="vertical accuracy metres")
    send.add_argument("--hdop", type=float, default=0.9)
    send.add_argument("--sats", type=int, default=12)
    send.add_argument("--fix-type", type=int, default=3, help="3 = Fix3d")
    send.add_argument("--instance", type=int, default=0, help="GnssFixData.id")
    send.add_argument("--rate", type=float, default=10.0, help="Hz")
    send.add_argument("--count", type=int, default=0, help="stop after N frames")
    send.set_defaults(func=cmd_send_gnss)

    monitor = sub.add_parser("monitor", help="decode frames streamed by the vehicle")
    add_port_args(monitor)
    monitor.set_defaults(func=cmd_monitor)

    selftest = sub.add_parser("selftest", help="check framing against known vectors")
    selftest.set_defaults(func=cmd_selftest)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
