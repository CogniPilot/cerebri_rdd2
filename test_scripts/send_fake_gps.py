#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Stream a fake GNSS fix to the drone over the SiK telemetry radio.

Bring-up check for the csyn serial transport: answers "is the drone receiving
anything at all?" without needing a real GPS.

    ./send_fake_gps.py                     # /dev/ttyUSB0 at 57600
    ./send_fake_gps.py /dev/ttyACM0
    ./send_fake_gps.py /dev/ttyUSB0 --baud 115200

Then on the drone shell:

    csyn_serial status      -> rx frames= should be climbing
    csyn topic echo gnss    -> latitude should be ticking upward

The position walks slowly north on purpose, so a frozen value on the drone
means stale data rather than a live link.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

# The wire format lives with the ground tool; never duplicate it here, or the
# two ends can silently drift apart.
_TOOLS = Path(__file__).resolve().parent.parent / "tools" / "synapse_serial"
sys.path.insert(0, str(_TOOLS))
try:
    from synapse_serial import (FLAG_COURSE_VALID, FLAG_TIME_VALID, FrameDecoder,
                                GnssFix, TOPIC_GNSS_FIX, TOPIC_NAMES, encode_frame)
except ImportError as exc:  # pragma: no cover
    sys.exit(f"cannot import the wire format from {_TOOLS}: {exc}")

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("device", nargs="?", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=57600, help="SiK default is 57600")
    ap.add_argument("--rate", type=float, default=5.0, help="fixes per second")
    ap.add_argument("--lat", type=float, default=37.7749, help="starting latitude")
    ap.add_argument("--lon", type=float, default=-122.4194, help="starting longitude")
    ap.add_argument("--alt", type=float, default=25.0, help="metres above MSL")
    ap.add_argument("--still", action="store_true", help="do not walk north")
    args = ap.parse_args()

    try:
        port = serial.Serial(args.device, args.baud, timeout=0)
    except serial.SerialException as exc:
        print(f"cannot open {args.device}: {exc}", file=sys.stderr)
        return 1

    print(f"sending fake gnss -> {args.device} @ {args.baud} baud, {args.rate} Hz")
    print("watch the drone with:  csyn_serial status   /   csyn topic echo gnss")
    print("ctrl-c to stop\n")

    lat_e7 = int(round(args.lat * 1e7))
    lon_e7 = int(round(args.lon * 1e7))
    alt_mm = int(round(args.alt * 1000.0))
    # 100 units of 1e-7 deg latitude per frame, about 1.11 m. Derive the
    # reported ground speed from that and the frame rate rather than stating a
    # constant, so --rate and --still cannot make the fix contradict itself.
    step_e7 = 0 if args.still else 100
    speed_cm_s = int(round(step_e7 * 1.11e-2 * args.rate * 100.0))
    flags = FLAG_TIME_VALID | (FLAG_COURSE_VALID if step_e7 else 0)

    decoder = FrameDecoder()
    start = time.monotonic()
    sent = 0
    from_drone: dict[str, int] = {}
    last_report = 0.0

    try:
        while True:
            fix = GnssFix(
                timestamp_us=int((time.monotonic() - start) * 1e6),
                time_unix_us=int(time.time() * 1e6),
                latitude_deg_e7=lat_e7,
                longitude_deg_e7=lon_e7,
                altitude_msl_mm=alt_mm,
                altitude_ellipsoid_mm=alt_mm,
                horizontal_accuracy_mm=1200,
                vertical_accuracy_mm=2000,
                velocity_accuracy_mm_s=300,
                hdop_centi=90,
                vdop_centi=140,
                ground_speed_cm_s=speed_cm_s,
                course_over_ground_cdeg=0,   # due north
                flags=flags,
                fix_type=3,          # Fix3d
                satellites_used=12,
                satellites_visible=17,
            )
            port.write(encode_frame(TOPIC_GNSS_FIX, fix.pack(), sent & 0xFF))
            sent += 1
            lat_e7 += step_e7

            # Anything coming back proves the radio pair works both ways.
            waiting = port.in_waiting
            if waiting:
                for topic_id, _seq, _payload in decoder.feed(port.read(waiting)):
                    name = TOPIC_NAMES.get(topic_id, f"id{topic_id}")
                    from_drone[name] = from_drone.get(name, 0) + 1

            now = time.monotonic()
            if now - last_report >= 1.0:
                last_report = now
                back = ", ".join(f"{k}={v}" for k, v in sorted(from_drone.items()))
                print(f"\rsent {sent:6d} frames  lat={lat_e7 / 1e7:.7f}  "
                      f"from drone: {back or 'nothing yet'}   ", end="", flush=True)

            time.sleep(1.0 / args.rate)
    except KeyboardInterrupt:
        pass

    print(f"\n\nsent {sent} frames ({sent * 74} bytes)")
    if from_drone:
        print(f"received from drone: {from_drone}  (radio link is good both ways)")
    else:
        print("received nothing from the drone -- see the notes in README.md")
    if decoder.crc_errors:
        print(f"crc errors on the inbound direction: {decoder.crc_errors}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
